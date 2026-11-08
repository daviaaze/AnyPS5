#include "prx/libSceAgcDriver/Execution/include/Driver/Prepare/PrepareFarm.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <deque>
#include <mutex>
#include <exception>
#include <thread>
#include <utility>
#include <vector>


namespace Driver {
namespace {
std::size_t EnvSize(const char *name, std::size_t fallback) {
  const char *text = std::getenv(name);
  if (!text || !*text)
    return fallback;
  char *end = nullptr;
  const auto value = std::strtoull(text, &end, 10);
  return end == text || *end ? fallback : static_cast<std::size_t>(value);
}
} // namespace

struct PrepareFarm::Impl {
  enum class State { Queued, Running, Ready, Failed, Consumed, Discarded };
  struct Entry {
    Job job;
    State state = State::Queued;
    bool discardRequested = false;
    std::shared_ptr<Result> result;
    std::string failure;
    std::size_t credit = 0;
  };
  explicit Impl(Config c) : config(c) {
    for (std::size_t i = 0; i < config.workers; ++i)
      workers.emplace_back([this] { Worker(); });
  }
  ~Impl() { Stop(); }
  static bool Same(const Entry &e, std::uint32_t q, std::uint64_t s,
                   std::size_t o) {
    return e.job.queue == q && e.job.submissionSerial == s &&
           e.job.packetOffset == o;
  }
  void Worker() {
    for (;;) {
      std::shared_ptr<Entry> e;
      {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return stopping || !queue.empty(); });
        if (stopping && queue.empty())
          return;
        e = std::move(queue.front());
        queue.pop_front();
        if (e->state != State::Queued)
          continue;
        e->state = State::Running;
      }
      Result result;
      std::string failure;
      bool failed = false;
      try {
        result = e->job.callable->Run();
      } catch (const std::exception &error) {
        failed = true;
        failure = error.what();
      } catch (...) {
        failed = true;
        failure = "unknown background preparation exception";
      }
      e->job.callable.reset();
      {
        std::lock_guard lock(mutex);
        if (e->discardRequested || stopping) {
          result = {};
          e->result.reset();
          e->state = State::Discarded;
          ++counters.discarded;
        } else if (failed) {
          e->failure = std::move(failure);
          lastError = e->failure;
          e->state = State::Failed;
        }
        else {
          e->result = std::make_shared<Result>(std::move(result));
          e->state = State::Ready;
        }
        usedBytes -= e->credit;
        e->credit = 0;
        cv.notify_all();
      }
    }
  }
  void Stop() {
    {
      std::lock_guard lock(mutex);
      if (stopping)
        return;
      stopping = true;
      for (auto &e : entries) {
        if (e->state == State::Queued) {
          e->job.callable.reset();
          usedBytes -= e->credit;
          e->credit = 0;
          e->state = State::Discarded;
          ++counters.discarded;
        } else if (e->state == State::Running)
          e->discardRequested = true;
      }
      queue.clear();
      cv.notify_all();
    }
    for (auto &t : workers)
      if (t.joinable())
        t.join();
    workers.clear();
  }
  Config config;
  mutable std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::shared_ptr<Entry>> queue;
  std::vector<std::shared_ptr<Entry>> entries;
  std::vector<std::thread> workers;
  std::size_t usedBytes = 0;
  bool stopping = false;
  Counters counters;
  std::array<std::uint64_t, 6> adoption{};
  std::chrono::steady_clock::time_point reported = std::chrono::steady_clock::now();
  std::string lastError;
  BeforeTakeHookForTest beforeTakeForTest = nullptr;
};

PrepareFarm::PrepareFarm(Config config)
    : impl_(std::make_unique<Impl>(config)) {}
PrepareFarm::~PrepareFarm() { Shutdown(); }
PrepareFarm &PrepareFarm::Get() {
  static PrepareFarm f({std::getenv("APS5_PREPARE_WORKERS_8") != nullptr
                            ? 8u : EnvSize("APS5_PREPARE_WORKERS", 0),
                        EnvSize("APS5_PREPARE_LOOKAHEAD", 64),
                        EnvSize("APS5_PREPARE_BYTES_MIB", 64) * 1024u * 1024u});
  return f;
}
PrepareFarm::SubmitStatus PrepareFarm::Submit(Job job) {
  auto &p = *impl_;
  std::lock_guard lock(p.mutex);
  if (p.stopping || p.config.workers == 0 || !job.callable ||
      p.queue.size() >= p.config.queueDepth ||
      p.usedBytes > p.config.byteBudget ||
      job.byteReservation > p.config.byteBudget - p.usedBytes) {
    ++p.counters.rejectedSaturated;
    return SubmitStatus::Rejected;
  }
  for (const auto &e : p.entries)
    if (Impl::Same(*e, job.queue, job.submissionSerial, job.packetOffset) &&
        e->state != Impl::State::Consumed &&
        e->state != Impl::State::Discarded) {
      ++p.counters.duplicateInline;
      return SubmitStatus::Rejected;
    }
  auto e = std::make_shared<Impl::Entry>();
  e->credit = job.byteReservation;
  p.usedBytes += e->credit;
  e->job = std::move(job);
  p.entries.push_back(e);
  p.queue.push_back(e);
  ++p.counters.submitted;
  p.cv.notify_one();
  return SubmitStatus::Accepted;
}
PrepareFarm::TakeResult PrepareFarm::Take(std::uint32_t q, std::uint64_t s,
                                          std::size_t o) {
  auto &p = *impl_;
  std::lock_guard lock(p.mutex);
  for (auto &e : p.entries)
    if (Impl::Same(*e, q, s, o))
      switch (e->state) {
      case Impl::State::Queued:
        e->state = Impl::State::Discarded;
        e->job.callable.reset();
        p.usedBytes -= e->credit;
        e->credit = 0;
        ++p.counters.discarded;
        return {TakeStatus::Cancelled, {}};
      case Impl::State::Running:
        e->discardRequested = true;
        return {TakeStatus::NotReady, {}};
      case Impl::State::Ready: {
        e->state = Impl::State::Consumed;
        ++p.counters.adopted;
        Result result = e->result != nullptr ? std::move(*e->result) : Result{};
        e->result.reset();
        return {TakeStatus::Ready, std::move(result)};
      }
      case Impl::State::Failed:
        e->state = Impl::State::Consumed;
        return {TakeStatus::Failed, {}};
      case Impl::State::Discarded:
        return {TakeStatus::Cancelled, {}};
      case Impl::State::Consumed:
        return {TakeStatus::Absent, {}};
      }
  return {TakeStatus::Absent, {}};
}
PrepareFarm::PeekResult PrepareFarm::Peek(std::uint32_t q, std::uint64_t s,
                                         std::size_t o) const {
  const auto &p = *impl_;
  std::lock_guard lock(p.mutex);
  for (const auto &e : p.entries) {
    if (!Impl::Same(*e, q, s, o))
      continue;
    switch (e->state) {
    case Impl::State::Queued: return {PeekStatus::Queued, nullptr};
    case Impl::State::Running: return {PeekStatus::Running, nullptr};
    case Impl::State::Ready: return {PeekStatus::Ready, e->result};
    case Impl::State::Failed: return {PeekStatus::Failed, nullptr};
    case Impl::State::Consumed:
    case Impl::State::Discarded: return {PeekStatus::Absent, nullptr};
    }
  }
  return {PeekStatus::Absent, nullptr};
}

void PrepareFarm::Cancel(std::uint32_t q) {
  auto &p = *impl_;
  std::lock_guard lock(p.mutex);
  for (auto &e : p.entries)
    if (e->job.queue == q) {
      if (e->state == Impl::State::Queued) {
        e->state = Impl::State::Discarded;
        e->job.callable.reset();
        p.usedBytes -= e->credit;
        e->credit = 0;
        ++p.counters.discarded;
      } else if (e->state == Impl::State::Running) {
        e->discardRequested = true;
      } else if (e->state == Impl::State::Ready) {
        e->result.reset();
        e->state = Impl::State::Discarded;
        ++p.counters.discarded;
      } else if (e->state == Impl::State::Failed) {
        e->state = Impl::State::Discarded;
      }
    }
  p.cv.notify_all();
}
void PrepareFarm::CancelAll() {
  auto &p = *impl_;
  std::lock_guard lock(p.mutex);
  for (auto &e : p.entries) {
    if (e->state == Impl::State::Queued) {
      e->state = Impl::State::Discarded;
      e->job.callable.reset();
      p.usedBytes -= e->credit;
      e->credit = 0;
      ++p.counters.discarded;
    } else if (e->state == Impl::State::Running) {
      e->discardRequested = true;
    } else if (e->state == Impl::State::Ready) {
      e->result.reset();
      e->state = Impl::State::Discarded;
      ++p.counters.discarded;
    } else if (e->state == Impl::State::Failed) {
      e->state = Impl::State::Discarded;
    }
  }
  p.queue.clear();
  p.cv.notify_all();
}
void PrepareFarm::WaitSettledForTest(std::uint32_t q, std::uint64_t s,
                                     std::size_t o) {
  auto &p = *impl_;
  std::unique_lock lock(p.mutex);
  p.cv.wait(lock, [&] {
    for (const auto &e : p.entries)
      if (Impl::Same(*e, q, s, o))
        return e->state == Impl::State::Ready ||
               e->state == Impl::State::Failed ||
               e->state == Impl::State::Discarded;
    return true;
  });
}
PrepareFarm::Counters PrepareFarm::GetCounters() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->counters;
}
std::string PrepareFarm::GetLastErrorForTest() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->lastError;
}
PrepareFarm::AdoptionCounters PrepareFarm::GetAdoptionCounters() const {
  std::lock_guard lock(impl_->mutex);
  return {impl_->adoption[0], impl_->adoption[1], impl_->adoption[2],
          impl_->adoption[3], impl_->adoption[4], impl_->adoption[5]};
}
void PrepareFarm::SetBeforeTakeHookForTest(BeforeTakeHookForTest hook) {
  std::lock_guard lock(impl_->mutex);
  impl_->beforeTakeForTest = hook;
}
void PrepareFarm::BeforeTakeForTest(std::uint32_t q, std::uint64_t s,
                                    std::size_t o) const {
  BeforeTakeHookForTest hook = nullptr;
  {
    std::lock_guard lock(impl_->mutex);
    hook = impl_->beforeTakeForTest;
  }
  if (hook != nullptr) hook(q, s, o);
}
bool PrepareFarm::Enabled() const {
  std::lock_guard lock(impl_->mutex);
  return !impl_->stopping && impl_->config.workers != 0;
}
void PrepareFarm::ConfigureForTest(Config config) {
  Shutdown();
  impl_ = std::make_unique<Impl>(config);
}
void PrepareFarm::NoteAdoption(const char *reason) {
  static constexpr const char *names[] = {"adopted", "absent", "not-ready",
      "stale-key", "stale-capture", "failed"};
  std::lock_guard lock(impl_->mutex);
  for (std::size_t i = 0; i < std::size(names); ++i)
    if (std::strcmp(reason, names[i]) == 0) ++impl_->adoption[i];
}
void PrepareFarm::ReportStats() {
  static const bool enabled = [] {
    const char *value = std::getenv("APS5_PREPARE_STATS");
    return value != nullptr && std::strcmp(value, "1") == 0;
  }();
  auto &p = *impl_;
  std::lock_guard lock(p.mutex);
  const auto now = std::chrono::steady_clock::now();
  if (now - p.reported < std::chrono::seconds(10)) return;
  std::fprintf(stderr, "[prepare-farm] submitted=%llu adopted=%llu discarded=%llu rejected=%llu duplicate=%llu adopted_outcome=%llu absent=%llu not_ready=%llu stale_key=%llu stale_capture=%llu failed=%llu\n",
      static_cast<unsigned long long>(p.counters.submitted),
      static_cast<unsigned long long>(p.counters.adopted),
      static_cast<unsigned long long>(p.counters.discarded),
      static_cast<unsigned long long>(p.counters.rejectedSaturated),
      static_cast<unsigned long long>(p.counters.duplicateInline),
      static_cast<unsigned long long>(p.adoption[0]), static_cast<unsigned long long>(p.adoption[1]),
      static_cast<unsigned long long>(p.adoption[2]), static_cast<unsigned long long>(p.adoption[3]),
      static_cast<unsigned long long>(p.adoption[4]), static_cast<unsigned long long>(p.adoption[5]));
  p.adoption = {};
  p.reported = now;
}
void PrepareFarm::Shutdown() {
  if (impl_)
    impl_->Stop();
}
} // namespace Driver
