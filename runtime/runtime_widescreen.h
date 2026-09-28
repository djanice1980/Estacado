#pragma once

// Widescreen (V406, experimental): the title renders a wider (or taller)
// video mode so the image fills a non-16:9 screen. The title's projection is
// already Hor+ (vertical field of view fixed, horizontal from the backbuffer
// width: sub_8275EEF8), so a consistent wider guest mode gives more view at
// the sides in every camera. Consistency points:
// - the guest-visible video mode (XGetVideoMode, VdQueryVideoMode, display
//   information: runtime_video_mode.h);
// - the title's chosen mode entry (width/height at +64/+68), patched when the
//   title applies it (sub_8223E268);
// - the title's 16:9 constant 0x8209F124 (scaler height, movie pixel aspect)
//   = the new aspect, so the scaler stays identity and movies keep a square
//   pixel aspect;
// - the host video mode cvars (presenter aspect, D1MODE_VIEWPORT_SIZE).

#include "runtime_widescreen_policy.h"

#include <cstdint>
#include <string>

// Test override DARKNESS_GUEST_DISPLAY (or REX_GUEST_DISPLAY) = <width>x<height>
// (developer checks).
RuntimeGuestDisplaySize RuntimeGuestDisplaySizeFromEnvironment();

// Before the title runs: the guest-visible mode and the apply-mode hook.
void ConfigureRuntimeWidescreen(RuntimeGuestDisplaySize size);

// The graphics configuration with the matching host video mode.
std::string RuntimePcConfigWithGuestDisplaySize(const std::string& contents,
                                                RuntimeGuestDisplaySize size);

// After the image is mapped, before the title runs.
void ApplyRuntimeWidescreenImagePatches(uint8_t* base);
