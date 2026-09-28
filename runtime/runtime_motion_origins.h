#pragma once

#include "runtime_motion_origins_policy.h"

struct PPCContext;

// All calls are serialized by runtime_camera.cpp's existing camera_trace_mutex.
// These observers neither write guest state nor identify persistent history.
void RuntimeMotionOriginsReset();
uint32_t RuntimeMotionOriginsObservedCount() noexcept;
void RuntimeMotionOriginsObserve(PPCContext& context, uint8_t* base, uint32_t address,
                                 const RuntimeCameraCaptureWindow& window,
                                 uint32_t producer_sequence = UINT32_MAX);
void RuntimeMotionOriginsFinish(PPCContext& context, const RuntimeCameraCaptureWindow& window);
