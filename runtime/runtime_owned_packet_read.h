#pragma once
#include "runtime_owned_camera_packets.h"
#include "../external/ReXGlue/include/rex/graphics/pc_owned_camera_packet.h"

// Caller holds camera mutex and payload Snapshot for the ENTIRE operation.
// ReadWord must return a host-endian word from the canonical physical backing.
template<class ReadWord>
bool RuntimeReadOwnedCameraPacket(const runtime_owned_camera_source::PacketConstantStore& store,
    const RuntimeSourceMemoryCoordinator::Snapshot& snapshot, uint32_t physical,
    uint32_t header, uint32_t count, uint32_t* payload,
    rex::graphics::pc_owned_camera_packet::Source* output, ReadWord&& read) {
    if (!payload || !output || !count || count > 1024 || (physical & 3) ||
        uint64_t(physical) + uint64_t(count + 1) * 4 > 0x20000000ull ||
        (header & 0xC0008000u) || ((header >> 16) & 0x3FFFu) + 1 != count) return false;
    const uint32_t first = header & 0x7FFFu;
    const uint32_t begin = first < 0x4030 ? 0x4030 : first;
    const uint32_t end = first + count > 0x4050 ? 0x4050 : first + count;
    if (end <= begin) return false;
    runtime_owned_camera_source::PacketConstantPublication packet;
    if (!store.Copy(snapshot, physical, (count + 1) * 4, begin, end - begin, packet) ||
        read(physical) != header) return false;
    // The ticket is live during these reads. Payload equality confirms the
    // transported slice; it is never used to discover or select a source.
    for (uint32_t i = 0; i < count; ++i) payload[i] = read(physical + (i + 1) * 4);
    for (uint32_t i = 0; i < packet.words; ++i)
        if (payload[begin - first + i] != packet.source.packed[packet.source_word + i]) return false;
    rex::graphics::pc_owned_camera_packet::Source result;
    result.packet = packet.id; result.constant = packet.constant_id;
    result.publication = packet.source.token.publication; result.item = packet.source.token.item;
    result.first_register = begin; result.source_word = packet.source_word; result.words = packet.words;
    for (uint32_t i = 0; i < 16; ++i) result.camera_current[i] = packet.source.camera_current[i];
    *output = result;
    return true;
}
