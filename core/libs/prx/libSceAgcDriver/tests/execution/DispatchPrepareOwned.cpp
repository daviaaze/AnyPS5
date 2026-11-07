#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include <algorithm>
#include <array>
#include <initializer_list>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <span>
#include <vector>

namespace {

using namespace AgcDriver::DriverDetail;
using namespace AgcDriver;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

alignas(256) std::array<std::uint32_t, 7> CounterCode{0xf4080200u, 0xfa000000u, 0xbf8cc07fu, 0x7e000281u, 0xe0c80000u, 0x80020000u, 0xbf810000u};
alignas(256) std::array<std::uint32_t, 7> StoreCode{0xf4080200u, 0xfa000000u, 0xbf8cc07fu, 0x7e000202u, 0xe0700000u, 0x80020000u, 0xbf810000u};
alignas(256) std::array<std::uint32_t, 15> SpinCode{0xf4080200u, 0xfa000000u, 0xbe840381u, 0x9304ff04u, 0x0019660du, 0x8004ff04u, 0x3c6ef35fu, 0x80828102u, 0xbf078002u, 0xbf85fff9u, 0xbf8cc07fu, 0x7e000204u, 0xe0700000u, 0x80020000u, 0xbf810000u};
constexpr std::uint32_t ComputeQueue = 0x20u;
constexpr std::uint32_t WriterQueue = 0x28u;
constexpr std::uint32_t SpinIterations = 1u << 20u;

alignas(256) std::array<std::uint32_t, 4> Stale{};
alignas(256) std::array<std::uint32_t, 4> Fresh{};
alignas(256) std::array<std::uint32_t, 4> Target{};
alignas(256) std::array<std::uint32_t, 4> TargetDescriptor{};
alignas(256) std::array<std::uint32_t, 4> Spun{};
alignas(256) std::array<std::uint32_t, 4> SpunDescriptor{};
alignas(256) volatile std::uint32_t OtherDone = 0;
std::uint32_t Flushes = 0;

std::uint32_t Low(const volatile void* pointer) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

std::uint32_t High(const volatile void* pointer) {
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer)) >> 32u);
}

std::array<std::uint32_t, 4> Descriptor(const void* pointer, std::uint32_t bytes) {
    return {Low(pointer), High(pointer) & 0xffffu, bytes, 0x31016facu};
}

void SetShaderRegister(std::vector<std::uint32_t>& words, std::uint32_t offset, std::uint32_t value) {
    words.insert(words.end(), {0xc0017600u, offset, value});
}

void DispatchPacket(std::vector<std::uint32_t>& words, const void* code, std::initializer_list<std::uint32_t> userData) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(code));
    SetShaderRegister(words, 0x20c, static_cast<std::uint32_t>(address >> 8u));
    SetShaderRegister(words, 0x20d, static_cast<std::uint32_t>(address >> 40u) & 0xffu);
    for (const std::uint32_t offset : {0x207u, 0x208u, 0x209u}) SetShaderRegister(words, offset, 1);
    SetShaderRegister(words, 0x212, 0);
    SetShaderRegister(words, 0x213, static_cast<std::uint32_t>(userData.size()) << 1u);
    std::uint32_t offset = 0x240;
    for (const auto value : userData) SetShaderRegister(words, offset++, value);
    words.insert(words.end(), {0xc0031500u, 1, 1, 1, 0x8041u});
}

void WriteData(std::vector<std::uint32_t>& words, volatile std::uint32_t* target, std::uint32_t value) {
    words.insert(words.end(), {0xc0033700u, 0x00100200u, Low(target), High(target), value});
}

void Submit(std::uint32_t queue, std::vector<std::uint32_t>& words) {
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require(sceAgcDriverSubmitAcb(queue, &packet) == 0, "dispatch preparation fixture submit failed");
}

ShaderRecompiler::RecompileRequest Request(const ShaderSnapshot& shader, VulkanDevice& device,
        std::span<const std::uint32_t> userData, std::span<const ShaderRecompiler::MemoryRegion> memory) {
    ShaderRecompiler::RecompileRequest request{};
    request.shader = {ShaderRecompiler::ShaderStage::Compute, shader.codeAddress, shader.code, shader.headerAddress, shader.header};
    request.context.waveSize = 32;
    request.context.userData = userData;
    request.context.compute = ShaderRecompiler::ShaderComputeStageInfo{{1, 1, 1}, 0, {false, false, false}, false, 1, {}};
    request.context.memory = memory;
    request.target = device.ComputeTarget(32);
    request.layout = {0, 0, 0, 128};
    return request;

}
DispatchPrepareInput MakeInput(std::shared_ptr<VulkanDevice> device, std::array<std::uint32_t, 4>& descriptor) {
    auto shader = std::make_shared<ShaderSnapshot>(ShaderSnapshot{
        reinterpret_cast<std::uintptr_t>(CounterCode.data()), 0, 0, {CounterCode.begin(), CounterCode.end()}, {}});
    auto registry = std::make_shared<ShaderRegistry>();
    registry->emplace(shader->codeAddress, {shader});
    QueueState queueState;
    queueState.shader[0x240] = Low(descriptor.data());
    queueState.shader[0x241] = High(descriptor.data());
    Submission submission{};
    submission.queue = ComputeQueue;
    submission.serial = 41;
    submission.shaders = registry;
    const std::array<std::uint32_t, 2> roots{queueState.shader.at(0x240), queueState.shader.at(0x241)};
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{
        {shader->codeAddress, std::as_bytes(std::span(shader->code))}}};
    auto request = Request(*shader, *device, roots, memory);
    constexpr std::array<std::uint32_t, 5> packet{0xc0031500u, 1, 1, 1, 0x8041u};
    return DispatchPrepareInput(packet, std::move(shader), submission.shaders, std::move(device), 0,
        request, 0x12345678u, 0xabcdef01u, submission.queue, submission.serial, 12);
}


void CountFlush(std::uint64_t address, std::size_t bytes) {
    const auto target = reinterpret_cast<std::uintptr_t>(Target.data());
    if (address < target + sizeof(Target) && target < address + bytes) ++Flushes;
}

void CompareInlineAndOwned(std::shared_ptr<VulkanDevice> device) {
    auto input = MakeInput(device, Target);
    const auto invocation = InvocationFor(*input.shader, input.codeOffset, input.request);
    ShaderMemory inlineMemory(input.memory);
    const auto inlineCapture = inlineMemory.Capture(invocation);
    const auto inlineRegions = inlineMemory.Regions();
    const auto inlineCompiled = invocation.Materialize(*inlineCapture);

    auto owned = PrepareDispatchOwned(input);
    Require(owned.has_value(), "nonpending owned dispatch capture returned empty");
    Require(owned->key == input.key && owned->registerFingerprint == input.registerFingerprint,
        "owned dispatch result lost its cache key or register fingerprint");
    Require(owned->captured.size() == inlineRegions.size(), "owned capture region count differs from inline capture");
    for (std::size_t i = 0; i < inlineRegions.size(); ++i) {
        Require(owned->captured[i].guestAddress == inlineRegions[i].guestAddress
                && owned->captured[i].bytes.size() == inlineRegions[i].bytes.size()
                && std::equal(owned->captured[i].bytes.begin(), owned->captured[i].bytes.end(), inlineRegions[i].bytes.begin()),
            "owned capture region differs from inline capture");
    }
    const auto ownedWords = owned->compiledResult->spirv.Words();
    const auto inlineWords = inlineCompiled->spirv.Words();
    Require(ownedWords.size() == inlineWords.size() && std::equal(ownedWords.begin(), ownedWords.end(), inlineWords.begin()),
        "owned preparation compiled different output from inline preparation");
}

void PendingCaptureDoesNotFlush(std::shared_ptr<VulkanDevice> device) {
    Stale = {};
    Fresh = {};
    Target = Descriptor(Stale.data(), sizeof(Stale));
    TargetDescriptor = Descriptor(Target.data(), sizeof(Target));
    SpunDescriptor = Descriptor(Spun.data(), sizeof(Spun));
    OtherDone = 0;
    std::vector<std::uint32_t> writer;
    for (std::uint32_t i = 0; i < 16; ++i) DispatchPacket(writer, SpinCode.data(), {Low(SpunDescriptor.data()), High(SpunDescriptor.data()), SpinIterations});
    DispatchPacket(writer, StoreCode.data(), {Low(TargetDescriptor.data()), High(TargetDescriptor.data()), Low(Fresh.data())});
    WriteData(writer, &OtherDone, 1);
    Submit(WriterQueue, writer);

    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(5)
            && Driver::QueryPendingSpan(reinterpret_cast<std::uintptr_t>(Target.data()), sizeof(Target), {}) == ShaderMemory::PendingWrite::None) {
        std::this_thread::yield();
    }
    if (Driver::QueryPendingSpan(reinterpret_cast<std::uintptr_t>(Target.data()), sizeof(Target), {}) == ShaderMemory::PendingWrite::None) {
        AgcDriverWaitIdle_nid_postfix();
        throw std::runtime_error("dispatch fixture did not establish a pending descriptor range");
    }

    auto input = MakeInput(device, Target);
    Flushes = 0;
    GuestMemory::SetFlushHook(&CountFlush);
    try {
        const auto prepared = PrepareDispatchOwned(input);
        GuestMemory::SetFlushHook(nullptr);
        Require(!prepared.has_value(), "owned preparation accepted a descriptor range with a pending GPU writer");
        Require(Flushes == 0, "nonblocking owned capture flushed a pending GPU write");
    } catch (...) {
        GuestMemory::SetFlushHook(nullptr);
        throw;
    }
    AgcDriverWaitIdle_nid_postfix();
}

DispatchPrepareInput AfterSourceLifetime(std::shared_ptr<VulkanDevice> device) {
    Target = Descriptor(Stale.data(), sizeof(Stale));
    auto source = MakeInput(std::move(device), Target);
    DispatchPrepareInput moved(std::move(source));
    return moved;
}

void SourceLifetime(std::shared_ptr<VulkanDevice> device) {
    auto input = AfterSourceLifetime(std::move(device));
    auto prepared = PrepareDispatchOwned(input);
    Require(prepared.has_value(), "owned request stopped working after source queue/request storage was destroyed");
    Require(input.request.context.userData.data() == input.userData.data()
            && input.request.context.memory.data() == input.memory.data()
            && input.request.shader.code.data() == input.shader->code.data() + input.codeOffset,
        "moved owned input retained spans into source queue/request storage");
}

}

int main() {
    try {
        auto opened = OpenVulkanTestDevice();
        if (!opened) return VulkanTestSkipped;
        std::shared_ptr<VulkanDevice> device(std::move(opened));
        Target = Descriptor(Stale.data(), sizeof(Stale));
        CompareInlineAndOwned(device);
        SourceLifetime(device);
        PendingCaptureDoesNotFlush(device);
        AgcDriverShutdown_nid_postfix();
        std::cout << "owned dispatch preparation: passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { AgcDriverShutdown_nid_postfix(); } catch (const std::exception& shutdownError) { std::cerr << shutdownError.what() << '\n'; }
        return 1;
    }
}
