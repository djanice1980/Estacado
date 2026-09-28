#pragma once

#include "runtime_motion_delta_policy.h"
struct PPCContext;

// Serialized by the existing camera trace mutex; default-off, bounded, read-only.
void RuntimeMotionDeltaReset();
uint32_t RuntimeMotionDeltaObservedCount() noexcept;
void RuntimeMotionDeltaObserve(PPCContext& context, uint8_t* base, uint32_t address,
                              const RuntimeCameraCaptureWindow& window);
void RuntimeMotionDeltaFinish(PPCContext& context, const RuntimeCameraCaptureWindow& window);
