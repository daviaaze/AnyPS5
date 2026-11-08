#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Prepare/PrepareFarm.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Farm = Driver::PrepareFarm;
constexpr std::uint32_t Compute = 0x20u;

alignas(256) std::array<std::uint32_t, 7> CounterCode{0xf4080200u, 0xfa000000u, 0xbf8cc07fu, 0x7e000281u, 0xe0c80000u, 0x80020000u, 0xbf810000u};
alignas(256) std::array<std::uint32_t, 7> StoreCode{0xf4080200u, 0xfa000000u, 0xbf8cc07fu, 0x7e000202u, 0xe0700000u, 0x80020000u, 0xbf810000u};
alignas(256) std::array<std::uint32_t, 4> A{};
alignas(256) std::array<std::uint32_t, 4> B{};
alignas(256) std::array<std::uint32_t, 4> C{};
alignas(256) std::array<std::uint32_t, 4> DescriptorA{};
alignas(256) std::array<std::uint32_t, 4> DescriptorB{};
alignas(256) std::array<std::uint32_t, 4> DescriptorC{};
std::atomic<bool> InvalidateCapture{false};
std::string BeforeTakeStatuses;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
std::uint32_t Low(const volatile void* pointer) { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer)); }
std::uint32_t High(const volatile void* pointer) { return static_cast<std::uint32_t>(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer)) >> 32u); }
std::array<std::uint32_t, 4> Descriptor(const void* pointer) { return {Low(pointer), High(pointer) & 0xffffu, sizeof(A), 0x31016fac}; }
void SetReg(std::vector<std::uint32_t>& words, std::uint32_t offset, std::uint32_t value) { words.insert(words.end(), {0xc0017600u, offset, value}); }
std::size_t Dispatch(std::vector<std::uint32_t>& words, const void* code, std::initializer_list<std::uint32_t> data) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(code));
    SetReg(words, 0x20c, static_cast<std::uint32_t>(address >> 8u));
    SetReg(words, 0x20d, static_cast<std::uint32_t>(address >> 40u) & 0xffu);
    for (const auto offset : {0x207u, 0x208u, 0x209u}) SetReg(words, offset, 1);
    SetReg(words, 0x212, 0);
    SetReg(words, 0x213, static_cast<std::uint32_t>(data.size()) << 1u);
    std::uint32_t offset = 0x240;
    for (const auto value : data) SetReg(words, offset++, value);
    const auto packetOffset = words.size();
    words.insert(words.end(), {0xc0031500u, 1, 1, 1, 0x8041u});
    return packetOffset;
}
std::uint64_t Submit(std::vector<std::uint32_t>& words) {
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require(sceAgcDriverSubmitAcb(Compute, &packet) == 0, "compute submit failed");
    std::uint64_t accepted = 0, completed = 0;
    AgcDriverSerialProgress_nid_postfix(&accepted, &completed);
    return accepted;
}
void BeforeAdoption() {
    if (InvalidateCapture.exchange(false, std::memory_order_acq_rel)) {
        reinterpret_cast<volatile std::uint32_t&>(DescriptorA[0]) = Low(B.data());
    }
}
void WaitIdle() {
    AgcDriverWaitIdle_nid_postfix();
    for (const auto* buffer : {&A, &B, &C}) AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(buffer->data()), sizeof(*buffer));
}
void Reset() {
    WaitIdle();
    A = {}; B = {}; C = {};
    DescriptorA = Descriptor(A.data());
    DescriptorB = Descriptor(B.data());
    DescriptorC = Descriptor(C.data());
}
const char *StatusName(Farm::PeekStatus status) {
    switch (status) {
    case Farm::PeekStatus::Queued: return "queued";
    case Farm::PeekStatus::Running: return "running";
    case Farm::PeekStatus::Ready: return "ready";
    case Farm::PeekStatus::Failed: return "failed";
    case Farm::PeekStatus::Absent: return "absent";
    }
    return "unknown";
}
std::string FarmCounters() {
    const auto farm = Farm::Get().GetCounters();
    const auto outcomes = Farm::Get().GetAdoptionCounters();
    return "farm{submitted=" + std::to_string(farm.submitted) +
        ",adopted=" + std::to_string(farm.adopted) +
        ",discarded=" + std::to_string(farm.discarded) +
        ",rejected=" + std::to_string(farm.rejectedSaturated) +
        ",outcome_adopted=" + std::to_string(outcomes.adopted) +
        ",absent=" + std::to_string(outcomes.absent) +
        ",not_ready=" + std::to_string(outcomes.notReady) +
        ",stale_key=" + std::to_string(outcomes.staleKey) +
        ",stale_capture=" + std::to_string(outcomes.staleCapture) +
        ",failed=" + std::to_string(outcomes.failed) +
        ",last_error=" + Farm::Get().GetLastErrorForTest() + "}";
}
void ReportOutcomes(const char *label) {
    std::cerr << label << ": " << FarmCounters() << '\n';
}
void WaitBeforeTake(std::uint32_t queue, std::uint64_t serial, std::size_t offset) {
    Farm::Get().WaitSettledForTest(queue, serial, offset);
    BeforeTakeStatuses += " [serial=" + std::to_string(serial) +
        ",offset=" + std::to_string(offset) + ":" +
        StatusName(Farm::Get().Peek(queue, serial, offset).status) + "]";
}
void WaitJob(std::uint64_t serial, std::size_t offset) {
    Farm::Get().WaitSettledForTest(Compute, serial, offset);
}
}

int main() {
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        device.reset();
        Farm::Get().ConfigureForTest({2, 64, 64u * 1024u * 1024u});
        Farm::Get().SetBeforeTakeHookForTest(&WaitBeforeTake);
        AgcDriverSetAheadAdoptHook_nid_postfix(&BeforeAdoption);

        Reset();
        std::vector<std::uint32_t> first;
        Dispatch(first, StoreCode.data(), {Low(DescriptorA.data()), High(DescriptorA.data()), 11});
        const auto secondOffset = Dispatch(first, StoreCode.data(), {Low(DescriptorB.data()), High(DescriptorB.data()), 22});
        const auto adoptedBaseline = Farm::Get().GetAdoptionCounters().adopted;
        BeforeTakeStatuses.clear();
        const auto serial = Submit(first);
        WaitJob(serial, secondOffset);
        WaitIdle();
        Require(A[0] == 11 && B[0] == 22, "worker preparation/adoption produced wrong output");
        Require(Farm::Get().GetAdoptionCounters().adopted > adoptedBaseline, "worker-prepared dispatch was not adopted");
        ReportOutcomes("worker adoption");

        Reset();
        const auto staleBaseline = Farm::Get().GetAdoptionCounters().staleCapture;
        InvalidateCapture = true;
        std::vector<std::uint32_t> stale;
        const auto staleOffset = Dispatch(stale, StoreCode.data(), {Low(DescriptorA.data()), High(DescriptorA.data()), 77});
        BeforeTakeStatuses.clear();
        const auto staleSerial = Submit(stale);
        WaitJob(staleSerial, staleOffset);
        WaitIdle();
        Require(DescriptorA[0] == Low(B.data()), "CPU store hook did not replace the captured descriptor");
        Require(A[0] == 0 && B[0] == 77 && C[0] == 0, "CPU store between prepare and adoption used stale captured data");
        Require(Farm::Get().GetAdoptionCounters().staleCapture > staleBaseline, "stale capture was not rejected");
        ReportOutcomes("stale capture");

        Reset();
        std::vector<std::uint32_t> dependent;
        const auto firstDependentOffset = Dispatch(dependent, StoreCode.data(), {Low(DescriptorA.data()), High(DescriptorA.data()), 4});
        const auto secondDependentOffset = Dispatch(dependent, CounterCode.data(), {Low(DescriptorA.data()), High(DescriptorA.data())});
        BeforeTakeStatuses.clear();
        const auto dependencyCountersBefore = Farm::Get().GetAdoptionCounters();
        const auto dependentSerial = Submit(dependent);
        WaitJob(dependentSerial, secondDependentOffset);
        WaitIdle();
        const auto dependencyCountersAfter = Farm::Get().GetAdoptionCounters();
        ReportOutcomes("same-run dependency");
        const auto dependencyDiagnostics = FarmCounters() +
            "; Peek before Take:" + BeforeTakeStatuses +
            "; offsets=" + std::to_string(firstDependentOffset) + "," +
            std::to_string(secondDependentOffset);
        Require(dependencyCountersAfter.adopted == dependencyCountersBefore.adopted + 1,
                "only the first independent dispatch should be adopted: " + dependencyDiagnostics);
        Require(dependencyCountersAfter.staleCapture > dependencyCountersBefore.staleCapture ||
                dependencyCountersAfter.failed > dependencyCountersBefore.failed,
                "the dependent second dispatch was neither rejected as stale nor failed preparation: " +
                dependencyDiagnostics);
        Require(A[0] == 5, "second dispatch did not read first output: actual=" +
            std::to_string(A[0]) + " expected=5; " + FarmCounters() +
            "; Peek before Take:" + BeforeTakeStatuses +
            "; offsets=" + std::to_string(firstDependentOffset) + "," +
            std::to_string(secondDependentOffset));

        Reset();
        std::vector<std::uint32_t> distinct;
        Dispatch(distinct, StoreCode.data(), {Low(DescriptorA.data()), High(DescriptorA.data()), 0x1111});
        const auto otherOffset = Dispatch(distinct, StoreCode.data(), {Low(DescriptorB.data()), High(DescriptorB.data()), 0x2222});
        const auto distinctSerial = Submit(distinct);
        WaitJob(distinctSerial, otherOffset);
        WaitIdle();
        Require(A[0] == 0x1111 && B[0] == 0x2222, "same shader with distinct bound data crossed outputs");
        ReportOutcomes("distinct bindings");

        Reset();
        Farm::Get().ConfigureForTest({0, 64, 64u * 1024u * 1024u});
        const auto submittedBefore = Farm::Get().GetCounters().submitted;
        std::vector<std::uint32_t> disabled;
        Dispatch(disabled, StoreCode.data(), {Low(DescriptorC.data()), High(DescriptorC.data()), 44});
        Dispatch(disabled, StoreCode.data(), {Low(DescriptorA.data()), High(DescriptorA.data()), 55});
        Submit(disabled);
        WaitIdle();
        Require(A[0] == 55 && C[0] == 44, "disabled worker inline path produced wrong output");
        Require(Farm::Get().GetCounters().submitted == submittedBefore, "disabled workers submitted farm jobs");
        ReportOutcomes("disabled workers");

        Reset();
        std::vector<std::uint32_t> disabledDependent;
        Dispatch(disabledDependent, StoreCode.data(), {Low(DescriptorA.data()), High(DescriptorA.data()), 4});
        Dispatch(disabledDependent, CounterCode.data(), {Low(DescriptorA.data()), High(DescriptorA.data())});
        Submit(disabledDependent);
        WaitIdle();
        Require(A[0] == 5, "workers-disabled dependency baseline failed: actual=" +
            std::to_string(A[0]) + " expected=5; " + FarmCounters());
        Require(Farm::Get().GetCounters().submitted == submittedBefore,
                "workers-disabled dependency submitted farm jobs");
        ReportOutcomes("workers-disabled dependency");

        Farm::Get().SetBeforeTakeHookForTest(nullptr);
        AgcDriverSetAheadAdoptHook_nid_postfix(nullptr);
        AgcDriverShutdown_nid_postfix();
        return 0;
    } catch (const std::exception& error) {
        Farm::Get().SetBeforeTakeHookForTest(nullptr);
        AgcDriverSetAheadAdoptHook_nid_postfix(nullptr);
        std::cerr << error.what() << '\n';
        try { AgcDriverShutdown_nid_postfix(); } catch (const std::exception& shutdownError) { std::cerr << shutdownError.what() << '\n'; }
        return 1;
    }
}
