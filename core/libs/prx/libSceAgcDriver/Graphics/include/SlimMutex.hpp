#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SLIMMUTEX_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SLIMMUTEX_HPP

#include <atomic>
#include <thread>
#include "prx/libc/include/PreciseWait.hpp"

#include "prx/libSceAgcDriver/Execution/include/TableWaits.hpp"

namespace AgcDriver::Graphics {

class SlimMutex {
public:
    SlimMutex() = default;
    SlimMutex(const SlimMutex&) = delete;
    SlimMutex& operator=(const SlimMutex&) = delete;

    void lock() {
        if (!locked.exchange(true, std::memory_order_acquire)) return;
        TableWaits::Timer wait(TableWaits::SlimMutexContended);
        for (unsigned spins = 0;;) {
            while (locked.load(std::memory_order_relaxed)) {
                if (spins < 128) {
                    ++spins;
                    PreciseSpinPause();
                } else {
                    std::this_thread::yield();
                }
            }
            if (!locked.exchange(true, std::memory_order_acquire)) return;
        }
    }
    bool try_lock() { return !locked.load(std::memory_order_relaxed) && !locked.exchange(true, std::memory_order_acquire); }
    void unlock() { locked.store(false, std::memory_order_release); }

private:
    std::atomic<bool> locked{false};
};

}

#endif
