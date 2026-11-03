#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include <atomic>
#include <cstdio>
#include <thread>

int main() {
    if (setenv("APS5_P0_METRICS", "1", 1) != 0) return 2;
    std::atomic<bool> start{false};
    std::thread profiler([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (unsigned i = 0; i < 200000; ++i)
            std::fprintf(stderr, "[capture] concurrent profiler record %u complete\n", i);
    });
    start.store(true, std::memory_order_release);
    for (unsigned i = 0; i < 64; ++i) AgcDriver::P0Metrics::Report();
    profiler.join();
    return 0;
}
