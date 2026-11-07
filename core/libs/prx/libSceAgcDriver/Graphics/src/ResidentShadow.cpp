#include "prx/libSceAgcDriver/Graphics/include/ResidentShadow.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string_view>
#include <tuple>

namespace AgcDriver::Graphics {
namespace {
using Key = std::tuple<std::uintptr_t, std::uint64_t, std::uint64_t,
                       std::uint64_t, std::uintptr_t, std::uint64_t,
                       std::uint64_t, std::uint64_t, std::uint64_t>;
struct Entry {
  std::weak_ptr<Buffer> buffer;
  std::uint64_t touched = 0;
  std::uint64_t producerSerial = 0;
  std::uint64_t publishedGeneration = 0;
  std::uint64_t completedUnhookedEpoch = 0;
  bool producerCompleted = false;
  bool gpuWritten = false;
  std::size_t cost = sizeof(Entry);
};
struct State {
  std::mutex mutex;
  std::map<Key, Entry> entries;
  ResidentShadowStats stats;
  std::uint64_t clock = 0;
  std::uint64_t unhookedWriteEpoch = 0;
};
State &state() { static State s; return s; }
std::uint64_t budget() {
  static const auto value = [] {
    const char *p = std::getenv("APS5_RESIDENT_SHADOW_MIB");
    return std::uint64_t(p ? std::strtoull(p, nullptr, 10) : 64) * 1024 * 1024;
  }();
  return value;
}
Key key(const ResidentShadowIdentity &i) {
  return {reinterpret_cast<std::uintptr_t>(i.device), i.allocationAddress,
          i.allocationBytes, i.backingSerial,
          reinterpret_cast<std::uintptr_t>(i.importBuffer), i.importBase,
          i.importEpoch, i.begin, i.end};
}
bool known(const ResidentShadowIdentity &i) {
  return i.device != VK_NULL_HANDLE && i.allocationAddress && i.allocationBytes &&
         i.backingSerial && i.importBuffer != VK_NULL_HANDLE && i.importEpoch &&
         i.begin < i.end && i.begin >= i.allocationAddress &&
         i.begin - i.allocationAddress <= i.allocationBytes &&
         i.end - i.begin <= i.allocationBytes - (i.begin - i.allocationAddress);
}
void report(State &s) {
  static auto last = std::uint64_t{};
  const auto now = std::uint64_t(std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
  if (now - last < 10) return;
  last = now;
  std::fprintf(stderr,
      "[resident-shadow] entries=%llu bytes=%llu observed=%llu copyin=%llu copyback=%llu retired=%llu rejected=%llu evicted=%llu reusable-would-skip=%llu reusable-bytes=%llu not-completed=%llu cpu-write=%llu gpu-write=%llu unwatched=%llu unhooked-write=%llu retired-reject=%llu\n",
      (unsigned long long)s.entries.size(), (unsigned long long)s.stats.liveBytes,
      (unsigned long long)s.stats.observations, (unsigned long long)s.stats.copyIns,
      (unsigned long long)s.stats.copyBacks, (unsigned long long)s.stats.retired,
      (unsigned long long)s.stats.rejectedIdentity, (unsigned long long)s.stats.evictions,
      (unsigned long long)s.stats.reusableWouldSkip,
      (unsigned long long)s.stats.reusableWouldSkipBytes,
      (unsigned long long)s.stats.rejected[0], (unsigned long long)s.stats.rejected[1],
      (unsigned long long)s.stats.rejected[2], (unsigned long long)s.stats.rejected[3],
      (unsigned long long)s.stats.rejected[4], (unsigned long long)s.stats.rejected[5]);
}
void reject(State &s, ResidentShadowReject why) {
  ++s.stats.rejected[static_cast<std::size_t>(why)];
}
}
ResidentShadowMode GetResidentShadowMode() {
  static const ResidentShadowMode mode = [] {
    const char *p = std::getenv("APS5_RESIDENT_SHADOW");
    if (p == nullptr) return ResidentShadowMode::Off;
    const std::string_view value(p);
    return value == "observe" || value == "1" ? ResidentShadowMode::Observe :
           value == "eager" ? ResidentShadowMode::Eager : ResidentShadowMode::Off;
  }();
  return mode;
}
void ResidentShadowObserveCopyIn(const ResidentShadowIdentity &id,
                                 const std::shared_ptr<Buffer> &buffer) {
  if (GetResidentShadowMode() == ResidentShadowMode::Off) return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!known(id)) { ++s.stats.rejectedIdentity; report(s); return; }
  auto k = key(id);
  auto it = s.entries.find(k);
  if (it == s.entries.end()) {
    const auto cost = sizeof(Entry);
    while (s.stats.liveBytes + cost > budget()) {
      auto victim = s.entries.end();
      for (auto p = s.entries.begin(); p != s.entries.end(); ++p)
        if (p->second.buffer.expired() && (victim == s.entries.end() ||
             p->second.touched < victim->second.touched)) victim = p;
      if (victim == s.entries.end()) break;
      s.stats.liveBytes -= victim->second.cost;
      s.entries.erase(victim);
      ++s.stats.evictions;
    }
    if (s.stats.liveBytes + cost <= budget()) {
      Entry e;
      e.buffer = buffer;
      e.touched = ++s.clock;
      e.cost = cost;
      s.stats.liveBytes += cost;
      s.entries.emplace(k, std::move(e));
    }
  } else {
    it->second.buffer = buffer;
    it->second.touched = ++s.clock;
  }
  ++s.stats.observations;
  ++s.stats.copyIns;
  report(s);
}
void ResidentShadowObserveCopyBack(const ResidentShadowIdentity &id,
                                   std::uint64_t token) {
  if (GetResidentShadowMode() == ResidentShadowMode::Off) return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  auto it = s.entries.find(key(id));
  if (it == s.entries.end()) return;
  it->second.producerSerial = token;
  it->second.producerCompleted = false;
  it->second.publishedGeneration = 0;
  it->second.gpuWritten = false;
  it->second.touched = ++s.clock;
  ++s.stats.copyBacks;
  report(s);
}
void ResidentShadowCopyBackCompleted(const ResidentShadowIdentity &id, std::uint64_t serial) {
  if (GetResidentShadowMode() == ResidentShadowMode::Off) return;
  const auto generation = GuestMemory::CollectWritesUncached(
      id.begin, static_cast<std::size_t>(id.end - id.begin));
  auto &s = state();
  std::lock_guard lock(s.mutex);
  auto it = s.entries.find(key(id));
  if (it == s.entries.end()) return;
  it->second.producerSerial = serial;
  it->second.gpuWritten = false;
  it->second.completedUnhookedEpoch = s.unhookedWriteEpoch;
  it->second.producerCompleted = true;
  it->second.publishedGeneration = generation;
}
bool ResidentShadowProducerCompleted(const ResidentShadowIdentity &id) {
  if (GetResidentShadowMode() == ResidentShadowMode::Off) return false;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  auto it = s.entries.find(key(id));
  return it != s.entries.end() && it->second.producerCompleted &&
         it->second.producerSerial != 0;
}
bool ResidentShadowCpuUnchanged(const ResidentShadowIdentity &id) {
  if (GetResidentShadowMode() == ResidentShadowMode::Off) return false;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  auto it = s.entries.find(key(id));
  return it != s.entries.end() && it->second.publishedGeneration != 0 &&
      GuestMemory::UnchangedSince(id.begin, static_cast<std::size_t>(id.end-id.begin),
                                  it->second.publishedGeneration);
}
bool ResidentShadowReusable(const ResidentShadowIdentity &id,
                            const ResidentShadowIdentity &current) {
  if (GetResidentShadowMode() == ResidentShadowMode::Off) return false;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  auto it = s.entries.find(key(id));
  if (it == s.entries.end()) { reject(s, ResidentShadowReject::Retired); return false; }
  if (key(id) != key(current) || id.end-id.begin != current.end-current.begin ||
      id.device != current.device) { reject(s, ResidentShadowReject::Retired); return false; }
  auto &e = it->second;
  if (!e.producerCompleted) {
    reject(s, ResidentShadowReject::NotCompleted); return false;
  }
  if (e.publishedGeneration == 0) { reject(s, ResidentShadowReject::Unwatched); return false; }
  if (e.completedUnhookedEpoch != s.unhookedWriteEpoch) {
    reject(s, ResidentShadowReject::UnhookedWrite); return false;
  }
  if (e.gpuWritten) { reject(s, ResidentShadowReject::GpuWrite); return false; }
  if (!GuestMemory::UnchangedSince(id.begin, static_cast<std::size_t>(id.end-id.begin),
                                  e.publishedGeneration)) {
    reject(s, ResidentShadowReject::CpuWrite); return false;
  }
  ++s.stats.reusableWouldSkip;
  s.stats.reusableWouldSkipBytes += id.end-id.begin;
  report(s);
  return true;
}
void ResidentShadowObserveGpuWrite(std::uint64_t address, std::size_t bytes) {
  if (GetResidentShadowMode() == ResidentShadowMode::Off || bytes == 0) return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  const auto end = address + bytes;
  for (auto &[entryKey, e] : s.entries) {
    const auto begin = std::get<1>(entryKey);
    const auto bytesInAllocation = std::get<2>(entryKey);
    if (address < begin + bytesInAllocation && begin < end) e.gpuWritten = true;
  }
}
void ResidentShadowNoteUnhookedWrite() {
  if (GetResidentShadowMode() == ResidentShadowMode::Off) return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  ++s.unhookedWriteEpoch;
}
void ResidentShadowRetireImport(VkDevice device, VkBuffer buffer, std::uint64_t epoch) {
  if (GetResidentShadowMode() == ResidentShadowMode::Off) return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  for (auto i=s.entries.begin(); i!=s.entries.end();)
    if (std::get<0>(i->first)==reinterpret_cast<std::uintptr_t>(device) &&
        std::get<4>(i->first)==reinterpret_cast<std::uintptr_t>(buffer) &&
        std::get<6>(i->first)==epoch) {
      s.stats.liveBytes -= i->second.cost;
      i=s.entries.erase(i); ++s.stats.retired;
    } else ++i;
  report(s);
}
ResidentShadowStats ResidentShadowCounters() {
  auto &s=state(); std::lock_guard lock(s.mutex); return s.stats;
}
} // namespace AgcDriver::Graphics
