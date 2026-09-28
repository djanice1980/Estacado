#pragma once

#include "runtime_motion_origins_policy.h"

enum class RuntimeTemporalCameraSite { None, Begin, Inverse, Clear, End, Forward, Reset, Callback };

inline RuntimeTemporalCameraSite RuntimeTemporalCameraFunction(uint32_t address, uint32_t caller) noexcept {
    if (address == 0x8249ADF0u) return RuntimeTemporalCameraSite::Begin;
    if (address == 0x82114838u && caller == 0x8249B0D4u) return RuntimeTemporalCameraSite::Inverse;
    if (address == 0x823EBAB8u && (caller == 0x8249B1E8u || caller == 0x8249ADD0u || caller == 0x8249ADD8u))
        return RuntimeTemporalCameraSite::Clear;
    if (address == 0x829B917Cu && caller == 0x8249B228u) return RuntimeTemporalCameraSite::End;
    if (address == 0x825EB6B8u && caller == 0x8249A388u) return RuntimeTemporalCameraSite::Forward;
    if (address == 0x8249AD40u) return RuntimeTemporalCameraSite::Reset;
    if (address == 0x82498F68u && caller == 0x823F8570u) return RuntimeTemporalCameraSite::Callback;
    return RuntimeTemporalCameraSite::None;
}

inline uint32_t RuntimeTemporalCameraTable(uint32_t scene, uint32_t slot) noexcept {
    return slot <= 1u && RuntimeMotionSourceRange(scene, 4264u, 16u) ? scene + 2672u + slot * 44u : 0u;
}

inline bool RuntimeTemporalCameraBucket(int32_t head, uint32_t count, uint32_t capacity) noexcept {
    return count <= capacity && capacity <= 32767u &&
        (head == -1 || (head >= 0 && uint32_t(head) < count));
}

inline uint32_t RuntimeTemporalCameraFrame(uint32_t stack, bool restored) noexcept {
    if (restored && stack < 416u) return 0u;
    const uint32_t frame = restored ? stack - 416u : stack;
    return RuntimeMotionSourceRange(frame, 416u, 16u) ? frame : 0u;
}
