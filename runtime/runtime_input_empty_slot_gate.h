#pragma once

#include <array>
#include <atomic>
#include <cstdint>

// Empty controller slot gate (V297).
//
// XInputGetState and XInputSetState on a slot without a controller enumerate
// devices through the Windows configuration manager on every call; with
// keyboard and mouse only, that was about 4% of the street game thread. A
// slot whose native call reported 'not connected' is re-probed at most every
// kMissingReprobeMs; in between, callers answer 'not connected' without the
// native call. A controller connected after launch therefore appears within
// that interval (the capabilities cache re-probes empty slots at the same
// rate), and a connected controller is queried natively on every call.
//
// Clock: milliseconds, monotonic.
template <uint32_t kSlots = 4>
class RuntimeInputEmptySlotGate {
public:
    static constexpr uint64_t kMissingReprobeMs = 1000;

    // True while the slot's last native answer was 'not connected' and the
    // re-probe interval has not elapsed.
    bool SkipNative(uint32_t slot, uint64_t nowMs) const {
        if (slot >= kSlots) return false;
        const uint64_t missing = missingProbe_[slot].load(std::memory_order_relaxed);
        return missing && nowMs + 1 >= missing && nowMs + 1 - missing < kMissingReprobeMs;
    }

    // Report every native result for the slot.
    void Note(uint32_t slot, bool connected, uint64_t nowMs) {
        if (slot >= kSlots) return;
        missingProbe_[slot].store(connected ? 0 : nowMs + 1, std::memory_order_relaxed);
    }

private:
    // Time of the last 'not connected' answer + 1 ms; 0 while connected or
    // never probed (always query natively).
    std::array<std::atomic<uint64_t>, kSlots> missingProbe_{};
};
