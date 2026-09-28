// Native mouse look policy (V310+): mouse counts become arguments of the
// title's look(dx, dy) that pack exactly the requested look units (checked
// against the title's own float math), remainders carry, every command fits
// int16, one command per frame up to 240/s while the client's command ring
// keeps half its room, and the axes' signs follow the title's mouse
// convention independently of its controller inversion.
#include "runtime_mouse_look_policy.h"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <string>

namespace {

// look(): f10 = sensX * dx (fmuls), yaw = f10 * 32.0f; f11 = sensY * dy,
// pitch = f11 * -32.0f; packed as int16 truncating toward zero.
int32_t TitleYaw(float sensX, float argX) { return int32_t((sensX * argX) * 32.0f); }
int32_t TitlePitch(float sensY, float argY) { return int32_t((sensY * argY) * -32.0f); }

}  // namespace

int main() {
  bool passed = true;
  const auto check = [&passed](bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      passed = false;
    }
  };
  namespace ml = runtime_mouse_look;
  constexpr int64_t kHz = 10000000;  // QPC ticks per second
  constexpr int64_t kInterval = kHz / 240 + 1;
  check(ml::kUnitsPerCount == 12.0, "calibrated 12 look units per count (0.066 deg)");

  {
    // The title packs exactly the requested units for any usable scale.
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> sens(0.2f, 6.0f);
    std::uniform_int_distribution<int32_t> units(-30000, 30000);
    bool exact = true;
    for (int i = 0; i < 20000; ++i) {
      const float sx = sens(rng) * ((i & 1) ? 1.0f : -1.0f);
      const int32_t u = units(rng);
      const float argX = ml::ArgumentFor(u, 32.0 * double(sx));
      const float argY = ml::ArgumentFor(u, -32.0 * double(sx));
      exact = exact && TitleYaw(sx, argX) == u && TitlePitch(sx, argY) == u;
    }
    check(exact, "title packs exactly the requested units (both signs of the scale)");
  }
  {
    // Slow motion: sub-unit remainders carry across commands.
    ml::State state;
    int64_t now = 0;
    int64_t yawSent = 0;
    for (int i = 0; i < 400; ++i) {
      ml::Accumulate(state, 1, 0, 0.1, false);  // 1.2 units per count
      const ml::Command command = ml::Next(state, 2.576, 1.2, now, kHz);
      if (command.send) {
        check(TitleYaw(2.576f, command.argX) == command.yawUnits, "command units exact");
        yawSent += command.yawUnits;
      }
      now += kInterval;
    }
    check(yawSent == 480, "400 counts at 1.2 units/count send 480 units in total");
    check(std::fabs(state.pendingYawUnits) < 1.0, "only a sub-unit remainder is left");
  }
  {
    // Command interval: at most one command per 1/240 s; a 144 Hz frame
    // (6.94 ms) always gets its command.
    ml::State state;
    ml::Accumulate(state, 10, 0, 1.0, false);
    const ml::Command first = ml::Next(state, 2.5, 1.2, 1000, kHz);
    ml::Accumulate(state, 10, 0, 1.0, false);
    const ml::Command early = ml::Next(state, 2.5, 1.2, 1000 + kHz / 480, kHz);
    const ml::Command later = ml::Next(state, 2.5, 1.2, 1000 + kInterval, kHz);
    check(first.send && !early.send && later.send && later.yawUnits == 120,
          "a command within 1/240 s is deferred and merged into the next");
    ml::State hfr;
    int sent = 0;
    for (int frame = 0; frame < 144; ++frame) {
      ml::Accumulate(hfr, 3, 0, 1.0, false);
      sent += ml::Next(hfr, 2.5, 1.2, int64_t(frame) * kHz / 144, kHz).send ? 1 : 0;
    }
    check(sent == 144, "every 144 Hz frame sends its own command");
  }
  {
    // Command ring room: half the ring (at least 8 entries) stays free for
    // the title's own commands; a full ring would drop every queued command.
    check(ml::CommandRingHasRoom(64, 40) && !ml::CommandRingHasRoom(64, 31) &&
              ml::CommandRingHasRoom(16, 8) && !ml::CommandRingHasRoom(16, 7) &&
              ml::CommandRingHasRoom(12, 8) && !ml::CommandRingHasRoom(12, 7) &&
              !ml::CommandRingHasRoom(0, 0) && !ml::CommandRingHasRoom(64, 64) &&
              !ml::CommandRingHasRoom(64, -1),
          "command ring keeps half its room (at least 8 entries)");
  }
  {
    // Large flicks: each command fits int16, the rest follows.
    ml::State state;
    ml::Accumulate(state, 7500, 0, 1.0, false);  // 90000 units
    int64_t now = 0;
    int32_t total = 0;
    int commands = 0;
    for (int i = 0; i < 10; ++i) {
      const ml::Command command = ml::Next(state, 2.5, 1.2, now, kHz);
      if (command.send) {
        check(std::abs(command.yawUnits) <= 30000, "command within int16 margin");
        total += command.yawUnits;
        ++commands;
      }
      now += kInterval;
    }
    check(total == 90000 && commands == 3, "a 90000-unit flick takes three commands");
  }
  {
    // Signs: mouse right = positive yaw (like the stick), mouse down = negative
    // pitch unless inverted; the title's controller inversion (negative sensY)
    // does not change the mouse's direction.
    ml::State normal, inverted;
    ml::Accumulate(normal, 4, 4, 1.0, false);
    ml::Accumulate(inverted, 4, 4, 1.0, true);
    const ml::Command a = ml::Next(normal, 2.5, 1.2, 0, kHz);
    const ml::Command b = ml::Next(inverted, 2.5, 1.2, 0, kHz);
    check(a.yawUnits == 48 && a.pitchUnits == -48 && b.pitchUnits == 48,
          "mouse right turns right, mouse down looks down, inversion flips pitch");
    ml::State titleInverted;
    ml::Accumulate(titleInverted, 0, 4, 1.0, false);
    const ml::Command c = ml::Next(titleInverted, 2.5, -1.2, 0, kHz);
    check(c.pitchUnits == -48 && TitlePitch(-1.2f, c.argY) == -48,
          "title Y inversion does not invert the mouse");
    // Yaw and pitch get the same units per count despite the title's ratio.
    check(std::abs(a.yawUnits) == std::abs(a.pitchUnits), "equal units per count on both axes");
  }
  {
    // Unusable scales or sensitivity send nothing; Reset drops pending units.
    ml::State state;
    ml::Accumulate(state, 5, 5, 0.0, false);
    check(!ml::Next(state, 2.5, 1.2, 0, kHz).send, "zero sensitivity accumulates nothing");
    ml::Accumulate(state, 5, 5, 1.0, false);
    check(!ml::Next(state, 0.0, 1.2, 0, kHz).send, "unusable title scale sends nothing");
    ml::Reset(state);
    check(state.pendingYawUnits == 0.0 && state.pendingPitchUnits == 0.0, "reset");
  }
  {
    // Hook policy (source): the client frame update runs first, then look();
    // volatile return registers are restored; an environment switch exists.
    std::ifstream file(std::string(DARKNESS_SOURCE_ROOT) + "/runtime/runtime_mouse_look.cpp");
    const std::string source((std::istreambuf_iterator<char>(file)), {});
    const auto hook = source.find("PPC_FUNC(sub_823F8AF0) {");
    const auto original = source.find("__imp__sub_823F8AF0(ctx, base);", hook);
    const auto after = source.find("AfterClientFrame(ctx, base, client);", hook);
    check(hook != std::string::npos && original < after, "original frame update runs first");
    const auto call = source.find("sub_823FCF68(ctx, base);");
    const auto restore = source.find("ctx.r3 = savedR3;", call);
    check(call != std::string::npos && restore != std::string::npos,
          "look() call restores the caller's return registers");
    check(source.find("DARKNESS_NATIVE_MOUSE_LOOK") != std::string::npos,
          "environment switch for comparisons");
    // Gates: the title's own routing (console mode, debug overlay, modal
    // window) is read before any command, and gated motion is dropped.
    const auto gate = source.find("const Gate gate = ReadGate(base);");
    const auto closed = source.find("if (!gate.open) {", gate);
    const auto drop = source.find("DropPending(\"gate\");", closed);
    const auto next = source.find("runtime_mouse_look::Next(", gate);
    check(gate != std::string::npos && closed != std::string::npos && drop < next &&
              closed < next,
          "gates are checked before a command and gated motion is dropped");
    check(source.find("gate.bindMode != 1 && !gate.overlay && !(gate.window && gate.modal)") !=
              std::string::npos,
          "console, debug overlay and modal windows close the gate");
    check(source.find("PPC_FUNC(sub_820E29D8) {") != std::string::npos &&
              source.find("__imp__sub_820E29D8(ctx, base);") != std::string::npos,
          "the input processor runs unchanged after recording the main game object");
    const auto ring = source.find("const CommandRing ring = ReadCommandRing(base, client);");
    const auto room = source.find("runtime_mouse_look::CommandRingHasRoom(ring.capacity, ring.free)",
                                  ring);
    check(ring != std::string::npos && room != std::string::npos && room < next,
          "command ring room is checked before every command");
  }

  if (!passed) return 1;
  std::cout << "runtime mouse look policy: PASS\n";
  return 0;
}
