#include <rex/ui/host_frame_limiter.h>

#include <chrono>
#include <iostream>

int main() {
  using Limiter = rex::ui::HostFrameLimiter;
  using namespace std::chrono;
  bool passed = true;
  const auto check = [&passed](bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      passed = false;
    }
  };

  check(Limiter::TargetInterval(0) == Limiter::Clock::duration::zero(),
        "zero must disable software pacing");
  check(duration_cast<microseconds>(Limiter::TargetInterval(30)).count() ==
            33333,
        "30 FPS must resolve to the expected host interval");
  check(duration_cast<microseconds>(Limiter::TargetInterval(60)).count() ==
            16666,
        "60 FPS must resolve to the expected host interval");
  check(duration_cast<microseconds>(Limiter::TargetInterval(120)).count() ==
            8333,
        "120 FPS must resolve to the expected host interval");

  if (passed) std::cout << "Host frame-limiter policy tests passed\n";
  return passed ? 0 : 1;
}
