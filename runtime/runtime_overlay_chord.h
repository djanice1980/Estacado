#pragma once

#include <array>
#include <cstdint>

// Back + Start together opens the in-game settings overlay (#7; the GPU
// plugin polls XInput for the chord itself). The title must see neither
// button of the chord, also when one lands a little before the other: a fresh
// lone Back or Start press is held back for kWindowMs while the other may
// still arrive. A chord hides both until both are released. A lone press
// reaches the title once the window has passed; a tap released inside the
// window is shown for kTapHoldMs from its release, so the title still sees
// it. Other buttons pass unchanged. One filter per controller user; not
// thread-safe (the caller serializes).
class RuntimeOverlayChordFilter {
public:
    static constexpr uint16_t kStart = 0x0010;
    static constexpr uint16_t kBack = 0x0020;
    static constexpr uint16_t kChord = kBack | kStart;
    static constexpr uint64_t kWindowMs = 100;
    static constexpr uint64_t kTapHoldMs = 60;

    uint16_t Filter(uint16_t buttons, uint64_t nowMs) {
        const uint16_t held = buttons & kChord;
        const uint16_t others = buttons & uint16_t(~kChord);
        if (chord_) {
            if (!held) chord_ = false;
            return others;
        }
        if (held == kChord) {
            chord_ = true;
            ++chords_;
            buttons_ = {};
            return others;
        }
        uint16_t shown = 0;
        for (size_t index = 0; index < buttons_.size(); ++index) {
            const uint16_t bit = index == 0 ? kBack : kStart;
            Button& button = buttons_[index];
            const bool down = (held & bit) != 0;
            switch (button.state) {
            case State::kIdle:
                if (down) button = {State::kPending, nowMs};
                break;
            case State::kPending:
                if (!down) {
                    button = {State::kTap, nowMs};
                } else if (nowMs - button.sinceMs >= kWindowMs) {
                    button.state = State::kPassing;
                }
                break;
            case State::kPassing:
                if (!down) button = {};
                break;
            case State::kTap:
                if (down) {
                    button = {State::kPending, nowMs};  // a new press: released first
                } else if (nowMs - button.sinceMs >= kTapHoldMs) {
                    button = {};
                }
                break;
            }
            if (button.state == State::kPassing || button.state == State::kTap) shown |= bit;
        }
        return others | shown;
    }

    bool InChord() const { return chord_; }
    uint64_t Chords() const { return chords_; }

private:
    enum class State : uint8_t { kIdle, kPending, kPassing, kTap };
    struct Button {
        State state = State::kIdle;
        uint64_t sinceMs = 0;
    };
    std::array<Button, 2> buttons_{};  // Back, Start
    bool chord_ = false;
    uint64_t chords_ = 0;
};
