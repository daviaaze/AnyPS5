#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RESIDENTSHADOW_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RESIDENTSHADOW_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>
#include <vulkan/vulkan.h>

namespace AgcDriver::Graphics {
class Buffer;

// Diagnostic-only bookkeeping. It never changes transfer or publication
// behavior.
enum class ResidentShadowMode : std::uint8_t { Off, Observe };
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
struct ResidentShadowVersion {
  std::uint64_t observedCpu = 0;
  std::uint64_t logicalGpuProducer =
      0; // Recorded producer sequence; not GPU completion.
  std::uint64_t batchToken =
      0; // Recorded batch identity; not a completion token.
  std::vector<std::pair<std::uint64_t, std::uint64_t>> dirty;
  bool dirtyTruncated = false;
};
struct ResidentShadowStats {
  std::uint64_t observations = 0;
  std::uint64_t copyIns = 0;
  std::uint64_t copyBacks = 0;
  std::uint64_t retired = 0;
  std::uint64_t rejectedIdentity = 0;
  std::uint64_t evictions = 0;
  std::uint64_t liveBytes = 0;
};
ResidentShadowMode GetResidentShadowMode();
void ResidentShadowObserveCopyIn(const ResidentShadowIdentity &,
                                 const ResidentShadowVersion &,
                                 const std::shared_ptr<Buffer> &);
void ResidentShadowObserveCopyBack(
    const ResidentShadowIdentity &, std::uint64_t batchToken,
    std::span<const std::pair<std::uint64_t, std::uint64_t>> dirty);
void ResidentShadowRetireImport(VkDevice, VkBuffer, std::uint64_t importEpoch);
ResidentShadowStats ResidentShadowCounters();
} // namespace AgcDriver::Graphics
#endif
