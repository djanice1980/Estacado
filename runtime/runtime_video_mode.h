#pragma once

#include <cstdint>

// The Darkness observes the original Xbox 360 720p60 mode through the native
// XAM/xboxkrnl imports. The embedded GPU must derive its guest VBlank cadence
// from this same contract. Host output resolution, presentation mode and frame
// limiting are intentionally separate and must not mutate these values.
namespace darkness::guest_video_mode {

inline constexpr uint32_t kDisplayWidth = 1280;
inline constexpr uint32_t kDisplayHeight = 720;
inline constexpr double kRefreshRateHz = 60.0;
inline constexpr uint32_t kRefreshRateFloatBits = 0x42700000u;

// The mode the title sees: kDisplayWidth x kDisplayHeight unless widescreen
// (runtime_widescreen.h) configured a wider or taller one before the title ran.
uint32_t DisplayWidth();
uint32_t DisplayHeight();
void ConfigureDisplaySize(uint32_t width, uint32_t height);

}  // namespace darkness::guest_video_mode
