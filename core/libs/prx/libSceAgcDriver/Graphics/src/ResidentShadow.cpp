#include "prx/libSceAgcDriver/Graphics/include/ResidentShadow.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include <algorithm>
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
  ResidentShadowVersion version;
  std::weak_ptr<Buffer> buffer;
  std::uint64_t touched = 0;
  std::size_t cost = sizeof(Entry);
};
struct State {
  std::mutex mutex;
  std::map<Key, Entry> entries;
  ResidentShadowStats stats;
  std::uint64_t clock = 0;
};
State &state() {
  static State s;
  return s;
}
std::uint64_t budget() {
  static const auto value = [] {
    const char *p = std::getenv("APS5_RESIDENT_SHADOW_MIB");
    return std::uint64_t(p ? std::strtoull(p, nullptr, 10) : 64) * 1024 * 1024;
  }();
  return value;
}
Key key(const ResidentShadowIdentity &i) {
  return {reinterpret_cast<std::uintptr_t>(i.device),
          i.allocationAddress,
          i.allocationBytes,
          i.backingSerial,
          reinterpret_cast<std::uintptr_t>(i.importBuffer),
          i.importBase,
          i.importEpoch,
          i.begin,
          i.end};
}
bool known(const ResidentShadowIdentity &i) {
  return i.device != VK_NULL_HANDLE && i.allocationAddress &&
         i.allocationBytes && i.backingSerial &&
         i.importBuffer != VK_NULL_HANDLE && i.importEpoch && i.begin < i.end &&
         i.begin >= i.allocationAddress &&
         i.begin - i.allocationAddress <= i.allocationBytes &&
         i.end - i.begin <= i.allocationBytes - (i.begin - i.allocationAddress);
}
void report(State &s) {
  static auto last = std::uint64_t{};
  const auto now =
      std::uint64_t(std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
  if (now - last < 10)
    return;
  last = now;
  std::fprintf(
      stderr,
      "[resident-shadow] entries=%llu bytes=%llu observed=%llu copyin=%llu "
      "copyback=%llu retired=%llu rejected=%llu evicted=%llu\n",
      (unsigned long long)s.entries.size(),
      (unsigned long long)s.stats.liveBytes,
      (unsigned long long)s.stats.observations,
      (unsigned long long)s.stats.copyIns,
      (unsigned long long)s.stats.copyBacks,
      (unsigned long long)s.stats.retired,
      (unsigned long long)s.stats.rejectedIdentity,
      (unsigned long long)s.stats.evictions);
}
} // namespace
constexpr std::size_t maxIntervals = 64;
void setDirty(ResidentShadowVersion &target,
              std::span<const std::pair<std::uint64_t, std::uint64_t>> dirty) {
  target.dirty.clear();
  const auto count = std::min(dirty.size(), maxIntervals);
  target.dirty.insert(target.dirty.end(), dirty.begin(), dirty.begin() + count);
  target.dirtyTruncated = dirty.size() > maxIntervals;
}
ResidentShadowMode GetResidentShadowMode() {
  static const bool enabled =
      std::getenv("APS5_RESIDENT_SHADOW") &&
      std::string_view(std::getenv("APS5_RESIDENT_SHADOW")) == "observe";
  return enabled ? ResidentShadowMode::Observe : ResidentShadowMode::Off;
}
void ResidentShadowObserveCopyIn(const ResidentShadowIdentity &id,
                                 const ResidentShadowVersion &version,
                                 const std::shared_ptr<Buffer> &buffer) {
  if (GetResidentShadowMode() == ResidentShadowMode::Off)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!known(id)) {
    ++s.stats.rejectedIdentity;
    report(s);
    return;
  }
  auto k = key(id);
  auto it = s.entries.find(k);
  if (it == s.entries.end()) {
    const auto cost = sizeof(Entry) + maxIntervals * sizeof(version.dirty[0]);
    while (s.stats.liveBytes + cost > budget()) {
      auto victim = s.entries.end();
      for (auto p = s.entries.begin(); p != s.entries.end(); ++p)
        if (p->second.buffer.expired() &&
            (victim == s.entries.end() ||
             p->second.touched < victim->second.touched))
          victim = p;
      if (victim == s.entries.end())
        break;
      s.stats.liveBytes -= victim->second.cost;
      s.entries.erase(victim);
      ++s.stats.evictions;
    }
    if (s.stats.liveBytes + cost <= budget()) {
      Entry e;
      e.version.observedCpu = version.observedCpu;
      e.buffer = buffer;
      e.touched = ++s.clock;
      e.cost = cost;
      e.version.dirty.reserve(maxIntervals);
      s.stats.liveBytes += cost;
      s.entries.emplace(k, std::move(e));
    }
  } else {
    it->second.version.observedCpu = version.observedCpu;
    it->second.buffer = buffer;
    it->second.touched = ++s.clock;
  }
  ++s.stats.observations;
  ++s.stats.copyIns;
  report(s);
}
void ResidentShadowObserveCopyBack(
    const ResidentShadowIdentity &id, std::uint64_t token,
    std::span<const std::pair<std::uint64_t, std::uint64_t>> dirty) {
  if (GetResidentShadowMode() == ResidentShadowMode::Off)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  auto it = s.entries.find(key(id));
  if (it == s.entries.end())
    return;
  it->second.version.batchToken = token;
  it->second.version.logicalGpuProducer++;
  setDirty(it->second.version, dirty);
  it->second.touched = ++s.clock;
  ++s.stats.copyBacks;
  report(s);
}
void ResidentShadowRetireImport(VkDevice device, VkBuffer buffer,
                                std::uint64_t epoch) {
  if (GetResidentShadowMode() == ResidentShadowMode::Off)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  for (auto i = s.entries.begin(); i != s.entries.end();)
    if (std::get<0>(i->first) == reinterpret_cast<std::uintptr_t>(device) &&
        std::get<4>(i->first) == reinterpret_cast<std::uintptr_t>(buffer) &&
        std::get<6>(i->first) == epoch) {
      s.stats.liveBytes -= i->second.cost;
      i = s.entries.erase(i);
      ++s.stats.retired;
    } else
      ++i;
  report(s);
}
ResidentShadowStats ResidentShadowCounters() {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  return s.stats;
}
} // namespace AgcDriver::Graphics
