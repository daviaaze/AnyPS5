#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchTiming.hpp"

namespace AgcDriver::DriverDetail {

void Driver::addDriverPhases(DispatchClass which, const PendingDispatchPhases& observation) {
    std::lock_guard lock(driverPhasesMutex);
    auto& totals = driverPhaseTotals[which];
    if (observation.phases) {
        for (std::size_t i = 0; i < DriverPhaseCount; ++i) totals.ms[i] += observation.ms[i];
        ++totals.dispatches;
        if (observation.hit) ++totals.hits;
        if (observation.validated) ++totals.validations;
    }
    ++totals.packets;
    const auto outcome = static_cast<std::size_t>(observation.outcome);
    ++totals.outcomes[outcome];
    totals.outcomeWallNs[outcome] += observation.endNs - observation.startNs;
    if (observation.zeroDirect) ++totals.zeroDirect;
    if (observation.cpuValid) {
        ++totals.cpuValid;
        totals.cpuNs += observation.cpuNs;
    }
    totals.wallNs += observation.endNs - observation.startNs;
    if (totals.minStartNs == 0 || observation.startNs < totals.minStartNs) totals.minStartNs = observation.startNs;
    totals.maxEndNs = std::max(totals.maxEndNs, observation.endNs);
    const auto now = std::chrono::steady_clock::now();
    if (now - driverPhasesReport < std::chrono::seconds(10)) return;
    driverPhasesReport = now;
    for (std::size_t cls = 0; cls < DispatchClassCount; ++cls) {
        auto& line = driverPhaseTotals[cls];
        if (line.packets == 0) continue;
        std::string report;
        double total = 0;
        for (std::size_t i = 0; i < DriverPhaseCount; ++i) {
            char text[64];
            std::snprintf(text, sizeof(text), " %s %.1f", DriverPhaseNames[i], line.ms[i] * 1000 / static_cast<double>(line.dispatches == 0 ? 1 : line.dispatches));
            report += text;
            total += line.ms[i];
        }
        const double perValidation = line.validations != 0 ? 1000 / static_cast<double>(line.validations) : 0.0;
        std::string outcomesReport;
        for (std::size_t i = 0; i < line.outcomes.size(); ++i) {
            char text[64];
            std::snprintf(text, sizeof(text), " %s=%llu/%.1fms", DispatchOutcomeNames[i], static_cast<unsigned long long>(line.outcomes[i]), line.outcomeWallNs[i] / 1000000.0);
            outcomesReport += text;
        }
        AgcDriver::ProfilePrint_nid_no_patch("[dispatch] driver phases %s (10 s, %llu completed packets, %llu phased dispatches, %llu cache hits), us per dispatch:%s total %.1f (%.1f ms); validate %.1f us per validation (%llu validations; GPU waits inside it %.1f us per validation apart); zero-direct %llu, CPU-valid %llu, CPU %.1f us/valid packet, wall %.1f us/packet, bounds %llu..%llu mono-ns; outcomes:%s\n", DispatchClassNames[cls], static_cast<unsigned long long>(line.packets), static_cast<unsigned long long>(line.dispatches), static_cast<unsigned long long>(line.hits), report.c_str(), line.dispatches ? total * 1000 / line.dispatches : 0.0, total, line.validations ? line.ms[PhaseValidate] * perValidation : 0.0, static_cast<unsigned long long>(line.validations), line.validations ? line.ms[PhaseValidateWait] * perValidation : 0.0, static_cast<unsigned long long>(line.zeroDirect), static_cast<unsigned long long>(line.cpuValid), line.cpuValid ? static_cast<double>(line.cpuNs) / line.cpuValid / 1000.0 : 0.0, static_cast<double>(line.wallNs) / line.packets / 1000.0, static_cast<unsigned long long>(line.minStartNs), static_cast<unsigned long long>(line.maxEndNs), outcomesReport.c_str());
        line = {};
    }
}

PendingDispatchPhases& Driver::pendingDispatchPhases() {
    static thread_local PendingDispatchPhases pending;
    return pending;
}

std::chrono::steady_clock::time_point& Driver::packetStartedAt() {
    static thread_local std::chrono::steady_clock::time_point started{};
    return started;
}

double DispatchPhaseTiming::Elapsed() {
    const auto now = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration<double, std::milli>(now - lap).count();
    lap = now;
    return ms;
}

void DispatchPhaseTiming::Phase(DriverPhase which) {
    if (!profile) return;
    const auto now = std::chrono::steady_clock::now();
    phaseMs[which] += std::chrono::duration<double, std::milli>(now - phaseLap).count();
    phaseLap = now;
}

}
