#pragma once

#include <atomic>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string_view>

struct RuntimeInputCapabilities {
    uint8_t type{};
    uint8_t subType{};
    uint16_t flags{};
    uint16_t buttons{};
    uint8_t leftTrigger{};
    uint8_t rightTrigger{};
    int16_t thumbLX{};
    int16_t thumbLY{};
    int16_t thumbRX{};
    int16_t thumbRY{};
    uint16_t leftMotorSpeed{};
    uint16_t rightMotorSpeed{};
};

struct RuntimeInputState {
    uint32_t packetNumber{};
    uint16_t buttons{};
    uint8_t leftTrigger{};
    uint8_t rightTrigger{};
    int16_t thumbLX{};
    int16_t thumbLY{};
    int16_t thumbRX{};
    int16_t thumbRY{};
};

struct RuntimeInputVibration {
    uint16_t leftMotorSpeed{};
    uint16_t rightMotorSpeed{};
};

enum class RuntimeInputButton : uint8_t {
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    Start,
    Back,
    LeftStick,
    RightStick,
    LeftShoulder,
    RightShoulder,
    Guide,
    A,
    B,
    X,
    Y,
    None,
};

inline constexpr size_t kRuntimeInputButtonCount = 15;
inline constexpr std::array<std::string_view, kRuntimeInputButtonCount>
    kRuntimeInputButtonNames = {
        "dpad_up", "dpad_down", "dpad_left", "dpad_right", "start",
        "back", "left_stick", "right_stick", "left_shoulder",
        "right_shoulder", "guide", "a", "b", "x", "y",
    };
inline constexpr std::array<uint16_t, kRuntimeInputButtonCount>
    kRuntimeInputButtonMasks = {
        0x0001, 0x0002, 0x0004, 0x0008, 0x0010,
        0x0020, 0x0040, 0x0080, 0x0100, 0x0200,
        0x0400, 0x1000, 0x2000, 0x4000, 0x8000,
    };

struct RuntimeInputButtonMap {
    std::array<uint8_t, kRuntimeInputButtonCount> sourceForGuest{};

    constexpr RuntimeInputButtonMap() {
        for (size_t index = 0; index < sourceForGuest.size(); ++index) {
            sourceForGuest[index] = static_cast<uint8_t>(index);
        }
    }
};

inline bool ParseRuntimeInputButtonName(std::string_view name,
                                        RuntimeInputButton& output) noexcept {
    if (name == "none") {
        output = RuntimeInputButton::None;
        return true;
    }
    for (size_t index = 0; index < kRuntimeInputButtonNames.size(); ++index) {
        if (name == kRuntimeInputButtonNames[index]) {
            output = static_cast<RuntimeInputButton>(index);
            return true;
        }
    }
    return false;
}

inline constexpr uint64_t EncodeRuntimeInputButtonMap(
    const RuntimeInputButtonMap& map) noexcept {
    uint64_t encoded = 0;
    for (size_t index = 0; index < map.sourceForGuest.size(); ++index) {
        encoded |= uint64_t(map.sourceForGuest[index] & 0x0F) << (index * 4);
    }
    return encoded;
}

inline constexpr uint64_t kRuntimeInputIdentityButtonMap =
    EncodeRuntimeInputButtonMap(RuntimeInputButtonMap{});

inline uint16_t RemapRuntimeInputButtons(uint16_t physicalButtons,
                                         uint64_t encodedMap) noexcept {
    if (encodedMap == kRuntimeInputIdentityButtonMap) return physicalButtons;
    constexpr uint16_t knownMask = 0xFFFFu & ~uint16_t(0x0800u);
    uint16_t guestButtons = physicalButtons & ~knownMask;
    for (size_t guest = 0; guest < kRuntimeInputButtonCount; ++guest) {
        const uint8_t source = uint8_t((encodedMap >> (guest * 4)) & 0x0F);
        if (source < kRuntimeInputButtonCount &&
            (physicalButtons & kRuntimeInputButtonMasks[source])) {
            guestButtons |= kRuntimeInputButtonMasks[guest];
        }
    }
    return guestButtons;
}

// Startup-only physical digital-button mapping. The Original/absent map is an
// exact identity fast path. Analog triggers and sticks remain title-owned.
void ConfigureRuntimeInputButtonMap(const RuntimeInputButtonMap& map);
uint64_t RuntimeInputButtonMapCode() noexcept;

inline RuntimeInputState RemapRuntimeControllerState(
    RuntimeInputState physical, uint64_t encodedMap) noexcept {
    physical.buttons = RemapRuntimeInputButtons(physical.buttons, encodedMap);
    return physical;
}

// Deterministic merge used only when the optional physical keyboard/mouse
// source is enabled. Controller-only operation bypasses this function so its
// already validated packet and state contract remains byte-for-byte intact.
inline RuntimeInputState MergeRuntimeInputStates(
    const RuntimeInputState& controller,
    const RuntimeInputState& keyboardMouse) noexcept {
    RuntimeInputState merged = controller;
    // A composite packet is assigned by the serialized publication tracker.
    merged.packetNumber = 0;
    merged.buttons = uint16_t(controller.buttons | keyboardMouse.buttons);
    merged.leftTrigger = std::max(controller.leftTrigger,
                                  keyboardMouse.leftTrigger);
    merged.rightTrigger = std::max(controller.rightTrigger,
                                   keyboardMouse.rightTrigger);
    const auto greaterMagnitude = [](int16_t first, int16_t second) {
        return std::abs(static_cast<int>(first)) >=
                       std::abs(static_cast<int>(second))
            ? first
            : second;
    };
    merged.thumbLX = greaterMagnitude(controller.thumbLX, keyboardMouse.thumbLX);
    merged.thumbLY = greaterMagnitude(controller.thumbLY, keyboardMouse.thumbLY);
    merged.thumbRX = greaterMagnitude(controller.thumbRX, keyboardMouse.thumbRX);
    merged.thumbRY = greaterMagnitude(controller.thumbRY, keyboardMouse.thumbRY);
    return merged;
}

// Caller serializes polls for one optional merged device. Compare payload,
// never independent host counters (which may reset on reconnect).
class RuntimeMergedInputPacketTracker {
public:
    explicit RuntimeMergedInputPacketTracker(uint32_t initial = 0) : packet_(initial) {}
    void Disconnect() noexcept { connected_ = false; }
    RuntimeInputState Publish(RuntimeInputState state) noexcept {
        if (!connected_ || state.buttons != previous_.buttons ||
            state.leftTrigger != previous_.leftTrigger ||
            state.rightTrigger != previous_.rightTrigger ||
            state.thumbLX != previous_.thumbLX || state.thumbLY != previous_.thumbLY ||
            state.thumbRX != previous_.thumbRX || state.thumbRY != previous_.thumbRY) {
            ++packet_; // unsigned DWORD wrap is intentional
        }
        connected_ = true;
        state.packetNumber = packet_;
        previous_ = state;
        return state;
    }
private:
    RuntimeInputState previous_{};
    uint32_t packet_{};
    bool connected_{};
};

struct RuntimeMergedInputSlot {
    std::mutex mutex;
    RuntimeMergedInputPacketTracker packets;
};

// Transition-only diagnostic tracker; never supplies guest input.
class RuntimeInputTransitionTracker {
public:
    bool Update(uint32_t result, uint32_t payload) noexcept {
        const uint64_t signature = (uint64_t(result) << 32) | payload;
        return signature_.exchange(signature, std::memory_order_relaxed) != signature;
    }

private:
    std::atomic<uint64_t> signature_{std::numeric_limits<uint64_t>::max()};
};

enum class RuntimeInputDiagnosticKind : uint8_t {
    // A diagnostic-only XInput observer. This is never used as guest input;
    // it provides an independent timestamp for physical digital edges.
    HostState,
    Capabilities,
    State,
    Vibration,
};

struct RuntimeInputDiagnosticEvent {
    RuntimeInputDiagnosticKind kind{};
    uint64_t hostMilliseconds{};
    int64_t hostPerformanceCounter{};
    uint32_t guestThread{};
    uint32_t guestLinkRegister{};
    uint32_t userIndex{};
    uint32_t flags{};
    uint32_t guestAddress{};
    uint32_t result{};
    uint32_t packetNumber{};
    uint16_t buttons{};
    uint8_t leftTrigger{};
    uint8_t rightTrigger{};
    int16_t thumbLX{};
    int16_t thumbLY{};
    int16_t thumbRX{};
    int16_t thumbRY{};
    uint16_t leftMotorSpeed{};
    uint16_t rightMotorSpeed{};
};

// Starts a bounded asynchronous transition log at logs/input_transitions.log.
// Queueing never waits for the writer; a contended/full queue drops diagnostics
// rather than delaying guest execution. Initialize before guest threads start
// and shut down after they have joined.
void InitializeRuntimeInputDiagnostics();
void ShutdownRuntimeInputDiagnostics();
void QueueRuntimeInputDiagnostic(const RuntimeInputDiagnosticEvent& event) noexcept;

// Writes the Xbox 360 X_INPUT_CAPABILITIES layout, including the guest's
// big-endian scalar fields. Exposed separately so encoding is regression-testable
// without requiring a controller to be attached to the host.
void StoreGuestInputCapabilities(uint8_t* base, uint32_t output,
                                 const RuntimeInputCapabilities& capabilities);

// Writes the 16-byte Xbox 360 X_INPUT_STATE layout with big-endian scalar
// fields. Kept separate from host polling for deterministic regression tests.
void StoreGuestInputState(uint8_t* base, uint32_t output,
                          const RuntimeInputState& state);

RuntimeInputVibration LoadGuestInputVibration(uint8_t* base, uint32_t input);

// The optional PC vibration control scales physical motor output only. Q16 one
// is exact identity, so the Original preset preserves every guest motor value.
inline uint16_t ScaleRuntimeInputMotorSpeed(uint16_t speed,
                                            uint32_t scaleQ16) noexcept {
    const uint64_t scaled = uint64_t(speed) * std::min(scaleQ16, 65536u) +
                            32768u;
    return static_cast<uint16_t>(std::min<uint64_t>(scaled >> 16, UINT16_MAX));
}
void ConfigureRuntimeInputVibrationScale(double scale);
uint32_t RuntimeInputVibrationScaleQ16() noexcept;

// Implements the reached XAM capability query against the host's real XInput
// connection state. The result uses the Xbox/Win32 X_ERROR value contract.
uint32_t QueryGuestInputCapabilities(uint8_t* base, uint32_t userIndex,
                                     uint32_t flags, uint32_t output);

uint32_t QueryGuestInputState(uint8_t* base, uint32_t userIndex,
                              uint32_t flags, uint32_t output);

uint32_t SetGuestInputVibration(uint8_t* base, uint32_t userIndex,
                                uint32_t flags, uint32_t input);
