#pragma once

// Right-stick look at high frame rates (V390).
//
// The title turns the controller view through a timed look emitter (client
// slot 0x3BC, sub_823FC650). Every client frame it takes the time since its
// last look command (client +7064, title seconds, times the client time scale
// at +424) and only when that exceeds its argument sends
// look(velocity x time) (slot 0x420, sub_823FCF68) and stores the time. The
// per-frame look paths (sub_823F9B00 every playing frame, sub_823F8AF0 when
// the stick velocity changes) pass 1/120 s (float at 0x8209E8A0), so above
// 120 FPS a command goes out only every other frame: at 144 Hz the view
// turned on ~half the displayed frames (~72 Hz camera motion; V387 captures).
// look() packs yaw = trunc(32 x sensX x arg) and pitch = trunc(-32 x sensY x
// arg) (client +0x2284/+0x2288) into int16 units of 1/65536 turn, truncating
// toward zero in the encoder (sub_8237C2F0).
//
// The hook runs the same emitter with a 1/300 s interval, so the view turns
// on every displayed frame up to 300 FPS. Sending more often would truncate
// more often (slower fine aiming), so sub-unit remainders carry from one
// command to the next and are dropped exactly where the title's own 1/120 s
// cadence would have sent - and truncated - a command. At 60 FPS and below
// every command is a cadence command: the stream is the title's, argument for
// argument. Above that, the rotation that reaches the title by each cadence
// point is what the title would have sent, only spread over the frames.
// Nothing else changes: the emitter still decides, measures and stores its
// own time; the look velocity, acceleration and zoom scaling are the title's.
//
// Packet budget: each simulation step (1/30 s) packs its commands into one
// 254-byte packet (runtime_movement_packet.h): 5 bytes for the kind-0 command
// every client frame, 10 per movement vector (one per frame with a moving
// stick since V379), 10 per look command. Per-frame looks would fill it at
// 300 FPS with both sticks moving (25 bytes x 10 frames), below the title's
// own limit (~430 FPS with 4 looks per step). So the 1/300 s interval is only
// used while client frames average at least 1/250 s apart (at most 208 bytes
// per step); faster, the title's own 1/120 s interval applies unchanged.
//
// DARKNESS_STICK_LOOK_HFR=0 restores the title's emitter for comparisons.

#include <cmath>
#include <cstdint>
#include <cstring>

namespace stick_look {

inline constexpr uint32_t kEmitter = 0x823FC650;        // client slot 0x3BC
inline constexpr uint32_t kLook = 0x823FCF68;           // client slot 0x420
inline constexpr uint32_t kLookReturn = 0x823FC7E4;     // the emitter's look() call
inline constexpr uint32_t kTitleIntervalAddress = 0x8209E8A0;  // 0x820A0000 - 5984
inline constexpr uint32_t kTitleIntervalBits = 0x3C088889;     // float 1/120
inline constexpr uint32_t kLastLookTimeOffset = 7064;   // double, title seconds
inline constexpr uint32_t kTimeScaleOffset = 424;       // float
inline constexpr uint32_t kSensXOffset = 0x2284;        // float, yaw sensitivity
inline constexpr uint32_t kSensYOffset = 0x2288;        // float, pitch sensitivity
inline constexpr float kYawScale = 32.0f;               // look(): float at 0x82A48A78
inline constexpr float kPitchScale = -32.0f;            // look(): float at 0x8209E058

inline constexpr float kHfrInterval = float(1.0 / 300.0);
inline constexpr double kMinHfrFrameSeconds = 1.0 / 250.0;
inline constexpr double kFrameAverageWeight = 0.1;
inline constexpr double kSameFrameSeconds = 0.0005;     // a second call in one frame
inline constexpr double kMaxFrameSampleSeconds = 1.0 / 60.0;
inline constexpr double kMaxUnitsPerCommand = 30000.0;  // inside int16 with margin

inline constexpr uint32_t kStepPacketBytes = 254;
inline constexpr uint32_t kFrameCommandBytes = 5 + 10;  // kind-0 command + one movement vector
inline constexpr uint32_t kLookCommandBytes = 10;

inline uint32_t FloatBits(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

inline bool IsTitleInterval(float interval) { return FloatBits(interval) == kTitleIntervalBits; }

// The emitter's elapsed time as the title computes it: frsp(now - last),
// clamped to [0, 1] second (fsel pair), then fmuls by the client time scale.
inline float TitleElapsed(double now, double last, float scale) {
  const float delta = float(now - last);
  double clamped = (delta - 1.0f) >= 0.0f ? 1.0 : double(delta);
  clamped = delta >= 0.0f ? clamped : 0.0;
  return float(clamped) * scale;
}

// The emitter sends when the elapsed time is not <= its interval (ble skips).
inline bool TitleSends(float elapsed, float interval) { return !(elapsed <= interval); }

// look()'s float units for one axis before the encoder truncates them.
inline float TitleUnitsFloat(float argument, float sensitivity, float scale) {
  return (sensitivity * argument) * scale;
}

inline int32_t TruncateUnits(double units) {
  const double clamped = std::fmax(-kMaxUnitsPerCommand, std::fmin(kMaxUnitsPerCommand, units));
  return int32_t(clamped);  // toward zero, like fctiwz
}

struct AxisLook {
  float argument = 0.0f;
  int32_t units = 0;   // what look() packs for this axis
  double carry = 0.0;  // sub-unit remainder for the next command
};

// One axis of the emitter's look() call. Without a carry the argument stays
// bit for bit. With one, the argument packs trunc(title units + carry) (half
// a unit of headroom absorbs float rounding before the title's truncation)
// and keeps the title's zero / non-zero state, so look() queues a command
// exactly when it would have.
inline AxisLook CarryAxis(float argument, float sensitivity, float scale, double carry) {
  AxisLook out;
  out.argument = argument;
  const float units = TitleUnitsFloat(argument, sensitivity, scale);
  const double scaleProduct = double(sensitivity) * double(scale);
  if (!std::isfinite(units) || !std::isfinite(scaleProduct) || std::fabs(scaleProduct) < 1e-9) {
    return out;  // unusable scales: the title's argument, nothing carried
  }
  if (carry == 0.0 || units == 0.0f) {
    out.units = TruncateUnits(units);
    out.carry = units == 0.0f ? carry : double(units) - double(out.units);
    return out;
  }
  const double exact = double(units) + carry;
  out.units = TruncateUnits(exact);
  const double target = out.units != 0 ? double(out.units) + (out.units > 0 ? 0.5 : -0.5)
                                       : (units > 0.0f ? 0.25 : -0.25);
  out.argument = float(target / scaleProduct);
  if (TruncateUnits(TitleUnitsFloat(out.argument, sensitivity, scale)) != out.units) {
    out.argument = argument;  // never expected: fall back to the title's own
    out.units = TruncateUnits(units);
    out.carry = 0.0;
    return out;
  }
  out.carry = exact - double(out.units);
  return out;
}

struct Tracker {
  uint32_t client = 0;
  bool haveCadence = false;
  double cadenceTime = 0.0;  // when the title's own 1/120 s cadence last sent
  double carryYaw = 0.0;
  double carryPitch = 0.0;
  bool haveTick = false;
  int64_t lastTick = 0;
  double frameSeconds = 0.0;  // average client frame interval (host clock)
};

inline void ResetFor(Tracker& tracker, uint32_t client) {
  tracker.client = client;
  tracker.haveCadence = false;
  tracker.cadenceTime = 0.0;
  tracker.carryYaw = 0.0;
  tracker.carryPitch = 0.0;
}

// Host clock of each per-frame emitter call; a second call within the same
// frame is ignored, long gaps (menus, loading) count as one 60 Hz frame.
inline void NoteFrame(Tracker& tracker, int64_t tick, int64_t frequency) {
  if (frequency <= 0) return;
  if (!tracker.haveTick) {
    tracker.haveTick = true;
    tracker.lastTick = tick;
    return;
  }
  const double seconds = double(tick - tracker.lastTick) / double(frequency);
  if (seconds < kSameFrameSeconds) return;
  tracker.lastTick = tick;
  const double sample = std::fmin(seconds, kMaxFrameSampleSeconds);
  tracker.frameSeconds = tracker.frameSeconds == 0.0
                             ? sample
                             : tracker.frameSeconds + kFrameAverageWeight * (sample - tracker.frameSeconds);
}

inline bool UseHfrInterval(const Tracker& tracker) {
  return tracker.frameSeconds >= kMinHfrFrameSeconds;
}

// Before a title-interval call: the cadence clock follows the title's own
// look time when that is earlier (first call, or the title reset it).
inline void SyncCadence(Tracker& tracker, double lastLookTime) {
  if (!tracker.haveCadence || lastLookTime < tracker.cadenceTime) {
    tracker.haveCadence = true;
    tracker.cadenceTime = lastLookTime;
    tracker.carryYaw = 0.0;
    tracker.carryPitch = 0.0;
  }
}

// After an emitter call that stored a new look time: a send the title's own
// interval would also have made is a cadence point (its remainder is dropped,
// as the title drops it).
inline bool AfterSend(Tracker& tracker, double lookTime, float timeScale, float titleInterval) {
  if (!TitleSends(TitleElapsed(lookTime, tracker.cadenceTime, timeScale), titleInterval)) {
    return false;
  }
  tracker.haveCadence = true;
  tracker.cadenceTime = lookTime;
  tracker.carryYaw = 0.0;
  tracker.carryPitch = 0.0;
  return true;
}

struct Look {
  AxisLook yaw;
  AxisLook pitch;
};

inline Look CarryLook(Tracker& tracker, float argX, float argY, float sensX, float sensY) {
  Look look{CarryAxis(argX, sensX, kYawScale, tracker.carryYaw),
            CarryAxis(argY, sensY, kPitchScale, tracker.carryPitch)};
  tracker.carryYaw = look.yaw.carry;
  tracker.carryPitch = look.pitch.carry;
  return look;
}

}  // namespace stick_look
