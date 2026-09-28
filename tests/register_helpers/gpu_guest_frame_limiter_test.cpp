// V285 guest frame-production pacing policy (rex::ui::GuestFrameDeadline and
// GuestFrameSpinMargin): exact average rate on a fixed grid, first frame never
// waits, late frames keep the grid within one period, and no catch-up bursts
// after a long stall.
#include <rex/ui/guest_frame_limiter.h>

#include <cstdint>
#include <iostream>
#include <vector>

int main() {
  bool passed = true;
  const auto check = [&passed](bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      passed = false;
    }
  };

  using rex::ui::GuestFrameDeadline;
  constexpr uint64_t kPeriod = 1000;

  {
    GuestFrameDeadline deadline;
    check(deadline.Begin(5000, kPeriod) == 5000, "first frame after arming must not wait");
    // Fast producer: every frame waits exactly to the grid.
    uint64_t now = 5100;
    for (int frame = 1; frame <= 100; ++frame) {
      const uint64_t wait_until = deadline.Begin(now, kPeriod);
      check(wait_until == 5000 + uint64_t(frame) * kPeriod,
            "a fast producer must be released exactly on the fixed grid");
      now = wait_until + 100;  // 100 ticks of work after release
    }
    check(deadline.grid_resets() == 0, "a fast producer never resets the grid");
  }

  {
    GuestFrameDeadline deadline;
    deadline.Begin(0, kPeriod);                 // next = 1000
    // Late by 300 ticks (< one period): grid kept, next slot is shorter.
    check(deadline.Begin(1300, kPeriod) == 1000, "late frame reports its missed deadline");
    check(deadline.next_deadline() == 2000, "late by less than a period keeps the grid");
    // Late by 2500 ticks (> one period): restart from now, no burst.
    const uint64_t stalled_now = 4500;
    check(deadline.Begin(stalled_now, kPeriod) == 2000, "stalled frame reports its deadline");
    check(deadline.next_deadline() == stalled_now + kPeriod,
          "behind by more than one period must restart the grid (no catch-up burst)");
    check(deadline.grid_resets() == 1, "the restart is counted");
    // After the restart the producer is released on the new grid only.
    check(deadline.Begin(4600, kPeriod) == 5500, "post-stall frames follow the new grid");
  }

  {
    GuestFrameDeadline deadline;
    deadline.Begin(0, kPeriod);
    check(deadline.Begin(10, 0) == 10 && !deadline.armed(), "zero period disables pacing");
    check(deadline.Begin(20, kPeriod) == 20, "re-arming never waits on its first frame");
    check(deadline.Begin(30, 2 * kPeriod) == 30, "changing the rate re-arms without waiting");
    check(deadline.next_deadline() == 30 + 2 * kPeriod, "new rate starts a new grid");
  }

  {
    // Average rate over a long run with alternating work is exact.
    GuestFrameDeadline deadline;
    uint64_t now = 0;
    uint64_t first_release = 0, last_release = 0;
    for (int frame = 0; frame < 1000; ++frame) {
      const uint64_t wait_until = deadline.Begin(now, kPeriod);
      const uint64_t release = std::max(now, wait_until);
      if (frame == 0) first_release = release;
      last_release = release;
      now = release + ((frame & 1) ? 900 : 300);  // work shorter than a period
    }
    check(last_release - first_release == 999 * kPeriod,
          "average production rate must be exactly the target");
  }

  {
    using rex::ui::GuestFrameSpinMargin;
    GuestFrameSpinMargin margin(50, 1500, 400);
    margin.Observe(900, 50);
    check(margin.margin() == 950, "margin rises immediately to observed lateness + headroom");
    for (int i = 0; i < 200; ++i) margin.Observe(0, 50);
    check(margin.margin() >= 50 && margin.margin() < 100, "margin decays slowly toward the floor");
    margin.Observe(5000, 50);
    check(margin.margin() == 1500, "margin is bounded above");
  }

  if (passed) std::cout << "Guest frame-limiter policy tests passed\n";
  return passed ? 0 : 1;
}
