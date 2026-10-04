#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// Button prompts that follow the input device (0.9.1, the owner's keyboard
// request). While the latest player input came from the keyboard or mouse:
// - the title's controller icons show keycaps with the bound keys (ReXGlue
//   prompt_icons.h, the GPU plugin's texture cache), and
// - prompt text built from the title's controller templates ("Press the §p0
//   button" with p0 = "A") reads like its PC templates ("Press §p0" with the
//   bound key), through the host's string lookup (runtime_language_pack.cpp).
// A controller input switches both back. input.button_prompts (Automatic /
// Xbox / Keyboard) is the plugin's input_button_prompts; the plugin decides
// and reports its state with the labels.

// Which device produced the latest input: 1 = controller, 2 = keyboard and
// mouse (0 = none yet). Observe() returns the new device when it changes,
// else 0. Both in one poll keep the current device.
class RuntimePromptDeviceTracker {
public:
    uint32_t Observe(bool controllerActivity, bool keyboardMouseActivity) noexcept {
        if (controllerActivity == keyboardMouseActivity) return 0;
        const uint32_t device = controllerActivity ? 1u : 2u;
        return current_.exchange(device, std::memory_order_relaxed) == device ? 0u : device;
    }
    uint32_t Current() const noexcept { return current_.load(std::memory_order_relaxed); }

private:
    std::atomic<uint32_t> current_{0};
};

// Controller activity in one poll: a newly pressed button, a trigger pulled
// past a third, or a stick pushed past half way (resting sticks and worn
// triggers never count).
inline bool RuntimeControllerActivity(uint16_t buttons, uint16_t previousButtons,
                                      uint8_t leftTrigger, uint8_t rightTrigger,
                                      int16_t thumbLX, int16_t thumbLY,
                                      int16_t thumbRX, int16_t thumbRY) noexcept {
    constexpr int kStick = 16384;
    constexpr int kTrigger = 85;
    auto beyond = [](int16_t value) { return std::abs(int(value)) > kStick; };
    return (buttons & ~previousButtons) != 0 || leftTrigger > kTrigger ||
           rightTrigger > kTrigger || beyond(thumbLX) || beyond(thumbLY) ||
           beyond(thumbRX) || beyond(thumbRY);
}

// Keyboard/mouse activity in one poll: any bound key or mouse button held, or
// the movement keys / stick bridge deflecting a stick.
inline bool RuntimeKeyboardMouseActivity(uint16_t buttons, uint8_t leftTrigger,
                                         uint8_t rightTrigger, int16_t thumbLX,
                                         int16_t thumbLY, int16_t thumbRX,
                                         int16_t thumbRY) noexcept {
    // Keys give full deflections; small right-stick values are mouse jitter in
    // stick mouse-look mode and never count (#16: the prompts flipped to keys
    // while a controller was in use).
    constexpr int kStick = 8000;
    auto beyond = [](int16_t value) { return std::abs(int(value)) > kStick; };
    return buttons != 0 || leftTrigger != 0 || rightTrigger != 0 || beyond(thumbLX) ||
           beyond(thumbLY) || beyond(thumbRX) || beyond(thumbRY);
}

// Native mouse look counts as keyboard/mouse activity only for a deliberate
// movement: at least kMinimumCounts within kWindowMs (sensor noise and a
// bumped desk never switch the prompts away from the controller, #16).
class RuntimeMouseActivityGate {
public:
    static constexpr uint32_t kMinimumCounts = 40;
    static constexpr uint64_t kWindowMs = 300;

    bool Observe(int32_t dx, int32_t dy, uint64_t nowMs) noexcept {
        if (nowMs - windowStartMs_ > kWindowMs) {
            windowStartMs_ = nowMs;
            counts_ = 0;
        }
        counts_ += uint32_t(std::abs(dx)) + uint32_t(std::abs(dy));
        if (counts_ < kMinimumCounts) return false;
        counts_ = 0;
        windowStartMs_ = nowMs;
        return true;
    }

private:
    uint64_t windowStartMs_ = 0;
    uint32_t counts_ = 0;
};

// Reports the device to the GPU plugin when it changes.
void RuntimeNoteInputActivity(bool controllerActivity, bool keyboardMouseActivity) noexcept;

// The text of one of the title's controller prompt strings while keyboard
// prompts are wanted (UTF-16), else nullopt. Keys outside the controller
// templates and button names are never touched.
std::optional<std::u16string> RuntimeKeyboardPromptText(std::string_view key);
// The mapping itself (tests): labels as "button=label" lines.
std::optional<std::u16string> RuntimeKeyboardPromptTextFor(std::string_view key,
                                                           std::string_view labels);

// The GPU configuration with gpu_prompt_icon_source (the title's GUI texture
// file, where the controller icons are recognised) as a top-level key.
std::string RuntimePcConfigWithPromptIconSource(const std::string& contents,
                                                const std::filesystem::path& guiTextures);
