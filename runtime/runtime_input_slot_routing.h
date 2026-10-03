#pragma once

#include <array>
#include <cstdint>

// Single-player controller routing (0.9.1, issue #8).
//
// The game binds the player to the controller port that pressed Start, and
// only user 0 holds the signed-in profile (saves, settings, achievements). A
// controller that Windows, Steam Input or a virtual-pad driver placed on
// XInput slot 1-3 therefore became a second player without a profile: "no
// player profile", no saving, and no switching to the keyboard afterwards.
// The Darkness has no split screen, so every physical controller drives guest
// user 0. The active pad is the slot with the most recent new button press
// (the lowest slot when several press in the same poll), falling back to the
// lowest connected slot; a connected slot that is never pressed (an idle
// virtual pad, a wheel) cannot take over from a pad in use.
//
// Usage per guest-0 poll: Observe() every host slot in slot order, then
// Finish(). The caller serializes polls.
class RuntimeInputSlotRouter {
public:
    static constexpr uint32_t kSlots = 4;
    static constexpr uint32_t kNone = kSlots;

    void Observe(uint32_t slot, bool connected, uint16_t buttons) noexcept {
        if (slot >= kSlots) return;
        const uint16_t pressed = connected ? uint16_t(buttons & ~previous_[slot]) : uint16_t(0);
        previous_[slot] = connected ? buttons : uint16_t(0);
        connected_[slot] = connected;
        if (pressed && !pressedThisPoll_) {
            active_ = slot;
            pressedThisPoll_ = true;
        }
    }

    // The slot that drives guest user 0 after this poll, or kNone.
    uint32_t Finish() noexcept {
        pressedThisPoll_ = false;
        if (active_ < kSlots && connected_[active_]) return active_;
        active_ = kNone;
        for (uint32_t slot = 0; slot < kSlots; ++slot) {
            if (connected_[slot]) {
                active_ = slot;
                break;
            }
        }
        return active_;
    }

    uint32_t Active() const noexcept { return active_; }

private:
    std::array<uint16_t, kSlots> previous_{};
    std::array<bool, kSlots> connected_{};
    uint32_t active_ = kNone;
    bool pressedThisPoll_ = false;
};
