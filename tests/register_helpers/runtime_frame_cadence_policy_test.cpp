#include "runtime_frame_cadence.h"
#include "runtime_present_interval_experiment.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main() {
    using namespace darkness::diagnostics;
    bool passed = true;
    const auto check = [&passed](bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            passed = false;
        }
    };

    using namespace darkness::experiments;
    check(IsTitlePresentationCreation(0x8286FD68, 0x8223E538, 0x1000D0, 0x100000),
          "exact title creation accepts verified stack parameter block");
    check(!IsTitlePresentationCreation(0x8286FD68, 0x8223E534, 0x1000D0, 0x100000) &&
          !IsTitlePresentationCreation(0x8286FD68, 0x8223E538, 0x1000D4, 0x100000) &&
          !IsTitlePresentationCreation(0x8286FFC8, 0x8223E538, 0x1000D0, 0x100000),
          "different caller, block or reset path must remain untouched");
    check(ExperimentalPresentInterval(false, 2) == 2 &&
          ExperimentalPresentInterval(true, 2) == 1,
          "only opt-in interval2 maps to interval1");
    for (auto value : {0u, 1u, 4u, 0x80000000u, 0xFFFFFFFFu}) {
        check(ExperimentalPresentInterval(true, value) == value,
              "other guest presentation policies are preserved");
        check(ExperimentalImmediateInterval(true, value) == value,
              "immediate experiment preserves all other guest policies");
    }
    check(ExperimentalImmediateInterval(false, 2) == 2 &&
          ExperimentalImmediateInterval(true, 2) == 0x80000000u,
          "opt-in immediate experiment selects genuine platform interval encoding");

    check(ShouldSampleFrameCadence(1), "first cadence event must be sampled");
    check(ShouldSampleFrameCadence(8), "eighth cadence event must be sampled");
    check(ShouldSampleFrameCadence(16), "power-of-two cadence event must be sampled");
    check(!ShouldSampleFrameCadence(17), "ordinary cadence event must stay silent");
    check(IsFrameControllerEntry(0x827A6198u) &&
              !IsFrameControllerEntry(0x827A619Cu) &&
              !IsFrameControllerEntry(0x820E2058u),
          "controller entry is distinct from nearby instructions and builder");
    check(IsPrebuildWaitEntry(0x825A4278u, 0x820DE1D4u) &&
              !IsPrebuildWaitEntry(0x825A4278u, 0x820DE1D8u) &&
              !IsPrebuildWaitEntry(0x825A43B0u, 0x820DE1D4u) &&
              IsPrebuildWaitEndBoundary(0x820C86A8u, 0x820DE1E0u) &&
              !IsPrebuildWaitEndBoundary(0x820C86A8u, 0x820DE1C4u),
          "prebuild bracket identifies exact caller and following timer, not other waits");
    PrebuildWaitBracket bracket;
    check(!bracket.Finish(7), "unstarted boundary is not a completed observation");
    bracket.Begin(7, 16, true);
    check(bracket.Finish(7) && !bracket.Finish(7), "matching boundary consumed once");
    bracket.Begin(7, 32, true);
    check(!bracket.Finish(8) && !bracket.Finish(7), "wrong owner discards bracket");
    bracket.Begin(7, 17, false);
    check(!bracket.Finish(7), "unsampled wait never emits a completion");
    bracket.Begin(7, 64, true);
    bracket.Reset();
    check(!bracket.Finish(7), "new driver entry discards an unfinished old bracket");
    check(ShouldSampleFrameCadence(4096), "periodic cadence event must be sampled");
    check(ShouldSampleFrameCadence(12288), "later periodic cadence event must be sampled");

    check(!IntrusiveQueueIsEmpty(0), "clear sentinel bit means queue data is available");
    check(IntrusiveQueueIsEmpty(1), "sentinel bit means queue is empty");
    check(IntrusiveQueueIsEmpty(3), "tagged sentinel bit means queue is empty");

    check(WaitCompletedByTimeout(kWaitTimeoutStatus), "STATUS_TIMEOUT must be classified");
    check(!WaitCompletedByTimeout(0), "successful signal must not be classified as timeout");

    check(ClassifyWorldWorkPath(0x824A7194u) == WorldWorkPath::Timed &&
          ClassifyWorldWorkPath(0x824A7254u) == WorldWorkPath::DriverDrain &&
          ClassifyWorldWorkPath(0x824A7058u) == WorldWorkPath::FlushDrain &&
          ClassifyWorldWorkPath(0x824A705Cu) == WorldWorkPath::Other,
          "all three verified callers and unknown callers stay distinct");
    check(IsWorldWorkDispatch(0x824A6F60u, 0x824A7058u) &&
          IsWorldWorkDispatch(0x824A6898u, 0x824A6FA8u) &&
          !IsWorldWorkDispatch(0x824A6898u, 0x824A6FACu) &&
          !IsWorldWorkDispatch(0x824A6F64u, 0x824A7058u),
          "only exact verified queue and nested consumer targets are measured");
    check(IsVerifiedQueueConsumption(0, 1, 1, 4, 1) &&
          IsVerifiedQueueConsumption(3, 1, 0, 4, 1),
          "normal and wrapped ring advancement require a returned consumer");
    check(!IsVerifiedQueueConsumption(1, 1, 2, 4, 1) &&
          !IsVerifiedQueueConsumption(0, 1, 1, 4, 0) &&
          !IsVerifiedQueueConsumption(0, 1, 1, 4, 2) &&
          !IsVerifiedQueueConsumption(0, 1, 0, 4, 1) &&
          !IsVerifiedQueueConsumption(0, 1, 1, 0, 1) &&
          !IsVerifiedQueueConsumption(4, 1, 1, 4, 1),
          "empty, no-return, nested, unchanged and invalid rings are not completed consumption");

    check(IsSimulationTickEntry(kSimulationTickAddress),
          "verified fixed-step driver entry must be classified");
    const auto oneStep = ObserveWorldClientSteps(5, 0, 1, 80, 81);
    check(oneStep.verified && oneStep.requested == 1 && oneStep.observed == 1,
          "returned client counter must agree with the packet step count");
    const auto batchedSteps = ObserveWorldClientSteps(5, 0, 4, 80, 84);
    check(batchedSteps.verified && batchedSteps.observed == 4,
          "one queue consumption may contain several actual client steps");
    check(ObserveWorldClientSteps(5, 2, 4, 80, 80).verified &&
          ObserveWorldClientSteps(1, 0, 4, 80, 80).verified &&
          ObserveWorldClientSteps(5, 0, 0, 80, 80).verified,
          "disabled mode, absent step flag and zero count apply no client steps");
    check(ObserveWorldClientSteps(5, 0, 1, 0xFFFFFFFFu, 0).verified,
          "a genuine unsigned counter wrap preserves the one-step observation");
    check(!ObserveWorldClientSteps(5, 0, 1, 80, 0).verified &&
          !ObserveWorldClientSteps(5, 0, 1, 80, 82).verified &&
          !ObserveWorldClientSteps(5, 0, 256, 80, 336).verified &&
          !ObserveWorldClientSteps(5, 2, 4, 80, 84).verified,
          "counter reset, excess work, invalid packet count or ignored mode reject inference");
    check(!IsSimulationTickEntry(kSimulationTickAddress + 4u),
          "nearby code must not be classified as fixed-step entry");
    check(IsSimulationStepCall(kSimulationStepCallerLr),
          "verified fixed-step virtual-call LR must be classified");
    check(!IsSimulationStepCall(kSimulationStepCallerLr + 4u),
          "nearby indirect calls must not be classified as simulation steps");
    check(IsSimulationQueuedDrainCall(0x824A7254u) &&
              !IsSimulationStepCall(0x824A7254u) &&
              !IsSimulationQueuedDrainCall(kSimulationStepCallerLr) &&
              !IsSimulationQueuedDrainCall(0x824A7258u),
          "raw-XEX queue-drain callback must remain separate from timed-step callbacks");
    check(IsFramePoolInitialAcquire(kFramePoolAcquireAddress,
                                    kFramePoolInitialCallerLr),
          "verified nonblocking frame-pool acquisition must be classified");
    check(IsFramePoolRetryAcquire(kFramePoolAcquireAddress,
                                  kFramePoolRetryCallerLr),
          "verified bounded frame-pool retry must be classified");
    check(!IsFramePoolInitialAcquire(kFramePoolAcquireAddress,
                                     kFramePoolRetryCallerLr),
          "frame-pool retry must not be counted as the initial acquisition");
    check(!IsFramePoolRetryAcquire(kFramePoolAcquireAddress + 4u,
                                   kFramePoolRetryCallerLr),
          "nearby functions must not be classified as frame-pool acquisition");

    const std::filesystem::path traceSource =
        std::filesystem::path(DARKNESS_SOURCE_ROOT) / "runtime" /
        "runtime_function_trace.cpp";
    std::ifstream traceInput(traceSource, std::ios::binary);
    const std::string traceText((std::istreambuf_iterator<char>(traceInput)),
                                std::istreambuf_iterator<char>());
    const auto functionStart = traceText.find(
        "void AppendFrameCadenceFunctionEntry(PPCContext& context");
    const auto controllerStart = traceText.find(
        "if (darkness::diagnostics::IsFrameControllerEntry(address))", functionStart);
    const auto controllerEnd = traceText.find("const bool simulationTick", controllerStart);
    const auto controllerText = controllerStart != std::string::npos &&
            controllerEnd != std::string::npos
        ? traceText.substr(controllerStart, controllerEnd - controllerStart)
        : std::string{};
    check(controllerStart != std::string::npos && controllerEnd != std::string::npos &&
              controllerText.find("ShouldSampleFrameCadence(ordinal)") <
                  controllerText.find("std::ostringstream details") &&
              controllerText.find("PPC_LOAD") == std::string::npos &&
              controllerText.find("PPC_STORE") == std::string::npos,
          "controller observation samples before formatting and never accesses guest memory");
    const auto unrelatedReturn = traceText.find(
        "if (!simulationTick && !simulationStep && !build && !poolInitial &&",
        functionStart);
    const auto clockRead = traceText.find(
        "const auto now = FrameCadenceClock::now();", functionStart);
    check(traceInput && functionStart != std::string::npos &&
              unrelatedReturn != std::string::npos &&
              clockRead != std::string::npos && unrelatedReturn < clockRead,
          "sparse cadence observer must reject unrelated function entries "
          "before reading the host clock");

    const auto enterStart = traceText.find(
        "void RuntimeFunctionEnter(PPCContext& context");
    const auto enabledGate = traceText.find(
        "frameCadenceAttributionEnabled.load(std::memory_order_relaxed)",
        enterStart);
    const auto experimentStart = traceText.find("if (address == 0x8286FD68u &&", enterStart);
    const auto experimentEnd = traceText.find("if (frameCadenceAttributionEnabled", experimentStart);
    const auto experimentText = experimentStart != std::string::npos && experimentEnd != std::string::npos
        ? traceText.substr(experimentStart, experimentEnd - experimentStart) : std::string{};
    check(!experimentText.empty() &&
          experimentText.find("IsTitlePresentationCreation") < experimentText.find("PPC_LOAD_U32") &&
          experimentText.find("context.r4.u32 + 0x34u") != std::string::npos &&
          experimentText.find("PPC_STORE_U32(slot, selected)") != std::string::npos &&
          experimentText.find("PPC_STORE", experimentText.find("PPC_STORE") + 1) == std::string::npos &&
          experimentText.find("< 8u") != std::string::npos &&
          experimentText.find("Sleep(") == std::string::npos,
          "experiment has one guarded parameter store, bounded reports, no added sleep");
    const auto appendCall = traceText.find(
        "AppendFrameCadenceFunctionEntry(context, base, address);",
        enterStart);
    check(enterStart != std::string::npos && enabledGate != std::string::npos &&
              appendCall != std::string::npos && enabledGate < appendCall,
          "ordinary launches must gate the cadence observer before its "
          "address-classification work");

    if (passed) std::cout << "runtime frame-cadence policy tests passed\n";
    return passed ? 0 : 1;
}
