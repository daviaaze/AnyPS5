#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/DrawWriteRanges.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics/StreamReuse.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/BufferCopy.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/DwordPatternFill.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleTarget.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BatchTimeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "Optimization/ShaderStageInputInfo.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Prepare/PrepareFarm.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <limits>
#include <utility>

namespace AgcDriver::DriverDetail {

namespace {

bool DispatchItemsEnabled() {
    static const bool enabled = [] {
        const char* text = std::getenv("APS5_PIPELINE_DISPATCH");
        return text != nullptr && text[0] != '\0' && text[0] != '0';
    }();
    return enabled;
}

enum class ItemFallback : std::size_t { Miss, Reads, NoRecipe, Writes, Verify, Count };
constexpr std::array<const char*, static_cast<std::size_t>(ItemFallback::Count)> ItemFallbackNames{"miss", "reads", "no recipe", "writes", "verify"};
std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(ItemFallback::Count)> itemFallbacks{};
std::atomic<std::uint64_t> itemCommits{0};
std::atomic<std::uint64_t> itemRebuilds{0};
std::atomic<std::uint64_t> itemMisses{0};
std::atomic<std::uint64_t> itemRecipeLess{0};
std::atomic<std::uint64_t> itemNotPlain{0};

bool DispatchMissItemsEnabled() {
    return Graphics::RuntimeSwitches::Set(Graphics::RuntimeSwitch::PipelineDispatchMisses);
}

void NoteItemFallback(ItemFallback reason) {
    itemFallbacks[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
}

void ReportDispatchItems(bool profile) {
    static auto lastReport = std::chrono::steady_clock::now();
    if (!profile) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport < std::chrono::seconds(10)) return;
    lastReport = now;
    std::string drained;
    for (std::size_t i = 0; i < ItemFallbackNames.size(); ++i) {
        char item[48];
        std::snprintf(item, sizeof(item), " %s %llu", ItemFallbackNames[i], static_cast<unsigned long long>(itemFallbacks[i].load(std::memory_order_relaxed)));
        drained += item;
    }
    std::fprintf(stderr, "[dispatch-item] %llu commit items (%llu recipes rebuilt on the committer, %llu queued after a cache miss, %llu queued without a recipe, %llu cached ones kept off the queue as not plain); drained instead, cumulative:%s\n", static_cast<unsigned long long>(itemCommits.load(std::memory_order_relaxed)), static_cast<unsigned long long>(itemRebuilds.load(std::memory_order_relaxed)), static_cast<unsigned long long>(itemMisses.load(std::memory_order_relaxed)), static_cast<unsigned long long>(itemRecipeLess.load(std::memory_order_relaxed)), static_cast<unsigned long long>(itemNotPlain.load(std::memory_order_relaxed)), drained.c_str());
}

std::atomic<std::uint64_t> aheadAdoptionsProven{0};

// Whether the guest bytes an ahead capture read are still there. Every region is first checked for
// pending GPU writes and has landed GPU writes flushed; then a write-tracking proof replaces the
// byte comparison, which stays the fallback: a miss's ShaderMemory::ReadsUnchanged, or a hit's
// `proof` (the cache entry's regions and the generations collected before the lookup validated
// them) when it covers every captured region.
bool CapturedCurrent(std::span<const ShaderRecompiler::MemoryRegion> captured, const ShaderSnapshot& code, const ShaderMemory* memory, std::span<const GuestMemory::UnchangedQuery> proof) {
    const auto skipped = [&](std::uint64_t address, std::size_t bytes) {
        return bytes == 0 || (address >= code.codeAddress && address < code.codeAddress + code.code.size() * sizeof(std::uint32_t)) || (!code.header.empty() && address >= code.headerAddress && address < code.headerAddress + code.header.size());
    };
    bool covered = !proof.empty();
    for (const auto& region : captured) {
        const auto address = region.guestAddress;
        const auto bytes = region.bytes.size();
        if (skipped(address, bytes)) continue;
        if (Driver::QueryPendingSpan(address, bytes, {}) != ShaderMemory::PendingWrite::None || !GuestMemory::Accessible(reinterpret_cast<const void*>(address), bytes)) return false;
        GuestMemory::GuardRead(address, bytes);
        GuestMemory::FlushGpuWrites(address, bytes);
        covered = covered && std::any_of(proof.begin(), proof.end(), [&](const auto& query) {
            return query.address <= address && address + bytes <= query.address + query.bytes;
        });
    }
    const bool proved = (memory != nullptr && memory->ReadsUnchanged()) || (covered && [&] {
        for (const auto& query : proof) static_cast<void>(GuestMemory::CollectWritesUncached(query.address, query.bytes));
        return GuestMemory::UnchangedSinceAll(proof);
    }());
    if (proved) {
        aheadAdoptionsProven.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    for (const auto& region : captured) {
        const auto address = region.guestAddress;
        const auto bytes = region.bytes.size();
        if (skipped(address, bytes)) continue;
        if (std::memcmp(region.bytes.data(), reinterpret_cast<const void*>(address), bytes) != 0) return false;
    }
    return true;
}

struct DispatchFarmValue final : ::Driver::PrepareFarm::Result::Value {
    DispatchFarmValue(DispatchPreparationResult preparation,
            std::shared_ptr<const ShaderSnapshot> shaderSnapshot,
            std::shared_ptr<VulkanDevice> deviceOwner,
            std::span<const std::uint32_t> words,
            std::span<const std::uint32_t> roots)
        : prepared(std::move(preparation)), shader(std::move(shaderSnapshot)),
          device(std::move(deviceOwner)), packet(words.begin(), words.end()),
          userDataRoots(roots.begin(), roots.end()) {}
    DispatchPreparationResult prepared;
    std::shared_ptr<const ShaderSnapshot> shader;
    std::shared_ptr<VulkanDevice> device;
    std::vector<std::uint32_t> packet;
    std::vector<std::uint32_t> userDataRoots;
};
std::uint64_t DispatchRegisterFingerprint(const QueueState &queue) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    const auto mix = [&](std::uint64_t value) {
        hash ^= value;
        hash *= 0x100000001b3ull;
    };
    const auto registers = [&](const auto &values, std::uint64_t group) {
        mix(group);
        for (const auto &[offset, value] : values) {
            mix(offset);
            mix(value);
        }
    };
    registers(queue.shader, 1);
    registers(queue.context, 2);
    registers(queue.userConfig, 3);
    return hash;
}
struct PriorDispatchWrites {
    std::uint64_t serial = 0;
    bool unknown = false;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
};

PriorDispatchWrites& EarlierDispatchWrites() {
    static thread_local PriorDispatchWrites writes;
    return writes;
}

void ResetEarlierDispatchWrites(std::uint64_t serial) {
    auto& writes = EarlierDispatchWrites();
    if (writes.serial != serial) {
        writes = {};
        writes.serial = serial;
    }
}

bool ReadsOverlapEarlierDispatchWrites(const ShaderRecompiler::RecompileResult& compiled,
                                       std::uint64_t serial) {
    ResetEarlierDispatchWrites(serial);
    const auto& earlier = EarlierDispatchWrites();
    if (!earlier.unknown && earlier.ranges.empty()) return false;
    bool bounded = compiled.bdaAbiVersion == 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> reads;
    for (const auto& binding : compiled.bindings) {
        if (binding.role == ShaderRecompiler::DescriptorRole::Gds ||
            binding.role == ShaderRecompiler::DescriptorRole::BdaPagetable) {
            bounded = false;
            continue;
        }
        if (binding.role == ShaderRecompiler::DescriptorRole::GuestImages) {
            if (binding.count != 0) bounded = false;
            continue;
        }
        if (binding.role != ShaderRecompiler::DescriptorRole::GuestBuffers) continue;
        for (std::uint32_t element = 0; element < binding.count; ++element) {
            // Atomic read-modify-write operations consume prior buffer contents even if
            // bufferRead metadata only marks ordinary loads.
            const bool elementReads = element >= binding.bufferRead.size() ||
                binding.bufferRead[element] ||
                (element < binding.bufferAtomic.size() && binding.bufferAtomic[element]);
            if (!elementReads) continue;
            const auto wordOffset = static_cast<std::size_t>(element) * 4u;
            if (binding.guestDescriptor.size() < wordOffset + 4u) {
                bounded = false;
                continue;
            }
            const auto words = binding.guestDescriptor.Span().subspan(wordOffset, 4u);
            const ShaderRecompiler::ShaderBufferResource descriptor{
                {words[0], words[1], words[2], words[3]}};
            const auto base = descriptor.Base48();
            const auto size = descriptor.GetSize();
            if (base == 0 || size == 0 ||
                size > std::numeric_limits<std::uint64_t>::max() - base) continue;
            if (element < binding.bufferFootprints.size()) {
                const auto& footprint = binding.bufferFootprints[element];
                if (footprint.bounded && footprint.readBegin <= footprint.readEnd &&
                    footprint.readEnd <= size) {
                    if (footprint.readBegin != footprint.readEnd)
                        reads.emplace_back(base + footprint.readBegin, base + footprint.readEnd);
                    continue;
                }
            }
            reads.emplace_back(base, base + size);
        }
    }
    if (!bounded) return true;
    if (earlier.unknown) return true;
    return std::any_of(reads.begin(), reads.end(), [&](const auto& read) {
        return std::any_of(earlier.ranges.begin(), earlier.ranges.end(), [&](const auto& write) {
            return read.first < write.second && write.first < read.second;
        });
    });
}

void RememberDispatchWrites(const ShaderRecompiler::RecompileResult& compiled,
                            std::uint64_t serial) {
    ResetEarlierDispatchWrites(serial);
    auto& earlier = EarlierDispatchWrites();
    bool bounded = false;
    auto ranges = DispatchWriteRanges(compiled, bounded);
    earlier.unknown |= !bounded;
    earlier.ranges.insert(earlier.ranges.end(), ranges.begin(), ranges.end());
}

struct DispatchFarmCallable final : ::Driver::PrepareFarm::Callable {
    explicit DispatchFarmCallable(DispatchPrepareInput input) : input(std::move(input)) {}
    ::Driver::PrepareFarm::Result Run() override {
        auto prepared = PrepareDispatchOwned(input);
        require(prepared.has_value(), "background dispatch capture returned no result (TryCapture found pending or unstable input)");
        ::Driver::PrepareFarm::Result result;
        result.value = std::make_unique<DispatchFarmValue>(
            std::move(*prepared), input.shader, input.device, input.packet, input.userData);
        return result;
    }
    DispatchPrepareInput input;
};
}

DispatchPrepareInput::DispatchPrepareInput(std::span<const std::uint32_t> words,
        std::shared_ptr<const ShaderSnapshot> shaderOwner, std::shared_ptr<const ShaderRegistry> registryOwner,
        std::shared_ptr<VulkanDevice> deviceOwner, std::size_t offset,
        const ShaderRecompiler::RecompileRequest& source, std::uint64_t dispatchKey,
        std::uint64_t fingerprint, std::uint32_t queueId, std::uint64_t serial, std::size_t packetPosition)
    : packet(words.begin(), words.end()), shader(std::move(shaderOwner)), registry(std::move(registryOwner)),
      device(std::move(deviceOwner)), codeOffset(offset),
      userData(source.context.userData.begin(), source.context.userData.end()),
      linkedCallFallbacks(source.context.linkedCallFallbacks.begin(), source.context.linkedCallFallbacks.end()),
      memory(source.context.memory.begin(), source.context.memory.end()), request(source),
      key(dispatchKey), registerFingerprint(fingerprint), queue(queueId), submissionSerial(serial), packetOffset(packetPosition) {
    RebindRequest();
}

DispatchPrepareInput::DispatchPrepareInput(DispatchPrepareInput&& other) noexcept
    : packet(std::move(other.packet)), shader(std::move(other.shader)), registry(std::move(other.registry)),
      device(std::move(other.device)), codeOffset(other.codeOffset), userData(std::move(other.userData)),
      linkedCallFallbacks(std::move(other.linkedCallFallbacks)), memory(std::move(other.memory)),
      request(std::move(other.request)), key(other.key), registerFingerprint(other.registerFingerprint),
      queue(other.queue), submissionSerial(other.submissionSerial), packetOffset(other.packetOffset) {
    RebindRequest();
}

DispatchPrepareInput& DispatchPrepareInput::operator=(DispatchPrepareInput&& other) noexcept {
    if (this == &other) return *this;
    packet = std::move(other.packet);
    shader = std::move(other.shader);
    registry = std::move(other.registry);
    device = std::move(other.device);
    codeOffset = other.codeOffset;
    userData = std::move(other.userData);
    linkedCallFallbacks = std::move(other.linkedCallFallbacks);
    memory = std::move(other.memory);
    request = std::move(other.request);
    key = other.key;
    registerFingerprint = other.registerFingerprint;
    queue = other.queue;
    submissionSerial = other.submissionSerial;
    packetOffset = other.packetOffset;
    RebindRequest();
    return *this;
}

void DispatchPrepareInput::RebindRequest() {
    require(shader != nullptr, "owned dispatch input is missing its shader snapshot");
    request.shader.code = std::span(shader->code).subspan(codeOffset);
    request.shader.header = shader->header;
    request.context.userData = userData;
    request.context.memory = memory;
    request.context.linkedCallFallbacks = linkedCallFallbacks;
}

std::uint64_t EstimateBytes(const DispatchPrepareInput& input) {
    return sizeof(input)
        + input.packet.capacity() * sizeof(std::uint32_t)
        + input.userData.capacity() * sizeof(std::uint32_t)
        + input.linkedCallFallbacks.capacity() * sizeof(std::uint32_t)
        + input.memory.capacity() * sizeof(ShaderRecompiler::MemoryRegion);
}

template <typename Capture, typename CaptureComplete, typename InvocationComplete>
std::optional<DispatchPreparationResult> PrepareDispatchSteps(
        std::shared_ptr<ShaderMemory> shaderMemory, std::uint64_t forgetSerial,
        const ShaderSnapshot& snapshot, std::size_t codeOffset,
        const ShaderRecompiler::RecompileRequest& request, std::uint64_t key,
        std::uint64_t registerFingerprint, Capture&& capture,
        CaptureComplete&& captureComplete, InvocationComplete&& invocationComplete) {
    require(shaderMemory != nullptr, "dispatch preparation is missing retained shader memory");
    auto invocation = InvocationFor(snapshot, codeOffset, request);
    std::forward<InvocationComplete>(invocationComplete)();
    auto capturedResource = std::forward<Capture>(capture)(invocation);
    if (!capturedResource) return std::nullopt;
    DispatchPreparationResult result;
    result.shaderMemory = std::move(shaderMemory);
    result.forgetSerial = forgetSerial;
    result.capture = std::move(*capturedResource);
    result.captured = result.shaderMemory->Regions();
    std::forward<CaptureComplete>(captureComplete)(result.captured);
    result.compiledResult = invocation.Materialize(*result.capture);
    result.key = key;
    result.registerFingerprint = registerFingerprint;
    return result;
}

std::optional<DispatchPreparationResult> PrepareDispatchOwned(DispatchPrepareInput& input) {
    input.RebindRequest();
    require(input.registry != nullptr && input.device != nullptr,
        "owned dispatch input is missing retained registry or device");
    auto shaderMemory = std::make_shared<ShaderMemory>(input.memory,
        &Driver::queryPendingWrite, &Driver::observePendingWrite,
        Driver::hookWaitCounter(), &Driver::QueryPendingSpan);
    const auto forgetSerial = GuestMemory::ForgetSerial();
    auto captureMemory = shaderMemory;
    return PrepareDispatchSteps(
        std::move(shaderMemory), forgetSerial, *input.shader, input.codeOffset,
        input.request, input.key, input.registerFingerprint,
        [captureMemory](auto& invocation) { return captureMemory->TryCapture(invocation); },
        [](const auto&) {}, [] {});
}

std::uint64_t Driver::AheadAdoptionsProven() {
    return aheadAdoptionsProven.load(std::memory_order_relaxed);
}

bool Driver::pipelineDispatch() {
    return DispatchItemsEnabled();
}

void Driver::dispatch(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission, std::uint64_t indirectArguments) {
    const auto submittedPacket = packet;
    const auto submittedIndirectArguments = indirectArguments;
    AgcDriver::P0Metrics::Scope p0(submission.queue, AgcDriver::P0Metrics::Layer::Packet, AgcDriver::P0Metrics::Operation::Dispatch);
    std::optional<TraceProgramScope> programScope;
    PerformanceTimer timing("Driver.Dispatch");
    const auto address = (static_cast<std::uint64_t>(readRegister(queue.shader, 0x20c)) << 8u) | (static_cast<std::uint64_t>(readRegister(queue.shader, 0x20d) & 0xffu) << 40u);
    if (SkippedShader(address)) { timing.Mark("skipped_shader"); return; }
    static const bool gpuTiming = std::getenv("APS5_PROFILE_GPU") != nullptr && std::getenv("APS5_PROFILE_DRAW") == nullptr;
    if (SkippedShader(address) || RefusedDispatch(address)) return;
    auto registeredShader = RegisteredProgram(*submission.shaders, address, {0});
    const bool registeredSource = gpuTiming && registeredShader != nullptr;
    if (!registeredShader) registeredShader = ReadRawShader(address);
    const auto& snapshot = *registeredShader;
    require(snapshot.type == 0, "compute program refers to a non-compute shader");
    const auto userCount = (readRegister(queue.shader, 0x213) >> 1u) & 0x1fu;
    std::vector<std::uint32_t> userData;
    for (std::uint32_t i = 0; i < userCount; ++i) {
        userData.push_back(readUserData(queue.shader, 0x240 + i));
    }
    auto compute = Graphics::DecodeComputeStageInfo(queue.shader, snapshot.header);
    const bool resolving = resolvingAhead();
    ResetEarlierDispatchWrites(submission.serial);
    // Only the GPU-only observer tracks recursive retry calls, not PM4 packet counts.
    static thread_local unsigned observerDepth = 0;
    struct ObserverScope {
        unsigned* depth;
        explicit ObserverScope(unsigned* value) : depth(value) { if (depth) ++*depth; }
        ~ObserverScope() { if (depth) --*depth; }
    } observerScope{gpuTiming && !resolving ? &observerDepth : nullptr};
    std::vector<ShaderRecompiler::MemoryRegion> memory{{snapshot.codeAddress, std::as_bytes(std::span(snapshot.code))}};
    if (!snapshot.header.empty()) memory.push_back({snapshot.headerAddress, snapshot.header});

    timing.Mark("shader_identity_decode");
    static const bool unlockedDevice = std::getenv("APS5_NO_UNLOCKED_DEVICE") == nullptr;
    std::shared_ptr<VulkanDevice> localDevice = unlockedDevice ? device.Load() : nullptr;
    if (localDevice == nullptr) {
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Dispatch);
        std::lock_guard gpuLock(GuestMemory::GpuMutex());
        if (device == nullptr) device = std::make_shared<VulkanDevice>();
        localDevice = device;
    }
    timing.Mark("device_acquire");
    const auto codeOffset = static_cast<std::size_t>((address - snapshot.codeAddress) / sizeof(std::uint32_t));
    std::array<std::uint32_t, 5> resolved{};
    ResolvedDispatch ahead;
    bool fromAhead = false;
    auto& groupStats = groupCaptureStats();
    if (!resolving) {
        auto& parked = resolvedAhead();
        if (const auto found = parked.find(currentPacketOffset()); found != parked.end()) {
            ahead = std::move(found->second);
            parked.erase(found);
            fromAhead = ahead.address == address;
            if (!fromAhead) ++groupStats.addressMismatches;
        }
    }
    if (fromAhead) {
        resolved = ahead.packet;
        packet = resolved;
        indirectArguments = ahead.indirectArguments;
        if (ahead.registeredShader != registeredShader || ahead.userDataRoots.size() != userData.size() || !std::equal(ahead.userDataRoots.begin(), ahead.userDataRoots.end(), userData.begin())) {
            fromAhead = false;
            packet = submittedPacket;
            indirectArguments = submittedIndirectArguments;
            ++groupStats.keyMismatches;
        }
    } else if (indirectArguments != 0 && !resolving && matchesFillKernel(std::span(snapshot.code).subspan(codeOffset), userData, compute)) {
        indirectSource().path = IndirectFillKernel;
        const auto readStart = std::chrono::steady_clock::now();
        resolved = readDispatchArguments(submission.queue, indirectArguments, packet[4]);
        countIndirect(IndirectFillKernel, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - readStart).count());
        packet = resolved;
        indirectArguments = 0;
    }
    timing.Mark("ahead_adopt_indirect");
    std::array<std::uint32_t, 3> partialGroups{};
    if (indirectArguments == 0 && (packet[4] & 0x2u) != 0) {
        partialGroups = Pm4::PartialGroupThreads({packet[1], packet[2], packet[3]}, {readRegister(queue.shader, 0x207), readRegister(queue.shader, 0x208), readRegister(queue.shader, 0x209)});
    }
    const bool pipelineCandidate = !resolving && !fromAhead && DispatchItemsEnabled() && DrawPipeline::Active() && indirectArguments == 0 && deferredLabels().labels.empty() && !matchesCopyKernel(std::span(snapshot.code).subspan(codeOffset), userData, compute) && !MatchesStreamCopyKernel(std::span(snapshot.code).subspan(codeOffset), userData, compute) && !MatchesIndexedCopyKernel(std::span(snapshot.code).subspan(codeOffset), userData, compute);
    const bool partialLastGroup = partialGroups != std::array<std::uint32_t, 3>{};
    timing.Mark("pipeline_candidate_match");
    std::optional<std::pair<std::uint64_t, std::size_t>> htileClearCompletion;
    Graphics::HtileMaskClear htileMask;
    if (resolving) {
        if (indirectArguments == 0 && Graphics::MatchMultisampledSingleClear(std::span(snapshot.code).subspan(codeOffset), userData, compute.numThreads)) { timing.Mark("resolving_skip_multisampled_clear"); return; }
        if (!partialLastGroup && matchesFillKernel(std::span(snapshot.code).subspan(codeOffset), userData, compute)) { timing.Mark("resolving_skip_fill_kernel"); return; }
        if (!partialLastGroup && indirectArguments == 0 && (MatchDwordPatternFill(packet, std::span(snapshot.code).subspan(codeOffset), userData, compute) || MatchConstantDwordFill(packet, std::span(snapshot.code).subspan(codeOffset), userData, compute) || MatchClampedGlobalFill(packet, std::span(snapshot.code).subspan(codeOffset), userData, compute) || MatchMemoryValueBufferFill(packet, std::span(snapshot.code).subspan(codeOffset), userData, compute))) { timing.Mark("resolving_skip_pattern_fill"); return; }
        if (!partialLastGroup && indirectArguments == 0 && (matchesCopyKernel(std::span(snapshot.code).subspan(codeOffset), userData, compute) || MatchesStreamCopyKernel(std::span(snapshot.code).subspan(codeOffset), userData, compute) || MatchesIndexedCopyKernel(std::span(snapshot.code).subspan(codeOffset), userData, compute))) { timing.Mark("resolving_skip_copy_kernel"); return; }
    } else {
        if (indirectArguments == 0 && Graphics::TakeMultisampledSingleClear(std::span(snapshot.code).subspan(codeOffset), userData, compute.numThreads)) {
            pendingDispatchPhases().outcome = DispatchOutcome::FillHle;
            if (resolvedAhead().empty()) gateOpened() = true;
            timing.Mark("hle_multisampled_clear");
            return;
        }
        if (indirectArguments == 0 && (packet[4] & 0x20u) == 0) {
            if (const auto fill = Graphics::MatchMemoryValueFill(std::span(snapshot.code).subspan(codeOffset), userData, compute.numThreads, {packet[1], packet[2], packet[3]})) Graphics::NoteMemoryValueFill(*fill);
        }
        if (!partialLastGroup && fillBuffer(queue, submission.queue, packet, std::span(snapshot.code).subspan(codeOffset), userData, compute, localDevice)) {
            pendingDispatchPhases().outcome = DispatchOutcome::FillHle;
            if (resolvedAhead().empty()) gateOpened() = true;
            timing.Mark("hle_fill_buffer");
            return;
        }
        if (DrawPipeline::Active() && !pipelineCandidate) DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Packet, 0x15);
        timing.Mark("hle_pipeline_drain");
        if (const auto htile = Graphics::HtileDepthClearAddress(std::span(snapshot.code).subspan(codeOffset), userData, compute.numThreads); htile != 0) {
            const auto threads = (packet[4] & 0x20u) != 0 ? static_cast<std::uint64_t>(packet[1]) : static_cast<std::uint64_t>(packet[1]) * compute.numThreads[0];
            const auto nativeBytes = Graphics::HtileDepthClearBytes_nid_no_patch(std::span(snapshot.code).subspan(codeOffset), userData, compute, packet, indirectArguments);
            const auto bytes = static_cast<std::size_t>(threads * 16u);
            if (Graphics::NoteHtileDepthClear(htile, bytes, nativeBytes)) htileClearCompletion = std::pair{htile, bytes};
            if (nativeBytes != 0u) htileMask = {htile, nativeBytes, userData[5], userData[4]};
        } else if (const auto buffered = Graphics::MatchHtileBufferMaskClear(std::span(snapshot.code).subspan(codeOffset), userData, compute, packet, indirectArguments)) {
            if (Graphics::NoteHtileDepthClear(buffered->address, buffered->bytes, buffered->bytes)) htileClearCompletion = std::pair{buffered->address, buffered->bytes};
            htileMask = *buffered;
        }
        if (!partialLastGroup && indirectArguments == 0 && copyBuffer(queue, submission.queue, packet, std::span(snapshot.code).subspan(codeOffset), userData, compute, localDevice, address)) {
            pendingDispatchPhases().outcome = DispatchOutcome::CopyHle;
            if (resolvedAhead().empty()) gateOpened() = true;
            timing.Mark("hle_copy_buffer");
            return;
        }
    }
    timing.Mark("hle_probe_miss");
    if (indirectArguments == 0 && (packet[4] & 0x20u) != 0) {
        const std::array<std::uint32_t, 3> threads{packet[1], packet[2], packet[3]};
        for (std::uint32_t axis = 0; axis < 3; ++axis) {
            if (threads[axis] % compute.numThreads[axis] != 0) compute.partialThreads = threads;
        }
    }
    if (partialLastGroup) compute.partialThreads = partialGroups;
    std::array<std::uint32_t, 3> threadGroup{};
    if (indirectArguments != 0 && (packet[4] & 0x20u) != 0) {
        for (std::uint32_t axis = 0; axis < 3; ++axis) threadGroup[axis] = std::max(readRegister(queue.shader, 0x207 + axis) & 0xffffu, 1u);
        compute.partialThreads = {~0u, ~0u, ~0u};
    }
    ShaderRecompiler::RecompileRequest request{
        {ShaderRecompiler::ShaderStage::Compute, address, std::span(snapshot.code).subspan(codeOffset), snapshot.headerAddress, snapshot.header},
        {(packet[4] & 0x8000u) != 0 ? 32u : 64u, 0, userData, compute, std::nullopt, std::nullopt, memory, DispatchFloatMode(snapshot, queue.shader)},
        localDevice->ComputeTarget((packet[4] & 0x8000u) != 0 ? 32u : 64u),
        {0, 0, 0, threadGroup != std::array<std::uint32_t, 3>{} ? 0u : 128u}
    };
    timing.Mark("request_build");
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    static std::atomic<double> captureMs{0}, keyMs{0}, recompileMs{0}, deviceMs{0};
    static std::atomic<std::uint64_t> cacheHits{0};
    static std::atomic<std::uint64_t> dispatches{0};
    auto lap = std::chrono::steady_clock::now();

    std::array<double, DriverPhaseCount> phaseMs{};
    auto phaseLap = lap;
    const bool phaseProfile = profile || (gpuTiming && !resolving);
    DispatchPhaseTiming phaseTiming{phaseProfile, lap, phaseMs, phaseLap};
    if (phaseProfile && packetStartedAt() != std::chrono::steady_clock::time_point{}) phaseMs[PhasePrologue] = std::chrono::duration<double, std::milli>(lap - packetStartedAt()).count();

    static const bool noDispatchCacheEnv = std::getenv("APS5_NO_DISPATCH_CACHE") != nullptr;

    static const std::pair<std::uint64_t, std::uint64_t> probeDispatch = [] {
        const char* text = std::getenv("APS5_PROBE_DISPATCH");
        if (text == nullptr) return std::pair<std::uint64_t, std::uint64_t>{0, 0};
        char* end = nullptr;
        const auto probeAddress = std::strtoull(text, &end, 16);
        const auto index = end != nullptr && *end == ':' ? std::strtoull(end + 1, nullptr, 10) : 0ull;
        return std::pair<std::uint64_t, std::uint64_t>{probeAddress, index};
    }();
    bool probeThis = false;

    if (probeDispatch.first != 0 && (address & 0xfffffffffull) == (probeDispatch.first & 0xfffffffffull)) {
        static std::atomic<std::uint64_t> dispatchesSeen{0};
        probeThis = dispatchesSeen.fetch_add(1) == probeDispatch.second;
        if (probeThis) std::fprintf(stderr, "[gpu] probing dispatch %llu of 0x%llx\n", static_cast<unsigned long long>(probeDispatch.second), static_cast<unsigned long long>(address));
    }

    struct ProbeScope {
        bool active;
        explicit ProbeScope(bool active) : active(active) { if (active) ShaderRecompiler::SetDebugProbeActive(true); }
        ~ProbeScope() { if (active) ShaderRecompiler::SetDebugProbeActive(false); }
    } probeScope{probeThis};
    const bool noDispatchCache = noDispatchCacheEnv || probeThis;
    std::uint64_t key = 0xcbf29ce484222325ull;
    const auto mix = [&](std::uint64_t value) {
        key ^= value;
        key *= 0x100000001b3ull;
    };
    mix(address);
    mix(packet[4] & 0x8000u);
    for (const auto threads : compute.partialThreads) mix(threads);
    mix(userData.size());
    if (registeredShader == nullptr) for (const auto word : userData) mix(word);
    static const bool keyHygiene = std::getenv("APS5_NO_DISPATCH_KEY_HYGIENE") == nullptr;
    if (keyHygiene) {
        for (const auto offset : {0x207u, 0x208u, 0x209u, 0x212u, 0x213u, 0x228u}) {
            const auto found = queue.shader.find(offset);

            mix(found == queue.shader.end() ? (1ull << 32u) : found->second);
        }
    } else {
        for (const auto& [offset, value] : queue.shader) {
            if (offset >= 0x240u && offset < 0x240u + userData.size()) continue;
            mix(offset);
            mix(value);
        }
    }
    timing.Mark("key_hash");
    std::optional<DispatchPreparationResult> farmPrepared;
    std::shared_ptr<const ShaderRecompiler::RecompileResult> compiledResult;

    std::shared_ptr<DispatchVariant> keepVariant;

    std::shared_ptr<DispatchVariant> attachVariant;

    std::shared_ptr<ShaderMemory> shaderMemory;
    std::vector<ShaderRecompiler::MemoryRegion> captured;

    std::vector<std::uint32_t> liveWords;
    bool dataHit = false;
    bool cached = false;
    bool validated = false;

    std::shared_ptr<DispatchEntry> missedEntry;
    bool missedDiffering = false;
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> capture;

    static const bool traceCache = std::getenv("APS5_TRACE_DISPATCH_CACHE") != nullptr;
    if (traceCache) {
        std::lock_guard traceLock(dispatchCacheMutex);
        struct Last { std::vector<std::uint32_t> userData; std::map<std::uint32_t, std::uint32_t> shader; std::uint64_t key; };
        static std::map<std::uint64_t, Last> last;
        static int reports = 0;
        auto& previous = last[address];
        if (previous.key != 0 && previous.key != key && reports < 200) {
            std::string what;
            for (std::size_t i = 0; i < userData.size(); ++i) {
                if (i >= previous.userData.size() || previous.userData[i] != userData[i]) {
                    char text[48];
                    std::snprintf(text, sizeof(text), " user[%zu] %08x->%08x", i, i < previous.userData.size() ? previous.userData[i] : 0u, userData[i]);
                    what += text;
                }
            }
            for (const auto& [offset, value] : queue.shader) {
                const auto old = previous.shader.find(offset);
                if (old == previous.shader.end() || old->second != value) {
                    char text[48];
                    std::snprintf(text, sizeof(text), " sh[%x] %08x->%08x", offset, old == previous.shader.end() ? 0u : old->second, value);
                    what += text;
                }
            }
            ++reports;
            std::fprintf(stderr, "[dispatch-cache] 0x%llx key changed:%s\n", static_cast<unsigned long long>(address), what.c_str());
        }
        previous.userData = userData;
        previous.shader = std::map<std::uint32_t, std::uint32_t>(queue.shader.begin(), queue.shader.end());
        previous.key = key;
    }

    mix(reinterpret_cast<std::uintptr_t>(registeredShader.get()));
    if (gpuTiming && !resolving && !profile) {
        std::lock_guard observerLock(dispatchCacheMutex);
        struct LastCandidate {
            std::uint64_t address = 0, config = 0, key = 0;
            std::uintptr_t owner = 0;
            std::array<std::uint32_t, 31> roots{};
            std::uint32_t count = 0;
        };
        struct Window {
            std::uint64_t observed = 0, first = 0, compared = 0, roots = 0, config = 0;
            std::uint64_t owner = 0, registered = 0, raw = 0, unchanged = 0, repeat = 0, untracked = 0, retries = 0;
            std::uint32_t changedRootMask = 0;
            std::array<std::uint64_t, 31> changedRootIndex{};
        };
        static std::array<LastCandidate, 512> states{};
        static std::array<std::uint16_t, 1024> index{};
        static std::size_t used = 0;
        static Window window;
        static auto start = std::chrono::steady_clock::time_point{};
        static auto firstPacket = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();
        if (start == std::chrono::steady_clock::time_point{}) start = now;
        if (window.observed == 0) firstPacket = packetStartedAt();
        ++window.observed;
        if (observerDepth > 1) ++window.retries;
        if (registeredSource) ++window.registered; else ++window.raw;
        // Diagnostic signature is intentionally non-authoritative, excluding roots and owner.
        std::uint64_t config = 0xcbf29ce484222325ull;
        const auto configMix = [&](std::uint64_t value) { config ^= value; config *= 0x100000001b3ull; };
        configMix(address);
        configMix(packet[4] & 0x8000u);
        for (const auto threads : compute.partialThreads) configMix(threads);
        configMix(userCount);
        if (keyHygiene) {
            for (const auto offset : {0x207u, 0x208u, 0x209u, 0x212u, 0x213u}) {
                const auto found = queue.shader.find(offset);
                configMix(found == queue.shader.end() ? (1ull << 32u) : found->second);
            }
        } else {
            for (const auto& [offset, value] : queue.shader) {
                if (offset >= 0x240u && offset < 0x240u + 31u) continue;
                configMix(offset);
                configMix(value);
            }
        }
        const auto owner = reinterpret_cast<std::uintptr_t>(registeredShader.get());
        auto slot = static_cast<std::size_t>(((address >> 8u) * 0x9e3779b97f4a7c15ull) >> 54u);
        while (index[slot] != 0 && states[index[slot] - 1].address != address) slot = (slot + 1) % index.size();
        const bool first = index[slot] == 0;
        if (first && used == states.size()) {
            ++window.untracked;
        } else {
            if (first) {
                index[slot] = static_cast<std::uint16_t>(++used);
                ++window.first;
            }
            auto& previous = states[index[slot] - 1];
            if (!first) {
                ++window.compared;
                bool rootsChanged = previous.count != userCount;
                const auto sharedCount = std::min<std::size_t>(previous.count, userCount);
                for (std::size_t i = 0; i < sharedCount; ++i) {
                    if (previous.roots[i] != userData[i]) {
                        rootsChanged = true;
                        window.changedRootMask |= 1u << i;
                        ++window.changedRootIndex[i];
                    }
                }
                for (std::size_t i = sharedCount; i < userCount; ++i) {
                    rootsChanged = true;
                    window.changedRootMask |= 1u << i;
                    ++window.changedRootIndex[i];
                }
                for (std::size_t i = userCount; i < previous.count; ++i) {
                    rootsChanged = true;
                    window.changedRootMask |= 1u << i;
                    ++window.changedRootIndex[i];
                }
                const bool configChanged = previous.config != config;
                const bool ownerChanged = previous.owner != owner;
                window.roots += rootsChanged;
                window.config += configChanged;
                window.owner += ownerChanged;
                window.unchanged += !rootsChanged && !configChanged && !ownerChanged;
                window.repeat += previous.key == key;
            }
            previous.address = address;
            previous.config = config;
            previous.key = key;
            previous.owner = owner;
            previous.count = userCount;
            std::copy(userData.begin(), userData.end(), previous.roots.begin());
        }
        if (now - start >= std::chrono::seconds(10)) {
            const auto seconds = [](auto point) { return std::chrono::duration<double>(point.time_since_epoch()).count(); };
            using U = unsigned long long;
            char rootCounts[512]{};
            std::size_t rootCountsLength = 0;
            for (std::size_t i = 0; i < window.changedRootIndex.size(); ++i) {
                if (window.changedRootIndex[i] == 0) continue;
                const int written = std::snprintf(rootCounts + rootCountsLength, sizeof(rootCounts) - rootCountsLength, " %zu:%llu", i, static_cast<unsigned long long>(window.changedRootIndex[i]));
                if (written > 0) rootCountsLength = std::min<std::size_t>(sizeof(rootCounts) - 1, rootCountsLength + static_cast<std::size_t>(written));
            }
            AgcDriver::ProfilePrint_nid_no_patch(
                "[gpu-key-observed] steady_s=[%.6f,%.6f] first_packet_start_s=%.6f (0=unavailable) candidates=%llu retry_calls=%llu first=%llu compared=%llu root_values_changed=%llu root_index_mask=0x%08x root_index_counts=%s config_shape_changed=%llu owner_pointer_value_changed=%llu registered=%llu raw_fallback=%llu unchanged=%llu full_key_repeat=%llu untracked=%llu tracked=%llu/512; overlapping transitions, not miss causes; config=hash only, pointer!=generation; core_cumulative_as_observed lookups=%llu absent=%llu equal=%llu differing=%llu inaccessible=%llu queuedLabel=%llu flushingImage=%llu publishMoved=%llu pendingMoved=%llu forgetMoved=%llu inserts=%llu variantsEvicted=%llu\n",
                seconds(start), seconds(now), seconds(firstPacket),
                U(window.observed), U(window.retries), U(window.first), U(window.compared), U(window.roots), window.changedRootMask, rootCounts, U(window.config), U(window.owner),
                U(window.registered), U(window.raw), U(window.unchanged), U(window.repeat), U(window.untracked), U(used),
                U(entryCounters.lookups), U(entryCounters.absent), U(entryCounters.equal), U(entryCounters.differing),
                U(entryCounters.inaccessible), U(entryCounters.queuedLabel), U(entryCounters.flushingImage),
                U(entryCounters.publishMoved), U(entryCounters.pendingMoved), U(entryCounters.forgetMoved),
                U(entryCounters.inserts), U(entryCounters.variantsEvicted));
            window = {};
            start = now;
            firstPacket = {};
        }
    }
    if (fromAhead && key != ahead.key) {
        ++groupStats.keyMismatches;
        if (ahead.preparedByFarm) {
            auto &farm = ::Driver::PrepareFarm::Get();
            static_cast<void>(farm.Take(submission.queue, submission.serial, currentPacketOffset()));
            farm.NoteAdoption("stale-key");
            farm.ReportStats();
        }
        dispatch(queue, submittedPacket, submission, submittedIndirectArguments);
        return;
    }
    if (fromAhead) {
        if (ahead.preparedByFarm) {
            auto &farm = ::Driver::PrepareFarm::Get();
            farm.BeforeTakeForTest(submission.queue, submission.serial, currentPacketOffset());
            auto taken = farm.Take(submission.queue, submission.serial, currentPacketOffset());
            if (taken.status != ::Driver::PrepareFarm::TakeStatus::Ready) {
                farm.NoteAdoption(taken.status == ::Driver::PrepareFarm::TakeStatus::Failed ? "failed" :
                    taken.status == ::Driver::PrepareFarm::TakeStatus::NotReady ? "not-ready" : "absent");
                farm.ReportStats();
                dispatch(queue, submittedPacket, submission, submittedIndirectArguments);
                return;
            }
            const auto *value = dynamic_cast<const DispatchFarmValue *>(taken.result.value.get());
            if (value == nullptr || value->prepared.key != ahead.key ||
                value->prepared.registerFingerprint != DispatchRegisterFingerprint(queue) ||
                value->device != localDevice ||
                value->device != device.Load() ||
                value->shader != registeredShader ||
                value->userDataRoots != userData ||
                value->packet.size() != packet.size() ||
                !std::equal(value->packet.begin(), value->packet.end(), packet.begin())) {
                farm.NoteAdoption("stale-key");
                farm.ReportStats();
                dispatch(queue, submittedPacket, submission, submittedIndirectArguments);
                return;
            }
            if (value->prepared.compiledResult == nullptr ||
                ReadsOverlapEarlierDispatchWrites(*value->prepared.compiledResult, submission.serial)) {
                farm.NoteAdoption("stale-capture");
                farm.ReportStats();
                dispatch(queue, submittedPacket, submission, submittedIndirectArguments);
                return;
            }
        }
        if (const auto hook = aheadAdoptHook.load(std::memory_order_acquire)) hook();
        const auto* aheadViews = ahead.capture != nullptr ? &ahead.capture->snapshot.registeredImages : ahead.keepVariant != nullptr && ahead.keepVariant->registeredViews != nullptr ? ahead.keepVariant->registeredViews.get() : nullptr;
        if (!CapturedCurrent(ahead.captured, snapshot, ahead.shaderMemory.get(), ahead.readProof) || (aheadViews != nullptr && !CaptureRegisteredViewsCurrent(*aheadViews))) {
            ++dispatchesRejectedAhead;
            ++groupStats.memoryMismatches;
            if (ahead.preparedByFarm) {
                ::Driver::PrepareFarm::Get().NoteAdoption("stale-capture");
                ::Driver::PrepareFarm::Get().ReportStats();
            }
            dispatch(queue, submittedPacket, submission, submittedIndirectArguments);
            return;
        }
        if (ahead.preparedByFarm) {
            ::Driver::PrepareFarm::Get().NoteAdoption("adopted");
            ::Driver::PrepareFarm::Get().ReportStats();
        }
    }
    phaseTiming.Phase(PhaseKey);
    timing.Mark("decode_key_device_setup");
    bool readsClear = false;
    const bool queueMisses = pipelineCandidate && DispatchMissItemsEnabled() && !noDispatchCache && !stampValidate() && !verifyDataHits();
    bool lateItem = false;
    bool recipeLess = false;
    if (pipelineCandidate) {
        std::vector<ShaderRecompiler::MemoryRegion> reads;
        bool entryFound = false;
        {
            std::lock_guard cacheLock(dispatchCacheMutex);
            if (const auto found = dispatchCache.find(key); found != dispatchCache.end()) {
                entryFound = true;
                for (const auto& variant : found->second->variants) appendEntryRegions(*variant, reads);
            }
        }
        if (!entryFound && queueMisses) {
            lateItem = true;
        } else if (!entryFound || noDispatchCache || stampValidate()) {
            NoteItemFallback(ItemFallback::Miss);
        } else if (verifyDataHits()) {
            NoteItemFallback(ItemFallback::Verify);
        } else {
            readsClear = std::none_of(reads.begin(), reads.end(), [](const auto& region) { return DrawPipeline::Queue0().Overlaps(region.guestAddress, region.bytes.size()); });
            if (!readsClear) NoteItemFallback(ItemFallback::Reads);
        }
        if (!readsClear && !lateItem && DrawPipeline::Queue0().Busy()) DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Packet, 0x15);
    }
    timing.Mark("pipeline_reads_check");
    // Resolving ahead: the cache entry's regions are collected before the lookup validates them, so
    // the adoption can prove a hit current by write tracking (CapturedCurrent).
    std::vector<GuestMemory::UnchangedQuery> readProof;
    if (resolving) {
        std::vector<ShaderRecompiler::MemoryRegion> entryReads;
        {
            std::lock_guard cacheLock(dispatchCacheMutex);
            if (const auto found = dispatchCache.find(key); found != dispatchCache.end()) {
                for (const auto& variant : found->second->variants) appendEntryRegions(*variant, entryReads);
            }
        }
        readProof.reserve(entryReads.size());
        for (const auto& region : entryReads) {
            const auto generation = GuestMemory::CollectWrites(region.guestAddress, region.bytes.size());
            if (generation == 0) {
                readProof.clear();
                break;
            }
            readProof.push_back({region.guestAddress, region.bytes.size(), generation});
        }
    }
    if (fromAhead) {
        compiledResult = std::move(ahead.compiledResult);
        keepVariant = std::move(ahead.keepVariant);
        attachVariant = std::move(ahead.attachVariant);
        shaderMemory = std::move(ahead.shaderMemory);
        captured = std::move(ahead.captured);
        liveWords = std::move(ahead.liveWords);
        capture = std::move(ahead.capture);
        dataHit = ahead.dataHit;
        cached = ahead.cached;
        validated = ahead.validated;
        missedEntry = std::move(ahead.missedEntry);
        missedDiffering = ahead.missedDiffering;
        if (ahead.preparedByFarm && !cached) {
            insertDispatch(address, key, userData, ahead.noDispatchCache, profile,
                registeredShader, ahead.forgetAtCapture, memory, shaderMemory,
                captured, capture, compiledResult, missedEntry, missedDiffering,
                attachVariant, phaseTiming);
        }
        ++groupStats.adopted;
        ++dispatchesAdoptedAhead;
        const auto age = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ahead.resolvedAt).count();
        groupStats.ageMs += age;
        groupStats.ageMaxMs = std::max(groupStats.ageMaxMs, age);
        phaseTiming.Phase(PhaseLookup);
        timing.Mark("lookup_ahead_adopt");
    } else {
        lookupDispatch(address, submission, registeredShader, key, userData, noDispatchCache, traceCache, profile, memory, phaseTiming, phaseMs, compiledResult, keepVariant, captured, liveWords, dataHit, cached, validated, missedEntry, missedDiffering);
    }
    if (readsClear && !fromAhead && keepVariant != nullptr && keepVariant->wordSource != nullptr) {
        readsClear = std::none_of(keepVariant->runs.begin(), keepVariant->runs.end(), [](const auto& run) {
            return DrawPipeline::Queue0().Overlaps(run.first, static_cast<std::size_t>(run.second - run.first));
        });
        if (!readsClear) NoteItemFallback(ItemFallback::Reads);
    }
    timing.Mark("lookup_cache");
    std::vector<DrawPipeline::Range> itemWrites;
    bool pipelined = false;
    if (readsClear) {
        if (cached && keepVariant != nullptr && keepVariant->recipe.load(std::memory_order_acquire) != nullptr) {
            bool bounded = false;
            itemWrites = DispatchWriteRanges(*compiledResult, bounded);
            pipelined = bounded;
            if (!bounded) NoteItemFallback(ItemFallback::Writes);
        } else if (queueMisses && cached && keepVariant != nullptr) {
            recipeLess = true;
        } else if (queueMisses && !cached) {
            lateItem = true;
        } else {
            NoteItemFallback(ItemFallback::NoRecipe);
        }
        if (!pipelined && !recipeLess && !lateItem && DrawPipeline::Queue0().Busy()) DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Packet, 0x15);
    }
    timing.Mark("pipeline_write_ranges");
    std::uint64_t forgetAtCapture = 0;
    if (cached) {
        require(keepVariant != nullptr && keepVariant->shader == registeredShader, "dispatch cache belongs to another registered shader");
        if (profile) captureMs.fetch_add(phaseTiming.Elapsed(), std::memory_order_relaxed);
        timing.Mark("cached_shader_check");
    } else if (!fromAhead) {
        if (resolving && ::Driver::PrepareFarm::Get().Enabled()) {
            auto &farm = ::Driver::PrepareFarm::Get();
            const auto identity = farm.Peek(submission.queue, submission.serial, currentPacketOffset());
            if (identity.status == ::Driver::PrepareFarm::PeekStatus::Ready && identity.result != nullptr) {
                const auto *value = dynamic_cast<const DispatchFarmValue *>(identity.result->value.get());
                if (value != nullptr && value->prepared.key == key &&
                    value->prepared.registerFingerprint == DispatchRegisterFingerprint(queue) &&
                    value->device == localDevice && value->device == device.Load() &&
                    value->shader == registeredShader &&
                    value->userDataRoots == userData && value->packet.size() == packet.size() &&
                    std::equal(value->packet.begin(), value->packet.end(), packet.begin())) {
                    if (value->prepared.compiledResult == nullptr ||
                        ReadsOverlapEarlierDispatchWrites(*value->prepared.compiledResult, submission.serial)) {
                        farm.ReportStats();
                        return;
                    }
                    farmPrepared = value->prepared;
                } else {
                    farm.ReportStats();
                    return;
                }
            } else if (identity.status == ::Driver::PrepareFarm::PeekStatus::Absent) {
                DispatchPrepareInput input(packet, registeredShader, submission.shaders,
                    localDevice, codeOffset, request, key, DispatchRegisterFingerprint(queue), submission.queue,
                    submission.serial, currentPacketOffset());
                ::Driver::PrepareFarm::Job job;
                job.queue = submission.queue;
                job.submissionSerial = submission.serial;
                job.packetOffset = currentPacketOffset();
                job.byteReservation = EstimateBytes(input);
                job.callable = std::make_unique<DispatchFarmCallable>(std::move(input));
                if (farm.Submit(std::move(job)) == ::Driver::PrepareFarm::SubmitStatus::Accepted) {
                    farm.ReportStats();
                    return;
                }
            } else {
                farm.ReportStats();
                return;
            }
        } else if (!resolving && ::Driver::PrepareFarm::Get().Enabled()) {
            auto &farm = ::Driver::PrepareFarm::Get();
            farm.BeforeTakeForTest(submission.queue, submission.serial, currentPacketOffset());
            auto taken = farm.Take(submission.queue, submission.serial, currentPacketOffset());
            if (taken.status == ::Driver::PrepareFarm::TakeStatus::Ready) {
                const auto *value = dynamic_cast<const DispatchFarmValue *>(taken.result.value.get());
                const bool sameIdentity = value != nullptr &&
                    value->prepared.key == key && value->prepared.registerFingerprint == DispatchRegisterFingerprint(queue) &&
                    value->device == localDevice &&
                    value->device == device.Load() &&
                    value->shader == registeredShader && value->userDataRoots == userData &&
                    value->packet.size() == packet.size() &&
                    std::equal(value->packet.begin(), value->packet.end(), packet.begin());
                if (!sameIdentity) {
                    farm.NoteAdoption("stale-key");
                } else if (value->prepared.compiledResult == nullptr ||
                    ReadsOverlapEarlierDispatchWrites(*value->prepared.compiledResult, submission.serial)) {
                    farm.NoteAdoption("stale-capture");
                } else {
                    if (const auto hook = aheadAdoptHook.load(std::memory_order_acquire)) hook();
                    const auto *views = value->prepared.capture != nullptr
                        ? &value->prepared.capture->snapshot.registeredImages : nullptr;
                    if (CapturedCurrent(value->prepared.captured, snapshot,
                            value->prepared.shaderMemory.get(), {}) &&
                        (views == nullptr || CaptureRegisteredViewsCurrent(*views))) {
                        farmPrepared = value->prepared;
                        farm.NoteAdoption("adopted");
                    } else {
                        farm.NoteAdoption("stale-capture");
                    }
                }
                farm.ReportStats();
            } else if (taken.status == ::Driver::PrepareFarm::TakeStatus::NotReady) {
                farm.NoteAdoption("not-ready");
                farm.ReportStats();
            } else if (taken.status == ::Driver::PrepareFarm::TakeStatus::Failed) {
                farm.NoteAdoption("failed");
                farm.ReportStats();
            } else if (taken.status == ::Driver::PrepareFarm::TakeStatus::Absent) {
                farm.NoteAdoption("absent");
                farm.ReportStats();
            }
        }
        if (farmPrepared.has_value()) {
            auto prepared = std::move(*farmPrepared);
            capture = std::move(prepared.capture);
            captured = std::move(prepared.captured);
            shaderMemory = std::move(prepared.shaderMemory);
            compiledResult = std::move(prepared.compiledResult);
            forgetAtCapture = prepared.forgetSerial;
        } else {
        shaderMemory = std::make_shared<ShaderMemory>(memory, &queryPendingWrite, &observePendingWrite, hookWaitCounter(), &QueryPendingSpan);

        static const bool dumpShaders = std::getenv("APS5_DUMP_SHADERS") != nullptr;
        try {
            const auto waitedBefore = traceCapSync() ? Graphics::Recorder::ThreadWaitedMs() : 0.0;
            forgetAtCapture = GuestMemory::ForgetSerial();
            auto started = std::chrono::steady_clock::now();
            auto preparedOptional = PrepareDispatchSteps(
                shaderMemory, forgetAtCapture, snapshot, codeOffset, request, key, 0,
                [&](auto& invocation) -> std::optional<std::shared_ptr<const ShaderRecompiler::ResourceCapture>> {
                    const SampledReadScope sampling(evidenceReads);
                    return CaptureInvocation(*shaderMemory, snapshot, codeOffset, request, invocation);
                },
                [&](const auto& regions) {
                    request.context.memory = regions;
                    if (traceCapSync()) traceCapture("dispatch-capture", address, submission.queue, regions, Graphics::Recorder::ThreadWaitedMs() - waitedBefore);
                    if (profile) captureMs.fetch_add(phaseTiming.Elapsed(), std::memory_order_relaxed);
                    phaseTiming.Phase(PhaseCapture);
                    if (dumpShaders) static_cast<void>(dumpRequest(address, request));
                    timing.Mark("capture_resources");
                    StreamReuseNoteTranslation();
                    started = std::chrono::steady_clock::now();
                },
                [&] { timing.Mark("prepared_invocation"); });
            require(preparedOptional.has_value(), "blocking dispatch capture returned no result");
            auto prepared = std::move(*preparedOptional);
            capture = std::move(prepared.capture);
            captured = std::move(prepared.captured);
            shaderMemory = std::move(prepared.shaderMemory);
            compiledResult = std::move(prepared.compiledResult);
            forgetAtCapture = prepared.forgetSerial;
            timing.Mark("materialize");
            if (profile && compiledResult->cacheHit) cacheHits.fetch_add(1, std::memory_order_relaxed);
            const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            static std::atomic<double> totalMs{0};
            if (profile) {
                const auto total = totalMs.fetch_add(elapsed, std::memory_order_relaxed) + elapsed;
                if (elapsed > 200) std::fprintf(stderr, "[gpu] compute shader 0x%llx recompile took %.0f ms (%zu SPIR-V words, %zu captured regions, total %.1f s)\n", static_cast<unsigned long long>(address), elapsed, compiledResult->spirv.size(), captured.size(), total / 1000);
            }
            if (const auto limit = MaxComputeSpirvWords(); limit != 0 && compiledResult->spirv.size() > limit)
                throw std::runtime_error(std::to_string(compiledResult->spirv.size()) + " SPIR-V words, above APS5_MAX_COMPUTE_SPIRV_WORDS (" + std::to_string(limit) + ")");
        } catch (const std::exception& error) {
            std::string dump;
            if (dumpShaders) {
                const auto regions = shaderMemory->Regions();
                auto failed = request;
                failed.context.memory = regions;
                dump = dumpRequest(address, failed, "_failed");
            }

            std::string reason = error.what();
            if (const auto newline = reason.find('\n'); newline != std::string::npos) reason.resize(newline);
            if (SkipRefusedDispatches()) {
                ReportRefusedDispatch(address, reason);
                return;
            }
            char where[96];
            if (dump.empty()) std::snprintf(where, sizeof(where), "compute shader 0x%llx: ", static_cast<unsigned long long>(address));
            else std::snprintf(where, sizeof(where), "compute shader 0x%llx (%s): ", static_cast<unsigned long long>(address), dump.c_str());
            throw std::runtime_error(where + reason);
        }
        if (profile) recompileMs.fetch_add(phaseTiming.Elapsed(), std::memory_order_relaxed);
        phaseTiming.Phase(PhaseRecompile);
        if (!resolving) insertDispatch(address, key, userData, noDispatchCache || !CacheableResult(*compiledResult), profile, registeredShader, forgetAtCapture, memory, shaderMemory, captured, capture, compiledResult, missedEntry, missedDiffering, attachVariant, phaseTiming);
        timing.Mark("insert_dispatch");
        }
    }
    const auto* registeredViews = capture != nullptr ? &capture->snapshot.registeredImages : keepVariant != nullptr && keepVariant->registeredViews != nullptr ? keepVariant->registeredViews.get() : nullptr;
    if (resolving) {
        auto& parked = resolvedAhead()[currentPacketOffset()];
        parked = ResolvedDispatch{};
        parked.address = address;
        parked.key = key;
        parked.registerFingerprint = DispatchRegisterFingerprint(queue);
        parked.userDataRoots.assign(userData.begin(), userData.end());
        parked.indirectArguments = indirectArguments;
        parked.forgetAtCapture = forgetAtCapture;
        std::copy_n(packet.begin(), std::min(packet.size(), parked.packet.size()), parked.packet.begin());
        parked.registeredShader = registeredShader;
        parked.compiledResult = compiledResult;
        localDevice->PrefetchComputePipeline(compiledResult);
        parked.keepVariant = keepVariant;
        parked.attachVariant = attachVariant;
        parked.missedEntry = missedEntry;
        parked.shaderMemory = shaderMemory;
        parked.captured = std::move(captured);
        parked.readProof = std::move(readProof);
        parked.liveWords = std::move(liveWords);
        parked.capture = capture;
        parked.dataHit = dataHit;
        parked.cached = cached;
        parked.validated = validated;
        parked.missedDiffering = missedDiffering;
        parked.noDispatchCache = noDispatchCache || (compiledResult != nullptr && !CacheableResult(*compiledResult));
        parked.preparedByFarm = farmPrepared.has_value();
        parked.resolvedAt = std::chrono::steady_clock::now();
        ++groupStats.resolved;
        if (cached) ++groupStats.resolvedHits;
        if (!cached && compiledResult != nullptr && !localDevice->HasComputePipeline(*compiledResult)) {
            SubmitComputePrebuild([localDevice, compiledResult, address] {
                try { localDevice->PrebuildComputePipeline(*compiledResult, address); } catch (const std::exception&) {}
            });
        }
        timing.Mark("resolve_park_return");
        return;
    }
    if (verifyDataHits() && dataHit) verifyDataHit(snapshot, codeOffset, request, memory, address, *keepVariant, liveWords, *compiledResult);
    timing.Mark("verify_data_hit");

    if (recordQueuedLabelsAfterCapture(submission.queue, captured)) {
        dispatch(queue, packet, submission, indirectArguments);
        timing.Mark("queued_labels_redispatch");
        return;
    }
    phaseTiming.Phase(PhaseQueuedLabels);
    timing.Mark("queued_labels");
    const auto& compiled = *compiledResult;
    programScope.emplace(TraceProgramKey(address, compiled.variantId));
    std::vector<Graphics::GuestMemorySnapshot> snapshots;
    for (const auto& region : captured) snapshots.push_back({region.guestAddress, region.bytes});
    std::array<std::uint32_t, 3> groups{packet[1], packet[2], packet[3]};
    if (indirectArguments == 0 && (packet[4] & 0x20u) != 0) {

        for (std::uint32_t axis = 0; axis < 3; ++axis) {
            const auto threads = std::max(readRegister(queue.shader, 0x207 + axis) & 0xffffu, 1u);
            groups[axis] = ThreadDimensionGroups(groups[axis], threads);
        }
    }
    const std::uint64_t dispatchThreads = indirectArguments != 0 ? 0 : std::uint64_t{groups[0]} * groups[1] * groups[2] * compute.numThreads[0] * compute.numThreads[1] * compute.numThreads[2];
    static const bool traceIo = std::getenv("APS5_TRACE_DISPATCH_IO") != nullptr;
    if (traceIo) {

        std::string words;
        if (std::getenv("APS5_TRACE_DISPATCH_IO")[0] == '2') {
            for (const auto word : userData) {
                char text[12];
                std::snprintf(text, sizeof(text), " %08x", word);
                words += text;
            }
        }
        std::fprintf(stderr, "[dispatch-io] queue=0x%x submission=%llu shader 0x%llx%s\n", submission.queue, static_cast<unsigned long long>(submission.serial), static_cast<unsigned long long>(address), words.c_str());
    }

    const auto rethrow = [&](const std::exception& error) {
        if (fromAhead) {
            ++groupStats.deviceRejects;
            dispatch(queue, packet, submission, indirectArguments);
            return true;
        }
        char where[64];
        std::snprintf(where, sizeof(where), "compute shader 0x%llx: ", static_cast<unsigned long long>(address));
        throw std::runtime_error(where + std::string(error.what()) + describeIndirectSource());
    };
    auto recipeOwner = keepVariant != nullptr && keepVariant->wordSource != nullptr ? keepVariant->wordSource : keepVariant;
    phaseTiming.Phase(PhaseSnapshots);
    timing.Mark("snapshots_prepare");

    bool plain = false;
    if ((pipelined && DispatchMissItemsEnabled()) || lateItem || recipeLess) {
        plain = compiledResult != nullptr && !htileClearCompletion.has_value() && htileMask.bytes == 0u;
        if (plain && !partialLastGroup && MatchSgprPatternFill(packet, std::span(snapshot.code).subspan(codeOffset), userData, compute)) plain = false;
        if (plain && !partialLastGroup) {
            if (const auto clear = Graphics::MatchStorageImageClear(std::span(snapshot.code).subspan(codeOffset), userData, compute, groups)) {
                const auto clearAddress = Graphics::StorageImageClearAddress(*clear);
                if (clearAddress != 0 && Graphics::DepthSurfaceAt(clearAddress)) plain = false;
            }
        }
    }
    if (pipelined && DispatchMissItemsEnabled() && !plain) {
        pipelined = false;
        itemWrites.clear();
        itemNotPlain.fetch_add(1, std::memory_order_relaxed);
        if (DrawPipeline::Queue0().Busy()) DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Packet, 0x15);
    }
    if (lateItem || recipeLess) {
        bool queued = plain;
        if (queued && lateItem) {
            queued = std::none_of(captured.begin(), captured.end(), [](const auto& region) { return DrawPipeline::Queue0().Overlaps(region.guestAddress, region.bytes.size()); });
            if (!queued) NoteItemFallback(ItemFallback::Reads);
        } else if (!plain) {
            NoteItemFallback(lateItem ? ItemFallback::Miss : ItemFallback::NoRecipe);
        }
        if (queued) {
            bool bounded = false;
            itemWrites = DispatchWriteRanges(*compiledResult, bounded);
            pipelined = bounded;
            if (!bounded) NoteItemFallback(ItemFallback::Writes);
        }
        if (pipelined) (lateItem ? itemMisses : itemRecipeLess).fetch_add(1, std::memory_order_relaxed);
        else if (DrawPipeline::Queue0().Busy()) DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Packet, 0x15);
    }
    if (pipelined) {
        runCaptureCheckHook();
        if (capturedMemoryChanged(captured)) {
            const RecaptureScope recapture;
            timing.Mark("pipelined_recapture_redispatch");
            dispatch(queue, packet, submission, indirectArguments);
            return;
        }
        const auto recipe = recipeOwner != nullptr ? recipeOwner->recipe.load(std::memory_order_acquire) : nullptr;
        const auto epoch = DrawPipeline::EpochToken().load(std::memory_order_relaxed);
        const auto queueId = submission.queue;
        itemCommits.fetch_add(1, std::memory_order_relaxed);
        DrawPipeline::Queue0().Enqueue(DrawPipeline::CommitKind::Dispatch, [this, localDevice = std::move(localDevice), result = compiledResult, capture, registeredViews, snapshots = std::move(snapshots), liveWords = std::move(liveWords), groups, address, queueId, recipe, keepVariant, recipeOwner, dataHit, epoch, htileClearCompletion, htileMask, origin = AgcDriver::BatchTimeline::Origin{submission.serial, submission.queue}] {
            AgcDriver::BatchTimeline::Scope originScope(origin);
            DrawPipeline::FollowEpoch(epoch);
            AgcDriver::P0Metrics::Scope p0(queueId, AgcDriver::P0Metrics::Layer::Committer, AgcDriver::P0Metrics::Operation::Dispatch);
            GuestMemory::SetCurrentPacket(0x15, queueId);
            const auto& compiledCommit = *result;
            const TraceProgramScope programScope(TraceProgramKey(address, compiledCommit.variantId));
            const bool noteWrites = writeEvidenceEnabled() || traceCapSync();
            try {
                std::shared_ptr<RecipeHit> recipeHit = htileClearCompletion.has_value() ? nullptr : localDevice->PrepareRecipe(recipe, false);
                std::shared_ptr<const Recipe> builtRecipe;
                bool writersNoted = false;
                for (;;) {
                    std::shared_ptr<PreparedDispatch> prepared;
                    if (recipeHit == nullptr || VulkanDevice::VerifyRecipes() || htileClearCompletion.has_value()) prepared = localDevice->PrepareDispatch(compiledCommit, snapshots, 0, address, htileClearCompletion, registeredViews);
                    GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Dispatch);
                    std::lock_guard gpuLock(GuestMemory::GpuMutex());
                    recordLabelsForPacket(localDevice.get(), queueId);
                    if (noteWrites && writerKeyedEvidence() && !writersNoted) {
                        noteWrittenBuffers(address, queueId, compiledCommit);
                        writersNoted = true;
                    }
                    if (recipeHit != nullptr) {
                        VulkanDevice::IndirectOutcome outcome{0, 0};
                        const auto recipeResult = localDevice->DispatchRecipe(compiledCommit, groups[0], groups[1], groups[2], 0, address, recipeHit, outcome, VulkanDevice::VerifyRecipes() ? prepared : nullptr, dataHit);
                        if (recipeResult == RecipeOutcome::Rebuild) {
                            recipeHit = nullptr;
                            VulkanDevice::NoteRecipe(VulkanDevice::RecipeEvent::Restart, false);
                            itemRebuilds.fetch_add(1, std::memory_order_relaxed);
                            continue;
                        }
                    } else {
                        localDevice->Dispatch(compiledCommit, groups[0], groups[1], groups[2], snapshots, address, std::move(prepared), htileClearCompletion.has_value() || recipeOwner == nullptr ? nullptr : &builtRecipe, htileClearCompletion, registeredViews);
                    }
                    if (htileMask.bytes != 0u) localDevice->NoteHtileMask(htileMask);
                    break;
                }
                if (noteWrites && !writerKeyedEvidence()) noteWrittenBuffers(address, queueId, compiledCommit);
                if (builtRecipe != nullptr) {
                    if (dataHit) {
                        auto own = std::make_shared<Recipe>(*builtRecipe);
                        own->dataWordsHash = Graphics::ShaderResources::DataWordsHash({ShaderRecompiler::ShaderStage::Compute, keepVariant->compiled.get(), 0});
                        builtRecipe = std::move(own);
                    }
                    recipeOwner->recipe.store(std::move(builtRecipe), std::memory_order_release);
                    VulkanDevice::NoteRecipe(VulkanDevice::RecipeEvent::Attach, false);
                }
            } catch (const std::exception& error) {
                char where[64];
                std::snprintf(where, sizeof(where), "compute shader 0x%llx: ", static_cast<unsigned long long>(address));
                throw std::runtime_error(where + std::string(error.what()));
            }
        }, std::move(itemWrites));
        RememberDispatchWrites(compiled, submission.serial);
        pendingDispatchPhases().outcome = DispatchOutcome::Enqueued;
        timing.Mark("pipelined_enqueue");
        ReportDispatchItems(profile);
        timing.Mark("pipelined_report");
        return;
    }

    std::shared_ptr<RecipeHit> recipeHit;
    if (cached && keepVariant != nullptr && !stampValidate()) {
        recipeHit = htileClearCompletion.has_value() || recipeOwner == nullptr ? nullptr : localDevice->PrepareRecipe(recipeOwner->recipe.load(std::memory_order_acquire), indirectArguments != 0);
        phaseTiming.Phase(PhaseRecipePrecheck);
    }
    timing.Mark("recipe_precheck");

    std::shared_ptr<const Recipe> builtRecipe;
    auto* const attachTo = stampValidate() ? nullptr : recipeOwner != nullptr ? recipeOwner.get() : attachVariant.get();
    bool writersNoted = false;
    const bool noteWrites = writeEvidenceEnabled() || traceCapSync();
    const auto keysFill = indirectArguments == 0 && !partialLastGroup ? MatchSgprPatternFill(packet, std::span(snapshot.code).subspan(codeOffset), userData, compute) : std::nullopt;
    const auto filledKeys = keysFill ? KeysFillMemo(keysFill->pattern, false, true) : std::nullopt;

    std::optional<Graphics::DepthPlaneClear> depthPlaneClear;
    if (indirectArguments == 0 && !partialLastGroup) {
        if (const auto clear = Graphics::MatchStorageImageClear(std::span(snapshot.code).subspan(codeOffset), userData, compute, groups)) {
            const auto clearAddress = Graphics::StorageImageClearAddress(*clear);
            if (clearAddress != 0 && Graphics::DepthSurfaceAt(clearAddress)) {
                GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Dispatch);
                std::lock_guard clearLock(GuestMemory::GpuMutex());
                recordLabelsForPacket(localDevice.get(), submission.queue);
                depthPlaneClear.emplace(*clear);
            }
        }
    }
    timing.Mark("fill_clear_match");
    bool captureChecked = fromAhead;

    for (;;) {

        std::shared_ptr<PreparedDispatch> prepared;
        if (recipeHit == nullptr || VulkanDevice::VerifyRecipes()) {
            try {
                prepared = localDevice->PrepareDispatch(compiled, snapshots, dispatchThreads, address, htileClearCompletion, registeredViews);
            } catch (const std::exception& error) {
                if (rethrow(error)) { timing.Mark("prepare_rethrow_redispatch"); return; }
            }
            if (phaseProfile) {
                const auto now = std::chrono::steady_clock::now();
                const auto prepareMs = std::chrono::duration<double, std::milli>(now - phaseLap).count();
                phaseLap = now;
                double parts = 0;
                if (prepared != nullptr) {
                    const auto phases = VulkanDevice::PreparePhaseMs(*prepared);
                    phaseMs[PhasePrepareKey] += phases[0];
                    phaseMs[PhasePrepareFind] += phases[1];
                    phaseMs[PhasePreparePrecollect] += phases[2];
                    phaseMs[PhasePreparePresync] += phases[3];
                    phaseMs[PhasePrepareStageA] += phases[4];
                    for (const auto part : phases) parts += part;
                }
                phaseMs[PhasePrepareOther] += std::max(0.0, prepareMs - parts);
            }
        }
        timing.Mark("prepare_dispatch");
        GuestMemory::TagGpuLockSite(indirectArguments != 0 ? GuestMemory::GpuLockSite::Indirect : GuestMemory::GpuLockSite::Dispatch);
        std::unique_lock gpuLock(GuestMemory::GpuMutex());
        phaseTiming.Phase(PhaseLockWait);
        timing.Mark("gpu_lock_wait");
        if (!captureChecked) {
            captureChecked = true;
            runCaptureCheckHook();
            if (capturedMemoryChanged(captured, snapshots)) {
                gpuLock.unlock();
                const RecaptureScope recapture;
                dispatch(queue, packet, submission, indirectArguments);
                timing.Mark("recapture_redispatch");
                return;
            }
        }
        timing.Mark("capture_check");

        recordLabelsForPacket(localDevice.get(), submission.queue);
        phaseTiming.Phase(PhaseLabels);
        timing.Mark("record_labels");
        if (noteWrites && writerKeyedEvidence() && !writersNoted) {
            noteWrittenBuffers(address, submission.queue, compiled);
            writersNoted = true;
        }
        phaseTiming.Phase(PhaseNoteWriters);
        timing.Mark("note_writers");
        auto* const keysRecorder = filledKeys ? Graphics::Recorder::Active() : nullptr;
        const auto keysNoteBefore = keysRecorder != nullptr ? keysRecorder->LastWriteNote(keysFill->base, keysFill->bytes) : 0;
        try {
            if (recipeHit != nullptr) {
                VulkanDevice::IndirectOutcome outcome{0, 0};
                const auto result = localDevice->DispatchRecipe(compiled, groups[0], groups[1], groups[2], indirectArguments, address, recipeHit, outcome, VulkanDevice::VerifyRecipes() ? prepared : nullptr, dataHit, threadGroup);
                if (result == RecipeOutcome::Rebuild) {

                    recipeHit = nullptr;
                    VulkanDevice::NoteRecipe(VulkanDevice::RecipeEvent::Restart, indirectArguments != 0);
                    timing.Mark("recipe_rebuild_restart");
                    continue;
                }
                if (indirectArguments != 0) countIndirect(outcome.cpuReason, outcome.argumentReadMs);
                timing.Mark("device_recipe");
            } else if (indirectArguments != 0) {
                const auto outcome = localDevice->DispatchIndirect(compiled, indirectArguments, snapshots, address, std::move(prepared), attachTo != nullptr ? &builtRecipe : nullptr, threadGroup, registeredViews);
                countIndirect(outcome.cpuReason, outcome.argumentReadMs);
                timing.Mark("device_indirect");
            } else {
                localDevice->Dispatch(compiled, groups[0], groups[1], groups[2], snapshots, address, std::move(prepared), htileClearCompletion.has_value() ? nullptr : (attachTo != nullptr ? &builtRecipe : nullptr), htileClearCompletion, registeredViews);
                timing.Mark("device_dispatch");
            }
        } catch (const std::exception& error) {
            if (rethrow(error)) { timing.Mark("device_rethrow_redispatch"); return; }
        }
        if (keysRecorder != nullptr && keysRecorder == Graphics::Recorder::Active()) {
            const auto note = keysRecorder->LastWriteNote(keysFill->base, keysFill->bytes);
            if (note != 0 && note != keysNoteBefore) Graphics::NoteKeysFillOnGpu(keysFill->base, keysFill->bytes, *filledKeys);
        }
        if (htileMask.bytes != 0u) localDevice->NoteHtileMask(htileMask);
        if (builtRecipe != nullptr) {
            if (dataHit) {

                auto own = std::make_shared<Recipe>(*builtRecipe);
                own->dataWordsHash = Graphics::ShaderResources::DataWordsHash({ShaderRecompiler::ShaderStage::Compute, keepVariant->compiled.get(), 0});
                builtRecipe = std::move(own);
            }
            attachTo->recipe.store(std::move(builtRecipe), std::memory_order_release);
            VulkanDevice::NoteRecipe(VulkanDevice::RecipeEvent::Attach, indirectArguments != 0);
        }
        timing.Mark("post_dispatch_bookkeeping");
        break;
    }
    phaseTiming.Phase(PhaseDevice);
    if (noteWrites && !writerKeyedEvidence()) noteWrittenBuffers(address, submission.queue, compiled);
    RememberDispatchWrites(compiled, submission.serial);
    timing.Mark("note_written_buffers");
    if (profile) deviceMs.fetch_add(phaseTiming.Elapsed(), std::memory_order_relaxed);
    phaseTiming.Phase(PhaseTail);
    if (phaseProfile) {
        auto& pending = pendingDispatchPhases();
        pending.outcome = DispatchOutcome::Real;
        pending.phases = true;
        pending.hit = cached;
        pending.validated = validated;
        pending.ms = phaseMs;
        pending.tailAt = phaseLap;
    }
    timing.Mark("profile_pending");
    if (profile && dispatches.fetch_add(1, std::memory_order_relaxed) % 100 == 99) {
        std::uint64_t hits = 0;
        std::uint64_t evictions = 0;
        {
            std::lock_guard cacheLock(dispatchCacheMutex);
            hits = dispatchCacheHits;
            evictions = dispatchCacheEvictions;
        }
        AgcDriver::ProfilePrint_nid_no_patch( "[gpu] %llu dispatches (%llu dispatch cache hits, %llu evictions, %llu recompile cache hits): capture %.1f s, cache key %.1f s, recompile %.1f s, device %.1f s\n", static_cast<unsigned long long>(dispatches.load(std::memory_order_relaxed)), static_cast<unsigned long long>(hits), static_cast<unsigned long long>(evictions), static_cast<unsigned long long>(cacheHits.load(std::memory_order_relaxed)), captureMs.load(std::memory_order_relaxed) / 1000, keyMs.load(std::memory_order_relaxed) / 1000, recompileMs.load(std::memory_order_relaxed) / 1000, deviceMs.load(std::memory_order_relaxed) / 1000);
    }
    timing.Mark("epilogue_report");
}

}
