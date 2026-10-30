#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VertexInput.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "DepthFastClearHarness.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace AgcDriver {
struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};
}

namespace {
using namespace AgcDriver::Graphics;
using ShaderRecompiler::ShaderStage;
constexpr std::size_t BlockBytes = 65536;
constexpr std::uint32_t Width = 64, Height = 32, Limit = 3, Far = Limit + 0x10000u;
constexpr std::size_t ColorBytes = Width * Height * 4u, VertexOffset = 0x4000, IndexOffset = 0x5000, ReferenceOffset = 0x5100, OutputOffset = 0x6000;
alignas(256) constexpr std::array<std::uint32_t, 7> VertexCode{0xbe822100, 0x34020a84, 0xe0781000, 0x80010801, 0xf80008cf, 0x0b0a0908, 0xbf810000};
alignas(256) constexpr std::array<std::uint32_t, 4> PixelCode{0x7e0e02f2, 0xf800180f, 0x07070707, 0xbf810000};

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        data = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, BlockBytes));
        if (data) GuestArena::GuestArenaCommit_nid_postfix(data, BlockBytes, PAGE_READWRITE, BlockBytes);
#else
        auto* mapped = mmap(nullptr, 2 * BlockBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + BlockBytes - 1u) & ~(BlockBytes - 1u);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + BlockBytes != begin + 2 * BlockBytes) munmap(reinterpret_cast<void*>(aligned + BlockBytes), begin + 2 * BlockBytes - aligned - BlockBytes);
            data = reinterpret_cast<std::uint8_t*>(aligned);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(data, BlockBytes);
        }
#endif
        Require(data != nullptr, "indexed render: guest allocation failed");
        std::memset(data, 0, BlockBytes);
        GuestAllocations::Mutation().Add(data, BlockBytes, true, true, true);
    }
    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(data);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(data, BlockBytes);
        GuestArena::GuestArenaRelease_nid_postfix(data, BlockBytes);
#else
        munmap(data, BlockBytes);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(data, BlockBytes);
#endif
    }
    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;
    std::uint8_t* At(std::size_t offset = 0) const { return data + offset; }
    std::uint64_t Address(std::size_t offset = 0) const { return reinterpret_cast<std::uintptr_t>(At(offset)); }
private:
    std::uint8_t* data = nullptr;
};

// Observe the actual bound host-visible index buffer. No transfer commands are inserted
// into the render pass, and production code/counters are unchanged.
class BindTrace {
public:
    struct Binding { VkDeviceMemory memory; VkDeviceSize offset; };
    static inline BindTrace* active = nullptr;
    PFN_vkGetDeviceProcAddr originalProc = nullptr;
    PFN_vkCmdBindIndexBuffer originalBind = nullptr;
    DeviceFunctions functions{};
    std::unordered_map<VkBuffer, Binding> bindings;
    std::unordered_map<VkDeviceMemory, void*> mappings;
    std::vector<std::byte> bytes;
    VkIndexType type = VK_INDEX_TYPE_MAX_ENUM;
    VkBuffer buffer = VK_NULL_HANDLE;
    std::size_t count = 0;
    ~BindTrace() { active = nullptr; }
    static VKAPI_ATTR VkResult VKAPI_CALL Bind(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
        auto& self = *active;
        const auto result = reinterpret_cast<PFN_vkBindBufferMemory>(self.originalProc(device, "vkBindBufferMemory"))(device, buffer, memory, offset);
        if (result == VK_SUCCESS) self.bindings[buffer] = {memory, offset};
        return result;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Map(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void** output) {
        auto& self = *active;
        const auto result = reinterpret_cast<PFN_vkMapMemory>(self.originalProc(device, "vkMapMemory"))(device, memory, offset, size, flags, output);
        if (result == VK_SUCCESS) {
            Require(offset == 0, "indexed render: unexpected nonzero mapping offset");
            self.mappings[memory] = *output;
        }
        return result;
    }
    static VKAPI_ATTR void VKAPI_CALL Unmap(VkDevice device, VkDeviceMemory memory) {
        auto& self = *active;
        self.mappings.erase(memory);
        reinterpret_cast<PFN_vkUnmapMemory>(self.originalProc(device, "vkUnmapMemory"))(device, memory);
    }
    static VKAPI_ATTR void VKAPI_CALL Free(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* callbacks) {
        auto& self = *active;
        self.mappings.erase(memory);
        reinterpret_cast<PFN_vkFreeMemory>(self.originalProc(device, "vkFreeMemory"))(device, memory, callbacks);
    }
    static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Resolve(VkDevice device, const char* name) {
        if (std::strcmp(name, "vkBindBufferMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&Bind);
        if (std::strcmp(name, "vkMapMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&Map);
        if (std::strcmp(name, "vkUnmapMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&Unmap);
        if (std::strcmp(name, "vkFreeMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&Free);
        return active->originalProc(device, name);
    }
    static VKAPI_ATTR void VKAPI_CALL Capture(VkCommandBuffer commands, VkBuffer buffer, VkDeviceSize offset, VkIndexType type) {
        auto& self = *active;
        self.originalBind(commands, buffer, offset, type);
        const auto binding = self.bindings.find(buffer);
        Require(binding != self.bindings.end(), "indexed render: bound index buffer was not observed");
        const auto mapping = self.mappings.find(binding->second.memory);
        Require(mapping != self.mappings.end(), "indexed render: bound index buffer is not mapped");
        const auto* source = static_cast<const std::byte*>(mapping->second) + binding->second.offset + offset;
        self.bytes.assign(source, source + self.count * (type == VK_INDEX_TYPE_UINT16 ? 2u : 4u));
        self.type = type;
        self.buffer = buffer;
    }
    void Install(Context& context) {
        active = this;
        originalProc = context.deviceProc;
        originalBind = context.Function<PFN_vkCmdBindIndexBuffer>("vkCmdBindIndexBuffer");
        functions = *context.functions;
        functions.cmdBindIndexBuffer = &Capture;
        context.deviceProc = &Resolve;
        context.functions = &functions;
        context.drawInputsInPlace = false;
        context.hostImportAlignment = 0;
        context.bufferPool = std::make_shared<BufferPool>(context);
    }
};

struct Programs { ShaderRecompiler::RecompileResult vertex, pixel; std::uint32_t vertexPush; };
Programs Compile(AgcDriver::VulkanDevice& device, const GuestBlock& input) {
    Programs result{};
    const auto address = input.Address(VertexOffset);
    const std::array<std::uint32_t, 4> vertexDescriptor{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (16u << 16u), Limit, 0x0104dfacu};
    const auto output = input.Address(OutputOffset);
    std::vector<std::uint32_t> userData(12, 0);
    userData[0] = 0x10000000u;
    userData[1] = 1;
    const std::array<std::uint32_t, 4> outputDescriptor{static_cast<std::uint32_t>(output), static_cast<std::uint32_t>((output >> 32u) & 0xffffu), 64u, 0x31016facu};
    std::copy(outputDescriptor.begin(), outputDescriptor.end(), userData.begin() + 4);
    ShaderRecompiler::ShaderVertexStageInfo info{};
    info.resourcesNum = 1;
    info.resources[0].fields = vertexDescriptor;
    info.resourcesDst[0] = {8, 4, 0, 0};
    info.fetchAttribReg = 8;
    info.fetchBufferReg = 10;
    info.fetchEmbedded = true;
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(VertexCode.data()), std::as_bytes(std::span(VertexCode))}}};
    ShaderRecompiler::RecompileRequest vertexRequest{{ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(VertexCode.data()), VertexCode, 0, {}},
        {64u, 0, userData, std::nullopt, std::nullopt, info, vertexMemory}, device.Target(), {0, 0, 0, 64}};
    vertexRequest.useCache = false;
    result.vertex = ShaderRecompiler::Recompile(vertexRequest);
    Require(result.vertex.vertexAttributes.size() == 1 && ZeroFetchVertex(result.vertex.vertexAttributes) == Limit, "indexed render: expected zero-fetch limit unavailable");
    result.vertexPush = static_cast<std::uint32_t>(result.vertex.pushConstants.size());
    ShaderRecompiler::ShaderPixelStageInfo pixelInfo{};
    pixelInfo.wave32 = true;
    pixelInfo.inputAddr = ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionX) | ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionY);
    pixelInfo.posX = true;
    pixelInfo.posY = true;
    pixelInfo.targetOutputMode[0] = 9;
    pixelInfo.targetExportMapping.fill(0xe4u);
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{reinterpret_cast<std::uintptr_t>(PixelCode.data()), std::as_bytes(std::span(PixelCode))}}};
    ShaderRecompiler::RecompileRequest pixelRequest{{ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(PixelCode.data()), PixelCode, 0, {}},
        {64u, 0, {}, std::nullopt, pixelInfo, std::nullopt, pixelMemory}, device.Target(), PixelPushLayout(result.vertexPush, device.Target())};
    pixelRequest.useCache = false;
    result.pixel = ShaderRecompiler::Recompile(pixelRequest);
    return result;
}

template<typename T, std::size_t Size>
void WriteIndices(const GuestBlock& input, std::size_t offset, const std::array<T, Size>& indices) {
    AgcDriver::GuestMemory::Write(input.Address(offset), std::as_bytes(std::span(indices)), alignof(T));
}

void Run(AgcDriver::VulkanDevice& device, BindTrace& trace) {
    auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
    trace.Install(context);
    GuestBlock input, color;
    AgcDriver::GuestMemory::Write(input.Address(VertexOffset), std::as_bytes(std::span(DepthFastClearHarness::Triangles())), alignof(float));
    auto programs = Compile(device, input);
    Recorder recorder(context);
    recorder.Activate();
    context.recorder = &recorder;
    const std::array<CompiledShader, 2> shaders{{{ShaderStage::Vertex, &programs.vertex, 0}, {ShaderStage::Fragment, &programs.pixel, PixelPushOffset(programs.vertexPush, device.Target())}}};
    const auto render = [&](std::size_t offset, std::uint32_t count, std::uint32_t indexSize = 4, std::uint32_t firstVertex = 0, bool restart = false) {
        std::vector<std::byte> clear(ColorBytes, std::byte{0x40});
        AgcDriver::GuestMemory::Write(color.Address(), clear);
        State state{};
        state.stages = {ShaderPath::Vertex, 0u, 64u, 64u, std::nullopt, std::nullopt};
        state.color = {color.Address(), {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, ColorBytes, 0xe4u};
        state.colors = {state.color};
        state.hasColorTarget = true;
        state.renderExtent = {Width, Height};
        state.topology = restart ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        state.primitiveRestart = restart;
        state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
        state.scissor = {{0, 0}, {Width, Height}};
        state.cullMode = VK_CULL_MODE_NONE;
        state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        state.blend.colorWriteMask = 15;
        state.blends = {state.blend};
        trace.count = count;
        trace.bytes.clear();
        const AgcDriver::Pm4::DrawParameters draw{input.Address(offset), count, indexSize, 1u, 0u, true, firstVertex};
        { std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex()); Draw(context, state, draw, shaders); recorder.Sync(); }
        std::vector<std::byte> pixels(ColorBytes);
        AgcDriver::GuestMemory::ReadCommitted(color.Address(), pixels);
        return pixels;
    };
    const auto checkBound = [&](const auto& indices) {
        const auto expected = std::as_bytes(std::span(indices));
        Require(trace.bytes.size() == expected.size() && std::equal(expected.begin(), expected.end(), trace.bytes.begin()), "indexed render: incorrect bound index bytes");
    };
    const std::array<std::uint32_t, 6> reference{0, 1, 2, Limit, Limit, Limit};
    const std::array<std::uint32_t, 6> outliers{0, 1, 2, Far, Far, Far};
    WriteIndices(input, ReferenceOffset, reference);
    WriteIndices(input, IndexOffset, outliers);
    const auto raw = render(ReferenceOffset, 6);
    Require(std::all_of(raw.begin(), raw.end(), [](std::byte b) { return b == std::byte{255}; }), "indexed render: reference did not cover the offscreen image in white");
    const auto first = render(IndexOffset, 6);
    Require(first == raw, "indexed render: clamp changed rendered pixels");
    checkBound(reference);
    const auto repeated = render(IndexOffset, 6);
    Require(repeated == raw, "indexed render: repeated clamp changed rendered pixels");
    checkBound(reference);
    const std::array<std::uint32_t, 6> degenerate{0, 0, 0, Far, Far, Far};
    WriteIndices(input, IndexOffset, degenerate);
    const auto changed = render(IndexOffset, 6);
    Require(std::all_of(changed.begin(), changed.end(), [](std::byte b) { return b == std::byte{0x40}; }), "indexed render: CPU mutation retained the previous triangle");
    checkBound(std::array<std::uint32_t, 6>{0, 0, 0, Limit, Limit, Limit});
    WriteIndices(input, IndexOffset, outliers);
    const std::array<std::uint32_t, 6> shifted{0, 1, 2, 2, 2, 2};
    WriteIndices(input, ReferenceOffset, shifted);
    const auto shiftedRaw = render(ReferenceOffset, 6, 4, 1);
    const auto shiftedClamp = render(IndexOffset, 6, 4, 1);
    Require(shiftedClamp == shiftedRaw, "indexed render: firstVertex-dependent clamp changed pixels");
    checkBound(shifted);
    const std::array<std::uint32_t, 7> restartReference{0, 1, 2, 0xffffffffu, Limit, Limit, Limit};
    const std::array<std::uint32_t, 7> restartOutlier{0, 1, 2, 0xffffffffu, Far, Far, Far};
    WriteIndices(input, ReferenceOffset, restartReference);
    WriteIndices(input, IndexOffset, restartOutlier);
    const auto restartRaw = render(ReferenceOffset, 7, 4, 0, true);
    Require(render(IndexOffset, 7, 4, 0, true) == restartRaw && restartRaw == raw, "indexed render: UINT32 restart changed pixels");
    checkBound(restartReference);
    // UINT16 cannot exceed the 0x10000 slack: this verifies consumer/restart behavior, not clamping.
    const std::array<std::uint16_t, 7> narrow{0, 1, 2, 0xffffu, 0, 1, 2};
    WriteIndices(input, IndexOffset, narrow);
    Require(render(IndexOffset, 7, 2, 0, true) == raw && trace.type == VK_INDEX_TYPE_UINT16, "indexed render: UINT16 restart changed pixels or width");
    checkBound(narrow);
    recorder.Sync();
    std::puts("actual indexed render: reference, clamp, repeat, CPU mutation, firstVertex and restart pixels verified");
}
}

int main() {
    try {
        BindTrace trace;
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Run(*device, trace);
        device.reset();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
