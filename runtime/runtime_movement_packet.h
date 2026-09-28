#pragma once

#include <cstddef>
#include <cstdint>

// Why this is on by default (V367). Every change of an analog movement axis
// makes the title emit a movement command at once (moveforward/moveright ->
// 823FCB20), and one input pass fires both half-axis binds of each stick
// axis, so a physical stick - which reports a slightly different value at
// almost every poll - adds several same-instant vectors per rendered frame.
// The fixed-step builders below pack a simulation step's commands into one
// 254-byte packet; above 60 FPS those same-instant vectors overflow it and
// Jackie covers more ground than the elapsed time allows. Measured with the
// scripted stick (40% deflection with poll jitter): 1.0x at 60 FPS, 1.5x at
// 90, 2.4x at 144, 2.9x uncapped; full deflection 1.56x at 144 with a doubled
// footstep/bob rhythm. A steady stick or the keyboard never showed it.
// Merging same-instant vectors keeps the movement integral exact and makes
// the stick match the keyboard at every frame rate (V366 runs, 45 FPS to
// uncapped). --no-movement-packet-compaction restores the original path.
//
// Both fixed-step builders convert header byte2 to an absolute fraction of
// the current simulation step:82433450 predicts without retiring the ring;
//82432F90 retires consumed commands. Keep the raw relative-millisecond sender
//82433380 and unrelated82433810 callers unchanged.
constexpr bool RuntimeMovementPacketCaller(uint32_t caller) noexcept {
    return caller == 0x8243355Cu || caller == 0x8243309Cu;
}

// Locate the final absolute movement-vector entry that an immediately
// following update supersedes at the SAME encoded simulation time. Type3 is
// a six-byte absolute vector (823FCB20 ->821C8420/821A72A0), not a delta or a
// button edge. No integration occurs between equal fractions in821C9100.
// Retain the incoming sequence so acknowledgment still covers consumed input.
// Return0 for any unsupported/malformed packet; callers run the guest path.
inline size_t RuntimeMovementPacketReplacement(
    const uint8_t* packet, size_t packet_bytes,
    const uint8_t* incoming, size_t incoming_bytes) noexcept {
    if (!packet || !incoming || packet_bytes < 2 || incoming_bytes < 10 ||
        packet[0] != 1 || incoming[0] != 3 || incoming[1] != 6 ||
        incoming[2] > 128) return 0;
    const size_t end = size_t(packet[1]) + 2;
    if (packet[1] > 254 || end > packet_bytes) return 0;
    size_t last = 0;
    for (size_t at = 2; at < end;) {
        if (end-at < 4 || packet[at+1] > 32) return 0;
        const size_t bytes = size_t(packet[at+1]) + 4;
        if (bytes > end-at) return 0;
        last = at;
        at += bytes;
    }
    if (!last || packet[last] != 3 || packet[last+1] != 6 ||
        packet[last+2] != incoming[2]) return 0;
    return last;
}

// V379: the same merge where the title queues the command. AddCommand
// (824A1720) copies each command into the client's 240-entry ring (client
// +0xB08; ring push 82433970: 36-byte entries at descriptor+24, head +16,
// tail +20); when the ring is full it drops EVERY queued command. A jittery
// stick queues ~13 commands per rendered frame, so above ~300 FPS the ring
// fills between simulation steps (measured V378: 239/240 at 366 FPS, Jackie
// at ~60% speed and irregular; 126/240 at 144). A movement vector with the
// same time stamp (byte 2) as the newest queued entry, itself such a vector,
// replaces that entry's payload and keeps its sequence: the packets the
// builders produce are the ones packet compaction already made.
inline bool RuntimeMovementRingMerges(const uint8_t* newest, const uint8_t* incoming) noexcept {
    return newest && incoming && newest[0] == 3 && newest[1] == 6 && incoming[0] == 3 &&
           incoming[1] == 6 && newest[2] == incoming[2];
}

// Index of the newest queued ring entry, -1 when empty or malformed.
inline int64_t RuntimeMovementRingNewest(uint32_t head, uint32_t tail, uint32_t capacity) noexcept {
    if (!capacity || capacity > 4096u || head >= capacity || tail >= capacity || head == tail) {
        return -1;
    }
    return int64_t((head + capacity - 1u) % capacity);
}

void ConfigureRuntimeMovementPacketCompaction(bool enabled) noexcept;
// Developer trace (DARKNESS_TRACE_STICK_COMMANDS=1): while the right stick is
// deflected, AddCommand logs each command (RUNTIME_STICK_CMD) with its caller
// and frame. The input import reports the stick (verified_imports.cpp).
void RuntimeNoteRightStick(int16_t x, int16_t y) noexcept;
bool RuntimeStickCommandTraceActive() noexcept;
