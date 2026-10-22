#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#ifdef __linux__
#include <dlfcn.h>

#endif
#include <sstream>
#include <thread>
#ifdef __linux__

extern "C" char** environ;
#endif

namespace {

using AgcDriver::PerformanceTrace;
using AgcDriver::TraceThread;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

void SetEnvironment(const char* key, const std::string& value) {
#ifdef _WIN32
    if (_putenv_s(key, value.c_str()) != 0) throw std::runtime_error("could not configure trace environment");
#else
    if (::setenv(key, value.c_str(), 1) != 0) throw std::runtime_error("could not configure trace environment");
#endif
}

void ClearEnvironment(const char* key) {
#ifdef _WIN32
    if (_putenv_s(key, "") != 0) throw std::runtime_error("could not clear trace environment");
#else
    if (::unsetenv(key) != 0) throw std::runtime_error("could not clear trace environment");
#endif
}
void Require(bool condition, const char* message);
#ifdef __linux__
bool IsDiagnosticEnvironmentKey(std::string_view name) {
    return name.starts_with("APS5_TRACE_") || name.starts_with("APS5_PROFILE_") ||
        name == "APS5_NO_LOCK_PROFILE" || name == "APS5_NO_HOLD_PROFILE" || name == "APS5_ASSERT_GPU_LOCK";
}

void ClearDiagnosticEnvironment() {
    for (;;) {
        std::string key;
        for (char** entry = ::environ; entry != nullptr && *entry != nullptr; ++entry) {
            const std::string_view assignment(*entry);
            const auto separator = assignment.find('=');
            if (separator == std::string_view::npos) continue;
            const auto name = assignment.substr(0, separator);
            if (IsDiagnosticEnvironmentKey(name)) {
                key.assign(name);
                break;
            }
        }
        if (key.empty()) return;
        ClearEnvironment(key.c_str());
    }
}

void DisabledSmoke() {
    using ProbeHook = void (*)();
    const auto probeBegin = reinterpret_cast<ProbeHook>(::dlsym(RTLD_DEFAULT, "aps5_probe_begin"));
    const auto probeEnd = reinterpret_cast<ProbeHook>(::dlsym(RTLD_DEFAULT, "aps5_probe_end"));
    if (probeBegin == nullptr || probeEnd == nullptr)
        throw std::runtime_error("--disabled-smoke requires the LD_PRELOAD probe exports aps5_probe_begin/end");
    ClearDiagnosticEnvironment();

    probeBegin();
    bool tryLockSucceeded = false;
    {
        auto& mutex = AgcDriver::GuestMemory::GpuMutex();
        mutex.lock();
        mutex.unlock();
        tryLockSucceeded = mutex.try_lock();
        if (tryLockSucceeded) mutex.unlock();

        AgcDriver::PerformanceTimer timer("disabled causal smoke");
        timer.Mark("disabled");
        const auto operation = PerformanceTrace::NextCausalOperation();
        PerformanceTrace::RecordCausal("disabled.smoke", 1, 1, 0, 0, nullptr, operation);
    }
    probeEnd();
    Require(tryLockSucceeded, "GpuMutex::try_lock failed in disabled smoke");
}
#endif


void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Gates {
    std::mutex mutex;
    std::condition_variable changed;
    bool ownerHeld = false;
    bool tryFailed = false;
    bool blockingCallStarted = false;
    bool releaseOwner = false;
};

bool HasOperation(const TraceThread::Snapshot& snapshot, const char* name, std::uint64_t operation) {
    for (const auto& event : snapshot.causal)
        if (std::string_view(event.name) == name && event.operation == operation) return true;
    return false;
}

void CaptureSmoke(const std::filesystem::path& directory) {
#ifndef __linux__
    throw std::runtime_error("--capture-smoke is Linux-only");
#else
    Require(std::filesystem::is_directory(directory), "capture-smoke run directory must already exist");
    const auto deadline = Clock::now() + 20s;
    const auto mono = [] { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); };
    const auto waitFile = [&](const char* name) {
        while (Clock::now() < deadline) {
            if (std::filesystem::exists(directory / name)) return true;
            std::this_thread::sleep_for(10ms);
        }
        return false;
    };
    ClearEnvironment("APS5_TRACE_SECONDS");
    ClearEnvironment("APS5_TRACE_SUMMARY_EVERY");
    ClearEnvironment("APS5_TRACE_BENCH");
    SetEnvironment("APS5_TRACE_FRAMES", "0:1");
    SetEnvironment("APS5_TRACE_CAUSAL", "1");
    SetEnvironment("APS5_TRACE_EVENTS", "65536");
    SetEnvironment("APS5_NO_LOCK_PROFILE", "1");
    PerformanceTrace::BindThread("causal smoke worker");
    const auto pid = static_cast<std::uint64_t>(::getpid());
    const auto tid = static_cast<std::uint64_t>(::syscall(SYS_gettid));
    std::uint64_t birth = 0;
    {
        std::ifstream stat("/proc/self/stat");
        std::string line;
        std::getline(stat, line);
        const auto close = line.rfind(')');
        Require(close != std::string::npos, "cannot parse /proc/self/stat");
        std::istringstream fields(line.substr(close + 2));
        std::string value;
        for (int field = 3; field <= 22; ++field) {
            Require(static_cast<bool>(fields >> value), "truncated /proc/self/stat");
            if (field == 22) birth = std::stoull(value);
        }
    }
    {
        std::ofstream ready(directory / "ready.json.tmp");
        Require(static_cast<bool>(ready), "cannot write ready.json");
        ready << "{\"pid\":" << pid << ",\"starttime_ticks\":" << birth << "}\n";
        ready.flush();
        Require(static_cast<bool>(ready), "cannot finish ready.json");
    }
    std::filesystem::rename(directory / "ready.json.tmp", directory / "ready.json");
    Require(waitFile("start"), "start control timed out");
    PerformanceTrace::Present(0, 0, nullptr);
    auto& gpu = AgcDriver::GuestMemory::GpuMutex();
    std::atomic<bool> held{false}, releaseOwner{false}, waitObserved{false};
    std::thread owner([&] {
        PerformanceTrace::BindThread("causal smoke mutex owner");
        gpu.lock();
        held.store(true, std::memory_order_release);
        while (!releaseOwner.load(std::memory_order_acquire)) std::this_thread::yield();
        gpu.unlock();
    });
    while (!held.load(std::memory_order_acquire)) {
        Require(Clock::now() < deadline, "owner lock timed out");
        std::this_thread::yield();
    }
    auto* worker = PerformanceTrace::Thread();
    const auto burn = [](std::chrono::milliseconds duration) {
        volatile std::uint64_t x = 1;
        const auto until = Clock::now() + duration;
        while (Clock::now() < until) x = x * 2862933555777941757ULL + 3037000493ULL;
        return static_cast<std::uint64_t>(x);
    };
    std::ofstream phases(directory / "phases.jsonl", std::ios::trunc);
    Require(static_cast<bool>(phases), "cannot write phases.jsonl");
    const auto emit = [&](const char* name, std::int64_t begin, std::int64_t end, std::uint64_t checksum) {
        phases << "{\"phase\":\"" << name << "\",\"pid\":" << pid << ",\"tid\":" << tid
               << ",\"starttime_ticks\":" << birth << ",\"begin_ns\":" << begin
               << ",\"end_ns\":" << end << ",\"checksum\":" << checksum << "}\n";
        phases.flush();
        Require(static_cast<bool>(phases), "phase write failed");
    };
    auto begin = mono();
    const auto before = burn(500ms);
    emit("cpu_before", begin, mono(), before);
    std::thread release([&] {
        const auto waitDeadline = Clock::now() + 5s;
        while (Clock::now() < waitDeadline) {
            for (const auto& event : worker->Collect().causal) {
                if (std::string_view(event.name) == "gpu_mutex.wait_begin") {
                    waitObserved.store(true, std::memory_order_release);
                    std::this_thread::sleep_for(600ms);
                    releaseOwner.store(true, std::memory_order_release);
                    return;
                }
            }
            std::this_thread::yield();
        }
        releaseOwner.store(true, std::memory_order_release);
    });
    begin = mono();
    gpu.lock();
    gpu.unlock();
    emit("gpu_mutex_wait", begin, mono(), 0);
    release.join();
    owner.join();
    Require(waitObserved.load(std::memory_order_acquire), "actual gpu_mutex.wait_begin was not observed");
    begin = mono();
    const auto after = burn(500ms);
    emit("cpu_after", begin, mono(), after);
    phases.close();
    PerformanceTrace::Present(0, 0, nullptr);
    PerformanceTrace::StopOutput();
    const auto finishedBegin = mono();
    {
        std::ofstream finished(directory / "finished.json.tmp");
        Require(static_cast<bool>(finished), "cannot write finished.json");
        finished << "{\"status\":\"completed\",\"begin_ns\":" << finishedBegin
                 << ",\"end_ns\":" << mono() << "}\n";
        finished.flush();
        Require(static_cast<bool>(finished), "cannot finish finished.json");
    }
    std::filesystem::rename(directory / "finished.json.tmp", directory / "finished.json");
    Require(waitFile("finish"), "finish control timed out");
#endif
}

void VerifyActualMutexCausality() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("aps5-gpu-mutex-causality-" + std::to_string(Clock::now().time_since_epoch().count()));
    ClearEnvironment("APS5_TRACE_SECONDS");
    ClearEnvironment("APS5_TRACE_SUMMARY_EVERY");
    ClearEnvironment("APS5_TRACE_BENCH");
    SetEnvironment("APS5_TRACE_FRAMES", "0:1");
    SetEnvironment("APS5_TRACE_CAUSAL", "1");
    SetEnvironment("APS5_TRACE_EVENTS", "32");
    SetEnvironment("APS5_TRACE_DIR", directory.string());
    SetEnvironment("APS5_NO_LOCK_PROFILE", "1");
    PerformanceTrace::Present(0, 0, nullptr);

    auto& gpu = AgcDriver::GuestMemory::GpuMutex();
    Gates gates;
    std::atomic<TraceThread*> ownerTrace{nullptr};
    std::atomic<TraceThread*> waiterTrace{nullptr};

    std::thread owner([&] {
        PerformanceTrace::BindThread("causal mutex owner");
        ownerTrace.store(PerformanceTrace::Thread(), std::memory_order_release);
        gpu.lock();
        gpu.lock();
        gpu.unlock();
        {
            std::lock_guard lock(gates.mutex);
            gates.ownerHeld = true;
        }
        gates.changed.notify_all();
        {
            std::unique_lock lock(gates.mutex);
            gates.changed.wait(lock, [&] { return gates.releaseOwner; });
        }
        gpu.unlock();
    });

    std::thread waiter([&] {
        PerformanceTrace::BindThread("causal mutex waiter");
        waiterTrace.store(PerformanceTrace::Thread(), std::memory_order_release);
        {
            std::unique_lock lock(gates.mutex);
            gates.changed.wait(lock, [&] { return gates.ownerHeld; });
        }
        const bool failed = !gpu.try_lock();
        if (!failed) gpu.unlock();
        {
            std::lock_guard lock(gates.mutex);
            gates.tryFailed = failed;
            gates.blockingCallStarted = true;
        }
        gates.changed.notify_all();
        if (!failed) return;
        gpu.lock();
        gpu.unlock();
    });

    bool observedBlockingAttempt = false;
    bool tryFailedObserved = false;
    {
        std::unique_lock lock(gates.mutex);
        const bool ready = gates.changed.wait_for(lock, 5s, [&] { return gates.blockingCallStarted; });
        tryFailedObserved = ready && gates.tryFailed;
    }
    const auto deadline = Clock::now() + 5s;
    auto* waiterBound = waiterTrace.load(std::memory_order_acquire);
    while (tryFailedObserved && waiterBound != nullptr && Clock::now() < deadline) {
        const auto snapshot = waiterBound->Collect();
        std::uint64_t attempts = 0;
        for (const auto& event : snapshot.causal)
            if (std::string_view(event.name) == "gpu_mutex.wait_begin") {
                observedBlockingAttempt = true;
                break;
            }
        if (observedBlockingAttempt) break;
        std::this_thread::yield();
    }
    {
        std::lock_guard lock(gates.mutex);
        gates.releaseOwner = true;
    }
    gates.changed.notify_all();
    owner.join();
    waiter.join();

    PerformanceTrace::Present(0, 0, nullptr);
    PerformanceTrace::StopOutput();
    auto* ownerBound = ownerTrace.load(std::memory_order_acquire);
    waiterBound = waiterTrace.load(std::memory_order_acquire);
    Require(tryFailedObserved, "the actual GPU mutex try_lock did not fail behind the held owner");
    Require(observedBlockingAttempt, "outer release was not gated on a confirmed wait_begin event");
    Require(ownerBound != nullptr && waiterBound != nullptr, "mutex workers did not bind trace threads");
    const auto ownerSnapshot = ownerBound->Collect();
    const auto waiterSnapshot = waiterBound->Collect();

    std::uint64_t ownerGeneration = 0;
    std::size_t ownerAcquisitions = 0;
    std::size_t ownerReleases = 0;
    bool depthOne = false;
    bool depthTwo = false;
    Clock::time_point releaseBegin{};
    Clock::time_point releaseEnd{};
    for (const auto& event : ownerSnapshot.causal) {
        if (std::string_view(event.name) == "gpu_mutex.acquired") {
            ++ownerAcquisitions;
            if (ownerGeneration == 0) ownerGeneration = event.generation;
            Require(event.generation == ownerGeneration, "recursive acquisition changed outer hold generation");
            depthOne = depthOne || event.recursion == 1;
            depthTwo = depthTwo || event.recursion == 2;
        }
        if (std::string_view(event.name) == "gpu_mutex.released") {
            ++ownerReleases;
            Require(event.generation == ownerGeneration, "outer release did not name the recursive hold generation");
            releaseBegin = event.begin;
            releaseEnd = event.begin + event.length;
        }
    }
    Require(ownerAcquisitions == 2 && ownerGeneration != 0 && depthOne && depthTwo,
        "actual recursive acquisitions and their depths were not recorded");
    Require(ownerReleases == 1, "the recursive inner unlock emitted an outer release");

    std::uint64_t waitOperation = 0;
    bool failedTrySeen = false;
    bool waitEdgeSeen = false;
    bool acquiredAfterWaitSeen = false;
    Clock::time_point waitBegin{};
    Clock::time_point waitEnd{};
    for (const auto& event : waiterSnapshot.causal) {
        const std::string_view name = event.name;
        if (name == "gpu_mutex.try_failed") failedTrySeen = true;
        if (name == "gpu_mutex.attempt" && event.operation != 0 && !HasOperation(waiterSnapshot, "gpu_mutex.try_failed", event.operation))
            waitOperation = event.operation;
        if (name == "gpu_mutex.wait" && event.operation == waitOperation && event.related == ownerGeneration && event.length > Clock::duration::zero()) {
            waitEdgeSeen = true;
            waitBegin = event.begin;
            waitEnd = event.begin + event.length;
        }
        if (name == "gpu_mutex.acquired" && event.operation == waitOperation && event.related == ownerGeneration)
            acquiredAfterWaitSeen = true;
    }
    Require(failedTrySeen, "the actual failed try_lock event was missing");
    Require(waitOperation != 0 && waitEdgeSeen && acquiredAfterWaitSeen,
        "the contended lock wait and acquisition did not join to the owner's actual release generation");
    Require(waitBegin <= releaseEnd && waitEnd >= releaseBegin,
        "the observed lock release was not bracketed by the actual wait span");
    std::filesystem::remove_all(directory);
}

}

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--capture-smoke") {
            CaptureSmoke(argv[2]);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--disabled-smoke") {
#ifdef __linux__
            DisabledSmoke();
            return 0;
#else
            throw std::runtime_error("--disabled-smoke is available only on Linux");
#endif
        }
        Require(argc == 1, "usage: agc_gpu_mutex_causality_tests [--capture-smoke RUN_DIR|--disabled-smoke]");
        VerifyActualMutexCausality();
        std::cout << "GPU mutex causality test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
