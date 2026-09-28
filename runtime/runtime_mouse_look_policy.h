#pragma once

// Native mouse look policy (V310+): turns raw relative mouse counts into the
// arguments of the title's own look(dx, dy) command (CWClient_Mod slot 0x420,
// sub_823FCF68 - the handler the title binds to "mousemove"). look() packs
// yaw = 32 * sensX * dx and pitch = -32 * sensY * dy (sensX/sensY = client
// +0x2284/+0x2288, title controller settings) into signed 16-bit look units
// of 1/65536 revolution, truncating toward zero, and queues one command.
//
// The policy aims at a fixed number of look units per mouse count on both
// axes (so yaw and pitch feel the same regardless of the title's controller
// axis ratio or its Y-inversion setting), carries sub-unit remainders so slow
// motion is exact and keeps every command inside the int16 range.
//
// Command rate: the client's predicted view turns once per look command, so
// a command every frame is what keeps high refresh rates smooth. Commands go
// into the client's command ring (client +0xB08, 36-byte entries, drained by
// the simulation step); when that ring is full the title discards EVERY
// queued command (movement and buttons included). A command is therefore
// sent only while at least half of the ring is free, and at most 240 per
// second; otherwise the motion waits for the next frame (nothing is lost).

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace runtime_mouse_look {

inline constexpr double kLookScale = 32.0;               // title constant in look()
inline constexpr double kMaxUnitsPerCommand = 30000.0;   // inside int16 with margin
inline constexpr double kMinCommandSeconds = 1.0 / 240.0;
inline constexpr int32_t kMinFreeCommandSlots = 8;
// Look units (1/65536 revolution) per mouse count at sensitivity 1.0. The
// title applies them 1:1 outside zoom (measured: 22.5 -> 0.1236 deg/count).
// 12 = 0.0659 deg/count: 360 degrees per ~17 cm at 800 DPI (~9 cm at 1600).
inline constexpr double kUnitsPerCount = 12.0;

// Room in the client's command ring (free entries as the title computes
// them) for one look command (look() may add a flags command first) while
// leaving at least half of the ring to the title's own commands.
inline bool CommandRingHasRoom(int32_t capacity, int32_t freeSlots) {
  if (capacity <= 0 || freeSlots < 0 || freeSlots >= capacity) {
    return false;
  }
  return freeSlots >= (std::max)(kMinFreeCommandSlots, capacity / 2);
}

struct State {
  double pendingYawUnits = 0.0;
  double pendingPitchUnits = 0.0;
  int64_t lastCommandTick = 0;
  bool haveLastCommand = false;
};

struct Command {
  bool send = false;
  float argX = 0.0f;
  float argY = 0.0f;
  int32_t yawUnits = 0;    // what look() will pack
  int32_t pitchUnits = 0;
};

// Adds one poll's counts. Mouse right turns right (positive yaw like the
// stick), mouse down looks down (negative pitch) unless inverted.
inline void Accumulate(State& state, int32_t dx, int32_t dy, double sensitivity,
                       bool invertY) {
  if (!std::isfinite(sensitivity) || sensitivity <= 0.0) {
    return;
  }
  const double units = kUnitsPerCount * sensitivity;
  state.pendingYawUnits += double(dx) * units;
  state.pendingPitchUnits += double(invertY ? dy : -dy) * units;
}

inline void Reset(State& state) {
  state.pendingYawUnits = 0.0;
  state.pendingPitchUnits = 0.0;
}

inline int32_t ClampTruncate(double units) {
  const double clamped =
      std::fmax(-kMaxUnitsPerCommand, std::fmin(kMaxUnitsPerCommand, units));
  return int32_t(clamped);  // toward zero, like the title
}

// Argument for look() that makes the title pack exactly `units`: half a unit
// of headroom in the direction of travel absorbs float rounding before the
// title's truncation toward zero.
inline float ArgumentFor(int32_t units, double titleScale) {
  if (!units || !std::isfinite(titleScale) || titleScale == 0.0) {
    return 0.0f;
  }
  const double target = double(units) + (units > 0 ? 0.5 : -0.5);
  return float(target / titleScale);
}

// Next command, or none while the interval since the previous command is
// shorter than 1/240 s, nothing whole is pending, or the title's scales are
// unusable. Sent whole units leave the pending amounts; remainders carry.
// (The caller checks CommandRingHasRoom first.)
inline Command Next(State& state, double sensX, double sensY, int64_t nowTick,
                    int64_t tickFrequency) {
  Command command;
  if (tickFrequency <= 0 || !std::isfinite(sensX) || !std::isfinite(sensY) ||
      std::fabs(sensX) < 1e-6 || std::fabs(sensY) < 1e-6) {
    return command;
  }
  if (state.haveLastCommand &&
      double(nowTick - state.lastCommandTick) < kMinCommandSeconds * double(tickFrequency)) {
    return command;
  }
  const int32_t yaw = ClampTruncate(state.pendingYawUnits);
  const int32_t pitch = ClampTruncate(state.pendingPitchUnits);
  if (!yaw && !pitch) {
    return command;
  }
  command.send = true;
  command.yawUnits = yaw;
  command.pitchUnits = pitch;
  command.argX = ArgumentFor(yaw, kLookScale * sensX);
  command.argY = ArgumentFor(pitch, -kLookScale * sensY);
  state.pendingYawUnits -= yaw;
  state.pendingPitchUnits -= pitch;
  state.lastCommandTick = nowTick;
  state.haveLastCommand = true;
  return command;
}

// The units the title will pack for an argument (float math as in look()).
inline int32_t TitleUnits(float argument, float titleScale) {
  return int32_t(argument * titleScale);
}

}  // namespace runtime_mouse_look
