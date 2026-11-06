#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DISPATCHTIMING_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DISPATCHTIMING_HPP

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace AgcDriver::DriverDetail {

enum DriverPhase { PhasePrologue, PhaseKey, PhaseLookup, PhaseValidate, PhaseValidateWait, PhaseRelock, PhaseCapture, PhaseRecompile, PhaseInsert, PhaseQueuedLabels, PhaseSnapshots, PhasePrepareKey, PhasePrepareFind, PhasePreparePrecollect, PhasePreparePresync, PhasePrepareStageA, PhasePrepareOther, PhaseRecipePrecheck, PhaseLockWait, PhaseLabels, PhaseNoteWriters, PhaseDevice, PhaseTail, PhaseEpilogue, DriverPhaseCount };

enum DispatchClass : std::size_t { Queue0Direct, Queue0Indirect, OtherQueues, DispatchClassCount };

enum class DispatchOutcome : std::size_t { Skipped = 0, Real, CopyHle, FillHle, SkippedMemo, Enqueued, Count };

inline constexpr const char* DriverPhaseNames[DriverPhaseCount] = {"prologue", "key", "lookup", "validate", "validate GPU wait", "relock", "capture", "recompile", "insert", "queued-label check", "snapshots", "prepare: key", "prepare: find", "prepare: precollect", "prepare: presync", "prepare: stage A (miss)", "prepare: other", "recipe pre-check", "lock wait", "labels", "note writers", "device call", "tail", "epilogue"};

inline constexpr const char* DispatchClassNames[DispatchClassCount] = {"queue 0 direct", "queue 0 indirect", "other queues"};

inline constexpr const char* DispatchOutcomeNames[static_cast<std::size_t>(DispatchOutcome::Count)] = {"skipped", "real", "copy HLE", "fill HLE", "skipped (memo)", "enqueued"};

struct DriverPhaseTotals {
    std::array<double, DriverPhaseCount> ms{};
    std::uint64_t dispatches = 0, hits = 0, validations = 0;
    std::uint64_t packets = 0, zeroDirect = 0, cpuValid = 0, cpuNs = 0, wallNs = 0;
    std::uint64_t minStartNs = 0, maxEndNs = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(DispatchOutcome::Count)> outcomes{};
    std::array<std::uint64_t, static_cast<std::size_t>(DispatchOutcome::Count)> outcomeWallNs{};
};

struct PendingDispatchPhases {
    DispatchOutcome outcome = DispatchOutcome::Skipped;
    bool phases = false;
    bool hit = false;
    bool validated = false;
    std::array<double, DriverPhaseCount> ms{};
    std::chrono::steady_clock::time_point tailAt{};
    bool zeroDirect = false, cpuValid = false;
    std::uint64_t startNs = 0, endNs = 0, cpuNs = 0;
};

struct DispatchPhaseTiming {
    bool profile;
    std::chrono::steady_clock::time_point& lap;
    std::array<double, DriverPhaseCount>& phaseMs;
    std::chrono::steady_clock::time_point& phaseLap;
    double Elapsed();
    void Phase(DriverPhase which);
};

}

#endif
