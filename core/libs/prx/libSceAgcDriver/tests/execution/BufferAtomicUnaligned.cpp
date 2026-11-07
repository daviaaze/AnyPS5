#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t MaxWave = 64;
constexpr std::uint32_t StructuredRecords = 4096;
constexpr std::uint32_t Sentinel = 0x5eed0001u;
constexpr std::size_t OffsetWord = 2;
alignas(256) std::array<std::uint32_t, StructuredRecords + 1> Buffer{};
alignas(256) std::array<std::uint32_t, MaxWave> Output{};

alignas(256) auto Code = std::to_array<std::uint32_t>({
    0x34020082,
    0x7e040281,
    0xe0c85000, 0x80000201,
    0xbf8c3f70,
    0xe0701000, 0x80010201,
    0xbf810000,
});

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

std::array<std::uint32_t, 4> StructuredBufferDescriptor(const void* data, std::uint32_t stride, std::uint32_t records) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address),
            static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u),
            records, 0x00005204u | (1u << 23u)};
}

void RunAddTid(AgcDriver::VulkanDevice& device, std::uint32_t waveSize, const ShaderRecompiler::SpirvTarget& target) {
    std::fill(Buffer.begin(), Buffer.end(), Sentinel);
    for (std::uint32_t lane = 0; lane < waveSize; ++lane) Buffer[lane] = lane * 100u;
    Output.fill(0xdeadbeefu);
    auto code = Code;
    // Start from the known zero-offset BUFFER_ATOMIC_ADD word: the raw cases
    // mutate Code's immediate offset. OFFEN and IDXEN must both remain clear
    // so descriptor ADD_TID alone supplies the guest lane index.
    code[OffsetWord] = 0xe0c85000u & ~0x1000u;
    std::vector<std::uint32_t> userData(8, 0u);
    const auto buffer = StructuredBufferDescriptor(Buffer.data(), sizeof(std::uint32_t), StructuredRecords);
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * 4u));
    std::copy(buffer.begin(), buffer.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> shaderCode(code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(shaderCode.data()), std::as_bytes(shaderCode)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{waveSize, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(shaderCode.data()), shaderCode, 0, {}},
        {waveSize, 0, userData, compute, std::nullopt, std::nullopt, memory},
        target,
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    const auto guestBuffers = std::ranges::find_if(result.bindings, [](const ShaderRecompiler::DescriptorBinding& binding) {
        return binding.role == ShaderRecompiler::DescriptorRole::GuestBuffers;
    });
    Require(guestBuffers != result.bindings.end() && guestBuffers->bufferFootprints.size() == 2u,
            "buffer atomic ADD_TID: compiled result omitted its guest-buffer footprints");
    Require(guestBuffers->bufferRead.size() == 2u && guestBuffers->bufferWritten.size() == 2u &&
            guestBuffers->bufferAtomic.size() == 2u && guestBuffers->bufferRead[0] &&
            guestBuffers->bufferWritten[0] && guestBuffers->bufferAtomic[0] &&
            !guestBuffers->bufferAtomic[1] && guestBuffers->bufferWritten[1],
            "buffer atomic ADD_TID: guest read/write/atomic permissions changed");
    const auto& footprint = guestBuffers->bufferFootprints[0];
    Require(footprint.bounded && footprint.readBegin == 0u && footprint.readEnd == waveSize * sizeof(std::uint32_t) &&
            footprint.writeBegin == 0u && footprint.writeEnd == waveSize * sizeof(std::uint32_t),
            "buffer atomic ADD_TID: structured descriptor did not narrow to the guest lane window");
    for (std::uint32_t round = 0; round < 2u; ++round) {
        Output.fill(0xdeadbeefu);
        device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(shaderCode.data()));
        device.WaitIdle();
        for (std::uint32_t lane = 0; lane < waveSize; ++lane) {
            const auto oldValue = lane * 100u + round;
            Require(Output[lane] == oldValue, "buffer atomic ADD_TID: lane " + std::to_string(lane) + " returned " + std::to_string(Output[lane]) + ", expected " + std::to_string(oldValue));
            Require(Buffer[lane] == oldValue + 1u, "buffer atomic ADD_TID: lane " + std::to_string(lane) + " left " + std::to_string(Buffer[lane]) + ", expected " + std::to_string(oldValue + 1u));
        }
        Require(std::all_of(Buffer.begin() + waveSize, Buffer.end(), [](std::uint32_t value) { return value == Sentinel; }),
                "buffer atomic ADD_TID: an unwritten structured record or padding dword changed");
        Require(std::all_of(Output.begin() + waveSize, Output.end(), [](std::uint32_t value) { return value == 0xdeadbeefu; }),
                "buffer atomic ADD_TID: output padding changed");
    }
    Output.fill(0xdeadbeefu);
    device.Dispatch(result, 2, 1, 1, {}, reinterpret_cast<std::uintptr_t>(shaderCode.data()));
    device.WaitIdle();
    for (std::uint32_t lane = 0; lane < waveSize; ++lane) {
        const auto firstPossibleOld = lane * 100u + 2u;
        Require(Output[lane] == firstPossibleOld || Output[lane] == firstPossibleOld + 1u,
                "buffer atomic ADD_TID: contended lane " + std::to_string(lane) + " returned an impossible old value");
        Require(Buffer[lane] == firstPossibleOld + 2u,
                "buffer atomic ADD_TID: two workgroups did not leave the contended lane at its final total");
    }
    Require(std::all_of(Buffer.begin() + waveSize, Buffer.end(), [](std::uint32_t value) { return value == Sentinel; }),
            "buffer atomic ADD_TID: contended workgroups changed an unwritten record or padding dword");
}

void Run(AgcDriver::VulkanDevice& device, std::uint32_t offset) {
    for (std::uint32_t lane = 0; lane < Threads; ++lane) Buffer[lane] = lane * 100u;
    Buffer[Threads] = Sentinel;
    Output.fill(0xdeadbeefu);
    Code[OffsetWord] = 0xe0c85000u | offset;
    std::vector<std::uint32_t> userData(8, 0u);
    const auto buffer = BufferDescriptor(Buffer.data(), static_cast<std::uint32_t>(Buffer.size() * 4u));
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * 4u));
    std::copy(buffer.begin(), buffer.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> code(Code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
}

void Check(std::uint32_t offset) {
    const auto what = "buffer_atomic_add at byte offset " + std::to_string(offset) + ": lane ";
    for (std::uint32_t lane = 0; lane < Threads; ++lane) {
        Require(Output[lane] == lane * 100u, what + std::to_string(lane) + " returned " + std::to_string(Output[lane]) + ", expected " + std::to_string(lane * 100u));
        Require(Buffer[lane] == lane * 100u + 1u, what + std::to_string(lane) + " left dword " + std::to_string(Buffer[lane]) + ", expected " + std::to_string(lane * 100u + 1u));
    }
    Require(Buffer[Threads] == Sentinel, what + "the dword after the last lane changed");
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (!TargetHasCapability(device->Target(), spv::CapabilityInt64)) {
            std::puts("skipped, the device has no shaderInt64");
            return VulkanTestSkipped;
        }
        for (std::uint32_t offset = 0; offset < 4u; ++offset) {
            Run(*device, offset);
            Check(offset);
        }
        RunAddTid(*device, 32u, device->Target());
        RunAddTid(*device, 64u, device->Target());
        RunAddTid(*device, 64u, device->ComputeTarget(32u));
        std::puts("buffer atomic unaligned tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
