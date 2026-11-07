#include "prx/libSceAgcDriver/Execution/include/Driver/Prepare/PrepareFarm.hpp"
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
namespace {
using Farm = Driver::PrepareFarm;
struct IntValue final : Farm::Result::Value {
  explicit IntValue(int x) : value(x) {}
  int value;
};
struct Gate {
  std::mutex m;
  std::condition_variable cv;
  bool entered = false, open = false;
  void Wait() {
    std::unique_lock l(m);
    cv.wait(l, [&] { return entered; });
  }
  void Release() {
    std::lock_guard l(m);
    open = true;
    cv.notify_all();
  }
};
struct Task final : Farm::Callable {
  int value;
  Gate *gate;
  bool throws;
  Task(int x, Gate *g = nullptr, bool t = false)
      : value(x), gate(g), throws(t) {}
  Farm::Result Run() override {
    if (gate) {
      std::unique_lock l(gate->m);
      gate->entered = true;
      gate->cv.notify_all();
      gate->cv.wait(l, [&] { return gate->open; });
    }
    if (throws)
      throw 1;
    Farm::Result r;
    r.value = std::make_unique<IntValue>(value);
    return r;
  }
};
struct Count final : Farm::Callable {
  std::atomic<int> &n;
  explicit Count(std::atomic<int> &x) : n(x) {}
  Farm::Result Run() override {
    ++n;
    return {};
  }
};
Farm::Job Job(unsigned q, unsigned long long s, std::size_t o, std::size_t b,
              int v, Gate *g = nullptr, bool t = false) {
  return {q, s, o, std::make_unique<Task>(v, g, t), b};
}
bool Check(bool x, const char *m) {
  if (!x)
    std::cerr << "PrepareFarm: " << m << '\n';
  return x;
}
} // namespace
int main() {
  bool ok = true;
  {
    Farm f({1, 8, 8});
    Gate g;
    ok &=
        Check(f.Submit(Job(1, 1, 0, 1, 0, &g)) == Farm::SubmitStatus::Accepted,
              "blocker submit");
    g.Wait();
    std::atomic<int> n{0};
    auto j = Job(1, 1, 1, 1, 0);
    j.callable = std::make_unique<Count>(n);
    ok &= Check(f.Submit(std::move(j)) == Farm::SubmitStatus::Accepted,
                "queued submit");
    auto r = f.Take(1, 1, 1);
    ok &= Check(r.status == Farm::TakeStatus::Cancelled && n == 0,
                "queued cancellation before claim");
    g.Release();
    f.Shutdown();
  }
  {
    Farm f({1, 8, 4});
    Gate g;
    ok &=
        Check(f.Submit(Job(2, 1, 1, 4, 7, &g)) == Farm::SubmitStatus::Accepted,
              "running submit");
    g.Wait();
    ok &= Check(f.Take(2, 1, 1).status == Farm::TakeStatus::NotReady,
                "running take nonblocking and discarding");
    ok &= Check(f.Submit(Job(2, 1, 2, 1, 8)) == Farm::SubmitStatus::Rejected,
                "credit retained while running");
    g.Release();
    f.WaitSettledForTest(2, 1, 1);
    ok &= Check(f.Take(2, 1, 1).status == Farm::TakeStatus::Cancelled,
                "running result discarded");
    ok &= Check(f.Submit(Job(2, 1, 2, 4, 8)) == Farm::SubmitStatus::Accepted,
                "credit returned after worker ends");
    f.Shutdown();
  }
  {
    Farm f({1, 8, 8});
    ok &= Check(f.Submit(Job(3, 9, 4, 1, 42)) == Farm::SubmitStatus::Accepted,
                "ready submit");
    f.WaitSettledForTest(3, 9, 4);
    auto r = f.Take(3, 9, 4);
    ok &= Check(r.status == Farm::TakeStatus::Ready &&
                    static_cast<IntValue *>(r.result.value.get())->value == 42,
                "ready result adopted");
    ok &= Check(f.Take(3, 9, 4).status == Farm::TakeStatus::Absent,
                "consumed exactly once");
  }
  {
    Farm f({1, 8, 2});
    Gate g;
    ok &=
        Check(f.Submit(Job(4, 1, 1, 2, 1, &g)) == Farm::SubmitStatus::Accepted,
              "budget fill");
    g.Wait();
    ok &= Check(f.Submit(Job(4, 1, 2, 1, 2)) == Farm::SubmitStatus::Rejected,
                "budget exhaustion");
    g.Release();
    f.WaitSettledForTest(4, 1, 1);
    ok &= Check(f.Take(4, 1, 1).status == Farm::TakeStatus::Ready,
                "budget job ready");
    ok &= Check(f.Submit(Job(4, 1, 3, 2, 3)) == Farm::SubmitStatus::Accepted,
                "credit reusable");
    f.Shutdown();
  }
  {
    Farm f({1, 8, 8});
    ok &= Check(f.Submit(Job(5, 1, 1, 1, 0, nullptr, true)) ==
                    Farm::SubmitStatus::Accepted,
                "exception submit");
    f.WaitSettledForTest(5, 1, 1);
    ok &= Check(f.Take(5, 1, 1).status == Farm::TakeStatus::Failed,
                "exception becomes failed");
    ok &= Check(f.Submit(Job(5, 1, 2, 1, 9)) == Farm::SubmitStatus::Accepted,
                "pool survives exception");
    f.Shutdown();
  }
  {
    Farm f({1, 8, 8});
    Gate g;
    ok &=
        Check(f.Submit(Job(6, 1, 1, 1, 1, &g)) == Farm::SubmitStatus::Accepted,
              "shutdown running submit");
    g.Wait();
    ok &= Check(f.Submit(Job(6, 1, 2, 1, 2)) == Farm::SubmitStatus::Accepted,
                "shutdown queued submit");
    std::thread opener([&] { g.Release(); });
    f.Shutdown();
    opener.join();
    ok &= Check(f.Take(6, 1, 1).status != Farm::TakeStatus::Ready &&
                    f.Take(6, 1, 2).status == Farm::TakeStatus::Cancelled,
                "shutdown cancels and joins");
  }
  {
    Farm f({0, 8, 8});
    ok &= Check(f.Submit(Job(7, 1, 1, 1, 1)) == Farm::SubmitStatus::Rejected,
                "zero workers rejects");
  }
  return ok ? 0 : 1;
}
