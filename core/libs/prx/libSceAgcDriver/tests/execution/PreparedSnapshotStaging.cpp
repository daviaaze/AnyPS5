#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <mutex>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Side = 512;
constexpr std::uint32_t Format8888Unorm = 56;
constexpr std::uint32_t Type2D = 9;
constexpr std::size_t TextureBytes = static_cast<std::size_t>(Side) * Side * 4u;
constexpr std::size_t OutputBytes = Threads * 16u;

alignas(256) constexpr std::array<std::uint32_t, 11> SampleCode{
    0x34060084, 0x7e2802ff, 0x3c400000, 0x7e2a02ff, 0x3b800000, 0xf09c0f08, 0x00610a14, 0xbf8c3f70,
    0xe0781000, 0x80000a03, 0xbf810000,
};
constexpr std::array<std::uint32_t, 4> PointSampler{0x00000092u, 0x00fff000u, 0u, 0u};
constexpr std::array<std::uint8_t, 4> Initial{0x11u, 0x42u, 0x73u, 0xffu};
constexpr std::array<std::uint8_t, 4> Updated{0xe1u, 0x9au, 0x35u, 0xffu};

std::uint64_t AddressOf(const void* data) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
}

class GuestBlock {
public:
    explicit GuestBlock(std::size_t bytes) : size(bytes) {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        const auto allocationBytes = (size + 65535u) & ~std::size_t{65535u};
        block = static_cast<std::uint8_t*>(std::aligned_alloc(65536, allocationBytes));
#endif
        Require(block != nullptr, "prepared snapshot staging: cannot allocate guest memory");
        GuestAllocations::Mutation().Add(block, size, true, true, true);
    }
    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        VirtualFree(block, 0, MEM_RELEASE);
#else
        std::free(block);
#endif
    }
    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;
    std::uint8_t* Data() { return block; }

private:
    std::size_t size;
    std::uint8_t* block = nullptr;
};
class WatchedTexture {
public:
    WatchedTexture(VkDevice device, std::size_t bytes) : device(device), size(bytes) {
#ifdef _WIN32
        memory = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, 65536));
        if (memory != nullptr) GuestArena::GuestArenaCommit_nid_postfix(memory, bytes, PAGE_READWRITE, bytes);
#else
        constexpr std::uintptr_t alignment = 65536;
        void* mapped = mmap(nullptr, bytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + alignment - 1u) & ~(alignment - 1u);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + bytes != begin + bytes + alignment) munmap(reinterpret_cast<void*>(aligned + bytes), begin + alignment - aligned);
            memory = reinterpret_cast<std::uint8_t*>(aligned);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(memory, bytes);
        }
#endif
        Require(memory != nullptr, "prepared snapshot staging: cannot allocate watched texture memory");
        Require(AgcDriver::GuestMemory::Watched(Address(), bytes), "prepared snapshot staging: texture memory is not write-watched");
        std::memset(memory, 0x5a, bytes);
        GuestAllocations::Mutation().Add(memory, bytes, true, true, true);
        AgcDriver::GuestMemory::CollectWritesUncached(Address(), bytes);
    }
    ~WatchedTexture() {
        AgcDriver::Graphics::ClearCachedTextures(device);
        GuestAllocations::Mutation().Remove(memory);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(memory, size);
        GuestArena::GuestArenaRelease_nid_postfix(memory, size);
#else
        munmap(memory, size);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(memory, size);
#endif
    }
    WatchedTexture(const WatchedTexture&) = delete;
    WatchedTexture& operator=(const WatchedTexture&) = delete;
    std::uint8_t* Data() { return memory; }
    std::uint64_t Address() const { return reinterpret_cast<std::uint64_t>(memory); }
private:
    VkDevice device;
    std::size_t size;
    std::uint8_t* memory = nullptr;
};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = AddressOf(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

std::array<std::uint32_t, 8> TextureDescriptor(const void* data) {
    const auto address = AddressOf(data);
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (Format8888Unorm << 20u),
        ((Side - 1u) >> 2u) | ((Side - 1u) << 14u),
        0xfacu | (Type2D << 28u),
        0u, 0u, 0u, 0u,
    };
}

ShaderRecompiler::RecompileResult Compile(AgcDriver::VulkanDevice& device, const std::vector<std::uint32_t>& userData) {
    const std::span<const std::uint32_t> code(SampleCode);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    return ShaderRecompiler::Recompile(request);
}

std::uint32_t ExpectedChannel(std::uint8_t value) {
    return std::bit_cast<std::uint32_t>(static_cast<float>(value) / 255.0f);
}

void RequireOutput(const std::uint32_t* output, const std::array<std::uint8_t, 4>& expected) {
    for (std::uint32_t thread = 0; thread < Threads; ++thread) {
        for (std::uint32_t channel = 0; channel < 4; ++channel) {
            const auto wanted = ExpectedChannel(expected[channel]);
            if (output[thread * 4u + channel] != wanted) {
                char message[192];
                std::snprintf(message, sizeof(message), "prepared snapshot staging: thread %u channel %u was 0x%08x, expected 0x%08x", thread, channel, output[thread * 4u + channel], wanted);
                Require(false, message);
            }
        }
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (!AgcDriver::GuestMemory::WriteWatched()) {
            std::puts("skipped, guest write watching is unavailable");
            return VulkanTestSkipped;
        }

        WatchedTexture texture(device->graphicsContext().device, TextureBytes);
        GuestBlock outputBlock(OutputBytes);
        auto* output = reinterpret_cast<std::uint32_t*>(outputBlock.Data());
        const auto textureWords = TextureDescriptor(texture.Data());
        const auto bufferWords = BufferDescriptor(output, static_cast<std::uint32_t>(OutputBytes));
        std::vector<std::uint32_t> userData(24, 0u);
        std::copy(bufferWords.begin(), bufferWords.end(), userData.begin());
        std::copy(textureWords.begin(), textureWords.end(), userData.begin() + 4);
        std::copy(PointSampler.begin(), PointSampler.end(), userData.begin() + 12);
        const auto program = Compile(*device, userData);

        for (std::size_t offset = 0; offset < TextureBytes; offset += 4u) std::copy(Initial.begin(), Initial.end(), texture.Data() + offset);
        AgcDriver::GuestMemory::CollectWritesUncached(texture.Address(), TextureBytes);
        std::fill(output, output + Threads * 4u, 0xdeadbeefu);
        AgcDriver::GuestMemory::BumpCollectEpoch();
        {
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            device->Dispatch(program, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(SampleCode.data()));
            device->WaitIdle();
        }
        RequireOutput(output, Initial);

        const bool disabled = std::getenv("APS5_NO_PREPARED_SNAPSHOT_STAGING") != nullptr;
        const auto before = AgcDriver::Graphics::PreparedSnapshotStagings();
        for (std::size_t offset = 0; offset < TextureBytes; offset += 4u) std::copy(Updated.begin(), Updated.end(), texture.Data() + offset);
        AgcDriver::GuestMemory::CollectWritesUncached(texture.Address(), TextureBytes);
        AgcDriver::GuestMemory::BumpCollectEpoch();
        auto prepared = device->PrepareDispatch(program, {}, Threads, reinterpret_cast<std::uintptr_t>(SampleCode.data()));
        const auto afterPrepare = AgcDriver::Graphics::PreparedSnapshotStagings();
        if (disabled) {
            Require(afterPrepare.prepared == before.prepared && afterPrepare.used == before.used, "prepared snapshot staging: disabled mode changed staging counters during PrepareDispatch");
        } else {
            Require(afterPrepare.prepared == before.prepared + 1u && afterPrepare.used == before.used, "prepared snapshot staging: PrepareDispatch did not create exactly one staging buffer before dispatch");
        }
        {
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            device->Dispatch(program, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(SampleCode.data()), std::move(prepared));
            device->WaitIdle();
        }
        const auto afterDispatch = AgcDriver::Graphics::PreparedSnapshotStagings();
        if (disabled) {
            Require(afterDispatch.prepared == before.prepared && afterDispatch.used == before.used, "prepared snapshot staging: disabled mode changed staging counters during dispatch");
        } else {
            Require(afterDispatch.prepared == before.prepared + 1u && afterDispatch.used == before.used + 1u, "prepared snapshot staging: the prepared staging buffer was not used exactly once");
        }
        RequireOutput(output, Updated);
        std::puts("prepared snapshot staging tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
