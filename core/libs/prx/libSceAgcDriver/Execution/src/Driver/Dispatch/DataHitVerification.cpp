#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <limits>

namespace AgcDriver::DriverDetail {

void Driver::verifyDataHit(const ShaderSnapshot& snapshot, std::size_t codeOffset, ShaderRecompiler::RecompileRequest request, std::span<const ShaderRecompiler::MemoryRegion> memory, std::uint64_t address, const DispatchVariant& variant, std::span<const std::uint32_t> liveWords, const ShaderRecompiler::RecompileResult& patched) {
    const auto fail = [&](const char* what, std::size_t position, std::size_t slot) {
        std::fprintf(stderr, "[dispatch-cache] APS5_VERIFY_DATA_HITS: %s disagrees (program 0x%llx, position %zu, slot %zu)\n", what, static_cast<unsigned long long>(address), position, slot);
        std::fflush(stderr);
        std::abort();
    };
    auto verifyMemory = std::make_shared<ShaderMemory>(memory, &queryPendingWrite, &observePendingWrite, hookWaitCounter(), &QueryPendingSpan);
    auto invocation = InvocationFor(snapshot, codeOffset, request);
    const auto capture = CaptureInvocation(*verifyMemory, snapshot, codeOffset, request, invocation);
    const auto regions = verifyMemory->Regions();
    request.context.memory = regions;
    const auto result = invocation.Materialize(*capture);
    std::vector<std::size_t> coveredBytes(variant.runs.size());
    std::size_t expectedBytes = 0;
    for (const auto& [begin, end] : variant.runs) {
        if (end <= begin) fail("the read set", expectedBytes, 0);
        const auto length = end - begin;
        if (length % sizeof(std::uint32_t) != 0 || length > std::numeric_limits<std::size_t>::max() || expectedBytes > std::numeric_limits<std::size_t>::max() - static_cast<std::size_t>(length)) fail("the read set", expectedBytes, 0);
        expectedBytes += static_cast<std::size_t>(length);
    }
    if (expectedBytes / sizeof(std::uint32_t) != liveWords.size()) fail("the read set", expectedBytes, 0);
    for (const auto& region : regions) {
        const bool registered = std::any_of(memory.begin(), memory.end(), [&](const auto& known) { return region.guestAddress >= known.guestAddress && region.guestAddress < known.guestAddress + known.bytes.size(); });
        if (registered) continue;
        if (region.guestAddress % sizeof(std::uint32_t) != 0 || region.bytes.size() % sizeof(std::uint32_t) != 0 || region.guestAddress > UINT64_MAX - region.bytes.size()) fail("the read set", 0, 0);
        std::size_t consumed = 0;
        while (consumed < region.bytes.size()) {
            const auto current = region.guestAddress + consumed;
            std::size_t matched = variant.runs.size();
            for (std::size_t run = 0; run < variant.runs.size(); ++run) {
                if (current >= variant.runs[run].first && current < variant.runs[run].second) {
                    if (matched != variant.runs.size()) fail("the read set", 0, run);
                    matched = run;
                }
            }
            if (matched == variant.runs.size()) fail("the read set", 0, 0);
            const auto [begin, end] = variant.runs[matched];
            const auto offset = current - begin;
            if (offset != coveredBytes[matched] || offset % sizeof(std::uint32_t) != 0) fail("the read set", matched, 0);
            const auto bytes = std::min(static_cast<std::size_t>(end - current), region.bytes.size() - consumed);
            if (bytes == 0 || bytes % sizeof(std::uint32_t) != 0) fail("the read set", matched, 0);
            std::size_t wordOffset = 0;
            for (std::size_t run = 0; run < matched; ++run) wordOffset += static_cast<std::size_t>((variant.runs[run].second - variant.runs[run].first) / sizeof(std::uint32_t));
            wordOffset += static_cast<std::size_t>(offset / sizeof(std::uint32_t));
            const auto count = bytes / sizeof(std::uint32_t);
            if (wordOffset > liveWords.size() || count > liveWords.size() - wordOffset || std::memcmp(region.bytes.data() + consumed, liveWords.data() + wordOffset, bytes) != 0) fail("the captured bytes", wordOffset, matched);
            coveredBytes[matched] += bytes;
            consumed += bytes;
        }
    }
    for (std::size_t run = 0; run < variant.runs.size(); ++run) {
        if (coveredBytes[run] != variant.runs[run].second - variant.runs[run].first) fail("the read set", run, run);
    }
    if (result->variantId != patched.variantId) fail("the variant", 0, 0);
    if (result->bindings.size() != patched.bindings.size() || result->pushConstants != patched.pushConstants) fail("the bindings", 0, 0);
    for (std::size_t b = 0; b < result->bindings.size(); ++b) {
        const auto& fresh = result->bindings[b];
        const auto& reused = patched.bindings[b];
        if (fresh.kind != reused.kind || fresh.role != reused.role || fresh.binding != reused.binding || fresh.count != reused.count || fresh.guestDescriptor != reused.guestDescriptor) fail("the bindings", b, 0);
        if (fresh.bufferFootprints != reused.bufferFootprints) fail("the buffer transfer footprints", b, 0);
    }
    std::lock_guard cacheLock(dispatchCacheMutex);
    ++entryCounters.dataVerified;
}

}
