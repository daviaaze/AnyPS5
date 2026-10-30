#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DeferredDestruction.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include <algorithm>
#include <exception>
#ifdef __APPLE__
#include <cstring>
#endif

namespace AgcDriver::Graphics {

namespace {

struct BufferCtorTiming {
    bool enabled;
    std::size_t size, capacity;
    VkBufferUsageFlags usage;
    VkMemoryPropertyFlags properties;
    std::chrono::steady_clock::time_point totalStart{}, stageStart{};
    double poolMs = 0, createMs = 0, requirementsMs = 0, memoryTypeMs = 0, allocationMs = 0, bindAddressMs = 0, mapMs = 0;
    std::int64_t memoryType = -1, heap = -1;
    VkMemoryPropertyFlags selectedProperties = 0;
    bool hit = false, slab = false;

    BufferCtorTiming(bool enabled, std::size_t size, std::size_t capacity, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties)
        : enabled(enabled), size(size), capacity(capacity), usage(usage), properties(properties) {
        if (enabled) totalStart = stageStart = std::chrono::steady_clock::now();
    }
    void lap(double& destination) {
        if (!enabled) return;
        const auto now = std::chrono::steady_clock::now();
        destination += std::chrono::duration<double, std::milli>(now - stageStart).count();
        stageStart = now;
    }
    void finish() {
        if (!enabled) return;
        const auto now = std::chrono::steady_clock::now();
        const auto totalMs = std::chrono::duration<double, std::milli>(now - totalStart).count();
        if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0 || totalMs < 50.0) return;
        std::uint32_t flags = memoryType >= 0 ? static_cast<std::uint32_t>(selectedProperties) : 0;
        AgcDriver::ProfilePrint_nid_no_patch("[host-buffer] size=%zu capacity=%zu usage=0x%x properties=0x%x memoryType=%lld selectedProperties=0x%x heap=%lld hit=%d slab=%d pool_ms=%.3f create_ms=%.3f requirements_ms=%.3f memory_type_ms=%.3f allocation_ms=%.3f bind_address_ms=%.3f map_ms=%.3f total_ms=%.3f\n",
            size, capacity, usage, properties, static_cast<long long>(memoryType), flags, static_cast<long long>(heap), hit ? 1 : 0, slab ? 1 : 0,
            poolMs, createMs, requirementsMs, memoryTypeMs, allocationMs, bindAddressMs, mapMs, totalMs);
    }
};

#ifdef __APPLE__
void ClearPooledTail(void* mapping, std::size_t size, VkDeviceSize mappedBytes, VkMemoryPropertyFlags properties) {
    if (mapping == nullptr || (properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0 || mappedBytes <= size) return;
    std::memset(static_cast<std::byte*>(mapping) + size, 0, static_cast<std::size_t>(mappedBytes - size));
}
#endif
VkDeviceSize ClassAllocationBytes(const VkMemoryRequirements& requirements, std::size_t capacity) {
    const auto alignment = std::max<VkDeviceSize>(requirements.alignment, 1);
    return std::max<VkDeviceSize>(requirements.size, (static_cast<VkDeviceSize>(capacity) + alignment - 1) / alignment * alignment);
}

VkBuffer BindExactBuffer(const Context& context, std::size_t size, VkBufferUsageFlags usage, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize availableBytes, BufferCtorTiming* timing = nullptr) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    auto stage = timing != nullptr && timing->enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    Check(context.Function<PFN_vkCreateBuffer>("vkCreateBuffer")(context.device, &info, nullptr, &buffer), "vkCreateBuffer pooled");
    if (timing != nullptr && timing->enabled) timing->createMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stage).count();
    try {
        stage = timing != nullptr && timing->enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(context.device, buffer, &requirements);
        if (timing != nullptr && timing->enabled) timing->requirementsMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stage).count();
        Require(requirements.size <= availableBytes, "pooled buffer memory is smaller than the requested buffer");
        Require(requirements.alignment == 0 || offset % requirements.alignment == 0, "pooled buffer offset breaks the requested buffer's alignment");
        stage = timing != nullptr && timing->enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        Check(context.Function<PFN_vkBindBufferMemory>("vkBindBufferMemory")(context.device, buffer, memory, offset), "vkBindBufferMemory pooled");
        if (timing != nullptr && timing->enabled) timing->bindAddressMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stage).count();
    } catch (...) {
        context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, buffer, nullptr);
        throw;
    }
    return buffer;
}

}

Buffer::Buffer(const Context& context, std::size_t size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) : context(context), size(size), capacity(BufferPool::Capacity(size, properties)), usage(BufferPool::Usage(usage, properties)), properties(properties) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    BufferCtorTiming timing(profile && (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0, size, capacity, this->usage, properties);
    const auto finish = [&] { timing.finish(); };
    Require(size != 0, "zero-sized GPU buffer");
    const bool addressable = (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0;
    Require(!addressable || context.bufferDeviceAddress, "buffer device address is not enabled");
    cache = GetBufferPool(context);
    const auto allocation = cache->Take(size, this->usage, properties);
    timing.lap(timing.poolMs);
    if (allocation) {
        timing.hit = true;
        buffer = allocation->buffer;
        memory = allocation->memory;
        mapping = allocation->mapping;
        deviceAddress = allocation->address;
        allocationBytes = allocation->allocationBytes;
        memoryType = allocation->memoryType;
        capacity = allocation->bytes;
        timing.capacity = capacity;
        offset = allocation->offset;
        slab = allocation->slab;
        timing.slab = slab;
#ifdef __APPLE__
        ClearPooledTail(mapping, size, std::min<VkDeviceSize>(capacity, allocationBytes), properties);
#endif
        if (allocation->bufferBytes != size) {
            context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            deviceAddress = 0;
            timing.lap(timing.createMs);
            try {
                buffer = BindExactBuffer(context, size, this->usage, memory, offset, allocationBytes, &timing);
                initializeAddress(usage);
            } catch (...) {
                release();
                finish();
                throw;
            }
        }
        ready = true;
        finish();
        return;
    }
    try {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = this->usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        auto stage = timing.enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        Check(context.Function<PFN_vkCreateBuffer>("vkCreateBuffer")(context.device, &info, nullptr, &buffer), "vkCreateBuffer");
        if (timing.enabled) timing.createMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stage).count();
        stage = timing.enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(context.device, buffer, &requirements);
        if (timing.enabled) timing.requirementsMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stage).count();
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        const VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, nullptr, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
        if (addressable) allocation.pNext = &flags;
        allocation.allocationSize = ClassAllocationBytes(requirements, capacity);
        allocationBytes = allocation.allocationSize;
        // The CPU reads most of these buffers back (write-back, diffs), which is very slow from
        // write-combined memory, so the default host properties prefer cached host memory.
        bool useSlab = (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 && BufferPool::SlabEligible(capacity, requirements.alignment, requirements.size, context.limits.nonCoherentAtomSize);
        stage = timing.enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        try {
            allocation.memoryTypeIndex = context.BufferMemoryType(requirements.memoryTypeBits, properties, useSlab ? BufferPool::SlabBlockBytes(capacity) : allocation.allocationSize);
        } catch (const std::runtime_error&) {
            if (!useSlab) throw;
            useSlab = false;
            allocation.memoryTypeIndex = context.BufferMemoryType(requirements.memoryTypeBits, properties, allocation.allocationSize);
        }
        memoryType = allocation.memoryTypeIndex;
        if (timing.enabled) {
            timing.memoryTypeMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stage).count();
            timing.memoryType = allocation.memoryTypeIndex;
            const auto& type = context.memory.memoryTypes[allocation.memoryTypeIndex];
            timing.selectedProperties = type.propertyFlags;
            timing.heap = type.heapIndex;
        }
        if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 && (!useSlab || capacity >= (std::size_t{1} << 20u))) {
            stage = timing.enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            const auto hostTransfer = cache->TakeHostTransfer(capacity, this->usage, properties, requirements.size, requirements.alignment, requirements.memoryTypeBits, allocation.memoryTypeIndex);
            if (timing.enabled) timing.poolMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stage).count();
            if (hostTransfer) {
                timing.hit = true;
                context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, hostTransfer->buffer, nullptr);
                memory = hostTransfer->memory;
                mapping = hostTransfer->mapping;
                allocationBytes = hostTransfer->allocationBytes;
#ifdef __APPLE__
                ClearPooledTail(mapping, size, allocationBytes, properties);
#endif
                capacity = hostTransfer->bytes;
                offset = hostTransfer->offset;
                memoryType = hostTransfer->memoryType;
                timing.capacity = capacity;
                Check(context.Function<PFN_vkBindBufferMemory>("vkBindBufferMemory")(context.device, buffer, memory, offset), "vkBindBufferMemory");
                stage = timing.enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                initializeAddress(usage);
                if (timing.enabled) timing.bindAddressMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stage).count();
                ready = true;
                finish();
                return;
            }
        }
        if (useSlab) {
            stage = timing.enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            const auto slot = cache->TakeSlot(context, allocation.memoryTypeIndex, capacity, addressable);
            if (timing.enabled) timing.allocationMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stage).count();
            if (slot.memory != VK_NULL_HANDLE) {
                memory = slot.memory;
                offset = slot.offset;
                slab = true;
                timing.slab = true;
                allocationBytes = capacity;
                stage = timing.enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                Check(context.Function<PFN_vkBindBufferMemory>("vkBindBufferMemory")(context.device, buffer, memory, offset), "vkBindBufferMemory slab");
                initializeAddress(usage);
                if (timing.enabled) timing.bindAddressMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stage).count();
                mapping = slot.mapping;
#ifdef __APPLE__
                ClearPooledTail(mapping, size, allocationBytes, properties);
#endif
                ready = true;
                finish();
                return;
            }
        }
        if (useSlab && memory == VK_NULL_HANDLE) {
            stage = timing.enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            allocation.memoryTypeIndex = context.BufferMemoryType(requirements.memoryTypeBits, properties, allocation.allocationSize);
            memoryType = allocation.memoryTypeIndex;
            if (timing.enabled) {
                timing.memoryTypeMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stage).count();
                timing.memoryType = allocation.memoryTypeIndex;
                const auto& type = context.memory.memoryTypes[allocation.memoryTypeIndex];
                timing.selectedProperties = type.propertyFlags;
                timing.heap = type.heapIndex;
            }
        }
        stage = timing.enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        memory = cache->AllocateMemory(allocation, "vkAllocateMemory buffer");
        if (timing.enabled) timing.allocationMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stage).count();
        stage = timing.enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        Check(context.Function<PFN_vkBindBufferMemory>("vkBindBufferMemory")(context.device, buffer, memory, 0), "vkBindBufferMemory");
        initializeAddress(usage);
        if (timing.enabled) timing.bindAddressMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stage).count();
        if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
            stage = timing.enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            Check(context.Function<PFN_vkMapMemory>("vkMapMemory")(context.device, memory, 0, VK_WHOLE_SIZE, 0, &mapping), "vkMapMemory");
            if (timing.enabled) timing.mapMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stage).count();
        }
        ready = true;
        finish();
    } catch (...) {
        release();
        finish();
        throw;
    }
}

Buffer::~Buffer() {
    release();
}

void Buffer::release() noexcept {
    if (ready && cache) {
        cache->Put({buffer, memory, mapping, deviceAddress, allocationBytes, capacity, usage, properties, offset, slab, size, memoryType});
        return;
    }
    if (slab) {
        if (buffer) context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, buffer, nullptr);
        cache->PutSlot(memory, offset);
        return;
    }
    if (mapping) context.Function<PFN_vkUnmapMemory>("vkUnmapMemory")(context.device, memory);
    if (buffer) context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, buffer, nullptr);
    if (memory) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
}

VkBuffer Buffer::Handle() const {
    return buffer;
}

std::span<std::byte> Buffer::Bytes() {
    Require(mapping != nullptr, "device-local buffer has no host mapping");
    return {static_cast<std::byte*>(mapping), size};
}

void Buffer::Invalidate() {
    Require(mapping != nullptr, "cannot invalidate an unmapped GPU buffer");
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = memory;
    range.offset = offset;
    range.size = slab ? static_cast<VkDeviceSize>(capacity) : VK_WHOLE_SIZE;
    Check(context.Function<PFN_vkInvalidateMappedMemoryRanges>("vkInvalidateMappedMemoryRanges")(context.device, 1, &range), "vkInvalidateMappedMemoryRanges");
}

DeviceBuffer::DeviceBuffer(const Context& context, std::size_t size, VkBufferUsageFlags usage) : context(context), size(size), capacity(BufferPool::Capacity(size)), usage(BufferPool::Usage(usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
    Require(size != 0, "zero-sized device buffer");
    cache = GetBufferPool(context);
    if (const auto allocation = cache->Take(size, this->usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        buffer = allocation->buffer;
        memory = allocation->memory;
        allocationBytes = allocation->allocationBytes;
        capacity = allocation->bytes;
        if (allocation->bufferBytes == size) {
            ready = true;
            return;
        }
        context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        try {
            buffer = BindExactBuffer(context, size, this->usage, memory, 0, allocationBytes);
        } catch (...) {
            release();
            throw;
        }
        ready = true;
        return;
    }
    try {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = this->usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(context.Function<PFN_vkCreateBuffer>("vkCreateBuffer")(context.device, &info, nullptr, &buffer), "vkCreateBuffer device");
        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(context.device, buffer, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = ClassAllocationBytes(requirements, capacity);
        allocationBytes = allocation.allocationSize;
        allocation.memoryTypeIndex = context.DeviceMemoryType(requirements.memoryTypeBits, allocation.allocationSize, "device buffer");
        const auto deviceType = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        std::uint32_t allocatedType = allocation.memoryTypeIndex;
        memory = cache->AllocateMemory(allocation, "vkAllocateMemory device buffer", context.RefusedDeviceMemoryFallback(requirements.memoryTypeBits, allocation.memoryTypeIndex), &allocatedType);
        hostPlaced = allocatedType != deviceType;
        Check(context.Function<PFN_vkBindBufferMemory>("vkBindBufferMemory")(context.device, buffer, memory, 0), "vkBindBufferMemory device");
        ready = true;
    } catch (...) {
        release();
        throw;
    }
}

DeviceBuffer::~DeviceBuffer() {
    release();
}

void DeviceBuffer::release() noexcept {
    if (ready && buffer && memory && cache && !hostPlaced) {
        cache->Put({buffer, memory, nullptr, 0, allocationBytes, capacity, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false, size});
        return;
    }
    if (buffer) context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, buffer, nullptr);
    if (memory) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
}

VkBuffer DeviceBuffer::Handle() const {
    return buffer;
}

std::size_t DeviceBuffer::Size() const {
    return size;
}

void CopyBuffer(const Context& context, VkCommandBuffer commands, VkBuffer source, VkDeviceSize sourceOffset, VkBuffer destination, VkDeviceSize destinationOffset, VkDeviceSize bytes) {
    const VkBufferCopy region{sourceOffset, destinationOffset, bytes};
    context.Resolved(&DeviceFunctions::cmdCopyBuffer, "vkCmdCopyBuffer")(commands, source, destination, 1, &region);
}

void RecordMemoryBarrier(const Context& context, VkCommandBuffer commands, VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage, VkAccessFlags sourceAccess, VkAccessFlags destinationAccess) {
    const VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, sourceAccess, destinationAccess};
    context.Resolved(&DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier")(commands, sourceStage, destinationStage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void FillDeviceFunctions(const Context& context, DeviceFunctions& functions) {
    functions.cmdPipelineBarrier = context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    functions.cmdCopyBuffer = context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
    functions.cmdUpdateBuffer = context.Function<PFN_vkCmdUpdateBuffer>("vkCmdUpdateBuffer");
    functions.cmdFillBuffer = context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer");
    functions.cmdBindPipeline = context.Function<PFN_vkCmdBindPipeline>("vkCmdBindPipeline");
    functions.cmdBindDescriptorSets = context.Function<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets");
    functions.cmdPushConstants = context.Function<PFN_vkCmdPushConstants>("vkCmdPushConstants");
    functions.cmdDispatch = context.Function<PFN_vkCmdDispatch>("vkCmdDispatch");
    functions.cmdDispatchIndirect = context.Function<PFN_vkCmdDispatchIndirect>("vkCmdDispatchIndirect");
    functions.cmdBeginRenderPass = context.Function<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass");
    functions.cmdEndRenderPass = context.Function<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass");
    functions.cmdSetViewport = context.Function<PFN_vkCmdSetViewport>("vkCmdSetViewport");
    functions.cmdSetScissor = context.Function<PFN_vkCmdSetScissor>("vkCmdSetScissor");
    functions.cmdSetDepthBounds = context.Function<PFN_vkCmdSetDepthBounds>("vkCmdSetDepthBounds");
    functions.cmdSetDepthBias = context.Function<PFN_vkCmdSetDepthBias>("vkCmdSetDepthBias");
    if (context.graphicsPipelineLibrary) {
        functions.cmdBeginRendering = context.Function<PFN_vkCmdBeginRenderingKHR>("vkCmdBeginRenderingKHR");
        functions.cmdEndRendering = context.Function<PFN_vkCmdEndRenderingKHR>("vkCmdEndRenderingKHR");
        functions.cmdSetCullMode = context.Function<PFN_vkCmdSetCullModeEXT>("vkCmdSetCullModeEXT");
        functions.cmdSetFrontFace = context.Function<PFN_vkCmdSetFrontFaceEXT>("vkCmdSetFrontFaceEXT");
        functions.cmdSetDepthTestEnable = context.Function<PFN_vkCmdSetDepthTestEnableEXT>("vkCmdSetDepthTestEnableEXT");
        functions.cmdSetDepthWriteEnable = context.Function<PFN_vkCmdSetDepthWriteEnableEXT>("vkCmdSetDepthWriteEnableEXT");
        functions.cmdSetDepthCompareOp = context.Function<PFN_vkCmdSetDepthCompareOpEXT>("vkCmdSetDepthCompareOpEXT");
        functions.cmdSetDepthBoundsTestEnable = context.Function<PFN_vkCmdSetDepthBoundsTestEnableEXT>("vkCmdSetDepthBoundsTestEnableEXT");
        functions.cmdSetStencilTestEnable = context.Function<PFN_vkCmdSetStencilTestEnableEXT>("vkCmdSetStencilTestEnableEXT");
        functions.cmdSetStencilOp = context.Function<PFN_vkCmdSetStencilOpEXT>("vkCmdSetStencilOpEXT");
        functions.cmdSetStencilCompareMask = context.Function<PFN_vkCmdSetStencilCompareMask>("vkCmdSetStencilCompareMask");
        functions.cmdSetStencilWriteMask = context.Function<PFN_vkCmdSetStencilWriteMask>("vkCmdSetStencilWriteMask");
        functions.cmdSetStencilReference = context.Function<PFN_vkCmdSetStencilReference>("vkCmdSetStencilReference");
    }
    functions.cmdBindVertexBuffers = context.Function<PFN_vkCmdBindVertexBuffers>("vkCmdBindVertexBuffers");
    functions.cmdBindIndexBuffer = context.Function<PFN_vkCmdBindIndexBuffer>("vkCmdBindIndexBuffer");
    functions.cmdDraw = context.Function<PFN_vkCmdDraw>("vkCmdDraw");
    functions.cmdDrawIndexed = context.Function<PFN_vkCmdDrawIndexed>("vkCmdDrawIndexed");
    functions.cmdDrawIndirect = context.Function<PFN_vkCmdDrawIndirect>("vkCmdDrawIndirect");
    functions.cmdDrawIndexedIndirect = context.Function<PFN_vkCmdDrawIndexedIndirect>("vkCmdDrawIndexedIndirect");
    functions.cmdCopyBufferToImage = context.Function<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage");
    functions.cmdCopyImageToBuffer = context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer");
    functions.cmdClearColorImage = context.Function<PFN_vkCmdClearColorImage>("vkCmdClearColorImage");
    functions.updateDescriptorSets = context.Function<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets");
    functions.allocateDescriptorSets = context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets");
    functions.getFenceStatus = context.Function<PFN_vkGetFenceStatus>("vkGetFenceStatus");
}

RenderTarget::RenderTarget(const Context& context, const ColorTarget& target, bool blending) : context(context) {
    VkFormatProperties properties{};
    context.formatProperties(context.physical, target.format, &properties);
    const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | (blending ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT : 0u);
    Require((properties.optimalTilingFeatures & required) == required, "render-target format does not support required operations");
    constexpr auto usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageFormatProperties supported{};
    Check(context.imageFormatProperties(context.physical, target.format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &supported), "vkGetPhysicalDeviceImageFormatProperties");
    Require(target.extent.width <= supported.maxExtent.width && target.extent.height <= supported.maxExtent.height && (supported.sampleCounts & VK_SAMPLE_COUNT_1_BIT) != 0 && target.bytes <= supported.maxResourceSize, "render target exceeds device image limits");
    Require(target.extent.width <= context.limits.maxFramebufferWidth && target.extent.height <= context.limits.maxFramebufferHeight, "render target exceeds framebuffer limits");
    try {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = target.format;
        info.extent = {target.extent.width, target.extent.height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        Check(context.Function<PFN_vkCreateImage>("vkCreateImage")(context.device, &info, nullptr, &image), "vkCreateImage");
        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(context.device, image, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        memory = AllocateImageMemory(context, allocation, requirements.memoryTypeBits, "vkAllocateMemory render target");
        Check(context.Function<PFN_vkBindImageMemory>("vkBindImageMemory")(context.device, image, memory, 0), "vkBindImageMemory");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = target.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &viewInfo, nullptr, &view), "vkCreateImageView");
    } catch (...) {
        release();
        throw;
    }
}

RenderTarget::~RenderTarget() {
    release();
}

void RenderTarget::release() noexcept {
    if (view) context.Function<PFN_vkDestroyImageView>("vkDestroyImageView")(context.device, view, nullptr);
    if (image) context.Function<PFN_vkDestroyImage>("vkDestroyImage")(context.device, image, nullptr);
    if (memory) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
}

VkImage RenderTarget::Image() const {
    return image;
}

VkImageView RenderTarget::View() const {
    return view;
}

CommandBatch::CommandBatch(const Context& context) : context(context) {
    // Records into the device's one command pool and submits to its queue: device-lock work only.
    GuestMemory::AssertGpuLockHeld("CommandBatch");
    try {
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = context.pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        Check(context.Function<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(context.device, &allocation, &commands), "vkAllocateCommandBuffers");
        VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence created = VK_NULL_HANDLE;
        Check(context.Function<PFN_vkCreateFence>("vkCreateFence")(context.device, &info, nullptr, &created), "vkCreateFence");
        fence = created;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(context.Function<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(commands, &begin), "vkBeginCommandBuffer");
        // Work recorded so far goes first in queue order, so this batch sees its results.
        if (auto* recorder = Recorder::Active(); recorder != nullptr && recorder->Recording()) recorder->Submit();
    } catch (...) {
        release();
        throw;
    }
}

CommandBatch::~CommandBatch() {
    release();
}

void CommandBatch::release() noexcept {
    if (pending) {
        auto result = context.Function<PFN_vkGetFenceStatus>("vkGetFenceStatus")(context.device, fence);
        if (result == VK_NOT_READY) result = OnQueue(context.queueLane, [] { return GuestMemory::GpuMutex().DepthOnThisThread(); },
            [] { GuestMemory::GpuMutex().unlock(); }, [] { GuestMemory::GpuMutex().lock(); },
            [&] { return context.Function<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(context.queue); });
        if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) std::terminate();
    }
    if (commands) context.Function<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers")(context.device, context.pool, 1, &commands);
    if (fence) context.Function<PFN_vkDestroyFence>("vkDestroyFence")(context.device, fence, nullptr);
}

VkCommandBuffer CommandBatch::Handle() const {
    return commands;
}

void CommandBatch::SubmitAndWait() {
    Submit();
    Wait();
}

void CommandBatch::Submit() {
    PerformanceTimer timing("Graphics.Submit");
    Require(!submitted, "command batch has already been submitted");
    Check(context.Function<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands), "vkEndCommandBuffer");
    VkSubmitInfo submission{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submission.commandBufferCount = 1;
    submission.pCommandBuffers = &commands;
    timing.Mark("command_end");
    Check(OnQueue(context.queueLane, [] { return GuestMemory::GpuMutex().DepthOnThisThread(); },
        [] { GuestMemory::GpuMutex().unlock(); }, [] { GuestMemory::GpuMutex().lock(); },
        [&] {
            DestroyDeferredAtIdle(context);
            return context.Function<PFN_vkQueueSubmit>("vkQueueSubmit")(context.queue, 1, &submission, fence);
        }), "vkQueueSubmit graphics");
    pending = true;
    submitted = true;
    timing.Mark("queue_submit");
}

void CommandBatch::Wait() {
    Require(submitted, "command batch has not been submitted");
    if (!pending) return;
    PerformanceTimer timing("Graphics.Wait");
    const auto result = context.Function<PFN_vkWaitForFences>("vkWaitForFences")(context.device, 1, &fence, VK_TRUE, 5'000'000'000ULL);
    timing.Mark("fence_wait");
    if (result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST) pending = false;
    Check(result, "vkWaitForFences graphics");
    // Recorded batches preceded this one, so their completions (write-backs) can run now.
    if (auto* recorder = Recorder::Active()) recorder->Reap();
}

}
