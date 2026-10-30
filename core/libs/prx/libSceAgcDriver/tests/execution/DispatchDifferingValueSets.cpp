#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <string>
#include <span>
#include <thread>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::DriverDetail::Driver;

constexpr std::array<std::uint32_t, 7> Code{0xf4080100u, 0xfa000000u, 0xbf8cc07fu, 0x7e020280u, 0xe0702000u, 0x80010100u, 0xbf810000u};
constexpr std::size_t SrtOffset = 4096;
constexpr std::size_t OutputOffset = 8192;
constexpr std::size_t BlockBytes = 524288;

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, 65536));
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, BlockBytes, PAGE_READWRITE, BlockBytes);
#else
        void* mapped = mmap(nullptr, BlockBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            block = static_cast<std::uint8_t*>(mapped);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, BlockBytes);
        }
#endif
        Require(block != nullptr, "dispatch differing value sets: cannot allocate the guest block");
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint64_t At(std::size_t offset) const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block + offset)); }
    std::uint8_t* Bytes(std::size_t offset) const { return block + offset; }

private:
    std::uint8_t* block = nullptr;
};

std::vector<std::uint32_t> Pm4(std::uint32_t opcode, std::initializer_list<std::uint32_t> body) {
    std::vector<std::uint32_t> words{0xc0000000u | (static_cast<std::uint32_t>(body.size() - 1) << 16u) | (opcode << 8u)};
    words.insert(words.end(), body);
    return words;
}

void WriteSrt(const GuestBlock& block, std::uint32_t records) {
    const auto address = block.At(OutputOffset);
    const std::array<std::uint32_t, 4> words{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (4u << 16u), records, 0x00014204u};
    std::memcpy(block.Bytes(SrtOffset), words.data(), sizeof(words));
}

void Dispatch(const GuestBlock& block, bool footprint = false) {
    const auto kernel = block.At(footprint ? 256 : 0);
    const auto srt = block.At(SrtOffset);
    const auto label = block.At(12288);
    const std::array<std::uint32_t, 1> zero{0};
    AgcDriver::GuestMemory::Write(label, std::as_bytes(std::span(zero)));
    std::vector<std::uint32_t> words;
    for (const auto& packet : {Pm4(0x76, {0x20c, static_cast<std::uint32_t>(kernel >> 8u)}), Pm4(0x76, {0x20d, static_cast<std::uint32_t>(kernel >> 40u) & 0xffu}), Pm4(0x76, {0x207, 64}), Pm4(0x76, {0x208, 1}), Pm4(0x76, {0x209, 1}), Pm4(0x76, {0x213, 2u << 1u}), Pm4(0x76, {0x240, static_cast<std::uint32_t>(srt)}), Pm4(0x76, {0x241, static_cast<std::uint32_t>(srt >> 32u)}), Pm4(0x15, {1, 1, 1, 0x41})}) {
        words.insert(words.end(), packet.begin(), packet.end());
    }
    if (footprint) {
        // Eight 64-thread groups meet the normal address-staging threshold. The
        // atomic region therefore takes device-local staging even with host import.
        words[words.size() - 4] = 8;
    }
    const auto release = Pm4(0x49, {0x0030c514u, 1u << 29u, static_cast<std::uint32_t>(label), static_cast<std::uint32_t>(label >> 32u), 1, 0, 0});
    words.insert(words.end(), release.begin(), release.end());
    Packet dcb{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&dcb) == 0, "dispatch differing value sets: the dispatch submission failed");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::array<std::uint32_t, 1> completed{};
    do {
        AgcDriver::GuestMemory::Read(label, std::as_writable_bytes(std::span(completed)));
        if (completed[0] == 1) break;
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    Require(completed[0] == 1, "dispatch differing value sets: dispatch completion label timed out");
}

struct Totals {
    std::uint64_t classified = 0, matched = 0;
    std::uint64_t lastClassified = 0, lastMatched = 0;

    void Add() {
        const auto counters = Driver::Get().DispatchCacheCounters();
        classified += counters.differingClassified >= lastClassified ? counters.differingClassified - lastClassified : counters.differingClassified;
        matched += counters.differingMatchedPrior >= lastMatched ? counters.differingMatchedPrior - lastMatched : counters.differingMatchedPrior;
        lastClassified = counters.differingClassified;
        lastMatched = counters.differingMatchedPrior;
    }
};

void AlternatingValueSets(const GuestBlock& block) {
    Totals totals;
    totals.Add();
    for (std::uint32_t dispatch = 0; dispatch < 5; ++dispatch) {
        WriteSrt(block, dispatch % 2u == 0u ? 64u : 65u);
        Dispatch(block);
        totals.Add();
    }
    std::printf("alternating value sets: %llu differing dispatches classified, %llu matched one of the last 3 value sets\n", static_cast<unsigned long long>(totals.classified), static_cast<unsigned long long>(totals.matched));
    Require(totals.classified == 4, "the four dispatches after the first were not all classified as differing (" + std::to_string(totals.classified) + ")");
    Require(totals.matched == 3, "the three dispatches bringing a value set seen before did not all match it (" + std::to_string(totals.matched) + ")");
}

void CapturedIndexTransferFootprint(const GuestBlock& block) {
    constexpr std::uint32_t records = 15000;
    constexpr std::size_t indexOffset = 65536;
    constexpr std::size_t outputOffset = 131072;
    constexpr std::array<std::uint32_t, 23> code{
        0xf40c0100u, 0xfa000000u, 0xbf8cc07fu,
        0xf4000504u, 0xfa000000u, 0xbf8cc07fu,
        0x7e000214u, 0x7e020287u, 0xe0c86000u, 0x80010100u, 0xbf8c3f70u,
        0x7e040214u, 0x7e0602ffu, 0x55667788u, 0x7e0802ffu, 0x11223344u, 0x7e0a02ffu, 0xa1b2c3d4u,
        0xe0742004u, 0x80010302u, 0xe0702010u, 0x80010502u, 0xbf810000u};
    std::memcpy(block.Bytes(256), code.data(), sizeof(code));
    const auto output = block.At(outputOffset);
    const auto indexAddress = block.At(indexOffset);
    std::array<std::uint32_t, 8> root{
        static_cast<std::uint32_t>(output),
        static_cast<std::uint32_t>((output >> 32u) & 0xffffu) | (20u << 16u),
        records, 0x00005204u,
        static_cast<std::uint32_t>(indexAddress), static_cast<std::uint32_t>(indexAddress >> 32u), 0, 0};
    AgcDriver::GuestMemory::Write(block.At(SrtOffset), std::as_bytes(std::span(root)));
    std::vector<std::uint32_t> expected(records * 5);
    for (std::size_t word = 0; word < expected.size(); ++word) expected[word] = 0x10000000u + static_cast<std::uint32_t>(word);
    AgcDriver::GuestMemory::Write(output, std::as_bytes(std::span(expected)));
    std::vector<std::uint32_t> actual(expected.size());
    // Warm the unchanged variant, then keep the shader, root and descriptor fixed.
    // Only the independently captured scalar word changes; this was incorrectly
    // eligible for data-only refresh, retaining the first capture's 0:20 hull.
    constexpr std::array<std::uint32_t, 10> indices{0, 0, 0, 14999, 7, 0, 14999, 7, 0, 0};
    for (const auto index : indices) {
        const std::array<std::uint32_t, 1> indexWord{index};
        // The preceding release completed before this ordered guest write.
        AgcDriver::GuestMemory::Write(indexAddress, std::as_bytes(std::span(indexWord)));
        Dispatch(block, true);
        expected[index * 5] += 7u * 512u;
        expected[index * 5 + 1] = 0x55667788u;
        expected[index * 5 + 2] = 0x11223344u;
        expected[index * 5 + 4] = 0xa1b2c3d4u;
        AgcDriver::GuestMemory::Read(output, std::as_writable_bytes(std::span(actual)));
        for (std::size_t word = 0; word < expected.size(); ++word) {
            if (actual[word] != expected[word]) {
                Require(false,
                        "captured index transfer footprint: index=" + std::to_string(index) +
                        " record=" + std::to_string(word / 5) + " dword=" + std::to_string(word % 5) +
                        " actual=" + std::to_string(actual[word]) + " expected=" + std::to_string(expected[word]));
            }
        }
    }
    const std::array<std::uint32_t, 1> linearIndex{1u};
    AgcDriver::GuestMemory::Write(indexAddress, std::as_bytes(std::span(linearIndex)));
    Dispatch(block, true);
    expected[5] += 7u * 512u;
    expected[6] = 0x55667788u;
    expected[7] = 0x11223344u;
    expected[9] = 0xa1b2c3d4u;
    AgcDriver::GuestMemory::Read(output, std::as_writable_bytes(std::span(actual)));
    for (std::size_t word = 0; word < expected.size(); ++word) {
        if (actual[word] != expected[word]) {
            Require(false,
                    "captured index linear transfer footprint: index=1 record=" + std::to_string(word / 5) +
                    " dword=" + std::to_string(word % 5) +
                    " actual=" + std::to_string(actual[word]) + " expected=" + std::to_string(expected[word]));
        }
    }
    // Keep the ordinary linear descriptor as a control, then enable the
    // descriptor swizzle with indexStride zero. At index one, the shader's
    // atomic and stores address byte offsets 4, 36, 68 and 132 respectively;
    // linear planning instead bounds record one to bytes 20..40.
    root[1] |= 1u << 31u;
    AgcDriver::GuestMemory::Write(block.At(SrtOffset), std::as_bytes(std::span(root)));
    const std::array<std::uint32_t, 1> swizzledIndex{1u};
    AgcDriver::GuestMemory::Write(indexAddress, std::as_bytes(std::span(swizzledIndex)));
    Dispatch(block, true);
    expected[1] += 7u * 512u;
    expected[9] = 0x55667788u;
    expected[17] = 0x11223344u;
    expected[33] = 0xa1b2c3d4u;
    AgcDriver::GuestMemory::Read(output, std::as_writable_bytes(std::span(actual)));
    for (std::size_t word = 0; word < expected.size(); ++word) {
        if (actual[word] != expected[word]) {
            Require(false,
                    "captured index swizzled transfer footprint: index=1 record=" + std::to_string(word / 5) +
                    " dword=" + std::to_string(word % 5) +
                    " actual=" + std::to_string(actual[word]) + " expected=" + std::to_string(expected[word]));
        }
    }
}

}

int main() {
    try {
#ifdef _WIN32
        _putenv_s("APS5_PROFILE_DRAW", "1");
        _putenv_s("APS5_DISPATCH_VARIANTS", "1");
#else
        setenv("APS5_PROFILE_DRAW", "1", 1);
        setenv("APS5_DISPATCH_VARIANTS", "1", 1);
#endif
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        GuestBlock block;
        const std::array<std::uint32_t, 1> initialIndex{0};
        AgcDriver::GuestMemory::Write(block.At(65536), std::as_bytes(std::span(initialIndex)));
        std::memcpy(block.Bytes(0), Code.data(), sizeof(Code));
        AlternatingValueSets(block);
        CapturedIndexTransferFootprint(block);
        AgcDriverShutdown_nid_postfix();
        std::puts("dispatch differing value sets: the differing statistics recognize a value set they saw before");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { AgcDriverShutdown_nid_postfix(); } catch (...) {}
        return 1;
    }
}
