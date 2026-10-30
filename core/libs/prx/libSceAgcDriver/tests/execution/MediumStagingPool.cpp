#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <cstddef>
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <cstdlib>
#include <vector>

namespace AgcDriver {
struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};
}

namespace {
using namespace AgcDriver::Graphics;
constexpr unsigned Rounds = 3;
constexpr std::size_t OddLayerBytes = 2818048;
constexpr std::size_t PowerOfTwoBytes = 4u << 20u;
constexpr std::size_t Layers = 4;


void Run(AgcDriver::VulkanDevice& device) {
    auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
    const bool mediumEnabled = std::getenv("APS5_NO_MEDIUM_BUFFER_SLABS") == nullptr && std::getenv("APS5_NO_BUFFER_CLASSES") == nullptr;
    constexpr auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    for (unsigned round = 0; round < Rounds; ++round) {
        const std::array<std::size_t, Layers> layerBytes{OddLayerBytes, PowerOfTwoBytes, OddLayerBytes, PowerOfTwoBytes};
        std::vector<std::unique_ptr<Buffer>> sources;
        std::vector<std::unique_ptr<Buffer>> destinations;
        sources.reserve(Layers);
        destinations.reserve(Layers);
        std::vector<std::pair<std::uintptr_t, std::uintptr_t>> ranges;
        ranges.reserve(2 * Layers);
        for (std::size_t layer = 0; layer < Layers; ++layer) {
            const auto requested = layerBytes[layer];
            auto source = std::make_unique<Buffer>(context, requested, usage);
            auto destination = std::make_unique<Buffer>(context, requested, usage);
            auto sourceBytes = source->Bytes();
            auto destinationBytes = destination->Bytes();
            Require(sourceBytes.size() == requested && destinationBytes.size() == requested, "native medium staging changed logical buffer length");
            for (std::size_t i = 0; i < requested; ++i)
                sourceBytes[i] = static_cast<std::byte>((i * 29u + layer * 47u + round * 61u) & 0xffu);
            std::fill(destinationBytes.begin(), destinationBytes.end(), std::byte{0xa5});
            for (const auto bytes : {sourceBytes, destinationBytes}) {
                const auto begin = reinterpret_cast<std::uintptr_t>(bytes.data());
                ranges.emplace_back(begin, begin + bytes.size());
            }
            sources.push_back(std::move(source));
            destinations.push_back(std::move(destination));
        }
        std::sort(ranges.begin(), ranges.end());
        for (std::size_t i = 1; i < ranges.size(); ++i)
            Require(ranges[i - 1].second <= ranges[i].first, "two live medium staging buffers overlap in host mapping");


        {
            CommandBatch batch(context);
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            const auto pipelineBarrier = context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
            pipelineBarrier(batch.Handle(), VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
            const auto copyBuffer = context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
            for (std::size_t layer = 0; layer < Layers; ++layer) {
                const VkBufferCopy region{0, 0, layerBytes[layer]};
                copyBuffer(batch.Handle(), sources[layer]->Handle(), destinations[layer]->Handle(), 1, &region);
            }
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            pipelineBarrier(batch.Handle(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
            batch.Submit();
            // Keep submitted buffers alive through the fence, and allocate another layer while
            // work is pending to prove the allocator cannot hand out an in-flight slot.
            auto probe = std::make_unique<Buffer>(context, PowerOfTwoBytes, usage);
            const auto probeBytes = probe->Bytes();
            Require(probeBytes.size() == PowerOfTwoBytes, "pending allocation changed its logical buffer length");
            const auto probeBegin = reinterpret_cast<std::uintptr_t>(probeBytes.data());
            const auto probeEnd = probeBegin + probeBytes.size();
            for (const auto& range : ranges)
                Require(probeEnd <= range.first || range.second <= probeBegin, "a pending allocation overlapped a submitted layer buffer");
            const auto oddProbe = std::make_unique<Buffer>(context, OddLayerBytes, usage);
            const auto oddProbeBytes = oddProbe->Bytes();
            Require(oddProbeBytes.size() == OddLayerBytes, "pending odd allocation changed its logical buffer length");
            const auto oddProbeBegin = reinterpret_cast<std::uintptr_t>(oddProbeBytes.data());
            const auto oddProbeEnd = oddProbeBegin + oddProbeBytes.size();
            for (const auto& range : ranges)
                Require(oddProbeEnd <= range.first || range.second <= oddProbeBegin, "a pending odd-sized allocation overlapped a submitted layer buffer");
            batch.Wait();
        }
        for (std::size_t layer = 0; layer < Layers; ++layer) {
            destinations[layer]->Invalidate();
            const auto output = destinations[layer]->Bytes();
            Require(output.size() == layerBytes[layer], "completed copy changed logical buffer length");
            for (std::size_t i = 0; i < layerBytes[layer]; ++i) {
                const auto expected = static_cast<std::byte>((i * 29u + layer * 47u + round * 61u) & 0xffu);
                Require(output[i] == expected, "completed Vulkan layer copy returned incorrect bytes at layer " + std::to_string(layer) + ", byte " + std::to_string(i));
            }
        }
        sources.clear();
        destinations.clear();
    }
    std::puts(mediumEnabled ? "Vulkan medium staging passed: odd-sized and power-of-two non-aliasing completed copy rounds" :
        "Vulkan dedicated staging passed: completed non-aliasing copy rounds");
}
}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Run(*device);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "medium Vulkan staging test failed: %s\n", error.what());
        return 1;
    }
    return 0;
}
