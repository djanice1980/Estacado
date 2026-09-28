#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// V376: the gameplay field of view is the horizontal angle on a 16:9 screen.
// The title renders its player view at 70 degrees horizontal for 4:3 (hor+:
// 86 at 16:9) from the character's replicated "fov" property; every frame the
// view owner writes it into the client's 416-byte view (+0x104), which the
// client copies (memcpy at 0x8249A12C) before the renderer derives the focal
// length and projection from it. The setting scales that view by a tangent
// ratio, so zoom and scripted FOV changes keep their magnification; 86 leaves
// the view untouched. (The V2xx hook edited a 95-degree viewport that the
// client overwrites every frame, so the old setting never changed the view.)
inline constexpr float kOriginalGameplayFovDegrees = 86.0f;
inline constexpr double kTitlePlayerFov43Degrees = 70.0;
inline constexpr uint32_t kMainViewportVtable = 0x82073218u;
inline constexpr uint32_t kViewportSetCurrentFovFunction = 0x82389CB8u;
inline constexpr uint32_t kMainViewportInitialFovCaller = 0x820DF16Cu;
inline constexpr uint32_t kGuestMemcpyFunction = 0x82899BF0u;
inline constexpr uint32_t kClientViewCopyReturn = 0x8249A12Cu;
// The view builder (sub_8249A168) keeps the view in persistent storage: it
// copies it into a local, lets the client build it (the copy above) and writes
// the local back (memcpy at return 0x8249A1EC). That persistent view is what
// sub_82347380 copies to client +0x640, whose FOV (+0x744) and focal length
// (+0x7A0) the renderer uses, so the scaled FOV must reach the write-back.
// V382-V390 restored the title's value there and rendered the original view
// whatever the setting (seen in play; the checks read the copy at +0x944).
// A view whose owner does not rewrite the FOV in a frame (no character owns
// it) still holds the last scaled value; RuntimeViewFovTitleBits maps it back
// to its title value, so it is never scaled twice.
inline constexpr uint32_t kClientViewWriteBackReturn = 0x8249A1ECu;
inline constexpr uint32_t kClientViewBytes = 416u;
inline constexpr uint32_t kClientViewFovOffset = 0x104u;  // near +0x12C, far +0x130

// One bounded observation window per launch. A screenshot request selects a
// neighborhood of guest work, not a GPU frame or temporal-history identity.
struct RuntimeCameraCaptureWindow {
    bool manual{};
    uint64_t generation{};
    uint32_t copies{};
    uint32_t builders{};
    uint32_t source_arena{}; // observed canonical-copy r31, not a frame identity
    const void* owner{};

    void FrameEnter() noexcept { owner = nullptr; source_arena = 0; }
    bool SelectCopy(uint64_t requested, const void* context, uint32_t arena = 0) noexcept {
        owner = nullptr;
        source_arena = 0;
        if (!context || copies >= 16u) return false;
        if (manual) {
            if (!requested) return false;
            if (!generation) generation = requested;
            if (generation != requested) return false;
        }
        ++copies;
        builders = 0;
        owner = context;
        source_arena = arena;
        return true;
    }
    bool SelectBuilder(const void* context) noexcept {
        if (!owner || owner != context || builders >= 16u) return false;
        ++builders;
        return true;
    }
};

// Input provenance in the existing opt-in manual camera neighborhood. This is
// a CPU observation window, never an inferred GPU frame or persistent object ID.
struct RuntimeMotionSourceBudget {
    // V216 observed three rotating arenas. Two producer neighborhoods require
    // later consumer copies to observe the same prepared allocation.
    static constexpr uint32_t kMaximumRecords = 4096, kMaximumCopies = 4;
    uint64_t generation{};
    uint32_t records{}, dropped{}, invalid{}, write_failures{};
    bool closed{};
    bool Select(const RuntimeCameraCaptureWindow& window, const void* context) noexcept {
        if (closed || !window.manual || !window.generation || !context ||
            window.owner != context || !window.copies || window.copies > kMaximumCopies) return false;
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
        if (closed || !context || window.owner != context || !generation || generation != window.generation ||
            window.copies < kMaximumCopies) return false;
        closed = true;
        return true;
    }
};

struct RuntimePacketSourceBudget : RuntimeMotionSourceBudget {
    static constexpr uint32_t kMaximumSegments = 128;
    static constexpr uint32_t kMaximumSegmentBytes = 256u * 1024u;
    static constexpr uint32_t kMaximumBytes = 8u * 1024u * 1024u;
    uint32_t bytes{};
    bool Select(const RuntimeCameraCaptureWindow& window, const void* context) noexcept {
        // Check the original window gates before reporting saturation.
        if (closed || !window.manual || !window.generation || !context ||
            window.owner != context || !window.copies || window.copies > kMaximumCopies ||
            (generation && generation != window.generation)) return false;
        if (records >= kMaximumSegments) {
            if (dropped != UINT32_MAX) ++dropped;
            return false;
        }
        return RuntimeMotionSourceBudget::Select(window, context);
    }
    bool Reserve(uint32_t count) noexcept {
        if (count > kMaximumSegmentBytes || count > kMaximumBytes - bytes) return false;
        bytes += count;
        return true;
    }
};

inline bool RuntimePacketSegmentBytes(uint32_t begin, uint32_t cursor,
                                       uint32_t& bytes) noexcept {
    bytes = 0;
    const uint64_t end = uint64_t(cursor) + 4;
    if (!begin || (begin & 31u) || (cursor & 3u) || end > UINT32_MAX ||
        end < begin || end - begin > RuntimePacketSourceBudget::kMaximumSegmentBytes)
        return false;
    bytes = uint32_t(end - begin);
    return true;
}

// Address arithmetic only. Callers use this for fields imminently read by the
// reviewed native functions, not as a general assertion of mapped guest memory.
inline bool RuntimeMotionSourceRange(uint32_t address, uint32_t bytes,
                                     uint32_t alignment = 4u) noexcept {
    return address && bytes && alignment && !(alignment & (alignment - 1u)) &&
        !(address & (alignment - 1u)) && uint64_t(address) + bytes <= (uint64_t(1) << 32);
}

// Raw822490B8..D0 supplies these arguments before entering8224A2E8's prologue.
inline bool RuntimeIsMotionConstantCopy(uint32_t caller, uint32_t stack,
                                       uint32_t descriptor, uint32_t register_base,
                                       uint32_t matrices, uint32_t modes) noexcept {
    return caller == 0x822490D4u && RuntimeMotionSourceRange(stack, 160u, 16u) &&
        descriptor == stack + 128u && register_base == 12u &&
        matrices == stack + 96u && modes == 0x82A6DD00u;
}

//8224A51C..A6B8 emits four vectors for each mode1 slot. With no optional
// matrices before them, the first two mode1 slots occupy logical c12..19.
inline bool RuntimeIsMotionVectorPair(uint32_t first_mode, uint32_t second_mode,
                                      uint32_t first_optional, uint32_t second_optional,
                                      uint32_t declared_vectors) noexcept {
    return first_mode == 1u && second_mode == 1u && !first_optional &&
        !second_optional && declared_vectors >= 8u;
}

inline uint32_t RuntimeMotionVectorPairCount(uint32_t mask) noexcept {
    uint32_t count = 0;
    for (uint32_t bit = 0; bit < 8u; ++bit) count += (mask >> bit) & 1u;
    return count;
}

// Compact82276148 has finished its vectors and record list when it calls
//8259D3B8 at8227695C. Observe this endpoint, not arbitrary list submissions.
inline bool RuntimeIsCompactMotionPublication(uint32_t function, uint32_t caller,
                                              uint32_t stack, uint32_t list,
                                              uint32_t count) noexcept {
    return function == 0x8259D3B8u && caller == 0x82276960u &&
        count && count <= 64u && RuntimeMotionSourceRange(stack, 576u, 16u) &&
        list == stack + 160u;
}

struct RuntimeMotionProducerBudget {
    static constexpr uint32_t kMaximumRecords = 256u, kMaximumCopies = 2u;
    uint64_t generation{};
    uint32_t records{}, dropped{}, invalid{}, write_failures{};
    bool closed{};
    bool Select(const RuntimeCameraCaptureWindow& window, const void* context) noexcept {
        // Scene builders can execute on worker contexts. The main CPU copies
        // bound the observation interval; they do not identify a worker frame.
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
        generation = window.generation; // an empty interval also gets a complete result
        closed = true;
        return true;
    }
};

// Correlation ticket only, not a frame/history identifier. Raw82762840 calls
// the viewport publisher; any other publication invalidates the observation.
struct RuntimeTransformObservationTicket {
    const void* context{};
    uint32_t copy_sequence{};
    bool published{};
    void Clear() noexcept { context = nullptr; published = false; }
    void Arm(const void* owner, uint32_t sequence) noexcept {
        context = owner; copy_sequence = sequence; published = false;
    }
    void Publication(const void* owner, uint32_t caller) noexcept {
        if (context != owner || published || caller != 0x82762844u) Clear();
        else published = true;
    }
    bool Consume(const void* owner, uint32_t& sequence) noexcept {
        const bool matches = context && context == owner && published;
        sequence = copy_sequence;
        Clear();
        return matches;
    }
};

// Raw82248A78 selects a 64-byte matrix at pool + index*656 + 16.
// Use wide arithmetic for observation so malformed metadata cannot wrap.
inline uint32_t RuntimeObservedTransformAddress(uint32_t pool, uint32_t index) noexcept {
    const uint64_t address = uint64_t(pool) + uint64_t(index) * 656u + 16u;
    return pool && !(pool & 15u) && address <= UINT32_MAX - 63u
        ? uint32_t(address) : 0u;
}

// Consumer copies a complete viewport into its stack then passes that copy.
// This identifies an observation boundary, not a previous-frame camera.
inline bool RuntimeIsFrameViewportCopy(uint32_t function, uint32_t caller,
                                       uint32_t stack, uint32_t source) noexcept {
    return function == 0x82762790u && caller == 0x820E3630u && stack &&
        stack <= UINT32_MAX - 2224u - 416u && source == stack + 2224u;
}

inline bool RuntimeGameplayFovIsOriginal(float degrees) noexcept {
    return std::fabs(degrees - kOriginalGameplayFovDegrees) < 0.01f;
}

// tan(half angle) of the configured view over the original's (1 = unchanged).
inline double RuntimeGameplayFovTangentScale(float degrees16x9) noexcept {
    if (!std::isfinite(degrees16x9) || RuntimeGameplayFovIsOriginal(degrees16x9)) return 1.0;
    constexpr double kRadians = 3.14159265358979323846 / 180.0;
    const double original =
        std::tan(kTitlePlayerFov43Degrees * 0.5 * kRadians) * (16.0 / 9.0) / (4.0 / 3.0);
    return std::tan(double(degrees16x9) * 0.5 * kRadians) / original;
}

// One view FOV (degrees, the title's own convention) scaled by that ratio.
inline float RuntimeScaledViewFov(float degrees, double tangentScale) noexcept {
    if (!(degrees > 0.0f && degrees < 180.0f) || tangentScale == 1.0) return degrees;
    constexpr double kRadians = 3.14159265358979323846 / 180.0;
    const double scaled =
        2.0 * std::atan(std::tan(double(degrees) * 0.5 * kRadians) * tangentScale) / kRadians;
    return float(std::clamp(scaled, 1.0, 170.0));
}

// The client's copy of its freshly filled per-frame view (sub_8249A030:
// memcpy(client + 2112, view, 416)).
inline bool RuntimeIsClientViewCopy(uint32_t caller, uint32_t bytes) noexcept {
    return caller == kClientViewCopyReturn && bytes == kClientViewBytes;
}

// One thread's last scaling: the title's FOV bits and the scaled bits the
// client's copy and, through the builder's write-back, the persistent view
// (the renderer's) received.
struct RuntimeViewFovScaling {
    uint32_t title_bits{};
    uint32_t scaled_bits{};
};

// The title's own FOV for this copy: the view's value, unless it still holds
// the last scaled value (its owner did not rewrite the FOV since), then the
// title value that came from. A scaled value is never scaled again.
inline uint32_t RuntimeViewFovTitleBits(const RuntimeViewFovScaling& last,
                                        uint32_t current_bits) noexcept {
    return last.scaled_bits && current_bits == last.scaled_bits ? last.title_bits : current_bits;
}
