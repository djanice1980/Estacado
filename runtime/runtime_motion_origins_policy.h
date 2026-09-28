#pragma once

#include "runtime_camera_policy.h"

enum class RuntimeMotionOriginSite {
    None, CameraBegin, CameraReady, RegisterSingle, RegisterPair,
    SpecialCopy, RenderDispatch, ModelBuilder, MeshBuilder, CompactOutput,
    CameraParent, RegisterSingleReady, RegisterPairReady
};

inline RuntimeMotionOriginSite RuntimeMotionOriginFunction(uint32_t address,
                                                           uint32_t caller) noexcept {
    switch (address) {
    case 0x825E54B0u: return RuntimeMotionOriginSite::CameraParent;
    case 0x821D3700u:
        if (caller == 0x825E47A8u) return RuntimeMotionOriginSite::RegisterSingleReady;
        if (caller == 0x825E4990u) return RuntimeMotionOriginSite::RegisterPairReady;
        return RuntimeMotionOriginSite::None;
    case 0x825DF8D0u: return RuntimeMotionOriginSite::CameraBegin;
    case 0x825DF350u: return caller == 0x825DFC98u
        ? RuntimeMotionOriginSite::CameraReady : RuntimeMotionOriginSite::None;
    case 0x825E45B8u: return RuntimeMotionOriginSite::RegisterSingle;
    case 0x825E47B8u: return RuntimeMotionOriginSite::RegisterPair;
    case 0x825DFD50u: return caller == 0x825DFD04u
        ? RuntimeMotionOriginSite::SpecialCopy : RuntimeMotionOriginSite::None;
    case 0x825E2958u: return RuntimeMotionOriginSite::RenderDispatch;
    case 0x825C4C50u: return RuntimeMotionOriginSite::ModelBuilder;
    case 0x825D3BF8u: return RuntimeMotionOriginSite::MeshBuilder;
    case 0x8259D3B8u: return caller == 0x82276960u
        ? RuntimeMotionOriginSite::CompactOutput : RuntimeMotionOriginSite::None;
    default: return RuntimeMotionOriginSite::None;
    }
}

// A predicted registration slot is an address, not evidence of publication or
// uninterrupted lifetime. The dispatch observation supplies the later contents.
inline uint32_t RuntimeMotionRegistrationSlot(uint32_t array, uint32_t count,
                                             uint32_t capacity) noexcept {
    const uint64_t address = uint64_t(array) + uint64_t(count) * 240u;
    return array && count < capacity && address <= UINT32_MAX - 239u &&
        RuntimeMotionSourceRange(uint32_t(address), 240u, 16u) ? uint32_t(address) : 0u;
}

// At the qualified unlock call the native count has already advanced and the
// registration lock is still held. Unlike the pre-lock prediction, r31 names
// the item actually written. Reject inactive modes and exhausted-list exits.
inline bool RuntimeCompletedMotionRegistration(uint32_t array, uint32_t count,
    uint32_t capacity, uint32_t actual_item, uint32_t camera, uint32_t saved_camera) noexcept {
    return camera && camera == saved_camera && count && count <= capacity && actual_item &&
        actual_item == RuntimeMotionRegistrationSlot(array, count - 1u, capacity);
}

struct RuntimeMotionOriginBudget {
    static constexpr uint32_t kMaximumRecords = 1024u, kMaximumCopies = 4u;
    uint64_t generation{};
    uint32_t records{}, dropped{}, invalid{}, write_failures{};
    bool closed{};
    bool Select(const RuntimeCameraCaptureWindow& window, const void* context) noexcept {
        if (closed || !context || !window.manual || !window.generation || !window.owner ||
            !window.copies || window.copies > kMaximumCopies) return false;
        if (!generation) generation = window.generation;
        if (generation != window.generation) return false;
        if (records == kMaximumRecords) {
            if (dropped != UINT32_MAX) ++dropped;
            return false;
        }
        ++records;
        return true;
    }
    bool FinishBeforeCopy(const RuntimeCameraCaptureWindow& window, const void* context) noexcept {
        if (closed || !context || window.owner != context || !window.manual || !window.generation ||
            window.copies < kMaximumCopies || (generation && generation != window.generation)) return false;
        generation = window.generation;
        closed = true;
        return true;
    }
};
