#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Automatic internal scale (resolution_scale = 0, V330). The scale is fixed
// for a run, so the runtime resolves it at startup and the GPU always
// receives a concrete value: 1x below 1000 output lines (720p/800p screens)
// or on graphics cards with less than 6 GB, 2x for 1080p, 1440p and 4K, 3x
// for 4K and above only on cards with 16 GB or more. Measured (V330, 16 GB
// card, heaviest street view): 2x 6.4 ms of GPU work per frame, 3x 12.7 ms
// (74-81 FPS), 2x on a 1080p output the same as on 1440p.
struct RuntimeAutoScale {
    uint32_t scale = 2;
    uint32_t outputWidth = 0;   // 0: unknown
    uint32_t outputHeight = 0;
    uint64_t dedicatedVideoMemory = 0;  // bytes, 0: unknown
    const char* reason = "default";
};

// Dedicated video memory stands in for GPU class (the only cheap, reliable
// signal before the device exists); a player can always pick another scale.
inline constexpr uint64_t kAutoScaleMinVideoMemory = 6ull << 30;
inline constexpr uint64_t kAutoScale3xVideoMemory = 16ull << 30;

inline RuntimeAutoScale RuntimeResolveAutomaticScale(uint32_t outputWidth, uint32_t outputHeight,
                                                     uint64_t dedicatedVideoMemory) {
    RuntimeAutoScale result;
    result.outputWidth = outputWidth;
    result.outputHeight = outputHeight;
    result.dedicatedVideoMemory = dedicatedVideoMemory;
    // Cards report slightly less than their nominal size (16 GB -> 15995 MB).
    const uint64_t slack = 512ull << 20;
    if (dedicatedVideoMemory && dedicatedVideoMemory + slack < kAutoScaleMinVideoMemory) {
        result.scale = 1;
        result.reason = "graphics_card_below_6gb";
    } else if (outputHeight && outputHeight < 1000) {
        result.scale = 1;
        result.reason = "output_below_1000_lines";
    } else if (outputHeight >= 2000) {
        const bool enough = dedicatedVideoMemory + slack >= kAutoScale3xVideoMemory;
        result.scale = enough ? 3 : 2;
        result.reason = enough ? "4k_output" : "4k_output_card_below_16gb";
    } else {
        result.scale = 2;
        result.reason = outputHeight ? "1080p_to_1440p_output" : "output_unknown";
    }
    return result;
}

// The output size a configuration asks for: explicit sizes ("720p", "1080p",
// "1440p", "4k", "WxH") or, for "native", the desktop size of the monitor
// (index 0 = the primary monitor; other indices follow the Win32 monitor
// order, which normally matches), plus the high-performance adapter's
// dedicated video memory.
RuntimeAutoScale RuntimeDetectAutomaticScale(std::string_view outputResolution, int64_t monitor);

// The refresh rate of a monitor (index as above); 0 when unknown.
double RuntimeMonitorRefreshHz(int64_t monitor);

// Display text of the automatic choice ("2x - 2560 x 1440").
std::string RuntimeAutoScaleLabel(uint32_t scale);
