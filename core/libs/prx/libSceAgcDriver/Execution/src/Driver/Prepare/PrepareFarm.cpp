#include "prx/libSceAgcDriver/Execution/include/Driver/Prepare/PrepareFarm.hpp"

#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
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
    Result result;
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
      bool failed = false;
      try {
        result = e->job.callable->Run();
      } catch (...) {
        failed = true;
      }
      e->job.callable.reset();
      {
        std::lock_guard lock(mutex);
        if (e->discardRequested || stopping) {
          result = {};
          e->result = {};
          e->state = State::Discarded;
          ++counters.discarded;
        } else if (failed)
          e->state = State::Failed;
        else {
          e->result = std::move(result);
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
};

PrepareFarm::PrepareFarm(Config config)
    : impl_(std::make_unique<Impl>(config)) {}
PrepareFarm::~PrepareFarm() { Shutdown(); }
PrepareFarm &PrepareFarm::Get() {
  static PrepareFarm f({EnvSize("APS5_PREPARE_WORKERS", 0),
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
      case Impl::State::Ready:
        e->state = Impl::State::Consumed;
        ++p.counters.adopted;
        return {TakeStatus::Ready, std::move(e->result)};
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
      } else if (e->state == Impl::State::Running)
        e->discardRequested = true;
    }
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
void PrepareFarm::Shutdown() {
  if (impl_)
    impl_->Stop();
}
} // namespace Driver
