#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/tests/execution/VulkanTestDevice.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <utility>
#include <vector>
#include <sys/mman.h>

using namespace AgcDriver::Graphics;
using AgcDriver::GuestMemory::GpuMutex;

namespace {
constexpr std::size_t Page = 16384;
constexpr std::size_t MiB = 1u << 20u;
PFN_vkGetDeviceProcAddr originalProc;
PFN_vkAllocateMemory originalAllocate;
PFN_vkFreeMemory originalFree;
std::uint64_t allocationCalls = 0;
std::map<VkDeviceMemory, VkDeviceSize> live;

VKAPI_ATTR VkResult VKAPI_CALL Allocate(VkDevice device, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks* callbacks, VkDeviceMemory* memory) {
    ++allocationCalls;
    const auto result = originalAllocate(device, info, callbacks, memory);
    if (result == VK_SUCCESS) live.emplace(*memory, info->allocationSize);
    return result;
}
VKAPI_ATTR void VKAPI_CALL Free(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* callbacks) {
    live.erase(memory);
    originalFree(device, memory, callbacks);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Proc(VkDevice device, const char* name) {
    if (std::strcmp(name, "vkAllocateMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(Allocate);
    if (std::strcmp(name, "vkFreeMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(Free);
    return originalProc(device, name);
}
template<class F> F Function(const Context& context, const char* name) {
    const auto function = reinterpret_cast<F>(originalProc(context.device, name));
    Require(function != nullptr, name);
    return function;
}
struct Fixture {
    Context context;
    explicit Fixture(const Context& base) : context(base) {
        context.deviceProc = Proc;
        context.functions = nullptr;
        context.bufferPool.reset();
        context.cacheReclaim = nullptr;
        context.recorder = nullptr;
        GetBufferPool(context);
    }
    ~Fixture() { context.bufferPool.reset(); }
};

struct Guest {
    std::byte* data = nullptr;
    std::size_t bytes;
    std::vector<bool> readable;
    explicit Guest(std::size_t bytes) : bytes(bytes), readable(bytes / Page, true) {
        Require(bytes != 0 && bytes % Page == 0, "guest fixture size is not page aligned");
        void* raw = mmap(nullptr, bytes + Page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        Require(raw != MAP_FAILED, "guest mmap failed");
        const auto begin = reinterpret_cast<std::uintptr_t>(raw);
        const auto aligned = (begin + Page - 1) & ~(std::uintptr_t(Page) - 1);
        if (aligned > begin) munmap(raw, aligned - begin);
        if (aligned < begin + Page) munmap(reinterpret_cast<void*>(aligned + bytes), begin + Page - aligned);
        data = reinterpret_cast<std::byte*>(aligned);
        GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(data, bytes);
        GuestAllocations::Mutation().Add(data, bytes, true, true, true);
    }
    ~Guest() {
        GuestAllocations::Mutation().Remove(data);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(data, bytes);
        munmap(data, bytes);
    }
    std::uint64_t Address() const { return reinterpret_cast<std::uintptr_t>(data); }
    void Protect(std::size_t offset, std::size_t length, bool accessible) {
        Require(offset % Page == 0 && length % Page == 0 && offset <= bytes && length <= bytes - offset, "invalid fixture protection interval");
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(data + offset, length);
        Require(mprotect(data + offset, length, accessible ? PROT_READ | PROT_WRITE : PROT_NONE) == 0, "guest mprotect failed");
        if (accessible) GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(data + offset, length);
        std::fill(readable.begin() + offset / Page, readable.begin() + (offset + length) / Page, accessible);
    }
    void Fill(std::byte value) {
        for (std::size_t page = 0; page < readable.size(); ++page)
            if (readable[page]) std::fill_n(data + page * Page, Page, value);
    }
    std::byte Expected(std::size_t offset) const { return readable[offset / Page] ? data[offset] : std::byte{0}; }
};

void Verify(const Buffer& buffer, const Guest& guest) {
    const auto bytes = buffer.Bytes();
    Require(bytes.size() <= guest.bytes, "snapshot exceeds guest fixture");
    for (std::size_t i = 0; i < bytes.size(); ++i)
        Require(bytes[i] == guest.Expected(i), "snapshot contains stale guest data or nonzero inaccessible bytes");
}
void Read(Buffer& buffer, const Guest& guest, const char* label) {
    try {
        buffer.ReadCommitted(guest.Address());
        Verify(buffer, guest);
    } catch (...) {
        std::fprintf(stderr, "snapshot case failed: %s\n", label);
        throw;
    }
}

void ContentTransitions(const Context& base) {
    Fixture fixture(base);
    Guest guest(6 * Page);
    Buffer buffer(fixture.context, guest.bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    guest.Fill(std::byte{0x19});
    Read(buffer, guest, "accessible");
    guest.Protect(2 * Page, Page, false);
    guest.Protect(4 * Page, 2 * Page, false);
    Read(buffer, guest, "holes-cold");
    guest.Fill(std::byte{0x3b});
    Read(buffer, guest, "holes-warm");
    guest.Protect(5 * Page, Page, true);
    guest.Fill(std::byte{0x66});
    Read(buffer, guest, "newly-readable-tail");
    guest.Protect(0, guest.bytes, false);
    Read(buffer, guest, "all-hole-cold");
    Read(buffer, guest, "all-hole-warm");
    guest.Protect(0, guest.bytes, true);
    guest.Fill(std::byte{0x91});
    Read(buffer, guest, "restored-full");
}

void CountedReuse(const Context& base, std::size_t bytes, const char* cold, const char* warm) {
    Fixture fixture(base);
    Guest guest(bytes);
    guest.Fill(std::byte{0x27});
    guest.Protect(2 * Page, bytes - 2 * Page, false);
    const void* mapping = nullptr;
    VkBuffer handle = VK_NULL_HANDLE;
    {
        Buffer poison(fixture.context, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        mapping = std::as_const(poison).Bytes().data();
        handle = poison.Handle();
        std::fill(poison.Bytes().begin(), poison.Bytes().end(), std::byte{0xe7});
    }
    for (const auto* label : {cold, warm}) {
        const auto before = allocationCalls;
        Buffer buffer(fixture.context, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        Require(allocationCalls == before && buffer.Handle() == handle && std::as_const(buffer).Bytes().data() == mapping, "exact staging allocation was not reused");
        guest.Fill(label == cold ? std::byte{0x39} : std::byte{0xb4});
        Read(buffer, guest, label);
        std::printf("{\"case\":\"%s\",\"bytes\":%zu,\"same_buffer_and_mapping\":true,\"new_vk_allocations\":0,\"all_bytes_verified\":true}\n", label, bytes);
    }
}

void MutableExposure(const Context& base) {
    Fixture fixture(base);
    Guest guest(6 * Page);
    guest.Fill(std::byte{0x52});
    guest.Protect(Page, 5 * Page, false);
    Buffer buffer(fixture.context, guest.bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    const auto escaped = buffer.Bytes();
    Read(buffer, guest, "exposed-first");
    std::fill(escaped.begin(), escaped.end(), std::byte{0xf1});
    Read(buffer, guest, "exposed-after-fill");
    // A fill must not re-enable tracking while the old writable span is still valid.
    std::fill(escaped.begin(), escaped.end(), std::byte{0x6d});
    Read(buffer, guest, "exposed-again");
}

void NearSize(const Context& base) {
    Fixture fixture(base);
    constexpr auto capacity = 4 * MiB;
    constexpr auto small = 2 * MiB + Page;
    constexpr auto large = small + 2 * Page;
    Guest guest(capacity);
    guest.Fill(std::byte{0x41});
    guest.Protect(2 * Page, capacity - 2 * Page, false);
    const void* mapping = nullptr;
    {
        Buffer poison(fixture.context, capacity, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        mapping = std::as_const(poison).Bytes().data();
        std::fill(poison.Bytes().begin(), poison.Bytes().end(), std::byte{0xcd});
    }
    for (const auto [bytes, label] : {std::pair{small, "near-small"}, {large, "near-grow"}, {small, "near-shrink"}, {capacity, "near-grow-full"}}) {
        const auto before = allocationCalls;
        Buffer buffer(fixture.context, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        Require(allocationCalls == before && std::as_const(buffer).Bytes().data() == mapping, "near-size case did not exercise the same physical allocation");
        guest.Fill(std::byte{0x71});
        Read(buffer, guest, label);
    }
}

void SlabIsolation(const Context& base) {
    Fixture fixture(base);
    constexpr auto bytes = 2 * MiB + 40 * Page;
    Guest first(bytes), second(bytes);
    first.Fill(std::byte{0x11});
    second.Fill(std::byte{0x22});
    first.Protect(2 * Page, bytes - 2 * Page, false);
    Buffer a(fixture.context, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    Buffer b(fixture.context, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    Require(std::as_const(a).Bytes().data() != std::as_const(b).Bytes().data(), "live slab slices alias");
    Read(a, first, "slab-a-cold");
    Read(b, second, "slab-b-accessible");
    second.Protect(2 * Page, bytes - 2 * Page, false);
    Read(b, second, "slab-b-new-holes");
    Read(a, first, "slab-a-warm");
}

void RecordRead(const Context& context, Recorder& recorder, const std::shared_ptr<Buffer>& source, Buffer& destination, std::size_t bytes) {
    const auto commands = recorder.Commands();
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    CopyBuffer(context, commands, source->Handle(), 0, destination.Handle(), 0, bytes);
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    recorder.Keep(source, bytes);
}
void CheckReadback(Buffer& destination, const Guest& guest) {
    destination.Invalidate();
    Verify(std::as_const(destination), guest);
}

void GpuLifetime(const Context& base) {
    Fixture fixture(base);
    const auto& context = fixture.context;
    constexpr auto bytes = 5 * MiB + 24 * Page;
    Guest guest(bytes);
    guest.Fill(std::byte{0x39});
    guest.Protect(2 * Page, bytes - 2 * Page, false);
    const auto create = Function<PFN_vkCreateSemaphore>(context, "vkCreateSemaphore");
    const auto destroy = Function<PFN_vkDestroySemaphore>(context, "vkDestroySemaphore");
    const auto signal = Function<PFN_vkSignalSemaphoreKHR>(context, "vkSignalSemaphoreKHR");
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    info.pNext = &type;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    Check(create(context.device, &info, nullptr, &semaphore), "create native snapshot lifetime gate");
    struct Gate {
        const Context& context;
        VkSemaphore semaphore;
        PFN_vkSignalSemaphoreKHR signal;
        PFN_vkDestroySemaphore destroy;
        bool opened = false;
        void Open() {
            if (opened) return;
            VkSemaphoreSignalInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
            info.semaphore = semaphore;
            info.value = 1;
            Check(signal(context.device, &info), "signal native snapshot lifetime gate");
            opened = true;
        }
        ~Gate() { destroy(context.device, semaphore, nullptr); }
    } gate{context, semaphore, signal, destroy};
    Buffer destination(context, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    Recorder recorder(context, true);
    struct Drain {
        Recorder& recorder;
        Gate& gate;
        ~Drain() {
            try { gate.Open(); recorder.Sync(); recorder.FlushDeferredReleases(); }
            catch (const std::exception& error) { std::fprintf(stderr, "snapshot fixture drain failed: %s\n", error.what()); }
        }
    } drain{recorder, gate};
    Require(recorder.HasTimeline(), "native snapshot lifetime fixture requires timeline semaphores");
    auto source = std::make_shared<Buffer>(context, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    const auto handle = source->Handle();
    const auto mapping = std::as_const(*source).Bytes().data();
    Read(*source, guest, "gpu-first");
    RecordRead(context, recorder, source, destination, bytes);
    std::weak_ptr<Buffer> owner = source;
    recorder.HoldNextSubmit(semaphore, 1);
    recorder.Submit();
    source.reset();
    Require(!owner.expired() && recorder.UnsignaledBatches() != 0, "GPU source lost its owner before blocked fence completion");
    const auto before = allocationCalls;
    auto competitor = std::make_shared<Buffer>(context, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    Require(allocationCalls == before + 1 && competitor->Handle() != handle && std::as_const(*competitor).Bytes().data() != mapping, "in-flight staging allocation was reused");
    gate.Open();
    recorder.Sync();
    recorder.FlushDeferredReleases();
    Require(owner.expired(), "completed GPU source retained a final owner");
    CheckReadback(destination, guest);
    const auto after = allocationCalls;
    auto reused = std::make_shared<Buffer>(context, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    Require(allocationCalls == after && reused->Handle() == handle && std::as_const(*reused).Bytes().data() == mapping, "final-owner release did not make exact allocation reusable");
    for (const auto value : {std::byte{0xb4}, std::byte{0x5d}}) {
        guest.Fill(value);
        Read(*reused, guest, "gpu-reused");
        RecordRead(context, recorder, reused, destination, bytes);
        recorder.Sync();
        recorder.FlushDeferredReleases();
        CheckReadback(destination, guest);
    }
    std::puts("{\"case\":\"gpu-final-owner\",\"blocked_fence\":true,\"distinct_while_blocked\":true,\"reused_after_final_owner\":true,\"all_gpu_bytes_verified\":true}");
}
}

int main() {
    try {
        Require(setenv("APS5_RELEASE_ON_UNLOCK", "1", 1) == 0, "cannot select deterministic fixture release mode");
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const auto base = device->graphicsContext();
        originalProc = base.deviceProc;
        originalAllocate = Function<PFN_vkAllocateMemory>(base, "vkAllocateMemory");
        originalFree = Function<PFN_vkFreeMemory>(base, "vkFreeMemory");
        std::lock_guard lock(GpuMutex());
        CountedReuse(base, 5 * MiB + 24 * Page, "dedicated-cold", "dedicated-warm");
        Require(live.empty(), "dedicated fixture leaked backing");
        CountedReuse(base, 2 * MiB + 40 * Page, "medium-cold", "medium-warm");
        Require(live.empty(), "medium fixture leaked backing");
        ContentTransitions(base);
        MutableExposure(base);
        NearSize(base);
        SlabIsolation(base);
        GpuLifetime(base);
        Require(live.empty(), "snapshot fixture leaked backing");
        std::printf("{\"result\":\"pass\",\"actual_vk_allocation_calls\":%llu,\"live_allocations\":0}\n", static_cast<unsigned long long>(allocationCalls));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "snapshot-zero-tail: FAIL: %s\n", error.what());
        return 1;
    }
}
