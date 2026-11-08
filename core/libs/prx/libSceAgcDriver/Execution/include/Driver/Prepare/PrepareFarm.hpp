#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace Driver {

class PrepareFarm {
public:
  struct Result {
    struct Value {
      virtual ~Value() = default;
    };
    std::unique_ptr<Value> value;
  };

  struct Callable {
    virtual ~Callable() = default;
    virtual Result Run() = 0;
  };

  struct Job {
    std::uint32_t queue = 0;
    std::uint64_t submissionSerial = 0;
    std::size_t packetOffset = 0;
    std::unique_ptr<Callable> callable;
    std::size_t byteReservation = 0;
  };

  enum class SubmitStatus { Accepted, Rejected };
  enum class TakeStatus { Ready, NotReady, Failed, Cancelled, Absent };
  enum class PeekStatus { Queued, Running, Ready, Failed, Absent };
  struct PeekResult {
    PeekStatus status = PeekStatus::Absent;
    std::shared_ptr<const Result> result;
  };
  struct TakeResult {
    TakeStatus status = TakeStatus::Absent;
    Result result;
  };
  struct Counters {
    std::uint64_t submitted = 0;
    std::uint64_t adopted = 0;
    std::uint64_t discarded = 0;
    std::uint64_t rejectedSaturated = 0;
    std::uint64_t duplicateInline = 0;
  };
  struct AdoptionCounters {
    std::uint64_t adopted = 0;
    std::uint64_t absent = 0;
    std::uint64_t notReady = 0;
    std::uint64_t staleKey = 0;
    std::uint64_t staleCapture = 0;
    std::uint64_t failed = 0;
  };
  struct Config {
    std::size_t workers = 0;
    std::size_t queueDepth = 64;
    std::size_t byteBudget = 64u * 1024u * 1024u;
  };

  static PrepareFarm &Get();
  explicit PrepareFarm(Config config);
  ~PrepareFarm();
  PrepareFarm(const PrepareFarm &) = delete;
  PrepareFarm &operator=(const PrepareFarm &) = delete;

  SubmitStatus Submit(Job job);
  TakeResult Take(std::uint32_t queue, std::uint64_t serial,
                  std::size_t offset);
  PeekResult Peek(std::uint32_t queue, std::uint64_t serial,
                  std::size_t offset) const;
  void Cancel(std::uint32_t queue);
  void CancelAll();
  // Test-only synchronization; production callers must never wait on
  // preparation.
  void WaitSettledForTest(std::uint32_t queue, std::uint64_t serial,
                          std::size_t offset);
  Counters GetCounters() const;
  AdoptionCounters GetAdoptionCounters() const;
  bool Enabled() const;
  std::string GetLastErrorForTest() const;
  void ConfigureForTest(Config config);
  void NoteAdoption(const char *reason);
  using BeforeTakeHookForTest = void (*)(std::uint32_t, std::uint64_t, std::size_t);
  void SetBeforeTakeHookForTest(BeforeTakeHookForTest hook);
  void BeforeTakeForTest(std::uint32_t queue, std::uint64_t serial,
                         std::size_t offset) const;
  void ReportStats();
  void Shutdown();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace Driver
