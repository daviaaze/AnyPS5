#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_TABLEWAITS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_TABLEWAITS_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

// APS5_PROFILE_DRAW: wall time the thread resolving sampled image tables
// (ShaderResources::resolveImageTables) spends in blocking primitives, by site, printed as
// [table-waits] next to [table-element-time]. ShaderResources::resolveImageTables opens a Scope
// (only when APS5_PROFILE_DRAW is set); a Timer outside a Scope reads no clock. Sites are inclusive
// where they nest (a pending flush that syncs the recorder is charged to both); a site's mutex
// timers suppress the generic contended-mutex site beneath them.
namespace AgcDriver::TableWaits {

enum Site : std::size_t {
    MutexContended,      // AgcDriver::Mutex::lockContended not attributed to a named site below
    SlimMutexContended,  // Graphics::SlimMutex::lock after its first exchange failed
    GpuMutexLock,        // GuestMemory::GpuMutexType::lock (every acquisition, wait included)
    UncachedMutex,
    FailuresMutex,
    StorageBudgetMutex,
    SampledCacheMutex,
    SurfacesMutex,
    StorageCacheMutex,
    StorageCachedQueryMutex,
    TableTexturesMutex,
    CompletePendingSync, // CompletePendingCpuWrites -> Recorder::SyncThrough
    StorageFlushPending, // StorageTexture::FlushPending
    CollectWrites,       // GuestMemory::CollectWrites / CollectCommittedWrites
    FlushDepthPlanes,
    TextureFlush,        // Texture::Flush
    RecorderTimelineWait,// Recorder::SyncThrough unlocked timeline wait
    RecorderFenceWait,   // Recorder fence wait (vkWaitForFences)
    BatchWait,           // CommandBatch::Wait (SubmitAndWait)
    Count
};

inline constexpr std::array<const char*, Count> Names{
    "mutex-contended", "slim-mutex", "gpu-mutex", "uncached-mutex", "failures-mutex", "storage-budget-mutex",
    "sampled-cache-mutex", "surfaces-mutex", "storage-cache-mutex", "storage-cached-query-mutex", "table-textures-mutex",
    "complete-pending-sync", "storage-flush-pending", "collect-writes", "flush-depth-planes", "texture-flush",
    "recorder-timeline-wait", "recorder-fence-wait", "batch-wait"};

struct Totals {
    std::array<std::atomic<std::uint64_t>, Count> ns{};
    std::array<std::atomic<std::uint64_t>, Count> count{};
    std::atomic<std::uint64_t> scopeNs{0};
    std::atomic<std::uint64_t> scopes{0};
};

inline Totals& Get() {
    static Totals totals;
    return totals;
}

inline thread_local bool inside = false;

inline bool Enabled() {
    static const bool enabled = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return enabled;
}

inline void Add(Site site, std::chrono::steady_clock::time_point begin) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
    Get().ns[site].fetch_add(static_cast<std::uint64_t>(ns), std::memory_order_relaxed);
    Get().count[site].fetch_add(1, std::memory_order_relaxed);
}

// Times its lifetime against `site` when the thread is inside a Scope. `suppress` hides nested
// sites for the lifetime (a named mutex over the generic contended-mutex site).
class Timer {
public:
    explicit Timer(Site site, bool suppress = false) noexcept : site(site), on(inside), suppress(suppress && on) {
        if (on) begin = std::chrono::steady_clock::now();
        if (this->suppress) inside = false;
    }
    ~Timer() {
        if (suppress) inside = true;
        if (on) Add(site, begin);
    }
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

private:
    Site site;
    bool on;
    bool suppress;
    std::chrono::steady_clock::time_point begin{};
};

// Runs `call` timed against `site`.
template <typename TCall>
decltype(auto) Timed(Site site, TCall&& call) {
    Timer timer(site);
    return call();
}

// Locks `mutex` (to be adopted by the caller's guard), timing only a contended acquisition.
template <typename TMutex>
TMutex& Acquire(Site site, TMutex& mutex) {
    if (!inside || mutex.try_lock()) {
        if (!inside) mutex.lock();
        return mutex;
    }
    Timer timer(site, true);
    mutex.lock();
    return mutex;
}

// The resolveImageTables scope: sets the thread flag and accumulates the scope's wall time.
class Scope {
public:
    Scope() noexcept : on(Enabled() && !inside) {
        if (!on) return;
        inside = true;
        begin = std::chrono::steady_clock::now();
    }
    ~Scope() {
        if (!on) return;
        inside = false;
        Get().scopeNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count()), std::memory_order_relaxed);
        Get().scopes.fetch_add(1, std::memory_order_relaxed);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    bool on;
    std::chrono::steady_clock::time_point begin{};
};

}

#endif
