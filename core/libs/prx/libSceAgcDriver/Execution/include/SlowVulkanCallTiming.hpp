#pragma once

#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

namespace AgcDriver::SlowVulkanCallTiming {

inline constexpr std::uint64_t NanosecondsPerSecond = 1'000'000'000;
inline constexpr std::uint64_t SlowThresholdNs = 5'000'000;
inline constexpr std::uint64_t StallThresholdNs = 100'000'000;
inline constexpr std::uint64_t RateWindowNs = NanosecondsPerSecond;
inline constexpr std::uint64_t UnsetWindow = std::numeric_limits<std::uint64_t>::max();

struct Sample {
    std::uint64_t wallNs = 0;
    std::uint64_t threadCpuNs = 0;
    bool wallAvailable = false;
    bool threadCpuAvailable = false;
};

inline bool Enabled() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_TRACE_SLOW_VULKAN_CALLS");
        return value != nullptr && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

#if !defined(_WIN32) && (defined(CLOCK_MONOTONIC) || defined(CLOCK_THREAD_CPUTIME_ID))
inline bool ReadClock(clockid_t clock, std::uint64_t& result) noexcept {
    timespec value{};
    if (::clock_gettime(clock, &value) != 0 || value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1'000'000'000L) return false;
    const auto seconds = static_cast<std::uint64_t>(value.tv_sec);
    const auto nanoseconds = static_cast<std::uint64_t>(value.tv_nsec);
    if (seconds > (std::numeric_limits<std::uint64_t>::max() - nanoseconds) / NanosecondsPerSecond) return false;
    result = seconds * NanosecondsPerSecond + nanoseconds;
    return true;
}
#endif

inline bool ReadWall(std::uint64_t& result) noexcept {
#if !defined(_WIN32) && defined(CLOCK_MONOTONIC)
    return ReadClock(CLOCK_MONOTONIC, result);
#else
    const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
    const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    if (nanoseconds < 0) return false;
    result = static_cast<std::uint64_t>(nanoseconds);
    return true;
#endif
}

inline bool ReadThreadCpu(std::uint64_t& result) noexcept {
#if defined(_WIN32)
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!::GetThreadTimes(::GetCurrentThread(), &creation, &exit, &kernel, &user)) return false;
    ULARGE_INTEGER kernelTicks{}, userTicks{};
    kernelTicks.LowPart = kernel.dwLowDateTime;
    kernelTicks.HighPart = kernel.dwHighDateTime;
    userTicks.LowPart = user.dwLowDateTime;
    userTicks.HighPart = user.dwHighDateTime;
    if (kernelTicks.QuadPart > std::numeric_limits<std::uint64_t>::max() - userTicks.QuadPart) return false;
    const auto ticks = kernelTicks.QuadPart + userTicks.QuadPart;
    if (ticks > std::numeric_limits<std::uint64_t>::max() / 100) return false;
    result = ticks * 100;
    return true;
#elif defined(CLOCK_THREAD_CPUTIME_ID)
    return ReadClock(CLOCK_THREAD_CPUTIME_ID, result);
#else
    static_cast<void>(result);
    return false;
#endif
}

inline Sample Begin() noexcept {
    Sample sample{};
    sample.threadCpuAvailable = ReadThreadCpu(sample.threadCpuNs);
    sample.wallAvailable = ReadWall(sample.wallNs);
    return sample;
}

inline Sample End() noexcept {
    Sample sample{};
    sample.wallAvailable = ReadWall(sample.wallNs);
    sample.threadCpuAvailable = ReadThreadCpu(sample.threadCpuNs);
    return sample;
}

inline bool ReadProcessId(std::uint64_t& result) noexcept {
#if defined(_WIN32)
    result = static_cast<std::uint64_t>(::GetCurrentProcessId());
    return result != 0;
#else
    const auto process = ::getpid();
    if (process <= 0) return false;
    result = static_cast<std::uint64_t>(process);
    return true;
#endif
}

inline bool ReadNativeThreadId(std::uint64_t& result) noexcept {
#if defined(_WIN32)
    result = static_cast<std::uint64_t>(::GetCurrentThreadId());
    return result != 0;
#elif defined(__linux__) && defined(SYS_gettid)
    const auto thread = ::syscall(SYS_gettid);
    if (thread <= 0) return false;
    result = static_cast<std::uint64_t>(thread);
    return true;
#else
    static_cast<void>(result);
    return false;
#endif
}

inline void FormatNumber(bool available, std::uint64_t value, char* text, std::size_t capacity) noexcept {
    if (!available) {
        std::snprintf(text, capacity, "%s", "unavailable");
        return;
    }
    std::snprintf(text, capacity, "%llu", static_cast<unsigned long long>(value));
}

inline void AtomicMax(std::atomic<std::uint64_t>& maximum, std::uint64_t value) noexcept {
    auto observed = maximum.load(std::memory_order_relaxed);
    while (observed < value && !maximum.compare_exchange_weak(observed, value, std::memory_order_relaxed)) {}
}

inline bool ReserveWindow(std::atomic<std::uint64_t>& lastWindow, std::uint64_t window) noexcept {
    auto observed = lastWindow.load(std::memory_order_relaxed);
    while (observed == UnsetWindow || window > observed) {
        if (lastWindow.compare_exchange_weak(observed, window, std::memory_order_relaxed)) return true;
    }
    return false;
}

inline std::atomic<std::uint64_t> lastSlowWindow{UnsetWindow};
inline std::atomic<std::uint64_t> lastStallWindow{UnsetWindow};
inline std::atomic<std::uint64_t> suppressedSlowCalls{0};
inline std::atomic<std::uint64_t> suppressedSlowMaxNs{0};
inline std::atomic<std::uint64_t> suppressedStallCalls{0};
inline std::atomic<std::uint64_t> suppressedStallMaxNs{0};

inline bool EmitIfSlow(const char* api, const Sample& begin, const Sample& end, std::uintptr_t device, std::uintptr_t queue, const char* identityName, std::uint64_t identity, const char* detailName, std::uint64_t detail, std::int32_t result, std::uint64_t inFlight, const char* inFlightBasis) {
    if (!begin.wallAvailable || !end.wallAvailable || end.wallNs < begin.wallNs) return false;
    const auto wallDelta = end.wallNs - begin.wallNs;
    if (wallDelta < SlowThresholdNs) return false;
    const bool stall = wallDelta >= StallThresholdNs;
    auto& lastWindow = stall ? lastStallWindow : lastSlowWindow;
    auto& suppressedCalls = stall ? suppressedStallCalls : suppressedSlowCalls;
    auto& suppressedMax = stall ? suppressedStallMaxNs : suppressedSlowMaxNs;
    const auto window = end.wallNs / RateWindowNs;
    if (!ReserveWindow(lastWindow, window)) {
        suppressedCalls.fetch_add(1, std::memory_order_relaxed);
        AtomicMax(suppressedMax, wallDelta);
        return false;
    }
    std::uint64_t processId = 0;
    std::uint64_t nativeThreadId = 0;
    const bool processAvailable = ReadProcessId(processId);
    const bool nativeThreadAvailable = ReadNativeThreadId(nativeThreadId);
    const bool cpuAvailable = begin.threadCpuAvailable && end.threadCpuAvailable && end.threadCpuNs >= begin.threadCpuNs;
    const auto cpuDelta = cpuAvailable ? end.threadCpuNs - begin.threadCpuNs : 0;
    char processText[32], threadText[32], wallStartText[32], wallEndText[32], wallDeltaText[32], cpuStartText[32], cpuEndText[32], cpuDeltaText[32];
    FormatNumber(processAvailable, processId, processText, sizeof(processText));
    FormatNumber(nativeThreadAvailable, nativeThreadId, threadText, sizeof(threadText));
    FormatNumber(begin.wallAvailable, begin.wallNs, wallStartText, sizeof(wallStartText));
    FormatNumber(end.wallAvailable, end.wallNs, wallEndText, sizeof(wallEndText));
    FormatNumber(true, wallDelta, wallDeltaText, sizeof(wallDeltaText));
    FormatNumber(cpuAvailable, begin.threadCpuNs, cpuStartText, sizeof(cpuStartText));
    FormatNumber(cpuAvailable, end.threadCpuNs, cpuEndText, sizeof(cpuEndText));
    FormatNumber(cpuAvailable, cpuDelta, cpuDeltaText, sizeof(cpuDeltaText));
    constexpr const char* threadCpuClock =
#if defined(_WIN32)
        "GetThreadTimes";
#elif defined(CLOCK_THREAD_CPUTIME_ID)
        "CLOCK_THREAD_CPUTIME_ID";
#else
        "unavailable";
#endif
    AgcDriver::ProfilePrint_nid_no_patch("[slow-vulkan] class=%s api=%s pid=%s native_tid=%s device=0x%llx queue=0x%llx %s=%llu %s=%llu result=%d in_flight_count=%llu in_flight_basis=%s timing_scope=vk_entrypoint_call wall_clock=monotonic wall_start_ns=%s wall_end_ns=%s wall_delta_ns=%s thread_cpu_clock=%s thread_cpu_bracket=cpu_start_before_wall_start,cpu_end_after_wall_end thread_cpu_start_ns=%s thread_cpu_end_ns=%s thread_cpu_delta_ns=%s rate_window_ns=%llu suppressed_slow_calls_total=%llu suppressed_slow_max_wall_ns=%llu suppressed_stall_calls_total=%llu suppressed_stall_max_wall_ns=%llu\n",
                                      stall ? "stall" : "slow", api, processText, threadText, static_cast<unsigned long long>(device), static_cast<unsigned long long>(queue), identityName, static_cast<unsigned long long>(identity), detailName, static_cast<unsigned long long>(detail), static_cast<int>(result), static_cast<unsigned long long>(inFlight), inFlightBasis, wallStartText, wallEndText, wallDeltaText, threadCpuClock, cpuStartText, cpuEndText, cpuDeltaText, static_cast<unsigned long long>(RateWindowNs), static_cast<unsigned long long>(suppressedSlowCalls.load(std::memory_order_relaxed)), static_cast<unsigned long long>(suppressedSlowMaxNs.load(std::memory_order_relaxed)), static_cast<unsigned long long>(suppressedStallCalls.load(std::memory_order_relaxed)), static_cast<unsigned long long>(suppressedStallMaxNs.load(std::memory_order_relaxed)));
    return true;
}

}
