#include "prx/libSceAgcDriver/Execution/include/PerformanceTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/TraceFile.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#if defined(__linux__)
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#endif
#include <thread>
#include "zlib.h"
#include <charconv>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <filesystem>
#include <optional>
#include <string_view>
#include <cstdlib>
#include <system_error>
#include <sys/stat.h>
#if defined(__linux__)
#include <time.h>
#endif

#if defined(__linux__)
#include <fcntl.h>
#include <semaphore.h>
#include <unistd.h>
#endif
namespace {

using AgcDriver::TraceFormat;
using AgcDriver::TraceThread;
using AgcDriver::TraceWindow;
using Clock = std::chrono::steady_clock;
using Edge = TraceWindow::Edge;
using std::chrono::microseconds;
using std::chrono::milliseconds;

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

#if defined(__linux__)
std::string ReadGzip(const std::filesystem::path& path);

struct RequestFixture {
    struct Variable {
        std::string name;
        std::optional<std::string> previous;
        explicit Variable(const char* key) : name(key) {
            if (const char* value = std::getenv(key)) previous = value;
        }
        ~Variable() {
            if (previous) setenv(name.c_str(), previous->c_str(), 1);
            else unsetenv(name.c_str());
        }
        void Set(const char* value) {
            Check((value ? setenv(name.c_str(), value, 1) : unsetenv(name.c_str())) == 0,
                "cannot isolate trace environment");
        }
    };
    Variable frames{"APS5_TRACE_FRAMES"}, seconds{"APS5_TRACE_SECONDS"};
    Variable summary{"APS5_TRACE_SUMMARY_EVERY"}, bench{"APS5_TRACE_BENCH"};
    Variable events{"APS5_TRACE_EVENTS"}, request{"APS5_TRACE_ON_REQUEST_SECONDS"};
    Variable semaphore{"APS5_TRACE_START_SEMAPHORE"}, output{"APS5_TRACE_DIR"};
    std::filesystem::path directory;
    std::string name;
    sem_t* owner = SEM_FAILED;

    RequestFixture() {
        auto pattern = (std::filesystem::temp_directory_path() / "aps5-request-test-XXXXXX").string();
        const char* created = mkdtemp(pattern.data());
        Check(created != nullptr, "cannot create private trace directory");
        directory = created;
        try {
            name = "/aps5-trace-test-" + std::to_string(getpid()) + "-" + directory.filename().string();
            owner = sem_open(name.c_str(), O_CREAT | O_EXCL, 0600, 0);
            Check(owner != SEM_FAILED, "cannot create owned request semaphore");
            struct stat info{};
            Check(stat(("/dev/shm/sem." + name.substr(1)).c_str(), &info) == 0
                && (info.st_mode & 0777) == 0600, "owned request semaphore is not mode 0600");
            int count = -1;
            Check(sem_getvalue(owner, &count) == 0 && count == 0, "request semaphore did not start at zero");
            frames.Set(nullptr); seconds.Set(nullptr); summary.Set(nullptr);
            bench.Set(nullptr); events.Set(nullptr);
            request.Set("15"); semaphore.Set(name.c_str()); output.Set(directory.c_str());
        } catch (...) {
            Release();
            throw;
        }
    }
    RequestFixture(const RequestFixture&) = delete;
    RequestFixture& operator=(const RequestFixture&) = delete;
    void Release() noexcept {
        if (owner != SEM_FAILED) {
            sem_close(owner);
            sem_unlink(name.c_str());
            owner = SEM_FAILED;
        }
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
    void Stop() {
        AgcDriver::PerformanceTrace::StopOutput();
    }
    ~RequestFixture() { Release(); }
    void Post() { Check(sem_post(owner) == 0, "cannot post owned request semaphore"); }
    void AssertCapture(bool closed) const {
        const auto summaryPath = directory / "aps5-summary.jsonl";
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (!std::filesystem::exists(summaryPath) && Clock::now() < deadline)
            std::this_thread::sleep_for(milliseconds(10));
        std::ifstream input(summaryPath, std::ios::binary);
        const std::string summary((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const auto key = summary.find("\"trace\":");
        Check(key != std::string::npos, "summary omitted the trace path");
        const auto start = summary.find('"', key + 8);
        const auto end = summary.find('"', start + 1);
        Check(start != std::string::npos && end != std::string::npos, "summary trace path is malformed");
        const auto capturePath = directory / summary.substr(start + 1, end - start - 1);
        const auto capture = ReadGzip(capturePath);
        if (closed)
            Check(capture.find("\"declared_window\":\"cpu-enable seconds 15\"") != std::string::npos,
                "published raw capture has the wrong declaration");
        Check(capture.find(closed ? "\"declared_window_closed\":true" : "\"declared_window_closed\":false") != std::string::npos,
            "published raw capture has the wrong completion state");
        Check(capture.find("\"name\":\"RequestFixture.Work\"") != std::string::npos,
            "published capture omitted genuine timed work");
        Check(capture.find("\"name\":\"present\"") != std::string::npos,
            "published capture omitted presentation witnesses");
    }
};

void RequestWork() {
    AgcDriver::PerformanceTimer scope("RequestFixture.Work");
    std::this_thread::sleep_for(milliseconds(10));
}

void RequestSemaphoreSmoke(bool complete = false, bool stopWaiting = false) {
    RequestFixture fixture;
    AgcDriver::PerformanceTrace::Present(1, 1, nullptr);
    Check(!AgcDriver::PerformanceTrace::CapturingRawWindow(), "native request window opened before semaphore post");
    if (stopWaiting) {
        AgcDriver::PerformanceTrace::StopOutput();
        fixture.Post();
        AgcDriver::PerformanceTrace::Present(1, 1, nullptr);
        Check(!AgcDriver::PerformanceTrace::CapturingRawWindow(), "stopped waiting session reopened");
        for (const auto& entry : std::filesystem::directory_iterator(fixture.directory))
            Check(!entry.path().filename().string().starts_with("aps5-trace-"), "stopped waiting session published a capture");
        return;
    }
    fixture.Post();
    const auto beforeOpen = Clock::now();
    AgcDriver::PerformanceTrace::Present(1, 1, nullptr);
    Check(AgcDriver::PerformanceTrace::CapturingRawWindow(), "native request semaphore did not open raw trace");
    do {
        RequestWork();
        AgcDriver::PerformanceTrace::Present(1, 1, nullptr);
    } while (complete && AgcDriver::PerformanceTrace::CapturingRawWindow()
        && Clock::now() - beforeOpen < std::chrono::seconds(17));
    if (complete) {
        Check(!AgcDriver::PerformanceTrace::CapturingRawWindow(), "request capture did not close automatically within 17 seconds");
        Check(Clock::now() - beforeOpen >= std::chrono::seconds(15), "request capture closed before its declared duration");
        fixture.Post();
        AgcDriver::PerformanceTrace::Present(1, 1, nullptr);
        Check(!AgcDriver::PerformanceTrace::CapturingRawWindow(), "completed one-shot capture reopened");
    } else {
        RequestWork();
        AgcDriver::PerformanceTrace::Present(1, 1, nullptr);
        Check(AgcDriver::PerformanceTrace::CapturingRawWindow(), "partial request capture unexpectedly closed");
    }
    fixture.Stop();
    fixture.AssertCapture(complete);
}
#endif
void WindowByFrames() {
    auto window = TraceWindow::Parse("3:2", nullptr).value();
    std::vector<Edge> edges;
    for (std::uint64_t present = 0; present < 8; ++present) edges.push_back(window.Present(present, std::chrono::seconds(present)));
    Check(edges == std::vector<Edge>{Edge::None, Edge::None, Edge::None, Edge::Open, Edge::None, Edge::Close, Edge::None, Edge::None}, "frame window opened or closed at the wrong present");
    Check(window.Describe() == "frames 3:2", "frame window description changed");
}

void WindowBySeconds() {
    auto window = TraceWindow::Parse(nullptr, "2:1").value();
    std::vector<Edge> edges;
    for (const auto elapsed : {milliseconds(0), milliseconds(1999), milliseconds(2000), milliseconds(2999), milliseconds(3000), milliseconds(5000)})
        edges.push_back(window.Present(0, elapsed));
    Check(edges == std::vector<Edge>{Edge::None, Edge::None, Edge::Open, Edge::None, Edge::Close, Edge::None}, "seconds window opened or closed at the wrong present");
    Check(window.Describe() == "seconds 2:1", "seconds window description changed");
}
void WindowByRequest() {
    auto window = TraceWindow::Parse(nullptr, nullptr, "15").value();
    Check(window.Describe() == "cpu-enable seconds 15", "request window declaration changed");
    Check(window.WaitingForRequest(), "request window did not start waiting");
    Check(window.Present(10, std::chrono::seconds(90)) == Edge::None, "request window opened before request");
    Check(window.Request(), "request was not accepted while waiting");
    Check(!window.Request(), "request was accepted more than once");
    Check(window.Present(11, std::chrono::seconds(90)) == Edge::Open, "request did not open at next present");
    Check(window.Present(12, std::chrono::seconds(104)) == Edge::None, "request window closed before 15 seconds");
    Check(window.Present(13, std::chrono::seconds(105)) == Edge::Close, "request window did not close after 15 seconds");
    Check(window.Present(14, std::chrono::seconds(120)) == Edge::None, "request window reopened");
}

void WindowRequestRejects() {
    for (const auto* text : {"", "0", "-1", "1:1", "x"}) {
        bool rejected = false;
        try { TraceWindow::Parse(nullptr, nullptr, text); }
        catch (const std::invalid_argument&) { rejected = true; }
        Check(rejected, "an invalid request duration was accepted");
    }
    bool rejected = false;
    try { TraceWindow::Parse("1:1", nullptr, "15"); }
    catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "fixed and request windows were accepted together");
}


void WindowRejects() {
    Check(!TraceWindow::Parse(nullptr, nullptr).has_value(), "a window was made without a switch");
    for (const auto* text : {"5", "a:1", "1:0", "1:", ":1", "1:2:3", "-1:2"}) {
        bool rejected = false;
        try { TraceWindow::Parse(text, nullptr); }
        catch (const std::invalid_argument&) { rejected = true; }
        Check(rejected, "a malformed window was accepted");
    }
    bool rejected = false;
    try { TraceWindow::Parse("1:1", "1:1"); }
    catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "frames and seconds were accepted together");
    bool requestRejected = false;
    try { TraceWindow::Parse(nullptr, "1:1", "15"); }
    catch (const std::invalid_argument&) { requestRejected = true; }
    Check(requestRejected, "seconds and request windows were accepted together");
}

void WindowTrigger() {
    const auto directory = std::filesystem::temp_directory_path() / ("aps5-trigger-test-" + std::to_string(Clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    const auto cleanup = std::unique_ptr<const std::filesystem::path, void(*)(const std::filesystem::path*)>(&directory, [](const auto* path) {
        std::error_code error;
        std::filesystem::remove_all(*path, error);
    });
    const auto path = directory / std::filesystem::path(u8"scene-\u00e9.ready");
    const auto utf8 = path.u8string();
    const std::string trigger(utf8.begin(), utf8.end());
    auto seconds = TraceWindow::Parse(nullptr, "2:1", nullptr, trigger.c_str()).value();
    Check(seconds.Present(0, milliseconds(2000)) == Edge::None, "missing trigger opened the window");
    std::ofstream(path).close();
    Check(seconds.Present(2, milliseconds(2250)) == Edge::Open, "trigger did not open the seconds window");
    std::filesystem::remove(path);
    Check(seconds.Present(3, milliseconds(3249)) == Edge::None, "seconds duration began before the trigger");
    Check(seconds.Present(4, milliseconds(3250)) == Edge::Close, "seconds window did not close after its full duration");
    std::ofstream(path).close();
    Check(seconds.Present(5, milliseconds(5000)) == Edge::None, "trigger reopened a completed window");
    auto frames = TraceWindow::Parse("3:2", nullptr, nullptr, trigger.c_str()).value();
    Check(frames.Present(2, milliseconds(0)) == Edge::None, "preexisting trigger bypassed the frame lower bound");
    Check(frames.Present(3, milliseconds(1)) == Edge::Open, "preexisting trigger did not open at the frame lower bound");
    Check(frames.Present(4, milliseconds(2)) == Edge::None, "trigger shortened the frame window");
    Check(frames.Present(5, milliseconds(3)) == Edge::Close, "triggered frame window did not close");
    std::filesystem::remove(path);
    auto late = TraceWindow::Parse("3:2", nullptr, nullptr, trigger.c_str()).value();
    Check(late.Present(100, milliseconds(0)) == Edge::None, "late frame window opened without a trigger");
    std::ofstream(path).close();
    Check(late.Present(120, milliseconds(250)) == Edge::Open, "late frame trigger was ignored");
    Check(late.Present(121, milliseconds(500)) == Edge::None, "frame duration began at its original lower bound");
    Check(late.Present(122, milliseconds(750)) == Edge::Close, "late frame window did not close after its full length");
    auto early = TraceWindow::Parse(nullptr, "2:1", nullptr, trigger.c_str()).value();
    Check(early.Present(0, milliseconds(1999)) == Edge::None, "preexisting trigger bypassed the time lower bound");
    Check(early.Present(1, milliseconds(2000)) == Edge::Open, "preexisting trigger did not open at the time lower bound");
    const auto reject = [](auto&& action) {
        bool rejected = false;
        try { action(); }
        catch (const std::exception&) { rejected = true; }
        Check(rejected, "invalid trigger configuration was accepted");
    };
    reject([] { TraceWindow::Parse(nullptr, nullptr, nullptr, "scene.ready"); });
    reject([] { TraceWindow::Parse("0:1", nullptr, nullptr, ""); });
    reject([&] { TraceWindow::Parse(nullptr, nullptr, "15", trigger.c_str()); });
    const auto dirText = directory.string();
    reject([&] { TraceWindow::Parse("0:1", nullptr, nullptr, dirText.c_str())->Present(0, milliseconds(0)); });
    const auto badPath = trigger + "/child";
    std::error_code error;
    const auto status = std::filesystem::status(std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(badPath.c_str()))), error);
    Check(!std::filesystem::exists(status), "a path beneath a regular file unexpectedly exists");
    Check(error == std::errc::not_a_directory || error == std::errc::no_such_file_or_directory, "unexpected error for a trigger beneath a regular file");
    if (error == std::errc::not_a_directory)
        reject([&] { TraceWindow::Parse("0:1", nullptr, nullptr, badPath.c_str())->Present(0, milliseconds(0)); });
    else
        Check(TraceWindow::Parse("0:1", nullptr, nullptr, badPath.c_str())->Present(0, milliseconds(0)) == Edge::None, "a missing trigger beneath a regular file opened the window");
}

void WindowEnvironment() {
    const auto directory = std::filesystem::temp_directory_path() / ("aps5-trigger-env-" + std::to_string(Clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    const auto files = std::unique_ptr<const std::filesystem::path, void(*)(const std::filesystem::path*)>(&directory, [](const auto* path) {
        std::error_code error;
        std::filesystem::remove_all(*path, error);
    });
    const auto marker = directory / std::filesystem::path(u8"scene-\u00e9-\u6f22.ready");
#ifdef _WIN32
    using Text = std::wstring;
    const std::array names{L"APS5_TRACE_FRAMES", L"APS5_TRACE_SECONDS", L"APS5_TRACE_TRIGGER_FILE"};
    const auto get = [](const auto* name) { return _wgetenv(name); };
    const auto set = [](const auto* name, const wchar_t* value) { return _wputenv_s(name, value != nullptr ? value : L""); };
    const Text frameSpec = L"0:1";
#else
    using Text = std::string;
    const std::array names{"APS5_TRACE_FRAMES", "APS5_TRACE_SECONDS", "APS5_TRACE_TRIGGER_FILE"};
    const auto get = [](const auto* name) { return std::getenv(name); };
    const auto set = [](const auto* name, const char* value) { return value != nullptr ? setenv(name, value, 1) : unsetenv(name); };
    const Text frameSpec = "0:1";
#endif
    std::array<std::optional<Text>, 3> saved;
    for (std::size_t index = 0; index < names.size(); ++index)
        if (const auto* value = get(names[index])) saved[index] = value;
    int token = 0;
    const auto restore = [&](int*) {
        for (std::size_t index = 0; index < names.size(); ++index) set(names[index], saved[index] ? saved[index]->c_str() : nullptr);
    };
    const auto environment = std::unique_ptr<int, decltype(restore)>(&token, restore);
    Check(set(names[0], frameSpec.c_str()) == 0 && set(names[1], nullptr) == 0 && set(names[2], marker.c_str()) == 0, "cannot configure the native trace environment");
    Check(TraceWindow::TriggerConfigured(), "the native trigger environment was not detected");
    auto window = TraceWindow::FromEnvironment().value();
    Check(window.Present(0, milliseconds(0)) == Edge::None, "environment trace opened before the marker existed");
    std::ofstream(marker).close();
    Check(window.Present(1, milliseconds(250)) == Edge::Open, "environment reader did not use the Unicode marker path");
    std::filesystem::remove(marker);
    Check(window.Present(2, milliseconds(251)) == Edge::Close, "environment trace did not retain its bounded frame window");
    Check(set(names[2], nullptr) == 0 && !TraceWindow::TriggerConfigured(), "clearing the native trigger environment did not disable its gate");
    Check(TraceWindow::FromEnvironment()->Present(0, milliseconds(0)) == Edge::Open, "an unset environment marker changed ordinary tracing");
    Check(set(names[0], nullptr) == 0 && set(names[2], marker.c_str()) == 0, "cannot configure the missing-window environment case");
    bool rejected = false;
    try { TraceWindow::FromEnvironment(); }
    catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "an environment trigger was accepted without a trace window");
}

void BufferCap() {
    std::atomic<std::uint64_t> presents{1};
    TraceThread thread("queue worker 0x0", presents, 2);
    const auto origin = Clock::now();
    for (int index = 0; index < 3; ++index) thread.Add("Driver.Packet", nullptr, origin + milliseconds(index), origin + milliseconds(index) + microseconds(30));
    const auto snapshot = thread.Collect();
    Check(snapshot.slices.size() == 2 && snapshot.dropped, "a full buffer did not retain a bounded rolling window and report overwrite");
    Check(snapshot.eventsLost == 1 && snapshot.eventsFullAt == origin + milliseconds(2) && snapshot.retainedFrom == origin + milliseconds(1) && snapshot.retainedThrough == origin + milliseconds(2) + microseconds(30) && !snapshot.totalsFull, "a full buffer did not report overwrite and retained timestamp span");
    Check(snapshot.totals.size() == 1 && snapshot.totals[0].count == 3 && snapshot.totals[0].length == microseconds(90), "totals missed a slice past the cap");
}

void LostEventsCount() {
    std::atomic<std::uint64_t> presents{1};
    TraceThread thread("queue worker 0x0", presents, 2);
    const auto origin = Clock::now();
    Check(!thread.Collect().eventsFullAt && thread.Collect().eventsLost == 0, "a thread that lost nothing reported a loss");
    for (int index = 0; index < 10; ++index) thread.Add("Driver.Packet", nullptr, origin + milliseconds(index), origin + milliseconds(index) + microseconds(30));
    const auto snapshot = thread.Collect();
    Check(snapshot.eventsLost == 8 && snapshot.eventsFullAt == origin + milliseconds(2) && snapshot.retainedFrom == origin + milliseconds(8) && snapshot.retainedThrough == origin + milliseconds(9) + microseconds(30), "the rolling buffer did not preserve the newest events and retained span");
}

void TotalsOverflow() {
    static char scopes[600];
    std::atomic<std::uint64_t> presents{1};
    TraceThread thread("presenter", presents, 4096);
    const auto origin = Clock::now();
    for (const auto& scope : scopes) thread.Add(&scope, nullptr, origin, origin + microseconds(30));
    const auto snapshot = thread.Collect();
    Check(snapshot.totalsFull && snapshot.dropped && snapshot.eventsLost == 0 && !snapshot.eventsFullAt, "a full totals table was not reported apart from the event buffer");
    Check(snapshot.totals.size() == 512 && snapshot.slices.size() == 600, "a full totals table stopped the event buffer or kept more than its entries");
}

void CapacitySettings() {
    Check(TraceThread::ParseCapacity(nullptr) == TraceThread::DefaultCapacity, "the default event capacity changed");
    Check(TraceThread::ParseCapacity("1") == 1, "the smallest event capacity was rejected");
    Check(TraceThread::ParseCapacity("3145728") == 3145728, "a larger event capacity was rejected");
    Check(TraceThread::ParseCapacity("4194304") == TraceThread::MaximumCapacity, "the maximum event capacity was rejected");
    std::atomic<std::uint64_t> presents{1};
    const auto requested = std::to_string(TraceThread::DefaultCapacity + 1);
    TraceThread thread("queue worker 0x0", presents, TraceThread::ParseCapacity(requested.c_str()));
    const auto origin = Clock::now();
    for (std::size_t index = 0; index <= TraceThread::DefaultCapacity; ++index)
        thread.Add("Driver.Packet", nullptr, origin, origin + microseconds(30));
    auto snapshot = thread.Collect();
    Check(snapshot.slices.size() == TraceThread::DefaultCapacity + 1 && !snapshot.dropped, "the configured buffer stopped at the default capacity");
    thread.Add("Driver.Packet", nullptr, origin, origin + microseconds(30));
    snapshot = thread.Collect();
    Check(snapshot.slices.size() == TraceThread::DefaultCapacity + 1 && snapshot.dropped, "the configured buffer grew beyond its capacity or failed to report overwrite");
    Check(snapshot.totals.size() == 1 && snapshot.totals[0].count == TraceThread::DefaultCapacity + 2, "totals lost the event past the configured capacity");
    for (const auto* text : {"", "0", "-1", "+1", "1.5", " 1", "1 ", "1x", "4194305", "18446744073709551616"}) {
        bool rejected = false;
        try { TraceThread::ParseCapacity(text); }
        catch (const std::invalid_argument& error) { rejected = std::string_view(error.what()).starts_with("APS5_TRACE_EVENTS="); }
        Check(rejected, "an invalid event capacity did not report its setting");
    }
}
void SnapshotBudgetFairShare() {
    const std::array<std::size_t, 5> available{2, 1000, 1, 1000, 0};
    const auto allocated = AgcDriver::TraceSnapshotBudget::Allocate(available, 103);
    Check(allocated == std::vector<std::size_t>{2, 50, 1, 50, 0},
        "snapshot budgeting starved a later small track or failed to split the remaining budget across hot tracks");
}

void WaitEdgeBoundsAndWindow() {
    std::atomic<std::uint64_t> presents{1};
    std::atomic<bool> buffering{false};
    TraceThread thread("queue worker 0x0", presents, 2, buffering);
    TraceThread::WaitTraceEvent event{};
    event.waitId = 7;
    thread.NoteWaitEvent(event);
    Check(thread.Collect().waitEvents.empty(), "a disabled raw window retained a wait edge");
    buffering.store(true);
    thread.NoteWaitEvent(event);
    event.kind = TraceThread::WaitTraceKind::End;
    thread.NoteWaitEvent(event);
    event.kind = TraceThread::WaitTraceKind::Store;
    event.waitId = 0;
    thread.NoteWaitEvent(event);
    const auto origin = Clock::now();
    thread.Add("Driver.Packet", nullptr, origin, origin + microseconds(30));
    const auto bounded = thread.Collect(2);
    Check(bounded.waitEvents.size() == 2 && bounded.slices.empty() && bounded.eventsOmitted == 1,
        "wait edges escaped the shared snapshot budget or lost ordinary-slice omission accounting");
    Check(bounded.waitEvents[0].event.kind == TraceThread::WaitTraceKind::End &&
        bounded.waitEvents[0].event.waitId == 7 && bounded.waitEvents[1].event.kind == TraceThread::WaitTraceKind::Store,
        "the wait ring did not retain its newest records in order");
    Check(bounded.dropped && bounded.waitEventsLost == 1 && bounded.waitEventsFullAt.has_value(),
        "a lost wait begin could be mistaken for a complete causal capture");
    Check(bounded.totals.size() == 1 && bounded.totals[0].count == 1,
        "wait-edge budgeting discarded ordinary phase totals");
    const auto truncated = thread.Collect(1);
    Check(truncated.waitEvents.size() == 1 && truncated.waitEvents[0].event.kind == TraceThread::WaitTraceKind::Store &&
        truncated.waitEventsOmitted == 1 && truncated.waitEventsOmittedAt.has_value(),
        "snapshot truncation did not identify omitted wait edges");
    buffering.store(false);
    thread.NoteWaitEvent(event);
    Check(thread.Collect().waitEventsLost == 1, "a closed window continued overwriting wait evidence");
    thread.ResetWaitTrace();
    const auto reset = thread.Collect();
    Check(reset.waitEvents.empty() && reset.waitEventsLost == 0 && reset.slices.size() == 1,
        "opening a new wait window retained old wait evidence or erased existing ordinary slices");
    TraceThread zero("queue worker 0x0", presents, 0);
    zero.NoteWaitEvent(event);
    Check(zero.Collect().waitEvents.empty(), "a zero-capacity wait ring retained a record");
}

void FoldShortSlices() {
    std::atomic<std::uint64_t> presents{1};
    TraceThread thread("queue worker 0x0", presents, 16);
    const auto origin = Clock::now();
    thread.Add("Driver.Packet", nullptr, origin, origin + microseconds(5));
    thread.Add("Driver.Packet", "draw", origin + microseconds(5), origin + microseconds(10));
    thread.Add("Driver.Draw", nullptr, origin + microseconds(10), origin + microseconds(40));
    presents.store(2);
    thread.Add("Driver.Completion", nullptr, origin + microseconds(50), origin + microseconds(90));
    const auto snapshot = thread.Collect();
    Check(snapshot.slices.size() == 3 && !snapshot.dropped, "short slices were written or long ones lost");
    Check(std::string_view(snapshot.slices[0].scope) == "Driver.Draw", "the first long slice is missing");
    Check(snapshot.slices[1].scope == nullptr && snapshot.slices[1].begin == origin && snapshot.slices[1].length == microseconds(10), "the folded sample of the previous present is wrong");
    Check(std::string_view(snapshot.slices[2].scope) == "Driver.Completion", "the slice after the present is missing");
    Check(snapshot.totals.size() == 4, "totals did not keep scope and stage apart");
    for (const auto& total : snapshot.totals) Check(total.count == 1, "a total counted a slice twice");
}

void EscapeStrings() {
    std::string output;
    TraceFormat::AppendString(output, "a\"b\\c\n\xe9");
    Check(output == "\"a\\\"b\\\\c\\u000a\\u00e9\"", "string escaping is not valid JSON");
}

void Utf8Strings() {
    std::string output;
    TraceFormat::AppendString(output, "Ghost of Y\xc5\x8dtei \xe2\x84\xa2 \xf0\x9f\x8e\xae");
    Check(output == "\"Ghost of Y\xc5\x8dtei \xe2\x84\xa2 \xf0\x9f\x8e\xae\"", "a UTF-8 title was not written as its own characters");
    output.clear();
    TraceFormat::AppendString(output, "\xc0\xaf\xed\xa0\x80\xf4\x90\x80\x80\xe2\x84\x7f\xc5");
    Check(output == "\"\\u00c0\\u00af\\u00ed\\u00a0\\u0080\\u00f4\\u0090\\u0080\\u0080\\u00e2\\u0084\\u007f\\u00c5\"", "a byte outside a valid UTF-8 sequence was not escaped");
    AgcDriver::TraceSummary summary;
    summary.title = AgcDriver::TraceTitle{"PPSA26344", "Ghost of Y\xc5\x8dtei", "01.000.000"};
    const auto origin = Clock::now();
    const std::vector<Clock::time_point> presents{origin, origin + milliseconds(16)};
    Check(TraceFormat::Summary(summary, {}, presents).find("\"title\":\"Ghost of Y\xc5\x8dtei\"") != std::string::npos, "the summary line did not keep a UTF-8 title");
}

void ProgramScopeLifecycle() {
    const auto origin = Clock::now();
    std::atomic<std::uint64_t> count{1};
    TraceThread thread("queue worker 0x0", count, 8);
    const auto record = [&](int step) {
        const auto begin = origin + milliseconds(step);
        thread.Add("Driver.Dispatch", nullptr, begin, begin + microseconds(30));
    };
    AgcDriver::TraceProgramTag.reset();
    AgcDriver::TraceBatchTag = 9;
    {
        AgcDriver::TraceProgramScope outer(std::uint64_t{0});
        record(2);
        {
            AgcDriver::TraceProgramScope inner(std::uint64_t{27});
            record(3);
        }
        record(4);
        try {
            AgcDriver::TraceProgramScope unwound(std::uint64_t{38});
            throw std::runtime_error("scope unwind");
        } catch (const std::runtime_error&) {
            record(5);
        }
    }
    record(6);
    AgcDriver::TraceBatchTag = 0;
    record(7);
    const auto snapshot = thread.Collect();
    using Owner = std::pair<std::optional<std::uint64_t>, std::uint64_t>;
    std::vector<Owner> owners;
    for (const auto& slice : snapshot.slices) owners.emplace_back(slice.program, slice.batch);
    const std::vector<Owner> expected{
        {std::uint64_t{0}, 9}, {std::uint64_t{27}, 9},
        {std::uint64_t{0}, 9}, {std::uint64_t{0}, 9},
        {std::nullopt, 9}, {std::nullopt, 0}
    };
    Check(owners == expected, "program scopes did not restore ownership independently of the batch");
}

void FullBufferEvents() {
    const auto origin = Clock::now();
    const std::vector<TraceThread::Slice> slices{{"Driver.Packet", nullptr, origin + milliseconds(1), microseconds(30)}};
    TraceThread::Snapshot snapshot{"queue worker 0x0", slices, {}, true, 7, origin + milliseconds(3), false};
    const std::vector<TraceThread::Snapshot> threads{snapshot};
    const std::vector<Clock::time_point> presents{origin, origin + milliseconds(16)};
    const auto json = TraceFormat::Events(7, origin, threads, 0, presents, 5);
    const std::string expected =
        "{\"name\":\"trace buffer overwritten\",\"ph\":\"i\",\"ts\":3000.000,\"pid\":7,\"tid\":1,\"s\":\"t\",\"args\":{\"events_overwritten\":7}},\n"
        "{\"name\":\"present\"";
    Check(json.find(expected) != std::string::npos, "the first overwrite instant event is missing or has the wrong shape");
    snapshot.eventsLost = 0;
    snapshot.eventsFullAt.reset();
    Check(TraceFormat::Events(7, origin, std::vector<TraceThread::Snapshot>{snapshot}, 0, presents, 5).find("trace buffer overwritten") == std::string::npos, "a thread that lost nothing got an overwrite event");
}

void PresentBoundaryWitnesses() {
    const auto origin = Clock::now();
    const auto number = [](std::string_view object, std::string_view key) {
        const auto position = object.find(key);
        Check(position != std::string_view::npos, "a presentation or summary numeric field is missing");
        const auto begin = object.data() + position + key.size();
        double value = 0;
        const auto result = std::from_chars(begin, object.data() + object.size(), value);
        Check(result.ec == std::errc{}, "a presentation or summary numeric field is invalid");
        return value;
    };
    for (const std::size_t count : {2u, 4u}) {
        std::vector<Clock::time_point> presents;
        for (std::size_t index = 0; index < count; ++index) presents.push_back(origin + milliseconds(7 + index * 16));
        const auto json = TraceFormat::Events(7, origin, {}, 0, presents, 42);
        std::size_t position = 0;
        std::size_t witnesses = 0;
        double firstTimestamp = -1;
        double lastTimestamp = -1;
        while ((position = json.find("\"name\":\"present\"", position)) != std::string::npos) {
            const auto end = json.find("}}", position);
            Check(end != std::string::npos, "a presentation witness is incomplete");
            const std::string_view event(json.data() + position, end + 2 - position);
            const auto timestamp = number(event, "\"ts\":");
            Check(number(event, "\"present\":") == 42 + witnesses, "presentation witness IDs are not consecutive");
            Check(timestamp == 7000 + witnesses * 16000, "a presentation witness has the wrong timestamp");
            if (witnesses == 0) firstTimestamp = timestamp;
            lastTimestamp = timestamp;
            ++witnesses;
            position = end + 2;
        }
        Check(witnesses == count, "the closing presentation boundary witness is missing");
        Check(firstTimestamp == 7000 && lastTimestamp == 7000 + (count - 1) * 16000, "presentation witnesses do not cover both window boundaries");
        AgcDriver::TraceSummary summary;
        summary.firstPresent = 42;
        const auto line = TraceFormat::Summary(summary, {}, presents);
        Check(number(line, "\"presents\":") == count - 1, "the closing boundary was counted as a completed frame");
        Check(number(line, "\"first_present\":") == 42 && number(line, "\"ms\":") == (count - 1) * 16, "the summary window does not match its presentation boundaries");
    }
    constexpr std::uint64_t nativeTid = 3197;
    const auto exactOrigin = Clock::now();
    const std::vector<Clock::time_point> exactPresents{
        exactOrigin + std::chrono::nanoseconds(123'000),
        exactOrigin + std::chrono::nanoseconds(16'789'000),
        exactOrigin + std::chrono::nanoseconds(33'654'000),
    };
    const std::vector<TraceThread::Snapshot> threads{{"queue worker", {}, {}, false, 0, {}, false, nativeTid}};
#if defined(__linux__) && (defined(__GLIBCXX__) || defined(_LIBCPP_VERSION))
    timespec before{};
    timespec after{};
    Check(::clock_gettime(CLOCK_MONOTONIC, &before) == 0, "failed to sample CLOCK_MONOTONIC before export");
    const auto exactJson = TraceFormat::Events(7, exactOrigin, threads, 0, exactPresents, 42);
    Check(::clock_gettime(CLOCK_MONOTONIC, &after) == 0, "failed to sample CLOCK_MONOTONIC after export");
    const auto parseInteger = [](std::string_view object, std::string_view key) {
        const auto position = object.find(key);
        Check(position != std::string_view::npos, "a clock integer field is missing");
        const auto begin = object.data() + position + key.size();
        std::int64_t value = 0;
        const auto result = std::from_chars(begin, object.data() + object.size(), value);
        Check(result.ec == std::errc{}, "a clock integer field is invalid");
        return value;
    };
    const auto clockObjectBegin = exactJson.find("\"clock\":{");
    const auto clockObjectEnd = exactJson.find('}', clockObjectBegin);
    Check(clockObjectBegin != std::string::npos && clockObjectEnd != std::string::npos, "clock metadata is missing");
    const std::string_view clockObject(exactJson.data() + clockObjectBegin, clockObjectEnd - clockObjectBegin);
    Check(clockObject.find("\"domain\":\"CLOCK_MONOTONIC\"") != std::string_view::npos, "Linux trace origin is not calibrated to CLOCK_MONOTONIC");
    const auto originNs = parseInteger(clockObject, "\"origin_ns\":");
    const auto uncertaintyNs = parseInteger(clockObject, "\"uncertainty_ns\":");
    const auto monotonicNs = [](const timespec& time) { return std::int64_t(time.tv_sec) * 1'000'000'000LL + time.tv_nsec; };
    constexpr std::int64_t marginNs = 5'000'000;
    Check(originNs >= monotonicNs(before) - marginNs && originNs <= monotonicNs(after) + marginNs, "exported origin is not in the sampled CLOCK_MONOTONIC interval");
    Check(uncertaintyNs >= 0 && uncertaintyNs <= marginNs, "cross-clock calibration uncertainty is invalid");
#else
    const auto exactJson = TraceFormat::Events(7, exactOrigin, threads, 0, exactPresents, 42);
    Check(exactJson.find("\"domain\":\"steady_clock\"") != std::string::npos, "steady-clock domain was not identified");
#endif
    const auto threadMetadata = exactJson.find("\"name\":\"thread_name\"");
    Check(threadMetadata != std::string::npos, "thread metadata is missing");
    const auto threadMetadataEnd = exactJson.find("}}", threadMetadata);
    Check(threadMetadataEnd != std::string::npos && exactJson.substr(threadMetadata, threadMetadataEnd - threadMetadata).find("\"native_tid\":3197") != std::string::npos, "the native thread ID was not associated with its trace lane");
    constexpr std::int64_t expectedOffsets[]{123000, 16789000, 33654000};
    std::size_t exactPosition = 0;
    std::size_t exactCount = 0;
    while ((exactPosition = exactJson.find("\"name\":\"present\"", exactPosition)) != std::string::npos) {
        Check(exactCount < std::size(expectedOffsets), "an unexpected presentation witness was emitted");
        const auto end = exactJson.find("}}", exactPosition);
        Check(end != std::string::npos, "a presentation witness is incomplete");
        const std::string_view event(exactJson.data() + exactPosition, end + 2 - exactPosition);
        const auto id = number(event, "\"present\":");
        const auto relativeUs = number(event, "\"ts\":");
        const auto expectedUs = static_cast<double>(expectedOffsets[exactCount]) / 1000.0;
        Check(relativeUs == expectedUs, "relative presentation time differs from its exported timestamp");
        Check(id == 42 + exactCount, "presentation IDs are not consecutive");
        ++exactCount;
        exactPosition = end + 2;
    }
    Check(exactCount == std::size(expectedOffsets), "the actual closing presentation did not survive clock-origin export");
}

void SummaryFields() {
    const auto origin = Clock::now();
    AgcDriver::TraceSummary summary;
    summary.commit = "abc";
    summary.dirty = false;
    summary.environment = {{"APS5_TRACE_FRAMES", "3:2"}};
    summary.gpu = "GPU";
    summary.driver = "Driver 1";
    summary.os = "OS 1";
    summary.width = 1920;
    summary.height = 1080;
    summary.window = "frames 3:2";
    summary.firstPresent = 3;
    summary.gpuBusyMs = 20;
    summary.trace = "aps5-trace-abc-7.json.gz";
    const std::vector<TraceThread::Snapshot> threads{
        {"queue worker 0x0", {}, {{"Driver.Packet", nullptr, milliseconds(12), 4}, {"Driver.WorkerWait", nullptr, milliseconds(8), 4}, {"Driver.Completion", nullptr, milliseconds(2), 4}, {"Driver.Packet", "draw", milliseconds(10), 2}}, false},
        {"presenter", {}, {{"Driver.Present", nullptr, milliseconds(6), 2}, {"Driver.Present", "inflight_wait", milliseconds(4), 2}}, false},
    };
    const std::vector<Clock::time_point> presents{origin, origin + milliseconds(16), origin + milliseconds(40)};
    const auto line = TraceFormat::Summary(summary, threads, presents);
    auto dropped = threads;
    dropped[1].dropped = true;
    Check(TraceFormat::Summary(summary, dropped, presents).find("\"trace_dropped\":true") != std::string::npos, "a dropped thread did not mark the summary");
    Check(line.find("trace_dropped_threads") == std::string::npos, "a summary without drops listed threads");
    dropped[0].dropped = true;
    dropped[0].eventsLost = 2240000;
    dropped[0].eventsFullAt = presents.front() + milliseconds(32);
    dropped[1].totalsFull = true;
    const auto detailed = TraceFormat::Summary(summary, dropped, presents);
    Check(detailed.find("\"trace_dropped\":true,\"trace_dropped_threads\":{\"queue worker 0x0\":{\"events_overwrite_started_after_s\":0.032,\"events_overwritten\":2240000,\"totals_full\":false},\"presenter\":{\"totals_full\":true}}") != std::string::npos, "the dropped threads were not listed with their reasons");
    auto shared = threads;
    shared.push_back({"shader prepare", {}, {}, true});
    shared.push_back({"shader prepare", {}, {}, true});
    shared[2].eventsLost = 5;
    shared[2].eventsFullAt = presents.front() + milliseconds(30);
    shared[3].eventsLost = 7;
    shared[3].eventsFullAt = presents.front() + milliseconds(10);
    shared[3].totalsFull = true;
    const auto merged = TraceFormat::Summary(summary, shared, presents);
    Check(merged.find("\"trace_dropped_threads\":{\"shader prepare\":{\"events_overwrite_started_after_s\":0.010,\"events_overwritten\":12,\"totals_full\":true}}") != std::string::npos, "threads with one name were not merged into one entry");
    Check(merged.find("shader prepare") == merged.rfind("shader prepare"), "threads with one name were listed twice");
}

void GpuClock() {
    const auto origin = Clock::now();
    const AgcDriver::TraceClockSample first{1000.0, origin + microseconds(10)};
    const AgcDriver::TraceClockSample last{41000.0, origin + microseconds(30)};
    const std::vector<AgcDriver::TraceGpuRange> ranges{{"batch", 5000.0, 9000.0}, {"draw", 6000.0, 7000.0}, {"batch", -39000.0, -30000.0}, {"copy", 8000.0, 8000.0}};
    const auto slices = TraceFormat::GpuSlices(ranges, first, last, origin);
    Check(slices.size() == 2, "a GPU range before the window or without length was kept");
    Check(std::string_view(slices[0].scope) == "batch" && slices[0].stage == nullptr && slices[0].begin == origin + microseconds(12) && slices[0].length == microseconds(2), "a GPU range was not mapped between the two calibrations");
    Check(std::string_view(slices[1].scope) == "draw" && slices[1].begin == origin + std::chrono::nanoseconds(12500) && slices[1].length == std::chrono::nanoseconds(500), "a nested GPU range was not mapped");
    const AgcDriver::TraceClockSample same{1000.0, origin + microseconds(10)};
    Check(TraceFormat::GpuSlices(ranges, same, same, origin)[0].begin == origin + microseconds(14), "one calibration does not map at the CPU rate");
}

void IntervalDue() {
    Check(!AgcDriver::TraceInterval::Parse(nullptr).has_value(), "an interval was made without a switch");
    for (const auto* text : {"", "0", "-5", "1.5", "a", "5s", "1:2", "99999999999"}) {
        bool rejected = false;
        try { AgcDriver::TraceInterval::Parse(text); }
        catch (const std::invalid_argument&) { rejected = true; }
        Check(rejected, "a malformed interval was accepted");
    }
    auto interval = AgcDriver::TraceInterval::Parse("30").value();
    Check(interval.Describe() == "summary_every 30", "interval description changed");
    Check(!interval.Due(milliseconds(29999)) && interval.Due(milliseconds(30000)), "the interval was due at the wrong time");
    interval.Restart(milliseconds(30500));
    Check(interval.Begun() == milliseconds(30500) && !interval.Due(milliseconds(60499)) && interval.Due(milliseconds(60500)), "a restarted interval did not count from its restart");
}

void SummaryOnlyThread() {
    std::atomic<std::uint64_t> presents{1};
    std::atomic<bool> buffering{false};
    TraceThread thread("queue worker 0x0", presents, 0, buffering);
    const auto origin = Clock::now();
    thread.Add("Driver.Packet", nullptr, origin, origin + milliseconds(2));
    thread.Add("Driver.Packet", nullptr, origin + milliseconds(3), origin + milliseconds(4));
    presents.store(2);
    thread.Add("Driver.Completion", nullptr, origin + milliseconds(5), origin + microseconds(5005));
    auto snapshot = thread.Collect();
    Check(snapshot.slices.empty() && !snapshot.dropped, "a thread that is not buffering kept a slice");
    Check(snapshot.totals.size() == 2 && snapshot.totals[0].count + snapshot.totals[1].count == 3, "a thread that is not buffering missed a total");
    buffering.store(true);
    thread.Add("Driver.Packet", nullptr, origin + milliseconds(6), origin + milliseconds(7));
    Check(thread.Collect().slices.empty(), "a zero capacity buffer held a slice");
}

void TotalsSince() {
    const std::vector<TraceThread::Total> before{{"Driver.Packet", nullptr, milliseconds(4), 2}, {"Driver.Present", nullptr, milliseconds(1), 1}};
    const std::vector<TraceThread::Total> now{{"Driver.Packet", nullptr, milliseconds(10), 5}, {"Driver.Present", nullptr, milliseconds(1), 1}, {"Driver.Completion", nullptr, milliseconds(3), 2}};
    const auto delta = TraceFormat::Since(now, before);
    Check(delta.size() == 2, "a total without a new slice was kept or a new one lost");
    Check(delta[0].length == milliseconds(6) && delta[0].count == 3, "a total was not reduced by the previous interval");
    Check(std::string_view(delta[1].scope) == "Driver.Completion" && delta[1].length == milliseconds(3) && delta[1].count == 2, "a total without a previous value changed");
    Check(TraceFormat::Since(now, {}).size() == 3, "the first interval dropped a total");
}

void IntervalDroppedThreads() {
    const auto origin = Clock::now();
    AgcDriver::TraceSummary summary;
    summary.interval = std::make_pair(30.0, 60.0);
    std::vector<TraceThread::Snapshot> threads{{"queue worker 0x0", {}, {}, true, 5}};
    const std::vector<Clock::time_point> presents{origin + milliseconds(100), origin + milliseconds(116)};
    threads[0].eventsFullAt = origin + milliseconds(40);
    auto line = TraceFormat::Summary(summary, threads, presents);
    Check(line.find("\"queue worker 0x0\":{\"events_overwritten\":5,\"totals_full\":false}") != std::string::npos, "an overwrite before the interval printed its time");
    threads[0].eventsFullAt = origin + milliseconds(104);
    line = TraceFormat::Summary(summary, threads, presents);
    Check(line.find("{\"events_overwrite_started_after_s\":0.004,\"events_overwritten\":5,") != std::string::npos, "an overwrite inside the interval lost its time");
    threads[0].eventsFullAt = origin + milliseconds(120);
    line = TraceFormat::Summary(summary, threads, presents);
    Check(line.find("\"queue worker 0x0\":{\"events_overwritten\":5,\"totals_full\":false}") != std::string::npos && line.find("events_overwrite_started_after_s") == std::string::npos, "an overwrite after the last present printed its time");
    line = TraceFormat::Summary(summary, threads, {});
    Check(line.find("\"queue worker 0x0\":{\"events_overwritten\":5,\"totals_full\":false}") != std::string::npos && line.find("events_overwrite_started_after_s") == std::string::npos, "an interval without presents printed an overwrite time");
}

void SummaryMachineFields() {
    const auto origin = Clock::now();
    AgcDriver::TraceSummary summary;
    summary.commit = "abc";
    summary.dirty = false;
    summary.gpu = "GPU";
    summary.driver = "Driver 1";
    summary.os = "OS 1";
    summary.width = 1920;
    summary.height = 1080;
    summary.window = "summary_every 30";
    summary.host = AgcDriver::TraceHost{"CPU \"X\"", 16, 32000};
    summary.title = AgcDriver::TraceTitle{"PPSA00000", "Example", "01.002.000"};
    const std::vector<Clock::time_point> presents{origin, origin + milliseconds(16)};
    const auto line = TraceFormat::Summary(summary, {}, presents);
    Check(line.find("\"os\":\"OS 1\",\"cpu\":\"CPU \\\"X\\\"\",\"cpu_threads\":16,\"memory_mib\":32000,\"title_id\":\"PPSA00000\",\"title\":\"Example\",\"content_version\":\"01.002.000\",\"resolution\":\"1920x1080\"") != std::string::npos, "the summary line did not name the machine and title");
    summary.title.reset();
    Check(TraceFormat::Summary(summary, {}, presents).find("title_id") == std::string::npos, "a summary without param.json named a title");
    summary.host.reset();
    const auto bare = TraceFormat::Summary(summary, {}, presents);
    Check(bare.find("\"os\":\"OS 1\",\"resolution\":\"1920x1080\"") != std::string::npos && bare.find("cpu") == std::string::npos, "a summary on a host without a CPU name changed its fields");
}

void FrameMicroseconds() {
    const auto origin = Clock::now();
    const std::vector<Clock::time_point> presents{origin, origin + microseconds(16667), origin + microseconds(50000), origin + microseconds(50000)};
    Check(TraceFormat::FrameMicroseconds(presents) == std::vector<std::uint32_t>{16667, 33333, 0}, "frame intervals in microseconds changed");
    Check(TraceFormat::FrameMicroseconds({}).empty(), "no presents gave frame intervals");
}

void BenchFormat() {
    AgcDriver::TraceSummary summary;
    summary.commit = "abc";
    summary.environment = {{"APS5_TRACE_BENCH", "1"}};
    summary.gpu = "GPU";
    summary.driver = "Driver 1";
    summary.os = "OS 1";
    summary.host = AgcDriver::TraceHost{"CPU", 8, 16000};
    const AgcDriver::TraceBenchIntervals intervals{
        std::make_shared<const AgcDriver::TraceBenchInterval>(AgcDriver::TraceBenchInterval{0, 30.5, 0, 1920, 1080, 120.25, {16667, 16666}}),
        std::make_shared<const AgcDriver::TraceBenchInterval>(AgcDriver::TraceBenchInterval{30.5, 61, 1830, 2560, 1440, 98.5, {}}),
    };
    const std::string expected =
        "{\"format\":\"aps5-bench/1\",\"commit\":\"abc\",\"dirty\":null,\"env\":{\"APS5_TRACE_BENCH\":\"1\"},\"gpu\":\"GPU\",\"driver\":\"Driver 1\",\"os\":\"OS 1\","
        "\"cpu\":\"CPU\",\"cpu_threads\":8,\"memory_mib\":16000,\"intervals_dropped\":3,\"intervals\":[\n"
        "{\"start_s\":0.000,\"end_s\":30.500,\"first_present\":0,\"resolution\":\"1920x1080\",\"gpu_busy_ms\":120.250,\"frame_us\":[16667,16666]},\n"
        "{\"start_s\":30.500,\"end_s\":61.000,\"first_present\":1830,\"resolution\":\"2560x1440\",\"gpu_busy_ms\":98.500,\"frame_us\":[]}\n]}\n";
    Check(TraceFormat::Bench(summary, intervals, 3) == expected, "benchmark file changed");
}

void BenchRotation() {
    AgcDriver::TraceBenchIntervals kept;
    std::uint64_t dropped = 0;
    for (std::size_t index = 0; index < AgcDriver::TraceBench::MaximumIntervals; ++index) AgcDriver::TraceBench::Keep(kept, std::make_shared<const AgcDriver::TraceBenchInterval>(AgcDriver::TraceBenchInterval{static_cast<double>(index)}), dropped);
    Check(kept.size() == AgcDriver::TraceBench::MaximumIntervals && dropped == 0, "intervals were dropped before the file was full");
    for (std::size_t index = 0; index < 5; ++index) AgcDriver::TraceBench::Keep(kept, std::make_shared<const AgcDriver::TraceBenchInterval>(AgcDriver::TraceBenchInterval{static_cast<double>(AgcDriver::TraceBench::MaximumIntervals + index)}), dropped);
    Check(kept.size() == AgcDriver::TraceBench::MaximumIntervals && dropped == 5, "a full file did not drop one interval per new one");
    Check(kept.front()->startS == 5 && kept.back()->startS == static_cast<double>(AgcDriver::TraceBench::MaximumIntervals + 4), "the file did not keep the newest intervals");
    const auto file = TraceFormat::Bench({}, kept, dropped);
    Check(file.find("\"intervals_dropped\":5,") != std::string::npos && file.find("{\"start_s\":5.000,") != std::string::npos && file.find("{\"start_s\":4.000,") == std::string::npos, "the file does not match the kept intervals");
}

void BenchWriterKeepsNewest() {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
    std::vector<std::uint64_t> written;
    AgcDriver::TraceBenchWriter writer([&](const AgcDriver::TraceBenchSnapshot& snapshot) {
        std::unique_lock lock(mutex);
        written.push_back(snapshot.dropped);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return release; });
    });
    writer.Write({{}, {}, 1});
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return entered; });
    }
    writer.Write({{}, {}, 2});
    writer.Write({{}, {}, 3});
    writer.Write({{}, {}, 4});
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    changed.notify_all();
    writer.Flush();
    writer.Stop();
    Check(written == std::vector<std::uint64_t>{1, 4}, "a benchmark snapshot was written after a newer one replaced it");
    Check(writer.Superseded() == 2, "replaced benchmark snapshots were not counted");
}

void BenchWriterStopWrites() {
    std::vector<std::uint64_t> written;
    {
        AgcDriver::TraceBenchWriter writer([&](const AgcDriver::TraceBenchSnapshot& snapshot) { written.push_back(snapshot.dropped); });
        writer.Write({{}, {}, 7});
    }
    Check(written == std::vector<std::uint64_t>{7}, "stopping the benchmark writer lost its pending snapshot");
}

std::string ReadGzip(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    Check(bytes.size() > 18, "the gzip file is truncated");
    std::size_t size = 0;
    for (unsigned index = 0; index < 4; ++index) size |= static_cast<std::size_t>(bytes[bytes.size() - 4 + index]) << (8 * index);
    z_stream stream{};
    Check(inflateInit2(&stream, -MAX_WBITS) == Z_OK, "inflateInit2 failed");
    std::string inflated(size, 0);
    stream.next_in = const_cast<Bytef*>(bytes.data() + 10);
    stream.avail_in = static_cast<uInt>(bytes.size() - 18);
    stream.next_out = reinterpret_cast<Bytef*>(inflated.data());
    stream.avail_out = static_cast<uInt>(inflated.size());
    const int status = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    Check(status == Z_STREAM_END && stream.avail_out == 0, "the gzip file does not inflate to its size");
    return inflated;
}

void ReplaceGzipFile() {
    const auto directory = std::filesystem::temp_directory_path() / ("aps5-bench-test-" + std::to_string(Clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const auto path = directory / "aps5-bench.json.gz";
    auto temporary = path;
    temporary += ".tmp";
    AgcDriver::TraceFile::ReplaceGzip(path, "{\"first\":1}\n");
    AgcDriver::TraceFile::ReplaceGzip(path, "{\"second\":2}\n");
    Check(ReadGzip(path) == "{\"second\":2}\n" && !std::filesystem::exists(temporary), "the benchmark file was not replaced");
    const auto blocked = directory / "aps5-bench-blocked.json.gz";
    auto blockedTemporary = blocked;
    blockedTemporary += ".tmp";
    std::filesystem::create_directory(blocked);
    bool failed = false;
    try { AgcDriver::TraceFile::ReplaceGzip(blocked, "{\"third\":3}\n"); } catch (const std::system_error&) { failed = true; }
    const bool blockedKept = std::filesystem::is_directory(blocked);
    const bool temporaryLeft = std::filesystem::exists(blockedTemporary);
    const auto kept = ReadGzip(path);
#ifndef _WIN32
    // A directory at <name>.tmp makes the write itself fail, which is the case ReplaceGzip exists
    // for: the previous file must survive, so an in-place rewrite fails this check. The failure is
    // a std::ios_base::failure thrown inside libstdc++'s ofstream, which MinGW builds could not
    // unwind to this catch, so the case runs on the other platforms only.
    std::filesystem::create_directory(temporary);
    bool writeFailed = false;
    try { AgcDriver::TraceFile::ReplaceGzip(path, "{\"third\":3}\n"); } catch (const std::exception&) { writeFailed = true; }
    const auto keptAfterWrite = ReadGzip(path);
#endif
#ifdef __linux__
    bool fullFailed = false;
    bool fullLeft = false;
    if (std::filesystem::exists("/dev/full")) {
        const auto fullPath = directory / "aps5-bench-full.json.gz";
        auto fullTemporary = fullPath;
        fullTemporary += ".tmp";
        std::filesystem::create_symlink("/dev/full", fullTemporary);
        try { AgcDriver::TraceFile::ReplaceGzip(fullPath, "{\"full\":1}\n"); } catch (const std::exception&) { fullFailed = true; }
        std::error_code linkError;
        fullLeft = std::filesystem::symlink_status(fullTemporary, linkError).type() != std::filesystem::file_type::not_found;
    } else {
        fullFailed = true;
    }
#endif
    std::filesystem::remove_all(directory);
    Check(failed && blockedKept && !temporaryLeft, "a replace that could not move its file over the destination did not fail cleanly or left its temporary file");
    Check(kept == "{\"second\":2}\n", "a failed replace of another file damaged the benchmark file");
#ifdef __linux__
    Check(fullFailed && !fullLeft, "a replace whose write failed left its temporary file behind");
#endif
#ifndef _WIN32
    Check(writeFailed && keptAfterWrite == "{\"second\":2}\n", "a failed rewrite damaged the previous benchmark file");
#endif
}

void RollingSnapshotCheckpoint() {
    const auto origin = Clock::now();
    const AgcDriver::TraceClockSample gpuFirst{1000.0, origin};
    const AgcDriver::TraceClockSample gpuLast{1001000.0, origin + milliseconds(1)};
    const auto gpuThread = [&](std::vector<AgcDriver::TraceGpuRange> ranges, std::uint64_t overwritten) {
        TraceThread::Snapshot gpu;
        gpu.name = "gpu";
        gpu.slices = TraceFormat::GpuSlices(ranges, gpuFirst, gpuLast, origin);
        gpu.eventsLost = overwritten;
        if (overwritten != 0) gpu.eventsFullAt = origin + microseconds(5);
        if (!gpu.slices.empty()) {
            gpu.retainedFrom = gpu.slices.front().begin;
            gpu.retainedThrough = gpu.slices.back().begin + gpu.slices.back().length;
        }
        return gpu;
    };
    auto window = TraceWindow::Parse(nullptr, "0:570").value();
    Check(window.Present(0, Clock::duration::zero()) == Edge::Open, "the long capture window did not open");
    Check(window.Present(1, std::chrono::seconds(30)) == Edge::None, "the test window closed before its declared endpoint");
    std::atomic<std::uint64_t> presents{1};
    TraceThread thread("queue worker 0x0", presents, 2);
    thread.Add("Driver.Packet1", nullptr, origin + milliseconds(1), origin + milliseconds(1) + microseconds(30));
    thread.Add("Driver.Packet2", nullptr, origin + milliseconds(2), origin + milliseconds(2) + microseconds(30));
    auto snapshot = thread.Collect();
    const auto directory = std::filesystem::temp_directory_path() / ("aps5-trace-checkpoint-" + std::to_string(origin.time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const auto path = directory / "aps5-trace.json.gz";
    auto gpu = gpuThread({{"GPU.first", 11000.0, 12000.0, 1}}, 0);
    std::vector<TraceThread::Snapshot> firstThreads{snapshot, gpu};
    auto events = TraceFormat::Events(7, origin, firstThreads, 0, {}, 0, "seconds 0:570", false, 0, true);
    AgcDriver::TraceFile::ReplaceGzip(path, events);
    const auto first = ReadGzip(path);
    Check(first.find("\"declared_window_closed\":false") != std::string::npos
        && first.find("\"gpu_track_available\":true") != std::string::npos
        && first.find("Driver.Packet1") != std::string::npos
        && first.find("GPU.first") != std::string::npos,
        "the first pre-deadline checkpoint did not preserve explicitly partial CPU and GPU events");

    thread.Add("Driver.Packet3", nullptr, origin + milliseconds(3), origin + milliseconds(3) + microseconds(30));
    thread.Add("Driver.Packet4", nullptr, origin + milliseconds(4), origin + milliseconds(4) + microseconds(30));
    snapshot = thread.Collect();
    gpu = gpuThread({{"GPU.old", 31000.0, 32000.0, 2}, {"GPU.latest", 41000.0, 42000.0, 3}}, 1);
    std::vector<TraceThread::Snapshot> latestThreads{snapshot, gpu};
    events = TraceFormat::Events(7, origin, latestThreads, 0, {}, 0, "seconds 0:570", false, 1, true);
    AgcDriver::TraceFile::ReplaceGzip(path, events);
    const auto latest = ReadGzip(path);
    std::filesystem::remove_all(directory);
    Check(latest.find("\"snapshot_superseded\":1") != std::string::npos
        && latest.find("\"events_overwritten\":2") != std::string::npos
        && latest.find("Driver.Packet3") != std::string::npos
        && latest.find("Driver.Packet4") != std::string::npos
        && latest.find("Driver.Packet1") == std::string::npos
        && latest.find("\"gpu_track_available\":true") != std::string::npos
        && latest.find("\"events_overwritten\":1") != std::string::npos
        && latest.find("GPU.latest") != std::string::npos,
        "the replaced checkpoint did not account for supersession and retain newest bounded CPU/GPU events");
    Check(snapshot.retainedFrom == origin + milliseconds(3)
        && snapshot.retainedThrough == origin + milliseconds(4) + microseconds(30),
        "the checkpoint's retained timestamp range did not match the surviving events");
    const auto constrained = thread.Collect(1);
    const auto constrainedEvents = TraceFormat::Events(7, origin, std::vector<TraceThread::Snapshot>{constrained}, 0, {}, 0,
        "seconds 0:570", false, 1);
    Check(constrained.slices.size() == 1 && constrained.eventsOmitted == 1
        && constrained.eventsOmittedAt == origin + milliseconds(3)
        && constrainedEvents.find("\"events_omitted_in_snapshot\":1") != std::string::npos,
        "the bounded event snapshot did not report omitted older events and their first timestamp");
}

void BenchSwitch() {
    Check(!AgcDriver::TraceBench::Parse(nullptr, false), "the benchmark file was enabled without its switch");
    Check(AgcDriver::TraceBench::Parse("1", true), "APS5_TRACE_BENCH=1 was not accepted");
    for (const auto& [text, every] : {std::pair{"1", false}, std::pair{"0", true}, std::pair{"yes", true}, std::pair{"", true}}) {
        bool rejected = false;
        try { AgcDriver::TraceBench::Parse(text, every); }
        catch (const std::invalid_argument&) { rejected = true; }
        Check(rejected, "a malformed or unsupported APS5_TRACE_BENCH was accepted");
    }
}

void CaptureOwnsSnapshots() {
    auto capture = std::make_shared<AgcDriver::TraceCapture>();
    const auto origin = Clock::now();
    capture->process = 7;
    capture->summary.commit = "capture";
    capture->summary.trace = "aps5-trace-capture-7.json.gz";
    capture->summary.firstPresent = 5;
    capture->presents = {origin, origin + milliseconds(16)};
    std::string expectedEvents, expectedSummary;
    {
        std::string scope = "Driver.Packet", stage = "draw", gpuName = "batch";
        std::atomic<std::uint64_t> presents{1};
        TraceThread thread("queue worker 0x0", presents, 2);
        thread.Add(scope.c_str(), stage.c_str(), origin + milliseconds(2), origin + milliseconds(2) + microseconds(30));
        thread.Add(scope.c_str(), nullptr, origin + milliseconds(1), origin + milliseconds(1) + microseconds(40));
        thread.Add(scope.c_str(), nullptr, origin + milliseconds(3), origin + milliseconds(3) + microseconds(30));
        auto snapshot = thread.Collect();
        std::vector<TraceThread::Slice> gpu{{gpuName.c_str(), nullptr, origin + microseconds(5), microseconds(10), 42}};
        std::vector<TraceThread::Snapshot> snapshots{snapshot, {"gpu", gpu, {}, false}};
        expectedEvents = TraceFormat::Events(7, origin, snapshots, 0, capture->presents, 5);
        expectedSummary = TraceFormat::Summary(capture->summary, snapshots, capture->presents);
        for (auto& value : snapshots) capture->Add(std::move(value));
        thread.Add(scope.c_str(), nullptr, origin + milliseconds(4), origin + milliseconds(4) + microseconds(30));
        gpu.clear();
        scope.assign("changed scope");
        stage.assign("changed stage");
        gpuName.assign("changed gpu");
    }
    const auto events = TraceFormat::Events(capture->process, capture->presents.front(), capture->threads, capture->presenter, capture->presents, capture->summary.firstPresent);
    const auto traceEvents = events.find(",\"traceEvents\":[");
    const auto expectedTraceEvents = expectedEvents.find(",\"traceEvents\":[");
    Check(traceEvents != std::string::npos && expectedTraceEvents != std::string::npos
        && events.substr(traceEvents) == expectedEvents.substr(expectedTraceEvents), "closed capture borrowed slices or labels, changed ordering or included a late append");
    Check(TraceFormat::Summary(capture->summary, capture->threads, capture->presents) == expectedSummary, "closed capture changed totals or loss cutoff");
}

void CaptureWriterDrainAndFailure() {
    for (const bool fail : {false, true}) {
        auto capture = std::make_shared<AgcDriver::TraceCapture>();
        capture->process = 7;
        capture->summary.trace = "aps5-trace-capture-7.json.gz";
        const auto origin = Clock::now();
        capture->presents = {origin, origin + milliseconds(16)};
        std::weak_ptr<AgcDriver::TraceCapture> released = capture;
        unsigned calls = 0;
        std::string events, line;
        AgcDriver::ProfileOutput output([capture = std::move(capture), &calls, &events, &line, fail](std::string_view) mutable {
            auto closed = std::move(capture);
            ++calls;
            const auto text = TraceFormat::Events(closed->process, closed->presents.front(), closed->threads, closed->presenter, closed->presents, closed->summary.firstPresent);
            if (fail) throw std::runtime_error("closed capture write failed");
            events = text;
            line = TraceFormat::Summary(closed->summary, closed->threads, closed->presents);
        });
        Check(output.Write("aps5-trace-capture-7.json.gz"), "closed capture was not queued");
        output.Stop();
        bool failed = false;
        try { output.Flush(); }
        catch (const std::runtime_error& error) { failed = std::string_view(error.what()) == "closed capture write failed"; }
        Check(failed == fail && calls == 1 && released.expired(), "closed capture stop lost or repeated its job, hid a failure or retained its payload");
        Check(fail ? events.empty() && line.empty() : !events.empty() && line.find("aps5-trace-capture-7.json.gz") != std::string::npos, "closed capture events and summary lost their pairing");
    }
}

void GzipFile() {
    std::string text;
    for (int index = 0; index < 20000; ++index) text += "{\"name\":\"Driver.Packet\",\"ph\":\"X\",\"ts\":" + std::to_string(index) + "}\n";
    const auto stem = "aps5-trace-test-" + std::to_string(Clock::now().time_since_epoch().count());
    const auto path = std::filesystem::temp_directory_path() / (stem + ".json.gz");
    AgcDriver::TraceFile::WriteGzip(path, text);
    std::ifstream input(path, std::ios::binary);
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    std::filesystem::remove(path);
    Check(bytes.size() > 18 && bytes[0] == 0x1f && bytes[1] == 0x8b && bytes[2] == 8 && bytes[3] == 0, "the gzip header is wrong");
    z_stream stream{};
    Check(inflateInit2(&stream, -MAX_WBITS) == Z_OK, "inflateInit2 failed");
    std::string inflated(text.size() + 1, 0);
    stream.next_in = const_cast<Bytef*>(bytes.data() + 10);
    stream.avail_in = static_cast<uInt>(bytes.size() - 18);
    stream.next_out = reinterpret_cast<Bytef*>(inflated.data());
    stream.avail_out = static_cast<uInt>(inflated.size());
    const int status = inflate(&stream, Z_FINISH);
    inflated.resize(inflated.size() - stream.avail_out);
    const bool consumed = stream.avail_in == 0;
    inflateEnd(&stream);
    Check(status == Z_STREAM_END && consumed, "the deflate stream does not end at the trailer");
    Check(inflated == text, "the gzip file does not hold the text");
    const auto word = [&](std::size_t at) {
        std::uint32_t value = 0;
        for (unsigned index = 0; index < 4; ++index) value |= static_cast<std::uint32_t>(bytes[at + index]) << (8 * index);
        return value;
    };
    Check(word(bytes.size() - 8) == crc32(0, reinterpret_cast<const Bytef*>(text.data()), static_cast<uInt>(text.size())), "the CRC-32 is wrong");
    Check(word(bytes.size() - 4) == text.size(), "the size is wrong");
}

void AppendLines() {
    const auto path = std::filesystem::temp_directory_path() / ("aps5-summary-test-" + std::to_string(Clock::now().time_since_epoch().count()) + ".jsonl");
    AgcDriver::TraceFile::AppendLine(path, "{\"presents\":1}\n");
    AgcDriver::TraceFile::AppendLine(path, "{\"presents\":2}\n");
    std::ifstream input(path, std::ios::binary);
    const std::string written((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    std::filesystem::remove(path);
    Check(written == "{\"presents\":1}\n{\"presents\":2}\n", "summary lines were not appended");
}

}

int main(int argc, char** argv) {
    try {
#if defined(__linux__)
        if (argc == 2 && std::string_view(argv[1]) == "--request-close") {
            RequestSemaphoreSmoke(true);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--request-stop-waiting") {
            RequestSemaphoreSmoke(false, true);
            return 0;
        }
#endif
        Check(argc == 1, "unknown performance trace test mode");
        WindowByFrames();
        WindowBySeconds();
        WindowByRequest();
        WindowRequestRejects();
        WindowRejects();
        WindowTrigger();
        WindowEnvironment();
        BufferCap();
        LostEventsCount();
        TotalsOverflow();
        CapacitySettings();
        SnapshotBudgetFairShare();
        WaitEdgeBoundsAndWindow();
        FullBufferEvents();
        FoldShortSlices();
        EscapeStrings();
        Utf8Strings();
        ProgramScopeLifecycle();
        PresentBoundaryWitnesses();
        SummaryFields();
        GpuClock();
        IntervalDue();
        SummaryOnlyThread();
        TotalsSince();
        IntervalDroppedThreads();
        SummaryMachineFields();
        FrameMicroseconds();
        BenchFormat();
        BenchRotation();
        BenchWriterKeepsNewest();
        BenchWriterStopWrites();
        BenchSwitch();
        CaptureOwnsSnapshots();
        CaptureWriterDrainAndFailure();
        GzipFile();
        ReplaceGzipFile();
        RollingSnapshotCheckpoint();
        AppendLines();
#if defined(__linux__)
        RequestSemaphoreSmoke();
#endif
        std::cout << "performance trace tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
