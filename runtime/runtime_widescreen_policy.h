#pragma once

// Widescreen guest mode sizes (V407; runtime_widescreen.h): pure rules, tested
// by tests/register_helpers/runtime_widescreen_policy_test.cpp.

#include <cstdint>
#include <string>

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

// The title's native-grid rules for a taller guest mode (16:10, 4:3). The
// bloom rules name the 16:9 buffers (the 1280 x 720 scene copies and the
// 160 x 90 glow); the title sizes those from the video mode (a 1280 x 800
// mode has 1280 x 800 and 160 x 100 buffers, measured), so the rules follow
// or none of them match. Guest heights are multiples of 16, so the glow (an
// eighth) stays whole. The lookup tables (324 x 18) are mode-independent.
// Wider modes keep the 16:9 rules: there the title column-tiles its 4-sample
// bloom passes, and rendering those tiles on the native grid produced a
// corrupt bloom atlas (1680 x 720 measured: out-of-range values at the glow
// edge), worse than the unmatched rules' combing. Not understood yet.
inline std::string RuntimeWidescreenNativeGridRules(std::string rules, uint32_t guestWidth,
                                                    uint32_t guestHeight) {
    if (guestWidth != 1280 || guestHeight == 720) return rules;
    const auto replace = [&rules](const std::string& from, const std::string& to) {
        for (size_t at = rules.find(from); at != std::string::npos; at = rules.find(from, at + to.size())) {
            rules.replace(at, from.size(), to);
        }
    };
    replace(":1280:720:", ":" + std::to_string(guestWidth) + ":" + std::to_string(guestHeight) + ":");
    replace(":160:90:", ":" + std::to_string(guestWidth / 8) + ":" + std::to_string(guestHeight / 8) + ":");
    return rules;
}
