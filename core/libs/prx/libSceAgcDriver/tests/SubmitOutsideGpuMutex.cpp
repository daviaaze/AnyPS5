#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace AgcDriver::Graphics;

struct MockVulkan {
    std::mutex mutex;
    std::condition_variable changed;
    std::uintptr_t next = 1;
    std::uint32_t submits = 0;
    std::uint32_t failCall = 0;
    VkResult failResult = VK_ERROR_OUT_OF_HOST_MEMORY;
    bool blockFirst = false;
    bool firstEntered = false;
    bool releaseFirst = false;
    bool latchTimedOut = false;
    std::uint64_t timelineValue = 0;
    std::map<VkFence, bool> fences;
    std::vector<int> events;
    std::vector<std::uint64_t> serials;
};

MockVulkan mock;
std::mutex failuresMutex;
int failures = 0;

void Expect(bool condition, const char* reason) {
    if (condition) return;
    std::lock_guard lock(failuresMutex);
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", reason);
}

void resetMock() {
    std::lock_guard lock(mock.mutex);
    mock.next = 1;
    mock.submits = 0;
    mock.failCall = 0;
    mock.failResult = VK_ERROR_OUT_OF_HOST_MEMORY;
    mock.blockFirst = false;
    mock.firstEntered = false;
    mock.releaseFirst = false;
    mock.latchTimedOut = false;
    mock.timelineValue = 0;
    mock.fences.clear();
    mock.events.clear();
    mock.serials.clear();
}

VKAPI_ATTR VkResult VKAPI_CALL AllocateCommands(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer* commands) {
    std::lock_guard lock(mock.mutex);
    *commands = reinterpret_cast<VkCommandBuffer>(mock.next++);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL FreeCommands(VkDevice, VkCommandPool, std::uint32_t, const VkCommandBuffer*) {}

VKAPI_ATTR VkResult VKAPI_CALL CreateFence(VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence* fence) {
    std::lock_guard lock(mock.mutex);
    *fence = reinterpret_cast<VkFence>(mock.next++);
    mock.fences[*fence] = false;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyFence(VkDevice, VkFence fence, const VkAllocationCallbacks*) {
    std::lock_guard lock(mock.mutex);
    mock.fences.erase(fence);
}

VKAPI_ATTR VkResult VKAPI_CALL GetFenceStatus(VkDevice, VkFence fence) {
    std::lock_guard lock(mock.mutex);
    return mock.fences.at(fence) ? VK_SUCCESS : VK_NOT_READY;
}

VKAPI_ATTR VkResult VKAPI_CALL ResetFences(VkDevice, std::uint32_t count, const VkFence* fences) {
    std::lock_guard lock(mock.mutex);
    for (std::uint32_t i = 0; i < count; ++i) mock.fences.at(fences[i]) = false;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL WaitForFences(VkDevice, std::uint32_t count, const VkFence* fences, VkBool32, std::uint64_t) {
    std::lock_guard lock(mock.mutex);
    for (std::uint32_t i = 0; i < count; ++i) mock.fences.at(fences[i]) = true;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL BeginCommands(VkCommandBuffer, const VkCommandBufferBeginInfo*) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL EndCommands(VkCommandBuffer) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue, std::uint32_t, const VkSubmitInfo* submissions, VkFence fence) {
    std::unique_lock lock(mock.mutex);
    const auto call = ++mock.submits;
    mock.events.push_back(static_cast<int>(call));
    if (call == 1 && mock.blockFirst) {
        mock.firstEntered = true;
        mock.changed.notify_all();
        if (!mock.changed.wait_for(lock, std::chrono::seconds(5), [] { return mock.releaseFirst; })) mock.latchTimedOut = true;
    }
    const auto result = call == mock.failCall ? mock.failResult : VK_SUCCESS;
    if (submissions->pNext != nullptr) {
        const auto* timeline = static_cast<const VkTimelineSemaphoreSubmitInfoKHR*>(submissions->pNext);
        if (timeline->signalSemaphoreValueCount != 0) {
            const auto serial = timeline->pSignalSemaphoreValues[0];
            mock.serials.push_back(serial);
            if (result == VK_SUCCESS) mock.timelineValue = serial;
        }
    }
    if (result == VK_SUCCESS) mock.fences[fence] = false;
    mock.changed.notify_all();
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL DeviceWaitIdle(VkDevice) { return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL CmdUpdateBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, const void*) {}
VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, std::uint32_t, const VkMemoryBarrier*, std::uint32_t, const VkBufferMemoryBarrier*, std::uint32_t, const VkImageMemoryBarrier*) {}
VKAPI_ATTR void VKAPI_CALL CmdBeginQuery(VkCommandBuffer, VkQueryPool, std::uint32_t, VkQueryControlFlags) {}
VKAPI_ATTR void VKAPI_CALL CmdEndQuery(VkCommandBuffer, VkQueryPool, std::uint32_t) {}
VKAPI_ATTR void VKAPI_CALL CmdResetQueryPool(VkCommandBuffer, VkQueryPool, std::uint32_t, std::uint32_t) {}
VKAPI_ATTR void VKAPI_CALL CmdCopyQueryPoolResults(VkCommandBuffer, VkQueryPool, std::uint32_t, std::uint32_t, VkBuffer, VkDeviceSize, VkDeviceSize, VkQueryResultFlags) {}
VKAPI_ATTR void VKAPI_CALL CmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {}
VKAPI_ATTR void VKAPI_CALL CmdPushConstants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, std::uint32_t, std::uint32_t, const void*) {}
VKAPI_ATTR void VKAPI_CALL CmdDispatch(VkCommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t) {}

VKAPI_ATTR VkResult VKAPI_CALL CreateSemaphore(VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore* semaphore) {
    std::lock_guard lock(mock.mutex);
    *semaphore = reinterpret_cast<VkSemaphore>(mock.next++);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroySemaphore(VkDevice, VkSemaphore, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL WaitSemaphores(VkDevice, const VkSemaphoreWaitInfoKHR*, std::uint64_t) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL GetSemaphoreCounter(VkDevice, VkSemaphore, std::uint64_t* value) {
    std::lock_guard lock(mock.mutex);
    *value = mock.timelineValue;
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL DeviceProc(VkDevice, const char* name) {
    static const std::map<std::string_view, PFN_vkVoidFunction> table{
        {"vkAllocateCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(AllocateCommands)},
        {"vkFreeCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(FreeCommands)},
        {"vkCreateFence", reinterpret_cast<PFN_vkVoidFunction>(CreateFence)},
        {"vkDestroyFence", reinterpret_cast<PFN_vkVoidFunction>(DestroyFence)},
        {"vkGetFenceStatus", reinterpret_cast<PFN_vkVoidFunction>(GetFenceStatus)},
        {"vkResetFences", reinterpret_cast<PFN_vkVoidFunction>(ResetFences)},
        {"vkWaitForFences", reinterpret_cast<PFN_vkVoidFunction>(WaitForFences)},
        {"vkBeginCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(BeginCommands)},
        {"vkEndCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(EndCommands)},
        {"vkQueueSubmit", reinterpret_cast<PFN_vkVoidFunction>(QueueSubmit)},
        {"vkDeviceWaitIdle", reinterpret_cast<PFN_vkVoidFunction>(DeviceWaitIdle)},
        {"vkCmdUpdateBuffer", reinterpret_cast<PFN_vkVoidFunction>(CmdUpdateBuffer)},
        {"vkCmdPipelineBarrier", reinterpret_cast<PFN_vkVoidFunction>(CmdPipelineBarrier)},
        {"vkCmdBeginQuery", reinterpret_cast<PFN_vkVoidFunction>(CmdBeginQuery)},
        {"vkCmdEndQuery", reinterpret_cast<PFN_vkVoidFunction>(CmdEndQuery)},
        {"vkCmdResetQueryPool", reinterpret_cast<PFN_vkVoidFunction>(CmdResetQueryPool)},
        {"vkCmdCopyQueryPoolResults", reinterpret_cast<PFN_vkVoidFunction>(CmdCopyQueryPoolResults)},
        {"vkCmdBindPipeline", reinterpret_cast<PFN_vkVoidFunction>(CmdBindPipeline)},
        {"vkCmdPushConstants", reinterpret_cast<PFN_vkVoidFunction>(CmdPushConstants)},
        {"vkCmdDispatch", reinterpret_cast<PFN_vkVoidFunction>(CmdDispatch)},
        {"vkCreateSemaphore", reinterpret_cast<PFN_vkVoidFunction>(CreateSemaphore)},
        {"vkDestroySemaphore", reinterpret_cast<PFN_vkVoidFunction>(DestroySemaphore)},
        {"vkWaitSemaphoresKHR", reinterpret_cast<PFN_vkVoidFunction>(WaitSemaphores)},
        {"vkGetSemaphoreCounterValueKHR", reinterpret_cast<PFN_vkVoidFunction>(GetSemaphoreCounter)},
    };
    const auto it = table.find(name);
    return it == table.end() ? nullptr : it->second;
}

Context MockContext(QueueLane& lane) {
    Context context{};
    context.device = reinterpret_cast<VkDevice>(std::uintptr_t{1});
    context.queue = reinterpret_cast<VkQueue>(std::uintptr_t{2});
    context.deviceProc = DeviceProc;
    context.queueLane = &lane;
    return context;
}

template<typename TPredicate>
bool WaitUntil(TPredicate&& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

std::vector<int> Events() {
    std::lock_guard lock(mock.mutex);
    return mock.events;
}

VkResult QueueEvent(int event) {
    std::lock_guard lock(mock.mutex);
    mock.events.push_back(event);
    mock.changed.notify_all();
    return VK_SUCCESS;
}

void OrderedSubmitAndPresent() {
    resetMock();
    {
        std::lock_guard lock(mock.mutex);
        mock.blockFirst = true;
    }
    QueueLane lane;
    const auto context = MockContext(lane);
    std::unique_lock gpu(AgcDriver::GuestMemory::GpuMutex());
    Recorder recorder(context, true);
    gpu.unlock();

    constexpr std::uint64_t firstAddress = 0x10000;
    constexpr std::uint64_t secondAddress = 0x20000;
    std::atomic<std::uint64_t> firstSerial{0};
    std::atomic<std::uint64_t> secondSerial{0};
    std::atomic<bool> observerAcquired{false};
    std::atomic<bool> observerValid{false};
    std::atomic<bool> threadFailed{false};

    std::thread first([&] {
        try {
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            recorder.NotePendingWrite(firstAddress, 4);
            firstSerial.store(recorder.PendingWriterSerial(firstAddress, 4), std::memory_order_release);
            recorder.Submit();
        } catch (const std::exception& error) {
            threadFailed.store(true, std::memory_order_release);
            Expect(false, error.what());
        }
    });
    bool submitBlocked = false;
    {
        std::unique_lock lock(mock.mutex);
        submitBlocked = mock.changed.wait_for(lock, std::chrono::seconds(5), [] { return mock.firstEntered; });
    }
    Expect(submitBlocked, "mock vkQueueSubmit did not reach its blocking latch");

    std::thread second([&] {
        try {
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            recorder.NotePendingWrite(secondAddress, 4);
            secondSerial.store(recorder.PendingWriterSerial(secondAddress, 4), std::memory_order_release);
            recorder.Submit();
        } catch (const std::exception& error) {
            threadFailed.store(true, std::memory_order_release);
            Expect(false, error.what());
        }
    });
    const bool secondReserved = WaitUntil([&] { return lane.Reserved() >= 2; });
    Expect(secondReserved, "second recorder submission did not reserve its queue turn");

    std::thread presenter;
    if (secondReserved) {
        presenter = std::thread([&] {
            try {
                std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
                observerAcquired.store(true, std::memory_order_release);
                const auto first = recorder.DescribePendingWrite(firstAddress, 4);
                const auto second = recorder.DescribePendingWrite(secondAddress, 4);
                observerValid.store(recorder.Submissions() == 0 && recorder.InFlightBatches() == 0 && first && second
                    && !first->open && !second->open && first->serial == 1 && second->serial == 2
                    && !first->signaled && !second->signaled
                    && recorder.PendingWriteOverlaps(firstAddress, 4) && recorder.PendingWriteOverlaps(secondAddress, 4),
                    std::memory_order_release);
                recorder.SubmitAndEpoch();
                Expect(recorder.Submissions() == 2 && recorder.InFlightBatches() == 2,
                    "successful staged submissions were not published to inFlight before presentation");
                const auto firstPublished = recorder.DescribePendingWrite(firstAddress, 4);
                const auto secondPublished = recorder.DescribePendingWrite(secondAddress, 4);
                Expect(firstPublished && secondPublished && !firstPublished->open && !secondPublished->open
                    && firstPublished->serial == 1 && secondPublished->serial == 2
                    && !firstPublished->signaled && !secondPublished->signaled,
                    "published batch fences or serials were not visible after the queue calls returned");
                const auto queueCall = [&](int event) {
                    const auto result = OnQueue(context.queueLane,
                        [] { return AgcDriver::GuestMemory::GpuMutex().DepthOnThisThread(); },
                        [] { AgcDriver::GuestMemory::GpuMutex().unlock(); },
                        [] { AgcDriver::GuestMemory::GpuMutex().lock(); },
                        [&] { return QueueEvent(event); });
                    Expect(result == VK_SUCCESS, "mock presentation queue operation failed");
                };
                queueCall(3);
                queueCall(4);
            } catch (const std::exception& error) {
                threadFailed.store(true, std::memory_order_release);
                Expect(false, error.what());
            }
        });
        Expect(WaitUntil([&] { return observerAcquired.load(std::memory_order_acquire); }),
            "presenter could not acquire GpuMutex while vkQueueSubmit was blocked");
    }

    std::atomic<bool> independentAcquired{false};
    std::thread independent([&] {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        independentAcquired.store(true, std::memory_order_release);
    });
    Expect(WaitUntil([&] { return independentAcquired.load(std::memory_order_acquire); }),
        "another thread could not acquire GpuMutex while vkQueueSubmit was blocked");
    Expect(Events() == std::vector<int>{1}, "a later primary-queue operation passed a blocked submission");
    {
        std::lock_guard lock(mock.mutex);
        mock.releaseFirst = true;
    }
    mock.changed.notify_all();
    first.join();
    second.join();
    if (presenter.joinable()) presenter.join();
    independent.join();

    Expect(!threadFailed.load(std::memory_order_acquire), "a submit or presentation worker threw");
    Expect(!mock.latchTimedOut, "the first submit latch timed out");
    Expect(observerValid.load(std::memory_order_acquire), "submitting batches were not visible to pending-write queries");
    Expect(firstSerial.load(std::memory_order_acquire) == 1 && secondSerial.load(std::memory_order_acquire) == 2,
        "reserved recorder serials were not unique and increasing");
    Expect(Events() == std::vector<int>{1, 2, 3, 4}, "recorded submits, blit submit, and present were not queue ordered");
    {
        std::lock_guard lock(mock.mutex);
        Expect(mock.serials == std::vector<std::uint64_t>{1, 2}, "Vulkan timeline signals did not carry unique increasing serials");
    }

    gpu.lock();
    Expect(recorder.Submissions() == 2 && recorder.InFlightBatches() == 2,
        "completed queue calls did not publish their accepted serials and fences");
    recorder.Sync();
    Expect(recorder.Idle(), "the test recorder did not drain after synchronization");
}

void LockedSubmitWhenLaneDisabled() {
    resetMock();
    {
        std::lock_guard lock(mock.mutex);
        mock.blockFirst = true;
    }
    QueueLane lane;
    auto context = MockContext(lane);
    context.queueLane = nullptr;
    std::unique_lock gpu(AgcDriver::GuestMemory::GpuMutex());
    Recorder recorder(context);
    gpu.unlock();
    std::atomic<bool> attempting{false};
    std::atomic<bool> acquired{false};
    std::atomic<bool> submitFailed{false};
    std::thread submitter([&] {
        try {
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            recorder.NotePendingWrite(0x40000, 4);
            recorder.Submit();
        } catch (const std::exception& error) {
            submitFailed.store(true, std::memory_order_release);
            Expect(false, error.what());
        }
    });
    bool submitBlocked = false;
    {
        std::unique_lock lock(mock.mutex);
        submitBlocked = mock.changed.wait_for(lock, std::chrono::seconds(5), [] { return mock.firstEntered; });
    }
    Expect(submitBlocked, "disabled-lane vkQueueSubmit did not reach its blocking latch");
    std::thread observer([&] {
        attempting.store(true, std::memory_order_release);
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        acquired.store(true, std::memory_order_release);
    });
    Expect(WaitUntil([&] { return attempting.load(std::memory_order_acquire); }), "disabled-lane observer did not start");
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    Expect(!acquired.load(std::memory_order_acquire), "disabled-lane submission released GpuMutex around vkQueueSubmit");
    {
        std::lock_guard lock(mock.mutex);
        mock.releaseFirst = true;
    }
    mock.changed.notify_all();
    submitter.join();
    observer.join();
    Expect(!submitFailed.load(std::memory_order_acquire), "disabled-lane submission worker threw");
    Expect(acquired.load(std::memory_order_acquire), "disabled-lane observer did not acquire GpuMutex after submit returned");
    Expect(!mock.latchTimedOut, "disabled-lane submit latch timed out");
    gpu.lock();
    Expect(recorder.Submissions() == 1 && recorder.InFlightBatches() == 1, "disabled-lane submission bookkeeping changed");
    recorder.Sync();
}


void FailedSubmitHasNoPhantomSerial() {
    resetMock();
    {
        std::lock_guard lock(mock.mutex);
        mock.failCall = 1;
        mock.failResult = VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    QueueLane lane;
    const auto context = MockContext(lane);
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Recorder recorder(context, true);
    constexpr std::uint64_t address = 0x30000;
    recorder.NotePendingWrite(address, 4);
    bool submitFailed = false;
    try {
        recorder.Submit();
    } catch (const std::runtime_error&) {
        submitFailed = true;
    }
    Expect(submitFailed, "failed vkQueueSubmit was not reported");
    Expect(recorder.Submissions() == 0 && recorder.InFlightBatches() == 0 && recorder.Idle()
        && !recorder.PendingWriteOverlaps(address, 4) && recorder.PendingWriterSerial(address, 4) == 0,
        "failed submission left a phantom serial, in-flight fence, or pending write");
    bool phantomWaitFailed = false;
    try {
        recorder.WaitSerial(1);
    } catch (const std::runtime_error&) {
        phantomWaitFailed = true;
    }
    Expect(phantomWaitFailed, "a wait for the failed serial did not fail instead of waiting forever");
    std::lock_guard mockLock(mock.mutex);
    Expect(mock.timelineValue == 0, "failed submission advanced the timeline semaphore");
}

}

int main() {
    OrderedSubmitAndPresent();
    LockedSubmitWhenLaneDisabled();
    FailedSubmitHasNoPhantomSerial();
    if (failures != 0) {
        std::fprintf(stderr, "%d submit-outside-GPU-mutex checks failed\n", failures);
        return 1;
    }
    std::printf("submit outside GPU mutex tests passed\n");
    return 0;
}
