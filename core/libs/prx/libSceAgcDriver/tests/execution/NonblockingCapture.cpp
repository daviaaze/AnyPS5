#include "Optimization/ResourceProgram.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <spirv/unified1/spirv.hpp>
#include <stdexcept>
#include <string_view>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {

std::uint64_t pendingBegin = 0;
std::uint64_t pendingEnd = 0;
std::uint32_t flushes = 0;

AgcDriver::ShaderMemory::PendingWrite
pending(std::uint64_t address, std::size_t bytes, std::span<std::byte>) {
  if (address < pendingEnd && pendingBegin < address + bytes)
    return AgcDriver::ShaderMemory::PendingWrite::Sync;
  return AgcDriver::ShaderMemory::PendingWrite::None;
}

void flushed(std::uint64_t, std::size_t) { ++flushes; }

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

struct RequestFixture {
  alignas(256) std::array<std::uint32_t, 64> output{};
  alignas(256) std::array<std::uint32_t, 16> root{};
  std::uint32_t payload = 0x3f800000u;
  std::array<std::uint32_t, 2> userData{};
  std::vector<std::uint32_t> code{
      0xf4080100u, 0xfa000000u, 0xf4000200u, 0xfa000010u, 0xf4040280u,
      0xfa000018u, 0xbe8c03ffu, 0x11111111u, 0xbf8cc07fu, 0xbf068008u,
      0xbf850003u, 0xf4000305u, 0xfa000000u, 0xbf8cc07fu, 0x7e02020cu,
      0x34040082u, 0xe0701000u, 0x80010102u, 0xbf810000u};
  ShaderRecompiler::RecompileRequest request{};
  static constexpr std::array<std::uint32_t, 6> capabilities{
      spv::CapabilityShader,
      spv::CapabilityImageGatherExtended,
      spv::CapabilityInt64,
      spv::CapabilityPhysicalStorageBufferAddresses,
      spv::CapabilityStorageBuffer8BitAccess,
      spv::CapabilityClipDistance};
  static constexpr std::array<std::string_view, 2> extensions{
      "SPV_KHR_physical_storage_buffer", "SPV_KHR_8bit_storage"};

  RequestFixture() {
    const auto outputAddress = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(output.data()));
    const auto rootAddress = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(root.data()));
    const auto payloadAddress =
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&payload));
    root = {static_cast<std::uint32_t>(outputAddress),
            static_cast<std::uint32_t>(outputAddress >> 32u),
            static_cast<std::uint32_t>(sizeof(output)),
            0x30027facu,
            1u,
            0u,
            static_cast<std::uint32_t>(payloadAddress),
            static_cast<std::uint32_t>(payloadAddress >> 32u)};
    userData = {static_cast<std::uint32_t>(rootAddress),
                static_cast<std::uint32_t>(rootAddress >> 32u)};
    request.shader = {
        ShaderRecompiler::ShaderStage::Compute, 0x60000u, code, 0, {}};
    request.context.waveSize = 64;
    request.context.userDataBaseRegister = 0;
    request.context.userData = userData;
    request.context.compute = ShaderRecompiler::ShaderComputeStageInfo{
        {64u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    request.target.supportedCapabilities = capabilities;
    request.target.supportedExtensions = extensions;
    request.target.bdaAbiVersion = 1u;
    request.target.vulkanVersion = 0x00401000u;
    request.target.spirvVersion = 0x00010300u;
    request.target.subgroupSize = 32u;
    request.layout.pushConstantSizeBytes = 128;
  }
};

struct NoAccessRegion {
  void *address = nullptr;
  std::size_t bytes = 0;

  NoAccessRegion() {
#ifdef _WIN32
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    bytes = info.dwPageSize;
    address = VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS);
#else
    bytes = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    address =
        mmap(nullptr, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (address == MAP_FAILED)
      address = nullptr;
#endif
    require(address != nullptr, "unable to reserve no-access test memory");
  }
  ~NoAccessRegion() {
#ifdef _WIN32
    if (address != nullptr)
      VirtualFree(address, 0, MEM_RELEASE);
#else
    if (address != nullptr)
      munmap(address, bytes);
#endif
  }
};

} // namespace

int main() {
  try {
    RequestFixture fixture;
    const auto payloadAddress = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(&fixture.payload));
    pendingBegin = payloadAddress;
    pendingEnd = payloadAddress + sizeof(fixture.payload);

    AgcDriver::GuestMemory::SetFlushHook(&flushed);
    AgcDriver::ShaderMemory blocking({});
    const auto blockingCapture = blocking.Capture(fixture.request);
    AgcDriver::ShaderMemory nonblocking({});
    const auto attempted = nonblocking.TryCapture(fixture.request);
    require(
        attempted.has_value() && *attempted != nullptr &&
            (*attempted)->snapshot.images == blockingCapture->snapshot.images &&
            (*attempted)->snapshot.buffers ==
                blockingCapture->snapshot.buffers &&
            (*attempted)->snapshot.flattenedSrt ==
                blockingCapture->snapshot.flattenedSrt &&
            (*attempted)->snapshot.tables == blockingCapture->snapshot.tables,
        "accessible block TryCapture differs from blocking Capture");
    const auto regions = nonblocking.Regions();
    std::uint64_t regionBytes = 0;
    for (const auto &region : regions)
      regionBytes += region.bytes.size();
    require(nonblocking.CapturedBytes() == regionBytes && regionBytes != 0,
            "CapturedBytes does not equal the nonempty captured region total");

    flushes = 0;
    AgcDriver::ShaderMemory pendingMemory({}, &pending);
    const auto pendingCapture = pendingMemory.TryCapture(fixture.request);
    require(!pendingCapture.has_value(),
            "pending GPU write did not abort TryCapture");
    require(flushes == 0, "TryCapture flushed a pending GPU write");

    NoAccessRegion noAccess;
    const auto noAccessAddress = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(noAccess.address));
    fixture.root[6] = static_cast<std::uint32_t>(noAccessAddress);
    fixture.root[7] = static_cast<std::uint32_t>(noAccessAddress >> 32u);
    AgcDriver::ShaderMemory unmappedMemory({});
    const auto unmappedCapture = unmappedMemory.TryCapture(fixture.request);
    require(!unmappedCapture.has_value(),
            "unmapped address did not abort TryCapture");

    fixture.root[6] = static_cast<std::uint32_t>(payloadAddress);
    fixture.root[7] = static_cast<std::uint32_t>(payloadAddress >> 32u);
    pendingEnd = 0;
    const auto recovered = pendingMemory.Capture(fixture.request);
    require(recovered != nullptr,
            "blocking Capture did not recover after an empty TryCapture");
    AgcDriver::GuestMemory::SetFlushHook(nullptr);
    std::cout << "nonblocking capture passed\n";
    return 0;
  } catch (const std::exception &error) {
    AgcDriver::GuestMemory::SetFlushHook(nullptr);
    std::cerr << "nonblocking capture failed: " << error.what() << '\n';
    return 1;
  }
}
