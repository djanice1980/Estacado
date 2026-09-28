#pragma once
#include <atomic>
#include <cstdint>

// Startup-only renderer input mode. Continuous production and bounded diagnostic
// logging have separate lifetimes; this does not enable temporal reprojection.
class RuntimeOwnedCameraMode {
public:
    bool ConfigureAtStartup(bool requested, bool memory_tracking) noexcept {
        if (requested && !memory_tracking) return false;
        const uint8_t selected = requested ? 2 : 1;
        uint8_t expected = 0;
        return mode_.compare_exchange_strong(expected, selected) || expected == selected;
    }
    bool Enabled() noexcept {
        auto selected = mode_.load(std::memory_order_acquire);
        if (!selected) {
            uint8_t expected = 0;
            if (mode_.compare_exchange_strong(expected, 1)) return false;
            selected = expected;
        }
        return selected == 2;
    }
private:
    std::atomic<uint8_t> mode_{};
};

inline RuntimeOwnedCameraMode& RuntimeOwnedCameraInputMode() {
    static RuntimeOwnedCameraMode mode;
    return mode;
}

// Three diagnostic intervals after the CPU capture closes. Ticks are observed
// CPU viewport entries, never GPU frames. Arming is one-shot until startup reset.
class RuntimeOwnedCameraFollowup {
public:
    void Arm() noexcept { if (!armed_) { armed_ = true; active_ = true; } }
    bool Active() const noexcept { return active_; }
    uint32_t Ticks() const noexcept { return ticks_; }
    bool Tick() noexcept {
        if (!active_) return false;
        ++ticks_;
        if (ticks_ != 16 && ticks_ != 64 && ticks_ != 256) return false;
        if (ticks_ == 256) active_ = false;
        return true;
    }
private:
    uint32_t ticks_{};
    bool armed_{}, active_{};
};
