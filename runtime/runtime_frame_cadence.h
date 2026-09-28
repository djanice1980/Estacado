#pragma once

#include <cstdint>

namespace darkness::diagnostics {

enum class WorldWorkPath : uint32_t { Other, Timed, DriverDrain, FlushDrain };
constexpr WorldWorkPath ClassifyWorldWorkPath(uint32_t callerLr) noexcept {
    return callerLr == 0x824A7194u ? WorldWorkPath::Timed :
           callerLr == 0x824A7254u ? WorldWorkPath::DriverDrain :
           callerLr == 0x824A7058u ? WorldWorkPath::FlushDrain : WorldWorkPath::Other;
}
constexpr bool IsWorldWorkDispatch(uint32_t target, uint32_t callerLr) noexcept {
    return target == 0x824A6F60u ||
           (target == 0x824A6898u && callerLr == 0x824A6FA8u);
}
// A returned call alone is not consumption. Require the verified ring advance
// and one returned nested consumer on this thread, not a global counter delta.
constexpr bool IsVerifiedQueueConsumption(uint32_t readBefore, uint32_t writeBefore,
    uint32_t readAfter, uint32_t capacity, uint64_t consumerReturns) noexcept {
    return capacity != 0 && readBefore < capacity && writeBefore < capacity &&
           readBefore != writeBefore && readAfter == (readBefore + 1u) % capacity &&
           consumerReturns == 1;
}

struct WorldClientStepDelta {
    uint32_t requested{};
    uint32_t observed{};
    bool verified{};
};
// Raw 824A6898 applies item+24 only with flag4 and world+204 != 2.
// 82447C00 decodes that count from one byte. Each 823F0010 update
// increments world+CA0; unlike world+198/+190, the consumer does not
// replace CA0 with the packet timestamp. Reject resets/unexpected deltas.
constexpr WorldClientStepDelta ObserveWorldClientSteps(
    uint32_t flags, uint32_t mode, uint32_t packetSteps,
    uint32_t before, uint32_t after) noexcept {
    const bool validPacket = packetSteps <= 255u;
    const uint32_t requested = validPacket && mode != 2u && (flags & 4u)
        ? packetSteps : 0u;
    const uint32_t observed = after - before;
    return {requested, observed, validPacket && observed == requested};
}

inline constexpr uint32_t kWaitTimeoutStatus = 0x102u;
// Verified raw-XEX frame-controller virtual +0x5C entry. Observation only:
// this is not a fixed-step update or a proven frame-rate limiter.
constexpr bool IsFrameControllerEntry(uint32_t address) noexcept {
    return address == 0x827A6198u;
}
constexpr bool IsPrebuildWaitEntry(uint32_t address, uint32_t callerLr) noexcept {
    return address == 0x825A4278u && callerLr == 0x820DE1D4u;
}
constexpr bool IsPrebuildWaitEndBoundary(uint32_t address, uint32_t callerLr) noexcept {
    return address == 0x820C86A8u && callerLr == 0x820DE1E0u;
}
// Bound a sparse observation to one actual driver invocation and thread.
// The end is the next timer call, not an invented generated return hook.
struct PrebuildWaitBracket {
    uint32_t owner{};
    uint64_t ordinal{};
    bool active{};
    void Begin(uint32_t newOwner, uint64_t newOrdinal, bool sampled) noexcept {
        owner = newOwner;
        ordinal = newOrdinal;
        active = sampled;
    }
    bool Finish(uint32_t currentOwner) noexcept {
        const bool matched = active && owner == currentOwner;
        active = false;
        return matched;
    }
    void Reset() noexcept { active = false; }
};
inline constexpr uint32_t kSimulationTickAddress = 0x824A7080u;
inline constexpr uint32_t kSimulationStepCallerLr = 0x824A7194u;
// Same vtable +0x2B8, but the queue-drain branch bypasses the elapsed-step loop.
// Count separately: this is not evidence of a fixed-step update.
inline constexpr uint32_t kSimulationQueuedDrainCallerLr = 0x824A7254u;
inline constexpr uint32_t kFramePoolAcquireAddress = 0x825A3FD8u;
inline constexpr uint32_t kFramePoolInitialCallerLr = 0x820E21A4u;
inline constexpr uint32_t kFramePoolRetryCallerLr = 0x820E21E0u;

constexpr bool ShouldSampleFrameCadence(uint64_t ordinal) noexcept {
    return ordinal <= 8u || (ordinal && (ordinal & (ordinal - 1u)) == 0u) ||
           (ordinal % 4096u) == 0u;
}

constexpr bool IntrusiveQueueIsEmpty(uint32_t sentinelFlags) noexcept {
    return (sentinelFlags & 1u) != 0u;
}

constexpr bool WaitCompletedByTimeout(uint32_t status) noexcept {
    return status == kWaitTimeoutStatus;
}

constexpr bool IsSimulationTickEntry(uint32_t address) noexcept {
    return address == kSimulationTickAddress;
}

constexpr bool IsSimulationStepCall(uint32_t callerLr) noexcept {
    return callerLr == kSimulationStepCallerLr;
}

constexpr bool IsSimulationQueuedDrainCall(uint32_t callerLr) noexcept {
    return callerLr == kSimulationQueuedDrainCallerLr;
}

constexpr bool IsFramePoolInitialAcquire(uint32_t address,
                                         uint32_t callerLr) noexcept {
    return address == kFramePoolAcquireAddress &&
           callerLr == kFramePoolInitialCallerLr;
}

constexpr bool IsFramePoolRetryAcquire(uint32_t address,
                                       uint32_t callerLr) noexcept {
    return address == kFramePoolAcquireAddress &&
           callerLr == kFramePoolRetryCallerLr;
}

}  // namespace darkness::diagnostics
