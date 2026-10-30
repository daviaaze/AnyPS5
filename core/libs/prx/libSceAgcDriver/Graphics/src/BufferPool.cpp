#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>

namespace AgcDriver::Graphics {

namespace {

// APS5_BUFFER_POOL_SHARED=1: one tier for every size, as before the split.
bool sharedTiers() {
    static const bool shared = std::getenv("APS5_BUFFER_POOL_SHARED") != nullptr;
    return shared;
}

bool exactSizes() {
    static const bool exact = std::getenv("APS5_NO_BUFFER_CLASSES") != nullptr;
    return exact;
}

bool mediumSlabsEnabled() {
    static const bool enabled = std::getenv("APS5_NO_MEDIUM_BUFFER_SLABS") == nullptr;
    return enabled;
}

bool systemMemoryHeap(const VkPhysicalDeviceMemoryProperties& memory, std::uint32_t heap) {
    bool typed = false;
    for (std::uint32_t type = 0; type < std::min<std::uint32_t>(memory.memoryTypeCount, VK_MAX_MEMORY_TYPES); ++type) {
        if (memory.memoryTypes[type].heapIndex != heap) continue;
        if ((memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) return false;
        typed = true;
    }
    return typed;
}

constexpr VkBufferUsageFlags deviceUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

}

namespace {

VkDeviceSize retainedCharge(const BufferAllocation& allocation) {
    // Medium slab backing is charged in full by mediumSlabBytes; cached slices
    // consume slots, but do not represent additional physical allocation bytes.
    if (allocation.slab && allocation.bytes >= (std::size_t{1} << 20u) && allocation.bytes <= (std::size_t{4} << 20u))
        return 0;
    return allocation.allocationBytes;
}

}

BufferPool::BufferPool(const Context& context) : maxSlots(std::max<std::size_t>(1, std::min<std::size_t>(MaxSlots(), context.limits.maxMemoryAllocationCount / 6u))), cacheReclaim(context.cacheReclaim), device(context.device), unmap(context.Function<PFN_vkUnmapMemory>("vkUnmapMemory")), destroyBuffer(context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")), freeMemory(context.Function<PFN_vkFreeMemory>("vkFreeMemory")), allocateMemory(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")), mapMemory(context.Function<PFN_vkMapMemory>("vkMapMemory")), physical(context.physical), memoryProperties(context.memory), memoryProperties2(context.memoryProperties2) {
    sharedPresentEpoch = context.presentEpoch;
    presentEpoch = sharedPresentEpoch ? sharedPresentEpoch.get() : &localPresentEpoch;
    smallTier.budget = smallBudget;
    largeTier.budget = budget;
    deviceTier.budget = deviceBudget(context);
    memoryProperties = context.memory;
    physical = context.physical;
    memoryProperties2 = context.memoryProperties2;
    imageBudget = ImageBudget(deviceTier.budget);
}

BufferPool::~BufferPool() {
    for (auto* tier : {&smallTier, &largeTier, &deviceTier}) {
        for (const auto& [key, slots] : tier->free) {
            for (const auto& slot : slots) destroy(slot.allocation);
        }
    }
    for (auto& [memory, block] : slabBlocks) freeBlock(*block);
}

VkDeviceMemory BufferPool::AllocateMemory(const VkMemoryAllocateInfo& allocation, const char* operation, std::optional<std::uint32_t> hostType, std::uint32_t* allocatedType, bool newImage, ImageMemoryTiming* timing) {
    if (allocatedType != nullptr) *allocatedType = allocation.memoryTypeIndex;
    // Vulkan does not define the output handle when allocation fails. Only publish a handle on success.
    VkDeviceMemory memory = VK_NULL_HANDLE;
    const auto allocate = [&](const VkMemoryAllocateInfo& request) {
        VkDeviceMemory output = VK_NULL_HANDLE;
        const auto started = timing != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const auto value = allocateMemory(device, &request, nullptr, &output);
        if (timing != nullptr) {
            timing->vkAllocateMilliseconds += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            ++timing->vkAllocateCalls;
        }
        memory = value == VK_SUCCESS ? output : VK_NULL_HANDLE;
        return value;
    };
    auto result = allocate(allocation);
    const auto initialResult = result;
    unsigned attempts = 1;
    VkDeviceSize reclaimed = 0;
    VkDeviceSize released = 0;
    const auto exhausted = [](VkResult value) { return value == VK_ERROR_OUT_OF_DEVICE_MEMORY || value == VK_ERROR_OUT_OF_HOST_MEMORY || value == VK_ERROR_TOO_MANY_OBJECTS; };
    if (exhausted(result)) {
        reclaimed = trim();
        if (reclaimed != 0) {
            memory = VK_NULL_HANDLE;
            result = allocate(allocation);
            ++attempts;
        }
    }
    if (exhausted(result)) {
        if (cacheReclaim != nullptr) released = cacheReclaim(ReclaimTarget(allocation.allocationSize), newImage);
        if (released != 0) {
            reclaimed += trim();
            memory = VK_NULL_HANDLE;
            result = allocate(allocation);
            ++attempts;
        }
    }
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY && hostType.has_value() && *hostType != allocation.memoryTypeIndex) {
        auto host = allocation;
        host.memoryTypeIndex = *hostType;
        memory = VK_NULL_HANDLE;
        result = allocate(host);
        ++attempts;
        if (result == VK_SUCCESS) {
            if (allocatedType != nullptr) *allocatedType = *hostType;
            static std::atomic<std::uint64_t> placements{0}, placedBytes{0};
            const auto count = placements.fetch_add(1, std::memory_order_relaxed) + 1;
            const auto total = placedBytes.fetch_add(allocation.allocationSize, std::memory_order_relaxed) + allocation.allocationSize;
            if (count <= 5 || (count & (count - 1)) == 0) std::fprintf(stderr, "[bufferpool] %s: memory type %u is out of device memory for %llu bytes after reclaiming %llu and releasing %llu; placed in host memory type %u until it is released; %llu such allocations, %llu MiB\n", operation, allocation.memoryTypeIndex, static_cast<unsigned long long>(allocation.allocationSize), static_cast<unsigned long long>(reclaimed), static_cast<unsigned long long>(released), *hostType, static_cast<unsigned long long>(count), static_cast<unsigned long long>(total >> 20u));
            return memory;
        }
    }
    if (result == VK_SUCCESS) {
        if (attempts > 1) std::fprintf(stderr, "[bufferpool] %s recovered: allocation=%llu reclaimed=%llu released=%llu initialResult=%d attempts=%u\n", operation, static_cast<unsigned long long>(allocation.allocationSize), static_cast<unsigned long long>(reclaimed), static_cast<unsigned long long>(released), static_cast<int>(initialResult), attempts);
        return memory;
    }
    char details[1536];
    std::snprintf(details, sizeof(details), "%s: allocation=%llu memoryType=%u reclaimed=%llu released=%llu initialResult=%d attempts=%u", operation, static_cast<unsigned long long>(allocation.allocationSize), allocation.memoryTypeIndex, static_cast<unsigned long long>(reclaimed), static_cast<unsigned long long>(released), static_cast<int>(initialResult), attempts);
    if (memoryProperties2 != nullptr && physical != VK_NULL_HANDLE) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &budget};
        memoryProperties2(physical, &properties);
        auto used = std::strlen(details);
        for (std::uint32_t heap = 0; heap < memoryProperties.memoryHeapCount && heap < VK_MAX_MEMORY_HEAPS; ++heap) {
            const auto remaining = sizeof(details) - used;
            const auto appended = std::snprintf(details + used, remaining, "; heap %u %llu/%llu MiB", heap, static_cast<unsigned long long>(budget.heapUsage[heap] >> 20u), static_cast<unsigned long long>(budget.heapBudget[heap] >> 20u));
            if (appended < 0 || static_cast<std::size_t>(appended) >= remaining) break;
            used += static_cast<std::size_t>(appended);
        }
    }
    Check(result, details);
    return VK_NULL_HANDLE;
}

// Image memory retention (see TakeImageMemory). By default retained image memory has a budget of its
// own (1 GiB), is not counted against the device staging tier, and a request takes a retained
// allocation of its memory type up to an eighth (at least 1 MiB) larger than it asks for: the
// sampled-texture cache drops and remakes snapshots of the same or similar size all the time, and
// each new allocation is a vkAllocateMemory under the device lock that took up to seconds in play.
// APS5_NO_IMAGE_SIZE_CLASSES restores exact-size reuse inside the device tier's budget (a quarter
// of it); APS5_IMAGE_POOL_MIB sets the image budget either way.
namespace {
bool ImageSizeClasses() {
    static const bool enabled = std::getenv("APS5_NO_IMAGE_SIZE_CLASSES") == nullptr;
    return enabled;
}
}

VkDeviceSize BufferPool::ImageBudget(VkDeviceSize deviceBudget) {
    static const char* value = std::getenv("APS5_IMAGE_POOL_MIB");
    if (value != nullptr) return static_cast<VkDeviceSize>(std::strtoull(value, nullptr, 10)) << 20u;
    return ImageSizeClasses() ? VkDeviceSize{1} << 30u : deviceBudget / 4u;
}

bool BufferPool::imagePooled(VkDeviceSize bytes, std::uint32_t memoryType) const {
    return imageBudget != 0 && bytes != 0 && bytes <= imageBudget && (ImageSizeClasses() || bytes <= deviceTier.budget) && memoryType < memoryProperties.memoryTypeCount && DeviceTiered(memoryProperties.memoryTypes[memoryType].propertyFlags);
}

bool BufferPool::heapOverBudget() {
    if (memoryProperties2 == nullptr || physical == VK_NULL_HANDLE) return false;
    const auto now = std::chrono::steady_clock::now();
    if (heapRead != std::chrono::steady_clock::time_point{} && now - heapRead < std::chrono::milliseconds(100)) return heapOver;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT reported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    properties.pNext = &reported;
    memoryProperties2(physical, &properties);
    heapRead = now;
    heapOver = false;
    for (std::uint32_t heap = 0; heap < memoryProperties.memoryHeapCount && heap < VK_MAX_MEMORY_HEAPS; ++heap) {
        if ((memoryProperties.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0 && reported.heapBudget[heap] != 0 && reported.heapUsage[heap] > reported.heapBudget[heap]) heapOver = true;
    }
    return heapOver;
}

VkDeviceMemory BufferPool::TakeImageMemory(const VkMemoryAllocateInfo& allocation, const char* operation, std::optional<std::uint32_t> hostType, std::uint32_t* allocatedType, bool newImage, ImageMemoryTiming* timing, VkDeviceSize* allocatedBytes) {
    if (allocatedBytes != nullptr) *allocatedBytes = 0;
    const auto type = allocation.memoryTypeIndex;
    if (imagePooled(allocation.allocationSize, type)) {
        std::lock_guard lock(mutex);
        ++clock;
        const auto properties = memoryProperties.memoryTypes[type].propertyFlags;
        auto found = deviceTier.free.find({static_cast<std::size_t>(allocation.allocationSize), 0, properties, type});
        if ((found == deviceTier.free.end() || found->second.empty()) && ImageSizeClasses()) {
            // The memory is bound at offset 0, so a larger one serves; its actual backing bytes are
            // reported with the allocation and returned to the pool by its owner.
            const auto request = static_cast<std::size_t>(allocation.allocationSize);
            const auto limit = request + std::max<std::size_t>(request / 8u, std::size_t{1} << 20u);
            const Slot* best = nullptr;
            for (const auto* slot : imageOrder) {
                const auto bytes = slot->allocation.bytes;
                if (slot->allocation.memoryType == type && bytes >= request && bytes <= limit && (best == nullptr || bytes < best->allocation.bytes)) best = slot;
            }
            if (best != nullptr) found = deviceTier.free.find({best->allocation.bytes, 0, properties, type});
        }
        if (found != deviceTier.free.end() && !found->second.empty()) {
            auto& slots = found->second;
            const auto memory = slots.back().allocation.memory;
            const auto bytes = slots.back().allocation.allocationBytes;
            imageOrder.erase(slots.back().order);
            slots.pop_back();
            if (slots.empty()) deviceTier.free.erase(found);
            --deviceTier.slots;
            if (!ImageSizeClasses()) deviceTier.retainedBytes -= bytes;
            --imageSlots;
            imageBytes -= bytes;
            ++imageHits;
            if (timing != nullptr) ++timing->poolHits;
            if (allocatedBytes != nullptr) *allocatedBytes = bytes;
            if (allocatedType != nullptr) *allocatedType = type;
            return memory;
        }
    }
    const auto memory = AllocateMemory(allocation, operation, hostType, allocatedType, newImage, timing);
    if (allocatedBytes != nullptr) *allocatedBytes = memory != VK_NULL_HANDLE ? allocation.allocationSize : 0;
    return memory;
}

void BufferPool::PutImageMemory(VkDeviceMemory memory, VkDeviceSize bytes, std::uint32_t memoryType) noexcept {
    if (memory == VK_NULL_HANDLE) return;
    if (!imagePooled(bytes, memoryType)) {
        freeMemory(device, memory, nullptr);
        return;
    }
    BufferAllocation allocation{VK_NULL_HANDLE, memory, nullptr, 0, bytes, static_cast<std::size_t>(bytes), 0, memoryProperties.memoryTypes[memoryType].propertyFlags};
    allocation.memoryType = memoryType;
    std::vector<BufferAllocation> evicted;
    try {
        std::lock_guard lock(mutex);
        if (heapOverBudget()) {
            while (imageSlots != 0 && evictOldestImage(evicted)) {
            }
            evicted.push_back(allocation);
        } else {
            while (imageSlots != 0 && imageBytes + bytes > imageBudget && evictOldestImage(evicted)) {
            }
            // Images have a budget of their own under size classes (not the staging tier's bytes).
            while (deviceTier.slots != 0 && ((!ImageSizeClasses() && deviceTier.retainedBytes + bytes > deviceTier.budget) || deviceTier.slots >= maxSlots)) evictOldest(deviceTier, evicted);
            const SlotKey imageKey{allocation.bytes, 0, allocation.properties, memoryType};
            auto& slots = deviceTier.free[imageKey];
            slots.push_back({allocation, ++clock, presentEpoch->load(std::memory_order_relaxed), {}});
            try {
                slots.back().order = imageOrder.insert(imageOrder.end(), &slots.back());
            } catch (...) {
                slots.pop_back();
                if (slots.empty()) deviceTier.free.erase(imageKey);
                throw;
            }
            ++deviceTier.slots;
            if (!ImageSizeClasses()) deviceTier.retainedBytes += bytes;
            ++imageSlots;
            imageBytes += bytes;
        }
    } catch (...) {
        destroy(allocation);
    }
    for (const auto& gone : evicted) destroy(gone);
}

std::size_t BufferPool::RetainedImages() {
    std::lock_guard lock(mutex);
    return imageSlots;
}

VkDeviceSize BufferPool::ReclaimTarget(VkDeviceSize bytes) {
    const auto reserve = std::max<VkDeviceSize>(bytes, VkDeviceSize{256} << 20u);
    const auto limit = std::numeric_limits<VkDeviceSize>::max();
    return bytes > limit - reserve ? limit : bytes + reserve;
}

bool BufferPool::SlabEligible(std::size_t capacity, VkDeviceSize alignment, VkDeviceSize size, VkDeviceSize atom) {
    const bool small = capacity < classLimit;
    const bool medium = capacity >= classLimit && capacity <= (std::size_t{4} << 20u) && mediumSlabsEnabled() && !exactSizes();
    if ((!small && !medium) || size > capacity || !std::has_single_bit(capacity)) return false;
    const auto alignmentOk = alignment == 0 || (std::has_single_bit(alignment) && alignment <= capacity);
    const auto atomOk = atom <= 1 || (std::has_single_bit(atom) && atom <= capacity);
    return alignmentOk && atomOk;
}

VkDeviceSize BufferPool::SlabBlockBytes(std::size_t capacity) {
    if (capacity >= classLimit && capacity <= (std::size_t{4} << 20u)) return VkDeviceSize{8} << 20u;
    return std::max<VkDeviceSize>(VkDeviceSize{2} << 20u, static_cast<VkDeviceSize>(capacity) * 8u);
}

std::uint64_t BufferPool::SlabKey(std::uint32_t memoryType, std::size_t capacity, bool addressable) {
    return (static_cast<std::uint64_t>(memoryType) << 40u) | (static_cast<std::uint64_t>(addressable ? 1u : 0u) << 39u) | static_cast<std::uint64_t>(capacity);
}

bool BufferPool::reserveMediumBlock(VkDeviceSize bytes) {
    if (bytes > largeTier.budget) return false;
    std::vector<BufferAllocation> evicted;
    bool reserved = false;
    {
        std::lock_guard lock(mutex);
        if (mediumSlabBytes > largeTier.budget - bytes) return false;
        const auto available = largeTier.budget - mediumSlabBytes - bytes;
        if (largeTier.retainedBytes > available) evicted.reserve(largeTier.slots);
        while (largeTier.retainedBytes > available && largeTier.slots != 0)
            evictOldest(largeTier, evicted);
        if (largeTier.retainedBytes <= available) {
            mediumSlabBytes += bytes;
            reserved = true;
        }
    }
    for (const auto& allocation : evicted) destroy(allocation);
    return reserved;
}

void BufferPool::releaseMediumBlock(VkDeviceSize bytes) noexcept {
    std::lock_guard lock(mutex);
    mediumSlabBytes -= bytes;
}

SlabSlot BufferPool::TakeSlot(const Context& context, std::uint32_t memoryType, std::size_t capacity, bool addressable) {
    const auto key = SlabKey(memoryType, capacity, addressable);
    const auto takeFrom = [&](SlabBlock& block, Slab& slab) {
        const auto index = block.free.back();
        block.free.pop_back();
        if (block.used++ == 0) --slab.emptyBlocks;
        if (block.free.empty()) slab.available.erase(std::find(slab.available.begin(), slab.available.end(), &block));
        const auto offset = static_cast<VkDeviceSize>(index) * block.slotBytes;
        return SlabSlot{block.memory, offset, block.mapping != nullptr ? block.mapping + offset : nullptr};
    };
    {
        std::lock_guard lock(slabMutex);
        auto& slab = slabs[key];
        if (!slab.available.empty()) return takeFrom(*slab.available.back(), slab);
    }
    const auto blockBytes = SlabBlockBytes(capacity);
    const auto release = [this](SlabBlock* block) noexcept {
        if (block->memory != VK_NULL_HANDLE || block->medium) freeBlock(*block);
        delete block;
    };
    std::unique_ptr<SlabBlock, decltype(release)> block(new SlabBlock, release);
    block->slotBytes = capacity;
    block->slab = key;
    if (capacity >= classLimit) {
        if (!reserveMediumBlock(blockBytes)) return {VK_NULL_HANDLE, 0, nullptr};
        block->medium = true;
    }
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    const VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, nullptr, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
    if (addressable) allocation.pNext = &flags;
    allocation.allocationSize = blockBytes;
    allocation.memoryTypeIndex = memoryType;
    if (block->medium) {
        try {
            block->memory = AllocateMemory(allocation, "vkAllocateMemory buffer medium slab");
        } catch (...) {
            return {VK_NULL_HANDLE, 0, nullptr};
        }
    } else {
        block->memory = AllocateMemory(allocation, "vkAllocateMemory buffer slab");
    }
    if ((context.memory.memoryTypes[memoryType].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
        void* mapping = nullptr;
        const auto mapped = mapMemory(device, block->memory, 0, VK_WHOLE_SIZE, 0, &mapping);
        if (mapped != VK_SUCCESS && block->medium) return {VK_NULL_HANDLE, 0, nullptr};
        Check(mapped, "vkMapMemory buffer slab");
        block->mapping = static_cast<std::byte*>(mapping);
    }
    const auto slots = static_cast<std::uint32_t>(blockBytes / capacity);
    block->free.reserve(slots);
    for (std::uint32_t index = slots; index-- != 0;) block->free.push_back(index);
    {
        std::lock_guard lock(slabMutex);
        auto& slab = slabs[key];
        if (slab.blocks == slab.available.capacity()) slab.available.reserve(std::max<std::size_t>(1, slab.available.capacity() * 2));
        const auto [owner, inserted] = slabBlocks.try_emplace(block->memory);
        if (!inserted) throw std::runtime_error("duplicate Vulkan buffer slab memory handle");
        owner->second.reset(block.release());
        slab.available.push_back(owner->second.get());
        ++slab.blocks;
        ++slab.emptyBlocks;
        return takeFrom(*slab.available.back(), slab);
    }

}
void BufferPool::freeBlock(SlabBlock& block) noexcept {
    if (block.memory != VK_NULL_HANDLE) {
        if (block.mapping != nullptr) unmap(device, block.memory);
        freeMemory(device, block.memory, nullptr);
    }
    if (block.medium) releaseMediumBlock(SlabBlockBytes(block.slotBytes));
}

VkDeviceSize BufferPool::PutSlot(VkDeviceMemory memory, VkDeviceSize offset) noexcept {
    std::unique_ptr<SlabBlock> released;
    {
        std::lock_guard lock(slabMutex);
        const auto found = slabBlocks.find(memory);
        if (found == slabBlocks.end()) return 0;
        auto& block = *found->second;
        auto& slab = slabs[block.slab];
        if (block.free.empty()) slab.available.push_back(&block);
        block.free.push_back(static_cast<std::uint32_t>(offset / block.slotBytes));
        if (--block.used != 0) return 0;
        if (!block.medium && slab.emptyBlocks == 0) {
            ++slab.emptyBlocks;
            return 0;
        }
        slab.available.erase(std::find(slab.available.begin(), slab.available.end(), &block));
        --slab.blocks;
        released = std::move(found->second);
        slabBlocks.erase(found);
    }
    freeBlock(*released);
    return SlabBlockBytes(released->slotBytes);
}

std::size_t BufferPool::SlabBlocks() {
    std::lock_guard lock(slabMutex);
    return slabBlocks.size();
}

VkDeviceSize BufferPool::MediumSlabBytes() {
    std::lock_guard lock(mutex);
    return mediumSlabBytes;
}

bool BufferPool::DeviceTierEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_STAGING_POOL_MIB");
        return value == nullptr || std::strtoull(value, nullptr, 10) != 0;
    }();
    return enabled;
}

VkDeviceSize BufferPool::DeviceBudget(const VkPhysicalDeviceMemoryProperties& memory, const VkPhysicalDeviceMemoryBudgetPropertiesEXT* reported) {
    static const char* value = std::getenv("APS5_STAGING_POOL_MIB");
    if (value != nullptr) return static_cast<VkDeviceSize>(std::strtoull(value, nullptr, 10)) << 20u;
    std::optional<std::uint32_t> largest;
    for (std::uint32_t heap = 0; heap < memory.memoryHeapCount && heap < VK_MAX_MEMORY_HEAPS; ++heap) {
        if ((memory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0 && (!largest || memory.memoryHeaps[heap].size > memory.memoryHeaps[*largest].size)) largest = heap;
    }
    VkDeviceSize size = largest ? memory.memoryHeaps[*largest].size : 0;
    if (largest && reported != nullptr && reported->heapBudget[*largest] != 0) size = std::min(size, reported->heapBudget[*largest]);
    if (largest && systemMemoryHeap(memory, *largest)) return std::max<VkDeviceSize>(VkDeviceSize{128} << 20u, size / 64u);
    return std::max<VkDeviceSize>(VkDeviceSize{512} << 20u, size / 8u);
}

VkDeviceSize BufferPool::deviceBudget(const Context& context) {
    if (!DeviceTierEnabled()) return 0;
    if (context.memoryProperties2 == nullptr || context.physical == VK_NULL_HANDLE) return DeviceBudget(context.memory);
    VkPhysicalDeviceMemoryBudgetPropertiesEXT reported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    properties.pNext = &reported;
    context.memoryProperties2(context.physical, &properties);
    return DeviceBudget(context.memory, &reported);
}

VkDeviceSize BufferPool::destroy(const BufferAllocation& allocation) noexcept {
    if (allocation.slab) {
        destroyBuffer(device, allocation.buffer, nullptr);
        return PutSlot(allocation.memory, allocation.offset);
    }
    // Device-local allocations (see DeviceBuffer) are never mapped.
    if (allocation.mapping != nullptr) unmap(device, allocation.memory);
    if (allocation.buffer != VK_NULL_HANDLE) destroyBuffer(device, allocation.buffer, nullptr);
    freeMemory(device, allocation.memory, nullptr);
    return allocation.allocationBytes;
}

bool BufferPool::DeviceTiered(VkMemoryPropertyFlags properties) {
    return (properties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 && (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0 && DeviceTierEnabled();
}

std::size_t BufferPool::Capacity(std::size_t bytes) {
    return Capacity(bytes, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
}

std::size_t BufferPool::Capacity(std::size_t bytes, VkMemoryPropertyFlags properties) {
    constexpr std::size_t smallest = 256;
    if (exactSizes()) return bytes;
    if (bytes < classLimit) return std::max(smallest, std::bit_ceil(bytes));
    if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 && bytes <= (std::size_t{4} << 20u) && mediumSlabsEnabled()) return std::bit_ceil(bytes);
    if (!DeviceTiered(properties)) return bytes;
    const auto step = std::bit_floor(bytes) >> deviceClassBits;
    return (bytes + step - 1) / step * step;
}

std::size_t BufferPool::NextCapacity(std::size_t capacity) {
    return capacity < classLimit ? capacity * 2 : capacity + (std::bit_floor(capacity) >> deviceClassBits);
}

VkBufferUsageFlags BufferPool::Usage(VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) {
    return DeviceTiered(properties) ? usage | deviceUsage : usage;
}

BufferPool::Tier& BufferPool::tierFor(std::size_t capacity, VkMemoryPropertyFlags properties) {
    const bool shared = sharedTiers();
    if (DeviceTiered(properties) && (shared || capacity >= classLimit)) return deviceTier;
    if (!shared && capacity < classLimit) return smallTier;
    return largeTier;
}

std::optional<BufferAllocation> BufferPool::Take(std::size_t bytes, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    const auto capacity = Capacity(bytes, properties);
    std::lock_guard lock(mutex);
    ++clock;
    if (profile) {
        static auto lastReport = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - lastReport > std::chrono::seconds(10)) {
            lastReport = now;
            AgcDriver::ProfilePrint_nid_no_patch("[bufferpool] small: %llu hits, %llu misses, %llu evictions, %zu retained (%.0f MiB); large: %llu hits, %llu misses, %llu evictions, %zu retained (%.0f MiB)%s; device: %llu hits, %llu misses, %llu evictions, %zu retained (%.0f MiB of %llu); images: %llu hits, %llu misses, %llu evictions, %zu retained (%.0f MiB of %llu) host_retained_mib=%.0f medium_slab_reserved_mib=%.0f host_budget_mib=512 steady_ns=%llu\n", static_cast<unsigned long long>(smallTier.hits), static_cast<unsigned long long>(smallTier.misses), static_cast<unsigned long long>(smallTier.evictions), smallTier.slots, smallTier.retainedBytes / 1048576.0, static_cast<unsigned long long>(largeTier.hits), static_cast<unsigned long long>(largeTier.misses), static_cast<unsigned long long>(largeTier.evictions), largeTier.slots, largeTier.retainedBytes / 1048576.0, sharedTiers() ? " (shared)" : "", static_cast<unsigned long long>(deviceTier.hits), static_cast<unsigned long long>(deviceTier.misses), static_cast<unsigned long long>(deviceTier.evictions), deviceTier.slots, deviceTier.retainedBytes / 1048576.0, static_cast<unsigned long long>(deviceTier.budget >> 20u), static_cast<unsigned long long>(imageHits), static_cast<unsigned long long>(imageMisses), static_cast<unsigned long long>(imageEvictions), imageSlots, imageBytes / 1048576.0, static_cast<unsigned long long>(imageBudget >> 20u), (largeTier.retainedBytes + mediumSlabBytes) / 1048576.0, mediumSlabBytes / 1048576.0, static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count()));
        }
    }
    auto& tier = tierFor(capacity, properties);
    const auto largest = &tier == &deviceTier && !exactSizes() && bytes <= std::numeric_limits<std::size_t>::max() / 2 ? std::max(capacity, bytes * 2) : capacity;
    auto found = tier.free.end();
    for (auto size = capacity; size <= largest && found == tier.free.end(); size = NextCapacity(size)) found = tier.free.find({size, usage, properties});
    if (found == tier.free.end() || found->second.empty()) {
        ++tier.misses;
        return std::nullopt;
    }
    auto& slots = found->second;
    const auto newest = std::prev(slots.end());
    auto chosen = newest;
    for (std::size_t scanned = 1; chosen->allocation.bufferBytes != bytes && scanned < exactSizeScan && chosen != slots.begin(); ++scanned) --chosen;
    if (chosen->allocation.bufferBytes != bytes) chosen = newest;
    auto result = chosen->allocation;
    slots.erase(chosen);
    if (slots.empty()) tier.free.erase(found);
    --tier.slots;
    tier.retainedBytes -= retainedCharge(result);
    ++tier.hits;
    return result;
}
std::optional<BufferAllocation> BufferPool::TakeHostTransfer(std::size_t bytes, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkDeviceSize requirementSize, VkDeviceSize alignment, std::uint32_t typeBits, std::uint32_t selectedType) {
    if (exactSizes() || bytes < classLimit || (usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) == 0 ||
        (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0 || bytes > hostGiantLimit ||
        alignment == 0 || selectedType >= 32 || (typeBits & (std::uint32_t{1} << selectedType)) == 0)
        return std::nullopt;
    const auto step = std::bit_floor(bytes) >> deviceClassBits;
    if (bytes > std::numeric_limits<std::size_t>::max() - step) return std::nullopt;
    const auto largest = NextCapacity(bytes);
    std::unique_lock lock(mutex);
    ++clock;
    // Shared by selection and the opt-in miss census; each idle slot has one first-failing gate.
    const auto keyGate = [&](const auto& key) {
        if (key.bytes < bytes) return 1u;
        if (key.bytes > largest) return 2u;
        if (key.usage != usage) return 3u;
        if (key.properties != properties) return 4u;
        return 0u;
    };
    const auto allocationGate = [&](const BufferAllocation& allocation) {
        if (allocation.memoryType != selectedType) return 5u;
        if (allocation.allocationBytes < requirementSize) return 6u;
        if (allocation.offset % alignment != 0) return 7u;
        if (allocation.slab) return 8u;
        if (allocation.buffer == VK_NULL_HANDLE) return 9u;
        if (allocation.memory == VK_NULL_HANDLE) return 10u;
        if (allocation.mapping == nullptr) return 11u;
        return 0u;
    };
    for (auto it = largeTier.free.begin(); it != largeTier.free.end(); ++it) {
        const auto& key = it->first;
        if (keyGate(key) != 0 || it->second.empty()) continue;
        auto& slots = it->second;
        for (auto slot = slots.begin(); slot != slots.end(); ++slot) {
            const auto& allocation = slot->allocation;
            if (allocationGate(allocation) != 0) continue;
            const auto result = allocation;
            slots.erase(slot);
            if (slots.empty()) largeTier.free.erase(it);
            --largeTier.slots;
            largeTier.retainedBytes -= retainedCharge(result);
            ++largeTier.hits;
            return result;
        }
    }
    ++largeTier.misses;
    static const bool diagnose = std::getenv("APS5_HOST_TRANSFER_MISS_DIAGNOSTIC") != nullptr;
    if (diagnose) {
        // Saturating process-wide reservation: no scans or log work after 64 observations.
        static std::atomic<unsigned> observations{0};
        auto observation = observations.load(std::memory_order_relaxed);
        while (observation < 64 && !observations.compare_exchange_weak(
                   observation, observation + 1, std::memory_order_relaxed)) {}
        if (observation < 64) {
            std::size_t gates[12]{};
            std::size_t eligible = 0;
            for (const auto& [key, slots] : largeTier.free) {
                if (slots.empty()) continue;
                const auto rejected = keyGate(key);
                if (rejected != 0) gates[rejected] += slots.size();
                else for (const auto& slot : slots) {
                    const auto allocationRejected = allocationGate(slot.allocation);
                    if (allocationRejected == 0) ++eligible;
                    else ++gates[allocationRejected];
                }
            }
            const auto poolClock = clock;
            const auto observed = std::chrono::steady_clock::now().time_since_epoch();
            lock.unlock();
            AgcDriver::ProfilePrint_nid_no_patch(
                "[host-transfer-miss] scope=idle-only-large-tier observation=%u pool=%p pool_clock=%llu steady_ns=%llu request_bytes=%zu requirement_size=%llu alignment=%llu typebits=0x%x selectedtype=%u usage=0x%x properties=0x%x range_ceiling=%zu eligible=%zu reject_below=%zu reject_above=%zu reject_usage=%zu reject_properties=%zu reject_type=%zu reject_size=%zu reject_alignment=%zu reject_slab=%zu reject_buffer=%zu reject_memory=%zu reject_mapping=%zu\n",
                observation + 1, static_cast<void*>(this), static_cast<unsigned long long>(poolClock),
                static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(observed).count()),
                bytes, static_cast<unsigned long long>(requirementSize), static_cast<unsigned long long>(alignment),
                typeBits, selectedType, usage, properties, largest,
                eligible, gates[1], gates[2], gates[3], gates[4], gates[5],
                gates[6], gates[7], gates[8], gates[9], gates[10], gates[11]);
        }
    }
    return std::nullopt;
}


void BufferPool::evictOldest(Tier& tier, std::vector<BufferAllocation>& evicted) {
    const auto oldest = std::min_element(tier.free.begin(), tier.free.end(), [](const auto& left, const auto& right) { return left.second.front().lastUse < right.second.front().lastUse; });
    // First: a throw here leaves the slot retained and counted.
    evicted.push_back(oldest->second.front().allocation);
    if (oldest->second.front().allocation.buffer != VK_NULL_HANDLE || !ImageSizeClasses()) tier.retainedBytes -= retainedCharge(oldest->second.front().allocation);
    if (oldest->second.front().allocation.buffer == VK_NULL_HANDLE) {
        imageOrder.erase(oldest->second.front().order);
        imageBytes -= oldest->second.front().allocation.allocationBytes;
        --imageSlots;
        ++imageEvictions;
    }
    oldest->second.pop_front();
    if (oldest->second.empty()) tier.free.erase(oldest);
    --tier.slots;
    ++tier.evictions;
}

bool BufferPool::evictOldestImage(std::vector<BufferAllocation>& evicted) {
    if (imageOrder.empty()) return false;
    const Slot* oldest = imageOrder.front();
    const SlotKey key{oldest->allocation.bytes, 0, oldest->allocation.properties, oldest->allocation.memoryType};
    const auto found = deviceTier.free.find(key);
    evicted.push_back(oldest->allocation);
    imageOrder.pop_front();
    const auto bytes = oldest->allocation.allocationBytes;
    if (!ImageSizeClasses()) deviceTier.retainedBytes -= bytes;
    imageBytes -= bytes;
    found->second.pop_front();
    if (found->second.empty()) deviceTier.free.erase(found);
    --deviceTier.slots;
    --imageSlots;
    ++deviceTier.evictions;
    ++imageEvictions;
    return true;
}

void BufferPool::NotePresent() noexcept {
    presentEpoch->fetch_add(1, std::memory_order_relaxed);
}

VkDeviceSize BufferPool::ReclaimIdleDevice(VkDeviceSize bytes, std::chrono::steady_clock::time_point now) {
    if (bytes == 0) return 0;
    const auto heaps = std::min<std::uint32_t>(memoryProperties.memoryHeapCount, VK_MAX_MEMORY_HEAPS);
    if (std::count_if(memoryProperties.memoryHeaps, memoryProperties.memoryHeaps + heaps, [](const auto& heap) { return (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0; }) != 1) return 0;
    std::vector<BufferAllocation> evicted;
    {
        std::lock_guard lock(mutex);
        const auto presentGeneration = presentEpoch->load(std::memory_order_relaxed);
        if (!idleRead) {
            idleRead = now;
            idleClock = clock;
            return 0;
        }
        if (now < *idleRead) return 0;
        if (now - *idleRead >= std::chrono::seconds(1)) {
            reclaimBefore = idleClock;
            idleClock = clock;
            idleRead = now;
        }
        if (reclaimBefore <= scannedIdleClock && presentGeneration <= scannedPresentGeneration) return 0;
        VkDeviceSize selected = 0;
        while (selected < bytes) {
            auto oldest = deviceTier.free.end();
            for (auto it = deviceTier.free.begin(); it != deviceTier.free.end(); ++it) {
                if (it->second.empty()) continue;
                const auto& slot = it->second.front();
                // Under size classes idle image memory (up to its own 1 GiB budget) yields to
                // texture-cache pressure too; without them the image share stays as before.
                if ((slot.allocation.buffer == VK_NULL_HANDLE && !ImageSizeClasses()) || slot.allocation.slab || slot.lastUse > reclaimBefore ||
                    presentGeneration < slot.lastPresent || presentGeneration - slot.lastPresent < 2) continue;
                if (oldest == deviceTier.free.end() || slot.lastUse < oldest->second.front().lastUse) oldest = it;
            }
            if (oldest == deviceTier.free.end()) {
                scannedIdleClock = reclaimBefore;
                scannedPresentGeneration = presentGeneration;
                break;
            }
            if (evicted.empty()) evicted.reserve(deviceTier.slots);
            const auto allocation = oldest->second.front().allocation;
            evicted.push_back(allocation);
            selected += allocation.allocationBytes;
            if (allocation.buffer == VK_NULL_HANDLE) {
                imageOrder.erase(oldest->second.front().order);
                imageBytes -= allocation.allocationBytes;
                --imageSlots;
                ++imageEvictions;
            } else {
                deviceTier.retainedBytes -= allocation.allocationBytes;
            }
            oldest->second.pop_front();
            if (oldest->second.empty()) deviceTier.free.erase(oldest);
            --deviceTier.slots;
            ++deviceTier.evictions;
        }
    }
    VkDeviceSize released = 0;
    for (const auto& allocation : evicted) released += destroy(allocation);
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (profile && released != 0) AgcDriver::ProfilePrint_nid_no_patch("[bufferpool] idle device reclaim: %llu bytes released for %llu bytes of texture-cache pressure\n", static_cast<unsigned long long>(released), static_cast<unsigned long long>(bytes));
    return released;
}

std::size_t BufferPool::MaxSlots() {
    // APS5_BUFFER_POOL_SLOTS=<n> bounds the retained allocations; 64 is the capacity the pool had
    // before least-recently-used retention, for A/B runs.
    static const std::size_t slots = [] {
        const char* value = std::getenv("APS5_BUFFER_POOL_SLOTS");
        const auto parsed = value != nullptr ? std::strtoull(value, nullptr, 10) : 0;
        return parsed != 0 ? static_cast<std::size_t>(parsed) : defaultSlots;
    }();
    return slots;
}

VkDeviceSize BufferPool::trim() {
    Tier retiredSmall;
    Tier retiredLarge;
    Tier retiredDevice;
    {
        std::lock_guard lock(mutex);
        for (const auto& [tier, retired] : {std::pair{&smallTier, &retiredSmall}, std::pair{&largeTier, &retiredLarge}, std::pair{&deviceTier, &retiredDevice}}) {
            tier->free.swap(retired->free);
            tier->evictions += tier->slots;
            tier->retainedBytes = 0;
            tier->slots = 0;
        }
        imageEvictions += imageSlots;
        imageBytes = 0;
        imageSlots = 0;
        imageOrder.clear();
    }
    VkDeviceSize bytes = 0;
    for (const auto* tier : {&retiredSmall, &retiredLarge, &retiredDevice}) {
        for (const auto& [key, slots] : tier->free) {
            for (const auto& slot : slots) bytes += destroy(slot.allocation);
        }
    }
    for (;;) {
        std::unique_ptr<SlabBlock> released;
        {
            std::lock_guard lock(slabMutex);
            const auto found = std::find_if(slabBlocks.begin(), slabBlocks.end(), [](const auto& entry) { return entry.second->used == 0; });
            if (found == slabBlocks.end()) break;
            auto& slab = slabs.at(found->second->slab);
            slab.available.erase(std::find(slab.available.begin(), slab.available.end(), found->second.get()));
            --slab.blocks;
            --slab.emptyBlocks;
            released = std::move(found->second);
            slabBlocks.erase(found);
        }
        freeBlock(*released);
        bytes += SlabBlockBytes(released->slotBytes);
    }
    return bytes;
}

void BufferPool::Put(const BufferAllocation& allocation) noexcept {
    // Destroy outside the mutex, then recheck the budget: evicting a cached slab buffer can
    // release its backing reservation, which must not evict an unrelated warm allocation.
    std::vector<BufferAllocation> evicted;
    try {
        std::unique_lock lock(mutex);
        auto& tier = tierFor(allocation.bytes, allocation.properties);
        const bool hostGiant = &tier == &largeTier && (allocation.properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 && allocation.allocationBytes > hostGiantLimit;
        const auto exceedsBudget = [&] {
            const auto reserved = &tier == &largeTier ? mediumSlabBytes : VkDeviceSize{0};
            return tier.retainedBytes + retainedCharge(allocation) + reserved > tier.budget;
        };
        if (retainedCharge(allocation) > tier.budget || hostGiant) {
            evicted.push_back(allocation);
        } else {
            while (tier.slots != 0 && (exceedsBudget() || tier.slots >= maxSlots)) {
                evictOldest(tier, evicted);
                lock.unlock();
                for (const auto& gone : evicted) destroy(gone);
                evicted.clear();
                lock.lock();
            }
            if (exceedsBudget()) {
                evicted.push_back(allocation);
            } else {
                tier.free[{allocation.bytes, allocation.usage, allocation.properties}].push_back({allocation, ++clock, presentEpoch->load(std::memory_order_relaxed), {}});
                ++tier.slots;
                tier.retainedBytes += retainedCharge(allocation);
            }
        }
    } catch (...) {
        destroy(allocation);
    }
    for (const auto& gone : evicted) destroy(gone);
}

std::shared_ptr<BufferPool> GetBufferPool(const Context& context) {
    if (!context.bufferPool) context.bufferPool = std::make_shared<BufferPool>(context);
    return context.bufferPool;
}

VkDeviceMemory AllocateImageMemory(const Context& context, const VkMemoryAllocateInfo& allocation, std::uint32_t mask, const char* operation, std::uint32_t* allocatedType, bool newImage, ImageMemoryTiming* timing, VkDeviceSize* allocatedBytes) {
    return GetBufferPool(context)->TakeImageMemory(allocation, operation, context.RefusedDeviceMemoryFallback(mask, allocation.memoryTypeIndex), allocatedType, newImage, timing, allocatedBytes);
}

void ReleaseImageMemory(const Context& context, VkDeviceMemory memory, VkDeviceSize bytes, std::uint32_t memoryType) noexcept {
    if (memory == VK_NULL_HANDLE) return;
    if (context.bufferPool) {
        context.bufferPool->PutImageMemory(memory, bytes, memoryType);
        return;
    }
    context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
}

}
