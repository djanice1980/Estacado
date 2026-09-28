#pragma once

#include <cstdint>
#include "runtime_owned_camera_source.h"
#include "runtime_owned_camera_constants.h"
#include "runtime_owned_camera_packets.h"
#include "../external/ReXGlue/include/rex/graphics/pc_owned_camera_packet.h"

struct PPCContext;

void ConfigureRuntimeCamera(float gameplay_fov_degrees,
                            bool trace_camera_state,
                            bool manual_capture = false);
void RuntimeCameraFunctionEnter(PPCContext& context, uint8_t* base,
                                uint32_t address);

// Non-consuming copy of CPU source data from the opt-in capture window. The
// token must be explicitly transported from its producer; guest pointer or
// byte equality is not a substitute. This does not certify a GPU association.
bool RuntimeCopyOwnedCameraSource(runtime_owned_camera_source::Token token,
                                 runtime_owned_camera_source::Source& output);

// Additionally checks watched native bytes and backing allocation at the copy
// boundary. Does not certify native arena retirement, current camera state or
// GPU association. Returns owned values; it does not retain a guest memory lease.
bool RuntimeCopyUnchangedOwnedCameraSource(runtime_owned_camera_source::Token token,
                                          runtime_owned_camera_source::Source& output);

// Unique watched native selection; not a GPU association. Failure preserves output.
bool RuntimeSelectOwnedCameraSource(uint32_t record,
                                   runtime_owned_camera_source::Source& output);
bool RuntimeCopyActiveOwnedCameraSource(const PPCContext& context,
                                       runtime_owned_camera_source::Source& output);
void RuntimeRunOwnedCameraConstantCopy(PPCContext& context, uint8_t* base,
                                      void (*native_copy)(PPCContext&, uint8_t*));
bool RuntimeCopyOwnedCameraConstants(uint32_t address, uint32_t bytes,
                                    runtime_owned_camera_source::ConstantPublication& output);
void RuntimeRunOwnedCameraPacketWrite(PPCContext&, uint8_t*, void (*)(PPCContext&, uint8_t*));
void RuntimeRunOwnedCameraPacketFlush(PPCContext&, uint8_t*, void (*)(PPCContext&, uint8_t*));
bool RuntimeCopyOwnedCameraPacket(void* physical_base, uint32_t physical, uint32_t header,
    uint32_t count, uint32_t* payload, rex::graphics::pc_owned_camera_packet::Source* source) noexcept;
