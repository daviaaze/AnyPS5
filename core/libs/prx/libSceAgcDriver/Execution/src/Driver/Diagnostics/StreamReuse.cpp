#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics/StreamReuse.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace AgcDriver {
namespace {

struct SubmissionStats {
    std::uint64_t hash = 14695981039346656037ull;
    std::uint32_t commands = 0;
    std::uint32_t translated = 0;
    double hostMs = 0.0;

    std::uint32_t Reused() const { return commands - std::min(commands, translated); }
};

struct Window {
    std::uint32_t frames = 0, submissions = 0, commands = 0, reused = 0;
    std::uint32_t ordinal = 0, anywhere = 0, matchedCommands = 0, matchedReused = 0;
    double time = 0.0, timeOrdinal = 0.0, timeAnywhere = 0.0;
};

struct State {
    std::uint64_t serial = 0;
    std::uint32_t queue = 0;
    bool active = false;
    SubmissionStats current;
    std::vector<SubmissionStats> frame, previous;
    Window window;
    std::chrono::steady_clock::time_point reported = std::chrono::steady_clock::now();
};

std::atomic<std::uint64_t> presentSerial{0};
std::atomic<std::uint32_t> finisherTranslations{0};
thread_local State state;
std::mutex reportMutex;
std::string lastReport;

double Percent(double part, double whole) { return whole > 0.0 ? 100.0 * part / whole : 0.0; }

// Compares the closed frame with the previous one of the same queue; a frame without a predecessor
// only becomes the reference.
void CloseFrame(State& s) {
    if (s.frame.empty()) return;
    if (!s.previous.empty()) {
        std::unordered_set<std::uint64_t> hashes;
        for (const auto& submission : s.previous) hashes.insert(submission.hash);
        auto& w = s.window;
        ++w.frames;
        for (std::size_t i = 0; i < s.frame.size(); ++i) {
            const auto& submission = s.frame[i];
            const bool ordinal = i < s.previous.size() && submission.hash == s.previous[i].hash;
            const bool anywhere = hashes.contains(submission.hash);
            ++w.submissions;
            w.commands += submission.commands;
            w.reused += submission.Reused();
            w.time += submission.hostMs;
            if (ordinal) {
                ++w.ordinal;
                w.timeOrdinal += submission.hostMs;
                w.matchedCommands += submission.commands;
                w.matchedReused += submission.Reused();
            }
            if (anywhere) {
                ++w.anywhere;
                w.timeAnywhere += submission.hostMs;
            }
        }
    }
    s.previous = std::move(s.frame);
    s.frame.clear();
}

void Report(State& s) {
    const auto& w = s.window;
    char line[512];
    std::snprintf(line, sizeof(line),
        "[stream-reuse] queue=0x%x presents=%u submissions=%u commands=%u ordinal_match=%.1f%% anywhere_match=%.1f%% time_ordinal=%.1f%% time_anywhere=%.1f%% reused_translation=%.1f%% reused_in_matched=%.1f%%\n",
        s.queue, w.frames, w.submissions, w.commands, Percent(w.ordinal, w.submissions), Percent(w.anywhere, w.submissions),
        Percent(w.timeOrdinal, w.time), Percent(w.timeAnywhere, w.time), Percent(w.reused, w.commands),
        Percent(w.matchedReused, w.matchedCommands));
    std::fputs(line, stderr);
    {
        std::lock_guard lock(reportMutex);
        lastReport = line;
    }
    s.window = {};
    s.reported = std::chrono::steady_clock::now();
}

}

bool StreamReuseEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_STREAM_REUSE_STATS");
        return value != nullptr && *value != '\0' && std::string_view(value) != "0";
    }();
    return enabled;
}

void StreamReuseBeginSubmission(std::uint32_t queue) {
    if (!StreamReuseEnabled()) return;
    const auto serial = presentSerial.load(std::memory_order_acquire);
    if (state.serial != serial) {
        CloseFrame(state);
        state.serial = serial;
    }
    state.queue = queue;
    state.current = {};
    state.active = true;
}

void StreamReusePacket(std::span<const std::uint32_t> packet) {
    if (!StreamReuseEnabled() || !state.active) return;
    for (const auto word : packet) {
        state.current.hash ^= word;
        state.current.hash *= 1099511628211ull;
    }
}

void StreamReuseCommand(bool) {
    if (!StreamReuseEnabled() || !state.active) return;
    ++state.current.commands;
}

void StreamReuseNoteTranslation() {
    if (!StreamReuseEnabled()) return;
    if (state.active) ++state.current.translated;
    else finisherTranslations.fetch_add(1, std::memory_order_relaxed);
}

void StreamReuseEndSubmission(double hostMs) {
    if (!StreamReuseEnabled() || !state.active) return;
    if (state.queue == 0) state.current.translated += finisherTranslations.exchange(0, std::memory_order_relaxed);
    state.current.hostMs = hostMs;
    state.frame.push_back(state.current);
    state.active = false;
    if (std::chrono::steady_clock::now() - state.reported >= std::chrono::seconds(10)) Report(state);
}

void StreamReusePresent() {
    if (StreamReuseEnabled()) presentSerial.fetch_add(1, std::memory_order_acq_rel);
}

void StreamReuseFlushForTest() {
    CloseFrame(state);
    state.serial = presentSerial.load(std::memory_order_acquire);
    Report(state);
}

std::string StreamReuseReportForTest() {
    std::lock_guard lock(reportMutex);
    return lastReport;
}

}
