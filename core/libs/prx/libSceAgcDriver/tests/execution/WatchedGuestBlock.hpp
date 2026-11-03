#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_TESTS_WATCHEDGUESTBLOCK_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_TESTS_WATCHEDGUESTBLOCK_HPP

#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

class WatchedGuestBlock {
public:
    static constexpr std::size_t Bytes = 65536;

    WatchedGuestBlock() {
        AgcDriver::Graphics::Require(AgcDriver::GuestMemory::WriteWatched(), "the recipe target requires guest write watch");
#ifdef _WIN32
        data = static_cast<std::byte*>(GuestArena::GuestArenaAllocate_nid_postfix(Bytes, Bytes));
        if (data != nullptr) GuestArena::GuestArenaCommit_nid_postfix(data, Bytes, PAGE_READWRITE, Bytes);
#else
        void* mapped = mmap(nullptr, Bytes * 2u, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        AgcDriver::Graphics::Require(mapped != MAP_FAILED, "cannot reserve watched guest color target memory");
        const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
        const auto aligned = (begin + Bytes - 1u) & ~(static_cast<std::uintptr_t>(Bytes) - 1u);
        if (aligned != begin) munmap(mapped, aligned - begin);
        if (aligned + Bytes != begin + Bytes * 2u) munmap(reinterpret_cast<void*>(aligned + Bytes), begin + Bytes * 2u - aligned - Bytes);
        data = reinterpret_cast<std::byte*>(aligned);
#endif
        AgcDriver::Graphics::Require(data != nullptr, "cannot allocate watched guest color target memory");
        std::memset(data, 0, Bytes);
#ifndef _WIN32
        GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(data, Bytes);
#endif
        GuestAllocations::Mutation().Add(data, Bytes, true, true, true);
        AgcDriver::Graphics::Require((reinterpret_cast<std::uintptr_t>(data) & (Bytes - 1u)) == 0 &&
                    AgcDriver::GuestMemory::Watched(reinterpret_cast<std::uintptr_t>(data), Bytes),
                "the resident recipe target is not a watched, aligned guest allocation");
    }

    ~WatchedGuestBlock() {
        GuestAllocations::Mutation().Remove(data);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(data, Bytes);
        GuestArena::GuestArenaRelease_nid_postfix(data, Bytes);
#else
        munmap(data, Bytes);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(data, Bytes);
#endif
    }

    WatchedGuestBlock(const WatchedGuestBlock&) = delete;
    WatchedGuestBlock& operator=(const WatchedGuestBlock&) = delete;

    std::byte* Data() const { return data; }

private:
    std::byte* data = nullptr;
};

#endif
