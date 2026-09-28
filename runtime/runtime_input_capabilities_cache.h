#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

// Per-slot cache for a slow controller-capabilities query (V287/V288).
//
// XInputGetCapabilities goes through the Windows configuration manager
// (DeviceIoControl) on every call, and the title asks every frame from two
// threads. A connected pad's capabilities are static, so the last answer is
// reused and re-validated every kConnectedRefreshMs; an empty slot is
// re-probed at most every kMissingReprobeMs, so a pad connected after launch
// appears within a second. Every XInputGetState result is reported through
// NoteStateResult: a connect/disconnect transition drops the cached answer,
// so hot-plugging during play is reflected on the very next query.
//
// Query: DWORD-like result, (slot, flags, Capabilities*) -> result.
// Clock: milliseconds, monotonic.
template <typename Capabilities, uint32_t kSlots = 4>
class RuntimeInputCapabilitiesCache {
public:
    static constexpr uint64_t kConnectedRefreshMs = 5000;
    static constexpr uint64_t kMissingReprobeMs = 1000;

    template <typename Query>
    uint32_t Get(uint32_t slot, uint32_t flags, uint64_t nowMs, uint32_t successResult,
                 Capabilities& out, Query&& query) {
        if (slot >= kSlots) return query(slot, flags, &out);
        Entry& entry = entries_[slot];
        std::lock_guard lock(entry.mutex);
        const uint64_t maxAge = entry.result == successResult ? kConnectedRefreshMs
                                                              : kMissingReprobeMs;
        if (!entry.valid || entry.flags != flags || nowMs - entry.queriedMs >= maxAge) {
            entry.value = Capabilities{};
            entry.result = query(slot, flags, &entry.value);
            entry.flags = flags;
            entry.queriedMs = nowMs;
            entry.valid = true;
        }
        out = entry.value;
        return entry.result;
    }

    // Report every state-poll result; connection changes invalidate the slot.
    void NoteStateResult(uint32_t slot, bool connected) {
        if (slot >= kSlots) return;
        const uint32_t state = connected ? 1u : 2u;
        if (lastState_[slot].exchange(state, std::memory_order_relaxed) != state) {
            std::lock_guard lock(entries_[slot].mutex);
            entries_[slot].valid = false;
        }
    }

private:
    struct Entry {
        std::mutex mutex;
        bool valid = false;
        uint32_t flags = 0;
        uint32_t result = 0;
        Capabilities value{};
        uint64_t queriedMs = 0;
    };
    std::array<Entry, kSlots> entries_;
    std::array<std::atomic<uint32_t>, kSlots> lastState_{};
};
