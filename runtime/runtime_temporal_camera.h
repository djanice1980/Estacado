#pragma once

#include "runtime_temporal_camera_policy.h"
struct PPCContext;

// Called only under the existing camera capture mutex. Never writes guest state.
void RuntimeTemporalCameraReset();
void RuntimeTemporalCameraObserve(PPCContext& context, uint8_t* base, uint32_t address,
                                  const RuntimeCameraCaptureWindow& window);
void RuntimeTemporalCameraFinish(PPCContext& context, const RuntimeCameraCaptureWindow& window);
