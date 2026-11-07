// A dispatch resolved ahead on a compute queue is adopted at its place in the stream only while the
// guest bytes its capture read are unchanged. With the descriptors in write-watched guest memory the
// adoption proves that through write tracking (ShaderMemory::ReadsUnchanged) instead of comparing
// bytes; every store between the capture and the adoption must still reject it. The same checks
// over unwatched memory (byte comparison) are DispatchAheadAdoptCheck.
#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

alignas(256) std::array<std::uint32_t, 7> CounterCode{0xf4080200u, 0xfa000000u, 0xbf8cc07fu, 0x7e000281u, 0xe0c80000u, 0x80020000u, 0xbf810000u};
alignas(256) std::array<std::uint32_t, 7> StoreCode{0xf4080200u, 0xfa000000u, 0xbf8cc07fu, 0x7e000202u, 0xe0700000u, 0x80020000u, 0xbf810000u};
alignas(256) std::array<std::uint32_t, 15> SpinCode{0xf4080200u, 0xfa000000u, 0xbe840381u, 0x9304ff04u, 0x0019660du, 0x8004ff04u, 0x3c6ef35fu, 0x80828102u, 0xbf078002u, 0xbf85fff9u, 0xbf8cc07fu, 0x7e000204u, 0xe0700000u, 0x80020000u, 0xbf810000u};

constexpr std::uint32_t Compute = 0x20u;
constexpr std::uint32_t OtherCompute = 0x28u;
constexpr std::uint32_t SpinIterations = 1u << 20u;
constexpr std::size_t Block = 65536;

// Every buffer the dispatches read or write, in one write-watched guest block.
struct Buffers {
    alignas(256) std::array<std::uint32_t, 4> stale{};
    alignas(256) std::array<std::uint32_t, 4> fresh{};
    alignas(256) std::array<std::uint32_t, 4> target{};
    alignas(256) std::array<std::uint32_t, 4> targetDescriptor{};
    alignas(256) std::array<std::uint32_t, 4> spun{};
    alignas(256) std::array<std::uint32_t, 4> spunDescriptor{};
    alignas(256) volatile std::uint32_t gate = 1;
    alignas(256) volatile std::uint32_t done = 0;
    alignas(256) volatile std::uint32_t otherDone = 0;
};
static_assert(sizeof(Buffers) <= Block);

Buffers* B = nullptr;

enum class Window { None, CpuStore, CpuStoreSameBytes, MarkedWritten, QueuedGpuStore };

std::atomic<Window> Armed{Window::None};
std::uint32_t SpinDispatches = 16;
std::vector<std::uint32_t> OtherWords;
bool SawPending = false;

void* AllocateWatched(std::size_t bytes) {
#ifdef _WIN32
    void* block = GuestArena::GuestArenaAllocate_nid_postfix(bytes, Block);
    Require(block != nullptr, "cannot reserve the watched guest block");
    GuestArena::GuestArenaCommit_nid_postfix(block, bytes, PAGE_READWRITE, bytes);
#else
    void* raw = mmap(nullptr, bytes + Block, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Require(raw != MAP_FAILED, "cannot map the watched guest block");
    const auto begin = reinterpret_cast<std::uintptr_t>(raw);
    const auto aligned = (begin + Block - 1) & ~static_cast<std::uintptr_t>(Block - 1);
    if (aligned != begin) munmap(raw, aligned - begin);
    if (aligned + bytes != begin + bytes + Block) munmap(reinterpret_cast<void*>(aligned + bytes), begin + Block - aligned);
    void* block = reinterpret_cast<void*>(aligned);
    GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, bytes);
#endif
    return block;
}

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

void Program(std::vector<std::uint32_t>& words, const void* code, std::initializer_list<std::uint32_t> userData) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(code));
    SetShaderRegister(words, 0x20c, static_cast<std::uint32_t>(address >> 8u));
    SetShaderRegister(words, 0x20d, static_cast<std::uint32_t>(address >> 40u) & 0xffu);
    for (const std::uint32_t offset : {0x207u, 0x208u, 0x209u}) SetShaderRegister(words, offset, 1);
    SetShaderRegister(words, 0x212, 0);
    SetShaderRegister(words, 0x213, static_cast<std::uint32_t>(userData.size()) << 1u);
    std::uint32_t offset = 0x240;
    for (const auto value : userData) SetShaderRegister(words, offset++, value);
}

void Dispatch(std::vector<std::uint32_t>& words, const void* code, std::initializer_list<std::uint32_t> userData) {
    Program(words, code, userData);
    words.insert(words.end(), {0xc0031500u, 1, 1, 1, 0x8041u});
}

void WriteData(std::vector<std::uint32_t>& words, const volatile std::uint32_t* target, std::uint32_t value) {
    words.insert(words.end(), {0xc0033700u, 0x00100200u, Low(target), High(target), value});
}

void Submit(std::uint32_t queue, std::vector<std::uint32_t>& words) {
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require(sceAgcDriverSubmitAcb(queue, &packet) == 0, "compute submit failed");
}

void QueueStoreAhead() {
    OtherWords.clear();
    for (std::uint32_t index = 0; index < SpinDispatches; ++index) Dispatch(OtherWords, SpinCode.data(), {Low(B->spunDescriptor.data()), High(B->spunDescriptor.data()), SpinIterations});
    Dispatch(OtherWords, StoreCode.data(), {Low(B->targetDescriptor.data()), High(B->targetDescriptor.data()), Low(B->fresh.data())});
    WriteData(OtherWords, &B->otherDone, 1);
    Submit(OtherCompute, OtherWords);
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(5) && B->otherDone == 0) {
        if (AgcDriver::DriverDetail::Driver::QueryPendingSpan(reinterpret_cast<std::uintptr_t>(B->target.data()), sizeof(B->target), {}) != AgcDriver::ShaderMemory::PendingWrite::None) {
            SawPending = true;
            return;
        }
        std::this_thread::yield();
    }
}

void BeforeAdoption() {
    auto& target = B->target;
    switch (Armed.exchange(Window::None)) {
        case Window::CpuStore: reinterpret_cast<volatile std::uint32_t&>(target[0]) = Low(B->fresh.data()); break;
        case Window::CpuStoreSameBytes: reinterpret_cast<volatile std::uint32_t&>(target[0]) = Low(B->stale.data()); break;
        case Window::MarkedWritten: AgcDriver::GuestMemory::MarkWritten(reinterpret_cast<std::uintptr_t>(target.data()), sizeof(target)); break;
        case Window::QueuedGpuStore: QueueStoreAhead(); break;
        case Window::None: break;
    }
}

void WaitIdle() {
    AgcDriverWaitIdle_nid_postfix();
    for (const auto* buffer : {&B->stale, &B->fresh, &B->target}) AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(buffer->data()), sizeof(*buffer));
}

void Reset() {
    WaitIdle();
    B->stale = {};
    B->fresh = {};
    B->target = Descriptor(B->stale.data(), sizeof(B->stale));
    B->done = 0;
    B->otherDone = 0;
    SawPending = false;
}

// One submission: wait on the gate (so the counter dispatch is resolved ahead), then count into
// whichever buffer the descriptor at `target` names when the dispatch is adopted.
void Gated(Window window) {
    Reset();
    std::vector<std::uint32_t> words;
    words.insert(words.end(), {0xc0053c00u, 0x13u, Low(&B->gate), High(&B->gate), 1u, 0xffffffffu, 0x19u});
    Dispatch(words, CounterCode.data(), {Low(B->target.data()), High(B->target.data())});
    WriteData(words, &B->done, 1);
    Armed = window;
    Submit(Compute, words);
    WaitIdle();
    Armed = Window::None;
    Require(B->done == 1, "the gated submission did not finish");
}

std::string Counts() {
    return "(stale " + std::to_string(B->stale[0]) + ", fresh " + std::to_string(B->fresh[0]) + ")";
}

}

int main() {
    try {
        if (!AgcDriver::GuestMemory::WriteWatched()) {
            std::cout << "no write watching: skipped\n";
            return VulkanTestSkipped;
        }
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        device.reset();
        B = new (AllocateWatched(Block)) Buffers{};
        Require(AgcDriver::GuestMemory::Watched(reinterpret_cast<std::uintptr_t>(B), Block), "the buffers are not write watched");
        Require(High(B->stale.data()) == High(B->fresh.data()), "the stale and fresh buffers differ in their high address word");
        B->targetDescriptor = Descriptor(B->target.data(), sizeof(B->target));
        B->spunDescriptor = Descriptor(B->spun.data(), sizeof(B->spun));
        AgcDriverSetAheadAdoptHook_nid_postfix(&BeforeAdoption);
        Reset();
        std::vector<std::uint32_t> warmUp;
        Dispatch(warmUp, CounterCode.data(), {Low(B->target.data()), High(B->target.data())});
        WriteData(warmUp, &B->done, 1);
        Submit(Compute, warmUp);
        std::vector<std::uint32_t> otherWarmUp;
        Dispatch(otherWarmUp, SpinCode.data(), {Low(B->spunDescriptor.data()), High(B->spunDescriptor.data()), SpinIterations});
        Dispatch(otherWarmUp, StoreCode.data(), {Low(B->targetDescriptor.data()), High(B->targetDescriptor.data()), Low(B->fresh.data())});
        Submit(OtherCompute, otherWarmUp);
        WaitIdle();
        Require(B->done == 1 && B->target[0] == Low(B->fresh.data()), "the warm-up dispatches did not run");

        const auto provenBefore = AgcDriver::DriverDetail::Driver::AheadAdoptionsProven();
        Gated(Window::None);
        Require(B->stale[0] == 1 && B->fresh[0] == 0, "the gated dispatch without a store in the window did not count into the stale buffer " + Counts());
        Require(AgcDriver::DriverDetail::Driver::AheadAdoptionsProven() > provenBefore, "the adoption over unchanged watched memory was not proven by write tracking");
        std::cout << "unchanged watched capture adopted by generation: passed\n";

        Gated(Window::CpuStore);
        Require(B->fresh[0] == 1 && B->stale[0] == 0, "a CPU store of a new descriptor before the adoption was not seen " + Counts());
        std::cout << "cpu store of other bytes before adoption: passed\n";

        Gated(Window::CpuStoreSameBytes);
        Require(B->stale[0] == 1 && B->fresh[0] == 0, "a CPU store of the same descriptor bytes changed the dispatch " + Counts());
        std::cout << "cpu store of the same bytes before adoption: passed\n";

        Gated(Window::MarkedWritten);
        Require(B->stale[0] == 1 && B->fresh[0] == 0, "a driver-marked write over unchanged bytes changed the dispatch " + Counts());
        std::cout << "marked write before adoption: passed\n";

        bool conclusive = false;
        for (int attempt = 0; attempt < 4 && !conclusive; ++attempt, SpinDispatches *= 4) {
            Gated(Window::QueuedGpuStore);
            conclusive = SawPending;
            if (conclusive) Require(B->fresh[0] == 1 && B->stale[0] == 0, "a GPU store queued on another queue before the adoption was not seen " + Counts());
        }
        AgcDriverSetAheadAdoptHook_nid_postfix(nullptr);
        if (!conclusive) std::cout << "queued gpu store before adoption: never observed pending, not checked\n";
        else std::cout << "queued gpu store before adoption: passed\n";
        std::cout << "generation validation test passed\n";
        AgcDriverShutdown_nid_postfix();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try {
            AgcDriverShutdown_nid_postfix();
        } catch (const std::exception& shutdownError) {
            std::cerr << shutdownError.what() << '\n';
        }
        return 1;
    }
}
