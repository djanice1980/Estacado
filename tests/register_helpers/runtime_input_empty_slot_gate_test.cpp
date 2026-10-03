// Empty controller slot gate contract (runtime_input_empty_slot_gate.h): the
// skipped native calls must never hide a connected controller for longer than
// the re-probe interval, and a connected slot is always queried natively.
#include "runtime_input_empty_slot_gate.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {
constexpr uint32_t kSuccess = 0;
constexpr uint32_t kNotConnected = 1167;  // ERROR_DEVICE_NOT_CONNECTED

// Mirrors the runtime call sites: skip while gated, otherwise query and note.
struct FakeSlotPoller {
    bool connected[4]{};
    uint32_t nativeCalls = 0;
    RuntimeInputEmptySlotGate<4> gate;
    uint32_t Poll(uint32_t slot, uint64_t nowMs) {
        if (gate.SkipNative(slot, nowMs)) return kNotConnected;
        ++nativeCalls;
        const uint32_t result = slot < 4 && connected[slot] ? kSuccess : kNotConnected;
        gate.Note(slot, result != kNotConnected, nowMs);
        return result;
    }
};
}  // namespace

int main() {
    bool passed = true;
    const auto check = [&passed](bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            passed = false;
        }
    };
    constexpr uint64_t kReprobe = RuntimeInputEmptySlotGate<4>::kMissingReprobeMs;

    {
        // Empty slot polled every frame: one native call per re-probe interval.
        FakeSlotPoller poller;
        uint32_t results = 0;
        for (uint64_t now = 5000; now < 5000 + 3 * kReprobe; now += 4) {
            results += poller.Poll(1, now) == kNotConnected;
        }
        check(poller.nativeCalls == 3, "empty slot must be re-probed once per interval");
        check(results == (3 * kReprobe) / 4, "every poll of an empty slot answers not connected");
    }
    {
        // Controller connected after launch appears within the re-probe interval.
        FakeSlotPoller poller;
        check(poller.Poll(0, 1000) == kNotConnected, "initially empty");
        poller.connected[0] = true;
        check(poller.Poll(0, 1000 + kReprobe - 1) == kNotConnected,
              "gated until the interval elapses");
        check(poller.Poll(0, 1000 + kReprobe) == kSuccess, "connected pad seen at the re-probe");
        const uint32_t before = poller.nativeCalls;
        for (uint64_t now = 3000; now < 3100; ++now) poller.Poll(0, now);
        check(poller.nativeCalls == before + 100, "connected slot is queried on every call");
    }
    {
        // Disconnect is reported immediately (native answer), then gated.
        FakeSlotPoller poller;
        poller.connected[2] = true;
        check(poller.Poll(2, 100) == kSuccess, "connected");
        poller.connected[2] = false;
        check(poller.Poll(2, 101) == kNotConnected, "disconnect seen on the next call");
        const uint32_t before = poller.nativeCalls;
        poller.Poll(2, 102);
        check(poller.nativeCalls == before, "then gated");
    }
    {
        // Slots are independent; out-of-range slots are never gated.
        FakeSlotPoller poller;
        poller.connected[0] = true;
        poller.Poll(3, 10);
        check(poller.Poll(0, 11) == kSuccess && !poller.gate.SkipNative(0, 12),
              "another slot's absence must not gate a connected slot");
        check(!poller.gate.SkipNative(7, 12), "out-of-range slots are never gated");
        poller.gate.Note(7, false, 12);
        check(!poller.gate.SkipNative(7, 13), "out-of-range notes are ignored");
        // A timestamp of 0 must still gate (0 is the 'connected' sentinel).
        RuntimeInputEmptySlotGate<4> gate;
        gate.Note(1, false, 0);
        check(gate.SkipNative(1, 0) && gate.SkipNative(1, kReprobe - 1) &&
                  !gate.SkipNative(1, kReprobe + 1),
              "a probe at time 0 is gated for one interval");
    }
    {
        // Source policy: both native state and vibration calls go through the gate
        // (0.9.1: one gated poll per host slot, used by the routed player 1 and
        // the console mapping alike).
        std::ifstream file(std::string(DARKNESS_SOURCE_ROOT) + "/runtime/runtime_input.cpp");
        const std::string source((std::istreambuf_iterator<char>(file)), {});
        const auto state = source.find("result = ResolveXInputGetState()(slot, &native);");
        const auto vibration = source.find("result = ResolveXInputSetState()(padSlot, &native);");
        check(state != std::string::npos && vibration != std::string::npos,
              "native state and vibration calls present");
        check(source.rfind("if (!emptySlotGate.SkipNative(slot, nowMs)) {", state) !=
                      std::string::npos &&
                  source.find("emptySlotGate.Note(slot, result != ERROR_DEVICE_NOT_CONNECTED, nowMs);",
                              state) != std::string::npos,
              "state poll must be gated and noted");
        // The only native state call is the gated helper's ("result = " precedes it).
        check(source.find("ResolveXInputGetState()(") == state + 9 &&
                  source.find("ResolveXInputGetState()(", state + 10) == std::string::npos,
              "every native state poll goes through the gated helper");
        const auto vibrationGate = source.rfind(
            "if (padSlot < XUSER_MAX_COUNT && !emptySlotGate.SkipNative(padSlot, nowMs)) {", vibration);
        check(vibrationGate != std::string::npos && vibrationGate > state,
              "vibration must be gated");
        check(source.find("NoteXInputStateResult(slot, results[slot]);", state) != std::string::npos &&
                  source.find("NoteXInputStateResult(actualUserIndex, result);", state) !=
                      std::string::npos,
              "capabilities cache still hears every state result");
    }

    if (!passed) return 1;
    std::cout << "runtime input empty slot gate: PASS\n";
    return 0;
}
