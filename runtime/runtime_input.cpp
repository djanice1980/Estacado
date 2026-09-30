#include "runtime_input.h"
#include "runtime_input_capabilities_cache.h"
#include "runtime_input_empty_slot_gate.h"

#include "runtime_graphics.h"

#include "runtime_memory_access.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Xinput.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {
constexpr uint32_t kXErrorBadArguments = 0x000000A0;
constexpr uint32_t kXErrorDeviceNotConnected = 0x0000048F;
constexpr uint32_t kXInputFlagGamepad = 0x00000001;
constexpr uint32_t kXInputFlagAnyUser = 0x40000000;
constexpr uint32_t kXInputDeviceTypeKeyboard = 0x00000002;
constexpr uint32_t kGuestCapabilitiesSize = 20;
constexpr uint32_t kGuestInputStateSize = 16;
constexpr uint32_t kVibrationScaleOneQ16 = 65536;
std::atomic<uint32_t> runtimeInputVibrationScaleQ16{kVibrationScaleOneQ16};
std::atomic<uint64_t> runtimeInputButtonMapCode{kRuntimeInputIdentityButtonMap};
std::array<RuntimeMergedInputSlot, XUSER_MAX_COUNT> mergedInputSlots;

using XInputGetCapabilitiesFn = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XInputSetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);

HMODULE ResolveXInputModule() {
    static const HMODULE module = [] {
        const HMODULE loaded = LoadLibraryW(L"xinput1_4.dll");
        if (!loaded) throw std::runtime_error("runtime input requires xinput1_4.dll");
        return loaded;
    }();
    return module;
}

XInputGetCapabilitiesFn ResolveXInputGetCapabilities() {
    static XInputGetCapabilitiesFn function = [] {
        const auto address = GetProcAddress(ResolveXInputModule(), "XInputGetCapabilities");
        if (!address) {
            throw std::runtime_error("xinput1_4.dll lacks XInputGetCapabilities");
        }
        return reinterpret_cast<XInputGetCapabilitiesFn>(address);
    }();
    return function;
}

// Cached XInputGetCapabilities (see runtime_input_capabilities_cache.h for
// the hot-plug contract).
RuntimeInputCapabilitiesCache<XINPUT_CAPABILITIES, XUSER_MAX_COUNT> cachedCapabilities;

XInputGetCapabilitiesFn ResolveXInputGetCapabilities();

DWORD CachedXInputGetCapabilities(DWORD userIndex, DWORD flags, XINPUT_CAPABILITIES* out) {
    return cachedCapabilities.Get(
        userIndex, flags, GetTickCount64(), ERROR_SUCCESS, *out,
        [](uint32_t slot, uint32_t queryFlags, XINPUT_CAPABILITIES* capabilities) {
            return uint32_t(ResolveXInputGetCapabilities()(slot, queryFlags, capabilities));
        });
}

void NoteXInputStateResult(DWORD userIndex, DWORD result) {
    cachedCapabilities.NoteStateResult(userIndex, result == ERROR_SUCCESS);
}

// Empty slots skip the native state and vibration calls between re-probes
// (see runtime_input_empty_slot_gate.h for the hot-plug contract).
RuntimeInputEmptySlotGate<XUSER_MAX_COUNT> emptySlotGate;

XInputGetStateFn ResolveXInputGetState() {
    static XInputGetStateFn function = [] {
        const auto address = GetProcAddress(ResolveXInputModule(), "XInputGetState");
        if (!address) throw std::runtime_error("xinput1_4.dll lacks XInputGetState");
        return reinterpret_cast<XInputGetStateFn>(address);
    }();
    return function;
}

XInputSetStateFn ResolveXInputSetState() {
    static XInputSetStateFn function = [] {
        const auto address = GetProcAddress(ResolveXInputModule(), "XInputSetState");
        if (!address) throw std::runtime_error("xinput1_4.dll lacks XInputSetState");
        return reinterpret_cast<XInputSetStateFn>(address);
    }();
    return function;
}

const char* InputDiagnosticKindName(RuntimeInputDiagnosticKind kind) {
    switch (kind) {
    case RuntimeInputDiagnosticKind::HostState:
        return "host_state";
    case RuntimeInputDiagnosticKind::Capabilities:
        return "capabilities";
    case RuntimeInputDiagnosticKind::State:
        return "state";
    case RuntimeInputDiagnosticKind::Vibration:
        return "vibration";
    }
    return "unknown";
}

class RuntimeInputDiagnosticRecorder {
public:
    RuntimeInputDiagnosticRecorder() {
        std::filesystem::create_directories("logs");
        stream_ = CreateFileW(L"logs\\input_transitions.log", FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (stream_ != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER frequency{};
            QueryPerformanceFrequency(&frequency);
            WriteLine("INPUT_DIAGNOSTIC_SESSION pid=%lu host_ms=%llu qpc_frequency=%lld "
                      "mode=transition_only_async capacity=%zu\n",
                      GetCurrentProcessId(),
                      static_cast<unsigned long long>(GetTickCount64()),
                      static_cast<long long>(frequency.QuadPart), kCapacity);
        }
        try {
            worker_ = std::thread([this] { Run(); });
            observer_ = std::thread([this] { ObserveHostInput(); });
        } catch (...) {
            observerStopping_.store(true, std::memory_order_release);
            if (observer_.joinable()) observer_.join();
            {
                std::lock_guard lock(queueMutex_);
                stopping_ = true;
            }
            wake_.notify_one();
            if (worker_.joinable()) worker_.join();
            if (stream_ != INVALID_HANDLE_VALUE) {
                CloseHandle(stream_);
                stream_ = INVALID_HANDLE_VALUE;
            }
            throw;
        }
    }

    ~RuntimeInputDiagnosticRecorder() { Stop(); }

    void Queue(RuntimeInputDiagnosticEvent event) noexcept {
        QueuedEvent queued{};
        static_cast<RuntimeInputDiagnosticEvent&>(queued) = event;
        queued.sequence = sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
        std::unique_lock lock(queueMutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (count_ == kCapacity) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        queue_[(head_ + count_) % kCapacity] = queued;
        ++count_;
        lock.unlock();
        wake_.notify_one();
    }

    void Stop() noexcept {
        {
            std::lock_guard lock(stopMutex_);
            if (stopInitiated_) return;
            stopInitiated_ = true;
        }
        observerStopping_.store(true, std::memory_order_release);
        if (observer_.joinable()) observer_.join();
        {
            std::lock_guard lock(queueMutex_);
            stopping_ = true;
        }
        wake_.notify_one();
        if (worker_.joinable()) worker_.join();
        if (stream_ != INVALID_HANDLE_VALUE) {
            WriteLine("INPUT_DIAGNOSTIC_END dropped=%llu\n",
                      static_cast<unsigned long long>(
                          dropped_.load(std::memory_order_relaxed)));
            CloseHandle(stream_);
            stream_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    struct QueuedEvent : RuntimeInputDiagnosticEvent {
        uint64_t sequence{};
    };

    static constexpr size_t kCapacity = 256;

    void ObserveHostInput() noexcept {
        XInputGetStateFn getState = nullptr;
        try {
            getState = ResolveXInputGetState();
        } catch (...) {
            return;
        }
        RuntimeInputTransitionTracker transition;
        while (!observerStopping_.load(std::memory_order_acquire)) {
            XINPUT_STATE state{};
            const DWORD result = getState(0, &state);
            const uint16_t buttons = result == ERROR_SUCCESS
                ? state.Gamepad.wButtons
                : 0;
            if (transition.Update(result, buttons)) {
                LARGE_INTEGER counter{};
                QueryPerformanceCounter(&counter);
                RuntimeInputDiagnosticEvent event{};
                event.kind = RuntimeInputDiagnosticKind::HostState;
                event.hostMilliseconds = GetTickCount64();
                event.hostPerformanceCounter = counter.QuadPart;
                event.userIndex = 0;
                event.result = result;
                if (result == ERROR_SUCCESS) {
                    event.packetNumber = state.dwPacketNumber;
                    event.buttons = state.Gamepad.wButtons;
                    event.leftTrigger = state.Gamepad.bLeftTrigger;
                    event.rightTrigger = state.Gamepad.bRightTrigger;
                    event.thumbLX = state.Gamepad.sThumbLX;
                    event.thumbLY = state.Gamepad.sThumbLY;
                    event.thumbRX = state.Gamepad.sThumbRX;
                    event.thumbRY = state.Gamepad.sThumbRY;
                }
                Queue(event);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    }

    void Run() noexcept {
        std::vector<QueuedEvent> batch;
        batch.reserve(kCapacity);
        while (true) {
            {
                std::unique_lock lock(queueMutex_);
                wake_.wait(lock, [this] { return stopping_ || count_ != 0; });
                while (count_) {
                    batch.push_back(queue_[head_]);
                    head_ = (head_ + 1) % kCapacity;
                    --count_;
                }
                if (stopping_ && batch.empty()) break;
            }
            Write(batch);
            batch.clear();
        }
    }

    void Write(const std::vector<QueuedEvent>& batch) noexcept {
        if (stream_ == INVALID_HANDLE_VALUE) return;
        for (const auto& event : batch) {
            WriteLine(
                "INPUT_TRANSITION sequence=%llu kind=%s host_ms=%llu host_qpc=%lld "
                "thread=%u lr=0x%08X user=%u flags=0x%08X address=0x%08X "
                "result=0x%08X packet=%u buttons=0x%04X lt=%u rt=%u "
                "lx=%d ly=%d rx=%d ry=%d motor_left=0x%04X motor_right=0x%04X\n",
                static_cast<unsigned long long>(event.sequence),
                InputDiagnosticKindName(event.kind),
                static_cast<unsigned long long>(event.hostMilliseconds),
                static_cast<long long>(event.hostPerformanceCounter), event.guestThread,
                event.guestLinkRegister, event.userIndex, event.flags,
                event.guestAddress, event.result, event.packetNumber, event.buttons,
                unsigned(event.leftTrigger), unsigned(event.rightTrigger),
                int(event.thumbLX), int(event.thumbLY), int(event.thumbRX),
                int(event.thumbRY), event.leftMotorSpeed, event.rightMotorSpeed);
        }
    }

    void WriteLine(const char* format, ...) noexcept {
        if (stream_ == INVALID_HANDLE_VALUE) return;
        std::array<char, 1024> line{};
        va_list arguments;
        va_start(arguments, format);
        const int length = _vsnprintf_s(line.data(), line.size(), _TRUNCATE, format,
                                        arguments);
        va_end(arguments);
        if (length <= 0) return;
        DWORD written{};
        WriteFile(stream_, line.data(), static_cast<DWORD>(length), &written, nullptr);
    }

    std::array<QueuedEvent, kCapacity> queue_{};
    size_t head_{};
    size_t count_{};
    bool stopping_{};
    bool stopInitiated_{};
    std::mutex queueMutex_;
    std::mutex stopMutex_;
    std::condition_variable wake_;
    std::thread worker_;
    std::thread observer_;
    HANDLE stream_{INVALID_HANDLE_VALUE};
    std::atomic<bool> observerStopping_{};
    std::atomic<uint64_t> sequence_{};
    std::atomic<uint64_t> dropped_{};
};

std::mutex inputDiagnosticStateMutex;
std::unique_ptr<RuntimeInputDiagnosticRecorder> inputDiagnosticRecorder;
std::atomic<RuntimeInputDiagnosticRecorder*> inputDiagnosticRecorderPointer{};
}

void InitializeRuntimeInputDiagnostics() {
    std::lock_guard lock(inputDiagnosticStateMutex);
    if (inputDiagnosticRecorder) return;
    inputDiagnosticRecorder = std::make_unique<RuntimeInputDiagnosticRecorder>();
    inputDiagnosticRecorderPointer.store(inputDiagnosticRecorder.get(),
                                         std::memory_order_release);
}

void ShutdownRuntimeInputDiagnostics() {
    std::unique_ptr<RuntimeInputDiagnosticRecorder> recorder;
    {
        std::lock_guard lock(inputDiagnosticStateMutex);
        inputDiagnosticRecorderPointer.store(nullptr, std::memory_order_release);
        recorder = std::move(inputDiagnosticRecorder);
    }
    if (recorder) recorder->Stop();
}

void QueueRuntimeInputDiagnostic(const RuntimeInputDiagnosticEvent& event) noexcept {
    auto* recorder = inputDiagnosticRecorderPointer.load(std::memory_order_acquire);
    if (recorder) recorder->Queue(event);
}

void StoreGuestInputCapabilities(uint8_t* base, uint32_t output,
                                 const RuntimeInputCapabilities& capabilities) {
    base[output + 0] = capabilities.type;
    base[output + 1] = capabilities.subType;
    PPC_STORE_U16(output + 2, capabilities.flags);
    PPC_STORE_U16(output + 4, capabilities.buttons);
    base[output + 6] = capabilities.leftTrigger;
    base[output + 7] = capabilities.rightTrigger;
    PPC_STORE_U16(output + 8, static_cast<uint16_t>(capabilities.thumbLX));
    PPC_STORE_U16(output + 10, static_cast<uint16_t>(capabilities.thumbLY));
    PPC_STORE_U16(output + 12, static_cast<uint16_t>(capabilities.thumbRX));
    PPC_STORE_U16(output + 14, static_cast<uint16_t>(capabilities.thumbRY));
    PPC_STORE_U16(output + 16, capabilities.leftMotorSpeed);
    PPC_STORE_U16(output + 18, capabilities.rightMotorSpeed);
}

void StoreGuestInputState(uint8_t* base, uint32_t output,
                          const RuntimeInputState& state) {
    PPC_STORE_U32(output + 0, state.packetNumber);
    PPC_STORE_U16(output + 4, state.buttons);
    base[output + 6] = state.leftTrigger;
    base[output + 7] = state.rightTrigger;
    PPC_STORE_U16(output + 8, static_cast<uint16_t>(state.thumbLX));
    PPC_STORE_U16(output + 10, static_cast<uint16_t>(state.thumbLY));
    PPC_STORE_U16(output + 12, static_cast<uint16_t>(state.thumbRX));
    PPC_STORE_U16(output + 14, static_cast<uint16_t>(state.thumbRY));
}

RuntimeInputVibration LoadGuestInputVibration(uint8_t* base, uint32_t input) {
    RuntimeInputVibration vibration{};
    vibration.leftMotorSpeed =
        static_cast<uint16_t>((uint16_t(base[input + 0]) << 8) | base[input + 1]);
    vibration.rightMotorSpeed =
        static_cast<uint16_t>((uint16_t(base[input + 2]) << 8) | base[input + 3]);
    return vibration;
}

void ConfigureRuntimeInputVibrationScale(double scale) {
    if (!std::isfinite(scale) || scale < 0.0 || scale > 1.0) {
        throw std::runtime_error("input.vibration_scale must be within 0..1");
    }
    runtimeInputVibrationScaleQ16.store(
        static_cast<uint32_t>(std::llround(scale * kVibrationScaleOneQ16)),
        std::memory_order_release);
}

uint32_t RuntimeInputVibrationScaleQ16() noexcept {
    return runtimeInputVibrationScaleQ16.load(std::memory_order_acquire);
}

void ConfigureRuntimeInputButtonMap(const RuntimeInputButtonMap& map) {
    for (uint8_t source : map.sourceForGuest) {
        if (source > static_cast<uint8_t>(RuntimeInputButton::None)) {
            throw std::runtime_error("input.controller_bind contains an invalid source");
        }
    }
    runtimeInputButtonMapCode.store(EncodeRuntimeInputButtonMap(map),
                                    std::memory_order_release);
}

uint64_t RuntimeInputButtonMapCode() noexcept {
    return runtimeInputButtonMapCode.load(std::memory_order_acquire);
}

uint32_t QueryGuestInputCapabilities(uint8_t* base, uint32_t userIndex,
                                     uint32_t flags, uint32_t output) {
    if (!output || output > UINT32_MAX - kGuestCapabilitiesSize) {
        return kXErrorBadArguments;
    }
    if ((flags & 0xFFu) && !(flags & kXInputFlagGamepad)) {
        return kXErrorDeviceNotConnected;
    }

    uint32_t actualUserIndex = userIndex;
    if ((actualUserIndex & 0xFFu) == 0xFFu || (flags & kXInputFlagAnyUser)) {
        actualUserIndex = 0;
    }

    XINPUT_CAPABILITIES native{};
    const DWORD hostFlags = flags & ~kXInputDeviceTypeKeyboard;
    const DWORD result = CachedXInputGetCapabilities(actualUserIndex, hostFlags, &native);
    const bool keyboardMouse =
        RuntimeGraphicsKeyboardMouseEnabled(actualUserIndex);
    if (result != ERROR_SUCCESS && !keyboardMouse) return result;

    RuntimeInputCapabilities guest{};
    if (result == ERROR_SUCCESS) {
        guest.type = native.Type;
        guest.subType = native.SubType;
        guest.flags = native.Flags;
        guest.buttons = RemapRuntimeInputButtons(
            native.Gamepad.wButtons, RuntimeInputButtonMapCode());
        guest.leftTrigger = native.Gamepad.bLeftTrigger;
        guest.rightTrigger = native.Gamepad.bRightTrigger;
        guest.thumbLX = native.Gamepad.sThumbLX;
        guest.thumbLY = native.Gamepad.sThumbLY;
        guest.thumbRX = native.Gamepad.sThumbRX;
        guest.thumbRY = native.Gamepad.sThumbRY;
        guest.leftMotorSpeed = native.Vibration.wLeftMotorSpeed;
        guest.rightMotorSpeed = native.Vibration.wRightMotorSpeed;
    } else {
        guest.type = XINPUT_DEVTYPE_GAMEPAD;
        guest.subType = XINPUT_DEVSUBTYPE_GAMEPAD;
        guest.buttons = 0xFFFFu;
        guest.leftTrigger = 0xFFu;
        guest.rightTrigger = 0xFFu;
        guest.thumbLX = INT16_MAX;
        guest.thumbLY = INT16_MAX;
        guest.thumbRX = INT16_MAX;
        guest.thumbRY = INT16_MAX;
    }
    StoreGuestInputCapabilities(base, output, guest);
    return ERROR_SUCCESS;
}

uint32_t QueryGuestInputState(uint8_t* base, uint32_t userIndex,
                              uint32_t flags, uint32_t output) {
    if (!output || output > UINT32_MAX - kGuestInputStateSize) {
        return kXErrorBadArguments;
    }
    if ((flags & 0xFFu) && !(flags & kXInputFlagGamepad)) {
        return kXErrorDeviceNotConnected;
    }

    uint32_t actualUserIndex = userIndex;
    if ((actualUserIndex & 0xFFu) == 0xFFu || (flags & kXInputFlagAnyUser)) {
        actualUserIndex = 0;
    }

    // Only the opt-in merged slot is serialized, including both physical polls
    // and guest publication. Original direct XInput keeps its original path.
    const bool mergedDevice = RuntimeGraphicsKeyboardMouseEnabled(actualUserIndex);
    RuntimeMergedInputSlot* mergedSlot = mergedDevice && actualUserIndex < mergedInputSlots.size()
        ? &mergedInputSlots[actualUserIndex] : nullptr;
    std::unique_lock<std::mutex> mergedLock;
    if (mergedSlot) mergedLock = std::unique_lock<std::mutex>(mergedSlot->mutex);
    XINPUT_STATE native{};
    const uint64_t nowMs = GetTickCount64();
    DWORD result = ERROR_DEVICE_NOT_CONNECTED;
    if (!emptySlotGate.SkipNative(actualUserIndex, nowMs)) {
        result = ResolveXInputGetState()(actualUserIndex, &native);
        emptySlotGate.Note(actualUserIndex, result != ERROR_DEVICE_NOT_CONNECTED, nowMs);
    }
    NoteXInputStateResult(actualUserIndex, result);
    RuntimeGraphicsHostInputState keyboardMouseNative{};
    const bool keyboardMouse = RuntimeGraphicsPollKeyboardMouse(
        actualUserIndex, keyboardMouseNative);
    if (result != ERROR_SUCCESS && !keyboardMouse) {
        if (mergedSlot) mergedSlot->packets.Disconnect();
        return result;
    }

    RuntimeInputState guest{};
    if (result == ERROR_SUCCESS) {
        guest.packetNumber = native.dwPacketNumber;
        guest.buttons = native.Gamepad.wButtons;
        guest.leftTrigger = native.Gamepad.bLeftTrigger;
        guest.rightTrigger = native.Gamepad.bRightTrigger;
        guest.thumbLX = native.Gamepad.sThumbLX;
        guest.thumbLY = native.Gamepad.sThumbLY;
        guest.thumbRX = native.Gamepad.sThumbRX;
        guest.thumbRY = native.Gamepad.sThumbRY;
        // Back + Start together opens the settings overlay (rexgpu-xenos
        // polls it); the title sees neither while both are held.
        constexpr uint16_t kOverlayChord = XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START;
        if ((guest.buttons & kOverlayChord) == kOverlayChord) guest.buttons &= ~kOverlayChord;
        guest = RemapRuntimeControllerState(guest, RuntimeInputButtonMapCode());
    }
    if (keyboardMouse) {
        RuntimeInputState keyboardMouseState{};
        keyboardMouseState.packetNumber = keyboardMouseNative.packetNumber;
        keyboardMouseState.buttons = keyboardMouseNative.buttons;
        keyboardMouseState.leftTrigger = keyboardMouseNative.leftTrigger;
        keyboardMouseState.rightTrigger = keyboardMouseNative.rightTrigger;
        keyboardMouseState.thumbLX = keyboardMouseNative.thumbLX;
        keyboardMouseState.thumbLY = keyboardMouseNative.thumbLY;
        keyboardMouseState.thumbRX = keyboardMouseNative.thumbRX;
        keyboardMouseState.thumbRY = keyboardMouseNative.thumbRY;
        guest = MergeRuntimeInputStates(guest, keyboardMouseState);
    }
    if (RuntimeGraphicsSettingsOverlayOpen()) {
        // The in-game settings overlay owns input while open: the title sees
        // an idle pad (host input only; no guest state is touched).
        const uint32_t packet = guest.packetNumber;
        guest = RuntimeInputState{};
        guest.packetNumber = packet;
    }
    if (mergedSlot) guest = mergedSlot->packets.Publish(guest);
    StoreGuestInputState(base, output, guest);
    return ERROR_SUCCESS;
}

uint32_t SetGuestInputVibration(uint8_t* base, uint32_t userIndex,
                                uint32_t flags, uint32_t input) {
    if (!input || input > UINT32_MAX - sizeof(uint32_t)) {
        return kXErrorBadArguments;
    }

    uint32_t actualUserIndex = userIndex;
    if ((actualUserIndex & 0xFFu) == 0xFFu) actualUserIndex = 0;
    (void)flags;  // The reached Xbox XAM contract reserves this argument.

    const RuntimeInputVibration guest = LoadGuestInputVibration(base, input);
    const uint32_t vibrationScale = RuntimeInputVibrationScaleQ16();
    XINPUT_VIBRATION native{};
    native.wLeftMotorSpeed =
        ScaleRuntimeInputMotorSpeed(guest.leftMotorSpeed, vibrationScale);
    native.wRightMotorSpeed =
        ScaleRuntimeInputMotorSpeed(guest.rightMotorSpeed, vibrationScale);
    const uint64_t nowMs = GetTickCount64();
    DWORD result = ERROR_DEVICE_NOT_CONNECTED;
    if (!emptySlotGate.SkipNative(actualUserIndex, nowMs)) {
        result = ResolveXInputSetState()(actualUserIndex, &native);
        emptySlotGate.Note(actualUserIndex, result != ERROR_DEVICE_NOT_CONNECTED, nowMs);
    }
    if (result == ERROR_SUCCESS) return result;
    // Keyboard/mouse represents a connected guest controller but has no
    // physical motors. Accepting vibration in that case is the truthful
    // no-actuator contract, not fabricated playback.
    return RuntimeGraphicsKeyboardMouseEnabled(actualUserIndex)
        ? ERROR_SUCCESS
        : result;
}
