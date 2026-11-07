#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RESIDENTSHADOW_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RESIDENTSHADOW_HPP

#include <cstddef>
#include <cstdint>
#include <memory>

#include <vulkan/vulkan.h>

namespace AgcDriver::Graphics {
class Buffer;

// Diagnostic-only bookkeeping. It never changes transfer or publication
// behavior.
enum class ResidentShadowMode : std::uint8_t { Off, Observe, Eager };
struct ResidentShadowIdentity {
  VkDevice device = VK_NULL_HANDLE;
  std::uint64_t allocationAddress = 0;
  std::uint64_t allocationBytes = 0;
  std::uint64_t backingSerial = 0;
  VkBuffer importBuffer = VK_NULL_HANDLE;
  std::uint64_t importBase = 0;
  std::uint64_t importEpoch = 0;
  std::uint64_t begin = 0;
  std::uint64_t end = 0;
};
enum class ResidentShadowReject : std::uint8_t {
  NotCompleted, CpuWrite, GpuWrite, Unwatched, UnhookedWrite, Retired, Count
};
struct ResidentShadowStats {
  std::uint64_t observations = 0;
  std::uint64_t copyIns = 0;
  std::uint64_t copyBacks = 0;
  std::uint64_t retired = 0;
  std::uint64_t rejectedIdentity = 0;
  std::uint64_t evictions = 0;
  std::uint64_t liveBytes = 0;
  std::uint64_t reusableWouldSkip = 0;
  std::uint64_t reusableWouldSkipBytes = 0;
  std::uint64_t rejected[static_cast<std::size_t>(ResidentShadowReject::Count)]{};
};
ResidentShadowMode GetResidentShadowMode();
void ResidentShadowObserveCopyIn(const ResidentShadowIdentity &,
                                 const std::shared_ptr<Buffer> &);
void ResidentShadowObserveCopyBack(const ResidentShadowIdentity &,
                                   std::uint64_t batchToken);
void ResidentShadowCopyBackCompleted(const ResidentShadowIdentity &,
                                     std::uint64_t batchSerial);
bool ResidentShadowProducerCompleted(const ResidentShadowIdentity &);
bool ResidentShadowCpuUnchanged(const ResidentShadowIdentity &);
bool ResidentShadowReusable(const ResidentShadowIdentity &,
                            const ResidentShadowIdentity &currentRegion);
void ResidentShadowObserveGpuWrite(std::uint64_t address, std::size_t bytes);
void ResidentShadowNoteUnhookedWrite();
void ResidentShadowRetireImport(VkDevice, VkBuffer, std::uint64_t importEpoch);
ResidentShadowStats ResidentShadowCounters();
} // namespace AgcDriver::Graphics
#endif
