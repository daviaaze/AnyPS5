#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

alignas(256) std::array<std::uint32_t, 15> SpinCode{0xf4080200u, 0xfa000000u, 0xbe840381u, 0x9304ff04u, 0x0019660du, 0x8004ff04u, 0x3c6ef35fu, 0x80828102u, 0xbf078002u, 0xbf85fff9u, 0xbf8cc07fu, 0x7e000204u, 0xe0700000u, 0x80020000u, 0xbf810000u};

constexpr std::uint32_t SpinIterations = 1u << 20u;
constexpr std::size_t BlockBytes = 65536;
constexpr std::uint32_t Waits = 16;

struct Slot {
    alignas(256) std::array<std::uint32_t, 4> spinOutput{};
    alignas(256) std::array<std::uint32_t, 4> spinDescriptor{};
    alignas(256) volatile std::uint32_t label = 0;
    alignas(8) volatile std::uint64_t wideLabel = 0;
    alignas(256) volatile std::uint32_t done = 0;
    alignas(256) volatile std::uint32_t gate = 0;
};

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
        Require(block != nullptr, "same-queue label wait: cannot allocate the guest block");
        std::memset(block, 0, BlockBytes);
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
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

    Slot& Data() { return *reinterpret_cast<Slot*>(block); }

private:
    std::uint8_t* block = nullptr;
};

std::uint32_t Low(const volatile void* pointer) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

std::uint32_t High(const volatile void* pointer) {
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer)) >> 32u);
}

void SetShaderRegister(std::vector<std::uint32_t>& words, std::uint32_t offset, std::uint32_t value) {
    words.insert(words.end(), {0xc0017600u, offset, value});
}

void Dispatch(std::vector<std::uint32_t>& words, const void* code, std::initializer_list<std::uint32_t> userData) {
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

void ReleaseLabel(std::vector<std::uint32_t>& words, volatile std::uint32_t* target, std::uint32_t value) {
    words.insert(words.end(), {0xc0064900u, 0x514u, (1u << 29u) | (2u << 24u), Low(target), High(target), value, 0u, 0u});
}

void WriteData(std::vector<std::uint32_t>& words, volatile std::uint32_t* target, std::uint32_t value) {
    words.insert(words.end(), {0xc0033700u, 0x00100200u, Low(target), High(target), value});
}

void WaitEqual(std::vector<std::uint32_t>& words, volatile std::uint32_t* target, std::uint32_t value) {
    words.insert(words.end(), {0xc0053c00u, 0x13u, Low(target), High(target), value, 0xffffffffu, 0x19u});
}

void WaitEqual64(std::vector<std::uint32_t>& words, volatile std::uint64_t* target, std::uint64_t value) {
    const auto address = reinterpret_cast<std::uintptr_t>(target);
    words.insert(words.end(), {0xc0079300u, 0x13u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u),
        static_cast<std::uint32_t>(value), static_cast<std::uint32_t>(value >> 32u), 0xffffffffu, 0xffffffffu, 0x19u});
}

std::uint64_t Run(Slot& slot, std::uint32_t value, bool wait) {
    slot.spinDescriptor = {Low(slot.spinOutput.data()), High(slot.spinOutput.data()) & 0xffffu, sizeof(slot.spinOutput), 0x31016facu};
    slot.done = 0;
    std::vector<std::uint32_t> words;
    Dispatch(words, SpinCode.data(), {Low(slot.spinDescriptor.data()), High(slot.spinDescriptor.data()), SpinIterations});
    for (std::uint32_t index = 0; index < Waits; ++index) {
        ReleaseLabel(words, &slot.label, value + index);
        if (wait) WaitEqual(words, &slot.label, value + index);
    }
    WriteData(words, &slot.done, 1);
    const auto before = AgcDriver::GuestMemory::CollectEpochBumps();
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require(sceAgcDriverSubmitAcb(0x20, &packet) == 0, "compute submit failed");
    const auto start = std::chrono::steady_clock::now();
    while (slot.done == 0) {
        Require(std::chrono::steady_clock::now() - start < std::chrono::seconds(10), "the submission did not finish");
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    AgcDriverWaitIdle_nid_postfix();
    Require(slot.label == value + Waits - 1, "the last label did not reach memory");
    return AgcDriver::GuestMemory::CollectEpochBumps() - before;
}

std::uint64_t RunLanded(Slot& slot, std::uint32_t value, bool wait) {
    slot.done = 0;
    slot.gate = 0;
    std::vector<std::uint32_t> words;
    for (std::uint32_t index = 0; index < Waits; ++index) {
        ReleaseLabel(words, &slot.label, value + index);
        WaitEqual(words, &slot.gate, index + 1);
        if (wait) WaitEqual(words, &slot.label, value + index);
    }
    WriteData(words, &slot.done, 1);
    const auto before = AgcDriver::GuestMemory::CollectEpochBumps();
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require(sceAgcDriverSubmitAcb(0x20, &packet) == 0, "gated compute submit failed");
    const auto start = std::chrono::steady_clock::now();
    for (std::uint32_t index = 0; index < Waits; ++index) {
        while (slot.label != value + index) {
            Require(std::chrono::steady_clock::now() - start < std::chrono::seconds(10), "label " + std::to_string(index) + " of the gated submission did not reach memory");
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        slot.gate = index + 1;
    }
    while (slot.done == 0) {
        Require(std::chrono::steady_clock::now() - start < std::chrono::seconds(10), "the gated submission did not finish");
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    AgcDriverWaitIdle_nid_postfix();
    return AgcDriver::GuestMemory::CollectEpochBumps() - before;
}

void SubmitWords(std::vector<std::uint32_t>& words, std::uint32_t queue) {
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require((queue == 0 ? sceAgcDriverSubmitDcb(&packet) : sceAgcDriverSubmitAcb(queue, &packet)) == 0, "wait-edge fixture submit failed");
}

void WaitFor(volatile std::uint32_t& value, std::uint32_t expected, const char* message) {
    const auto start = std::chrono::steady_clock::now();
    while (value != expected) {
        Require(std::chrono::steady_clock::now() - start < std::chrono::seconds(10), message);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

void RunWaitEdgeTrace(Slot& slot, const AgcDriver::VulkanDevice* traceDevice) {
    constexpr std::uint32_t expected = 0x12345678u;
    slot.spinDescriptor = {Low(slot.spinOutput.data()), High(slot.spinOutput.data()) & 0xffffu, sizeof(slot.spinOutput), 0x31016facu};
    slot.label = 0;
    slot.done = 0;
    slot.gate = 0;
    AgcDriverWaitIdle_nid_postfix();
    AgcDriver::PerformanceTrace::Present(1, 1, traceDevice);

    std::vector<std::uint32_t> waitWords;
    WriteData(waitWords, &slot.done, 1);
    WaitEqual(waitWords, &slot.label, expected);
    WriteData(waitWords, &slot.done, 2);
    SubmitWords(waitWords, 0);
    WaitFor(slot.done, 1, "queue 0 did not reach the pre-wait marker");

    std::vector<std::uint32_t> wrongWords;
    WriteData(wrongWords, &slot.gate, expected);
    WriteData(wrongWords, &slot.label, expected - 1);
    WriteData(wrongWords, &slot.gate, 0x87654321u);
    WriteData(wrongWords, &slot.gate, 1);
    SubmitWords(wrongWords, 0x20);
    WaitFor(slot.gate, 1, "the wrong-value writer did not complete");
    Require(slot.done == 1, "a wrong-address or wrong-value write satisfied the wait");

    std::vector<std::uint32_t> matchingWords;
    Dispatch(matchingWords, SpinCode.data(), {Low(slot.spinDescriptor.data()), High(slot.spinDescriptor.data()), SpinIterations});
    ReleaseLabel(matchingWords, &slot.label, expected);
    SubmitWords(matchingWords, 0x20);
    WaitFor(slot.done, 2, "the matching writer did not release the blocked wait");
    AgcDriverWaitIdle_nid_postfix();
    Require(slot.label == expected, "matching writer value was not stored");
    constexpr std::uint64_t wideExpected = 0x1122334455667788ull;
    slot.wideLabel = wideExpected;
    slot.done = 0;
    std::vector<std::uint32_t> entrySatisfiedWords;
    WaitEqual64(entrySatisfiedWords, &slot.wideLabel, wideExpected);
    WriteData(entrySatisfiedWords, &slot.done, 3);
    SubmitWords(entrySatisfiedWords, 0);
    WaitFor(slot.done, 3, "the already-satisfied 64-bit wait did not complete");
    AgcDriverWaitIdle_nid_postfix();
    Require(slot.wideLabel == wideExpected, "the 64-bit entry value changed");
    AgcDriver::PerformanceTrace::Present(1, 1, traceDevice);
    AgcDriver::PerformanceTrace::StopOutput();
    std::cout << "wait-edge blocked transition passed: wrong-address/value held; matching queue 0x20 store released queue 0x0 wait; expected 0x"
              << std::hex << expected << std::dec << '\n';
}

}

int main(int argc, char** argv) {
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        GuestBlock block;
        auto& slot = block.Data();
        if (argc == 2 && std::string_view(argv[1]) == "wait-edge-trace") {
            RunWaitEdgeTrace(slot, device.get());
            AgcDriverShutdown_nid_postfix();
            return 0;
        }
        device.reset();
        Run(slot, 1, false);
        std::uint64_t extra = ~std::uint64_t{0};
        for (std::uint32_t attempt = 0; attempt < 3; ++attempt) {
            const auto without = Run(slot, 100 + attempt * 2 * Waits, false);
            const auto with = Run(slot, 100 + (attempt * 2 + 1) * Waits, true);
            std::cout << "attempt " << attempt << ": " << with << " bumps with " << Waits << " waits, " << without << " without\n";
            extra = std::min(extra, with > without ? with - without : 0);
        }
        Require(extra < Waits / 2, "WAIT_REG_MEMs satisfied by labels their own queue released earlier in the submission started new collect epochs: " + std::to_string(extra) + " extra bumps for " + std::to_string(Waits) + " waits");
        std::uint64_t landedExtra = ~std::uint64_t{0};
        for (std::uint32_t attempt = 0; attempt < 3; ++attempt) {
            const auto without = RunLanded(slot, 1000 + attempt * 2 * Waits, false);
            const auto with = RunLanded(slot, 1000 + (attempt * 2 + 1) * Waits, true);
            std::cout << "landed attempt " << attempt << ": " << with << " bumps with " << Waits << " waits, " << without << " without\n";
            landedExtra = std::min(landedExtra, with > without ? with - without : 0);
        }
        Require(landedExtra < Waits / 2, "WAIT_REG_MEMs that found the labels their own queue released earlier in the submission already in memory started new collect epochs: " + std::to_string(landedExtra) + " extra bumps for " + std::to_string(Waits) + " waits");
        std::cout << "same-queue label wait test passed\n";
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
