#pragma once

// Widescreen guest mode sizes (V407; runtime_widescreen.h): pure rules, tested
// by tests/register_helpers/runtime_widescreen_policy_test.cpp.

#include <cstdint>

struct RuntimeGuestDisplaySize {
    uint32_t width = 1280;
    uint32_t height = 720;
    bool Widescreen() const { return width != 1280 || height != 720; }
};

// The guest mode that fills an output of this size: 1280 x 720 for 16:9
// (within 2%), wider at 720 rows for wider screens (21:9 -> ~1680, 32:9 ->
// 2560), taller at 1280 columns for taller ones (16:10 -> 800, 4:3 -> 960).
// Multiples of 16 (the title's tile and resolve granularity).
inline RuntimeGuestDisplaySize RuntimeGuestDisplaySizeForOutput(uint32_t outputWidth,
                                                                uint32_t outputHeight) {
    RuntimeGuestDisplaySize size;
    if (!outputWidth || !outputHeight) return size;
    const double aspect = double(outputWidth) / double(outputHeight);
    const double sixteenNine = 16.0 / 9.0;
    if (aspect > sixteenNine * 1.02) {
        size.width = uint32_t(720.0 * aspect / 16.0 + 0.5) * 16;
        if (size.width > 3840) size.width = 3840;  // beyond 48:9
    } else if (aspect < sixteenNine / 1.02) {
        size.height = uint32_t(1280.0 / aspect / 16.0 + 0.5) * 16;
        if (size.height > 1280) size.height = 1280;  // up to 1:1
    }
    return size;
}

// Internal-scale class boundary for a wider guest mode. With internal scaling
// the host renders surfaces up to draw_resolution_scale_threshold pixels of
// pitch at native scale; the title requirement is 640 = half of 1280, which
// keeps the title's half-resolution post buffers (bloom, exposure) native.
// A wider mode widens those buffers (21:9: 840, pitch 880), so the boundary
// follows half the guest width, rounded up to 80-pixel EDRAM tiles; full-width
// surfaces stay scaled. Widths up to 1280 keep the configured threshold.
inline uint32_t RuntimeWidescreenScaleThreshold(uint32_t guestWidth, uint32_t configured) {
    constexpr uint32_t kTitleThreshold = 640;
    constexpr uint32_t kTile = 80;
    if (configured != kTitleThreshold || guestWidth <= 1280) return configured;
    return (guestWidth / 2 + kTile - 1) / kTile * kTile;
}
