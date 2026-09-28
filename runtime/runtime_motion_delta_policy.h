#pragma once

#include "runtime_motion_origins_policy.h"

enum class RuntimeMotionDeltaSite { None, CameraSource, ViewCallback, Inverse, HistoryStore };

inline RuntimeMotionDeltaSite RuntimeMotionDeltaFunction(uint32_t address, uint32_t caller) noexcept {
    if (address == 0x825EB6B8u) return RuntimeMotionDeltaSite::CameraSource;
    if (address == 0x82498F68u && (caller == 0x825E572Cu || caller == 0x825E58B8u))
        return RuntimeMotionDeltaSite::ViewCallback;
    if (address == 0x82114838u && (caller == 0x82499664u || caller == 0x824998F4u || caller == 0x82499A38u))
        return RuntimeMotionDeltaSite::Inverse;
    if (address == 0x82498B08u && caller == 0x82499A28u) return RuntimeMotionDeltaSite::HistoryStore;
    return RuntimeMotionDeltaSite::None;
}

inline bool RuntimeMotionDeltaItem(uint32_t array, uint32_t count, uint32_t capacity, uint32_t item) noexcept {
    return count && count <= capacity && item >= array && array &&
        (item - array) % 240u == 0 && (item - array) / 240u < count &&
        RuntimeMotionRegistrationSlot(array, (item - array) / 240u, capacity) == item;
}

inline uint32_t RuntimeMotionDeltaKeyBucket(uint16_t entity, uint16_t part, uint32_t object) noexcept {
    return ((object >> 4u) + uint32_t(entity) + uint32_t(part)) & 255u;
}

inline uint32_t RuntimeMotionDeltaHistoryRecord(uint32_t array, uint32_t count, uint32_t index) noexcept {
    const uint64_t address = uint64_t(array) + uint64_t(index) * 96u;
    return array && count <= 32767u && index < count && address <= UINT32_MAX - 95u &&
        RuntimeMotionSourceRange(uint32_t(address), 96u, 16u) ? uint32_t(address) : 0u;
}
