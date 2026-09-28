#include <rex/input/mnk/mouse_motion_policy.h>

#include <iostream>

int main() {
  using rex::input::mnk::mouse_motion_policy::AccelerateEventDelta;
  using rex::input::mnk::mouse_motion_policy::AccumulateEventDelta;
  using rex::input::mnk::mouse_motion_policy::TwoSampleSmoother;
  using rex::input::mnk::mouse_motion_policy::Translate;
  bool passed = true;
  auto check = [&](bool condition, const char* message) {
    if (!condition) {
      std::cerr << message << '\n';
      passed = false;
    }
  };

  const auto normal = Translate(2, 3, 1.0, false);
  check(normal.x == 400 && normal.y == -600,
        "linear relative-mouse transform changed");
  const auto inverted = Translate(2, 3, 1.0, true);
  check(inverted.x == 400 && inverted.y == 600,
        "optional Y inversion did not preserve magnitude");
  const auto saturated = Translate(INT32_MAX, INT32_MIN, 10.0, false);
  check(saturated.x == INT16_MAX && saturated.y == INT16_MAX,
        "large physical deltas did not clamp safely");
  const auto invalid = Translate(1, 1,
                                 std::numeric_limits<double>::infinity(),
                                 false);
  check(invalid.x == 0 && invalid.y == 0,
        "non-finite sensitivity escaped the transform");
  check(AccelerateEventDelta(17, 0.0) == 17 &&
            AccelerateEventDelta(-17, 0.0) == -17,
        "default-off mouse acceleration must be exact identity");
  check(AccelerateEventDelta(16, 1.0) == 24 &&
            AccelerateEventDelta(-32, 1.0) == -64 &&
            AccelerateEventDelta(64, 1.0) == 128,
        "bounded per-event mouse acceleration curve changed");
  check(AccumulateEventDelta(INT32_MAX - 2, 32, 1.0) == INT32_MAX &&
            AccumulateEventDelta(INT32_MIN + 2, -32, 1.0) == INT32_MIN,
        "accelerated mouse-event accumulation did not saturate safely");
  check(AccelerateEventDelta(5,
                             std::numeric_limits<double>::infinity()) == 0,
        "non-finite mouse acceleration escaped the event transform");

  TwoSampleSmoother smoother;
  check(smoother.Apply(17, 0.0) == 17 &&
            smoother.Apply(-9, 0.0) == -9,
        "default-off mouse smoothing must be exact identity");
  check(smoother.Apply(10, 1.0) == 5 &&
            smoother.Apply(0, 1.0) == 5 &&
            smoother.Apply(0, 1.0) == 0,
        "full two-sample smoothing must conserve an impulse with one tail poll");
  smoother.Reset();
  check(smoother.Apply(10, 0.5) == 8 &&
            smoother.Apply(0, 0.5) == 2,
        "partial mouse smoothing blend or residual changed");
  smoother.Reset();
  (void)smoother.Apply(20, 1.0);
  smoother.Reset();
  check(smoother.Apply(0, 1.0) == 0,
        "mouse smoothing reset replayed stale motion");
  check(smoother.Apply(5, std::numeric_limits<double>::infinity()) == 0 &&
            smoother.Apply(0, 1.0) == 0,
        "invalid mouse smoothing escaped or retained history");

  if (passed) std::cout << "Keyboard/mouse motion policy passed\n";
  return passed ? 0 : 1;
}
