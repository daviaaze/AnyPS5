#include "prx/libc/include/GuestAllocations.hpp"
#include <array>
#include <atomic>
#include <barrier>
#include <cstdio>
#include <dlfcn.h>
#include <exception>
#include <thread>

namespace {

std::barrier rendezvous(3);
std::atomic<unsigned> waiting{0};

bool waitForRelease(std::uintptr_t, std::size_t) {
    waiting.fetch_add(1);
    waiting.notify_one();
    rendezvous.arrive_and_wait();
    return false;
}

}

int main() {
    Dl_info image{};
    if (dladdr(reinterpret_cast<const void*>(&waitForRelease), &image) == 0 || image.dli_fbase == nullptr) return 1;
    {
        GuestAllocations::Mutation mutation;
        mutation.RegisterImage(image.dli_fbase);
    }
    auto lease = GuestAllocations::GuestAllocationsAcquire_nid_postfix();
    if (lease.empty()) return 2;
    GuestAllocations::GuestAllocationsSetPinWaiter_nid_postfix(waitForRelease);
    std::array<std::exception_ptr, 2> failures{};
    const auto unregister = [&](unsigned index) {
        try {
            GuestAllocations::Mutation mutation;
            mutation.UnregisterImage(image.dli_fbase);
        } catch (...) {
            failures[index] = std::current_exception();
        }
    };
    std::jthread first(unregister, 0);
    std::jthread second(unregister, 1);
    for (unsigned count = waiting.load(); count < 2; count = waiting.load()) waiting.wait(count);
    lease.clear();
    rendezvous.arrive_and_wait();
    first.join();
    second.join();
    GuestAllocations::GuestAllocationsSetPinWaiter_nid_postfix(nullptr);
    for (const auto& failure : failures) if (failure) std::rethrow_exception(failure);
    if (!GuestAllocations::GuestAllocationsAcquireAll_nid_postfix().empty()) return 3;
    {
        GuestAllocations::Mutation mutation;
        mutation.RegisterImage(image.dli_fbase);
    }
    {
        const auto registered = GuestAllocations::GuestAllocationsLookup_nid_postfix(reinterpret_cast<std::uintptr_t>(&waitForRelease), false);
        if (!registered || registered->releasable) return 4;
    }
    {
        GuestAllocations::Mutation mutation;
        mutation.UnregisterImage(image.dli_fbase);
    }
    if (!GuestAllocations::GuestAllocationsAcquireAll_nid_postfix().empty()) return 5;
    std::puts("PASS: concurrent pinned-image unregister and subsequent image registration remain valid");
}
