#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#if defined(libSceAgcDriver_EXPORTS)
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#endif
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string_view>
#if defined(__linux__)
#include <time.h>
#endif

namespace AgcDriver {

namespace {
struct P0OperationRow { std::atomic<std::uint64_t> count{0}, elapsed{0}, cpu{0}, cpuCount{0}, cpuUnavailable{0}; };
struct P0ReadRow { std::atomic<std::uint64_t> count{0}, bytes{0}; };
struct P0State {
    P0OperationRow operations[257][3][2]{};
    P0ReadRow reads[17][5]{};
    P0ReadRow reasonReads[static_cast<unsigned>(P0Metrics::ReadReason::Count)][5]{};
    std::atomic<std::uint64_t> lookup[static_cast<unsigned>(P0Metrics::Lookup::Count)]{};
    std::mutex reportMutex;
    std::uint64_t snapshotId = 0; // Owned by reportMutex, not the operation hot path.
    std::atomic<std::int64_t> lastReportNs{0};
};
P0State& p0State() { static P0State state; return state; }
thread_local unsigned p0Depth[3]{};
thread_local unsigned p0ReadReason = static_cast<unsigned>(P0Metrics::ReadReason::OutsideLookup);
}

bool P0Metrics::Enabled() {
    static const bool enabled = [] {
        const auto* value = std::getenv("APS5_P0_METRICS");
        return value != nullptr && std::string_view(value) == "1";
    }();
    return enabled;
}

bool P0Metrics::CpuNow(std::uint64_t& ns) {
#if defined(__linux__)
    timespec time{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0) return false;
    ns = static_cast<std::uint64_t>(time.tv_sec) * 1000000000ull + time.tv_nsec;
    return true;
#else
    return false;
#endif
}

bool P0Metrics::Begin(unsigned layer) { return p0Depth[layer]++ == 0; }
P0Metrics::ReadContext::ReadContext(ReadReason reason) : previous(static_cast<ReadReason>(p0ReadReason)), enabled(P0Metrics::Enabled()) {
    if (enabled) p0ReadReason = static_cast<unsigned>(reason);
}
P0Metrics::ReadContext::~ReadContext() {
    if (enabled) p0ReadReason = static_cast<unsigned>(previous);
}
void P0Metrics::ReadContext::Set(ReadReason reason) {
    if (enabled) p0ReadReason = static_cast<unsigned>(reason);
}
void P0Metrics::LookupObserved(Lookup outcome) {
    if (!Enabled()) return;
    const auto index = static_cast<unsigned>(outcome);
    if (index < static_cast<unsigned>(Lookup::Count)) p0State().lookup[index].fetch_add(1, std::memory_order_relaxed);
}
void P0Metrics::ReadRequested(unsigned site, unsigned api, std::size_t bytes) {
    if (!Enabled() || api >= 5) return;
    auto& state = p0State();
    auto& row = state.reads[site < static_cast<unsigned>(GuestMemory::ReadSite::Count) ? site : 16][api];
    row.count.fetch_add(1, std::memory_order_relaxed);
    row.bytes.fetch_add(bytes, std::memory_order_relaxed);
    auto& reason = state.reasonReads[p0ReadReason < static_cast<unsigned>(ReadReason::Count) ? p0ReadReason : static_cast<unsigned>(ReadReason::LookupUnclassified)][api];
    reason.count.fetch_add(1, std::memory_order_relaxed);
    reason.bytes.fetch_add(bytes, std::memory_order_relaxed);
}
void P0Metrics::End(std::uint32_t queue, unsigned layer, unsigned operation, bool active,
        std::uint64_t cpuStart, bool cpuAvailable, std::chrono::steady_clock::time_point start) {
    auto& state = p0State();
    --p0Depth[layer];
    if (!active) return;
    const auto end = std::chrono::steady_clock::now();
    std::uint64_t cpuEnd = 0;
    const bool cpuValid = CpuNow(cpuEnd) && cpuAvailable && cpuEnd >= cpuStart;
    auto& row = state.operations[queue < 256 ? queue : 256][layer][operation];
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    row.count.fetch_add(1, std::memory_order_relaxed);
    row.elapsed.fetch_add(static_cast<std::uint64_t>(elapsed), std::memory_order_relaxed);
    if (cpuValid) {
        row.cpu.fetch_add(cpuEnd - cpuStart, std::memory_order_relaxed);
        row.cpuCount.fetch_add(1, std::memory_order_relaxed);
    } else {
        row.cpuUnavailable.fetch_add(1, std::memory_order_relaxed);
    }
    const auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(end.time_since_epoch()).count();
    auto prior = state.lastReportNs.load(std::memory_order_relaxed);
    if (nowNs - prior >= 10000000000ll && state.lastReportNs.compare_exchange_strong(prior, nowNs, std::memory_order_relaxed)) Report();
}

void P0Metrics::Report() {
    if (!Enabled()) return;
    auto& state = p0State();
    std::lock_guard lock(state.reportMutex);
    const auto monotonicNow = [](std::uint64_t& ns) {
#if defined(__linux__)
        timespec time{};
        if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) return false;
        ns = static_cast<std::uint64_t>(time.tv_sec) * 1000000000ull + time.tv_nsec;
        return true;
#else
        return false;
#endif
    };
    std::uint64_t timestamp = 0;
    if (!monotonicNow(timestamp)) return;
    const auto snapshotId = ++state.snapshotId;
    std::uint64_t rowCount = 0;
    std::fprintf(stderr, "{\"type\":\"aps5_p0_snapshot\",\"version\":1,\"phase\":\"begin\",\"snapshot_id\":%llu,\"monotonic_ns\":%llu}\n",
        static_cast<unsigned long long>(snapshotId), static_cast<unsigned long long>(timestamp));
    // Every finite dimension is visited below. Only a complete begin/end pair
    // establishes omitted rows as zero; relaxed loads remain non-transactional.
    static constexpr const char* layers[] = {"packet", "committer", "finisher"};
    static constexpr const char* operations[] = {"draw", "dispatch"};
    static constexpr const char* apis[] = {"Read", "ReadCommitted", "ReadCommittedWithKnownZeros", "ReadCommittedUnsynced", "CompareCommitted"};
    static constexpr const char* readReasons[] = {"outside_lookup", "lookup_unclassified", "resident_validation_unknown", "resident_stale_unknown", "cache_absent_origin_unknown", "pending_gpu_overlap_observed", "cache_disabled"};
    static constexpr const char* lookupOutcomes[] = {"unclassified", "snapshot_hit", "view_hit", "resident_stale", "cache_absent_origin_unknown", "cache_disabled"};
    for (unsigned q = 0; q < 257; ++q) for (unsigned l = 0; l < 3; ++l) for (unsigned o = 0; o < 2; ++o) {
        const auto& row = state.operations[q][l][o];
        const auto count = row.count.load(std::memory_order_relaxed);
        if (count == 0) continue;
        const auto cpuCount = row.cpuCount.load(std::memory_order_relaxed);
        std::fprintf(stderr, "{\"type\":\"aps5_p0_operation\",\"version\":1,\"cumulative\":true,\"snapshot\":\"non_transactional_during_run\",\"scope\":\"attempt_including_early_returns\",\"queue\":%d,\"layer\":\"%s\",\"operation\":\"%s\",\"count\":%llu,\"elapsed_ns\":%llu,\"thread_cpu_ns\":%llu,\"cpu_sample_count\":%llu,\"cpu_unavailable_count\":%llu,\"gpu_cause\":\"unknown\"}\n",
            q == 256 ? -1 : static_cast<int>(q), layers[l], operations[o],
            static_cast<unsigned long long>(count), static_cast<unsigned long long>(row.elapsed.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(row.cpu.load(std::memory_order_relaxed)), static_cast<unsigned long long>(cpuCount),
            static_cast<unsigned long long>(row.cpuUnavailable.load(std::memory_order_relaxed)));
        ++rowCount;
    }
    for (unsigned site = 0; site < 17; ++site) for (unsigned api = 0; api < 5; ++api) {
        const auto& row = state.reads[site][api];
        const auto count = row.count.load(std::memory_order_relaxed);
        if (count == 0) continue;
        std::fprintf(stderr, "{\"type\":\"aps5_p0_guest_read\",\"version\":1,\"cumulative\":true,\"read_site\":\"%s\",\"api\":\"%s\",\"count\":%llu,\"requested_bytes\":%llu,\"accessible_bytes\":null,\"cause\":\"read_site_only\"}\n",
            site < static_cast<unsigned>(GuestMemory::ReadSite::Count) ? GuestMemory::ReadSiteName(static_cast<GuestMemory::ReadSite>(site)) : "invalid",
            apis[api], static_cast<unsigned long long>(count), static_cast<unsigned long long>(row.bytes.load(std::memory_order_relaxed)));
        ++rowCount;
    }
    for (unsigned reason = 0; reason < static_cast<unsigned>(ReadReason::Count); ++reason) for (unsigned api = 0; api < 5; ++api) {
        const auto& row = state.reasonReads[reason][api];
        const auto count = row.count.load(std::memory_order_relaxed);
        if (count == 0) continue;
        std::fprintf(stderr, "{\"type\":\"aps5_p0_guest_read_reason\",\"version\":1,\"cumulative\":true,\"reason\":\"%s\",\"api\":\"%s\",\"count\":%llu,\"requested_bytes\":%llu,\"writer_cause\":\"unknown\",\"pending_gpu_overlap\":\"%s\"}\n",
            readReasons[reason], apis[api], static_cast<unsigned long long>(count), static_cast<unsigned long long>(row.bytes.load(std::memory_order_relaxed)),
            reason == static_cast<unsigned>(ReadReason::PendingGpuOverlapObserved) ? "observed_only_not_proof" : "not_observed");
        ++rowCount;
    }
    for (unsigned outcome = 0; outcome < static_cast<unsigned>(Lookup::Count); ++outcome) {
        const auto count = state.lookup[outcome].load(std::memory_order_relaxed);
        std::fprintf(stderr, "{\"type\":\"aps5_p0_texture_lookup\",\"version\":1,\"cumulative\":true,\"outcome\":\"%s\",\"count\":%llu}\n",
            lookupOutcomes[outcome], static_cast<unsigned long long>(count));
        ++rowCount;
    }
#if defined(libSceAgcDriver_EXPORTS)
    Graphics::ReportP0TableMetrics();
    ++rowCount; // Enabled() was already checked; the reporter emits one typed row.
#endif
    // A clock failure leaves an incomplete snapshot rather than inventing a time.
    if (monotonicNow(timestamp)) {
        std::fprintf(stderr, "{\"type\":\"aps5_p0_snapshot\",\"version\":1,\"phase\":\"end\",\"snapshot_id\":%llu,\"monotonic_ns\":%llu,\"row_count\":%llu}\n",
            static_cast<unsigned long long>(snapshotId), static_cast<unsigned long long>(timestamp),
            static_cast<unsigned long long>(rowCount));
        std::fflush(stderr);
    }
}

}
