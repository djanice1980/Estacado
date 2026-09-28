// V288 player-facing frame-rate modes (rex/ui/frame_rate_policy.h): targets
// that divide the refresh are display-paced with VSync (even, no limiter);
// other targets use the guest limiter and are labeled uneven without VRR;
// immediate/VRR presentation always uses the limiter; original/uncapped never
// cap.
#include <rex/ui/frame_rate_policy.h>

#include <iostream>

int main() {
  using namespace rex::ui;
  bool passed = true;
  const auto check = [&passed](bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      passed = false;
    }
  };

  FrameRateMode mode{};
  check(ParseFrameRateMode("original", mode) && mode == FrameRateMode::kOriginal, "parse original");
  check(ParseFrameRateMode("60", mode) && mode == FrameRateMode::k60, "parse 60");
  check(ParseFrameRateMode("half_refresh", mode) && mode == FrameRateMode::kHalfRefresh,
        "parse half_refresh");
  check(ParseFrameRateMode("refresh", mode) && mode == FrameRateMode::kRefresh, "parse refresh");
  check(ParseFrameRateMode("custom", mode) && mode == FrameRateMode::kCustom, "parse custom");
  check(ParseFrameRateMode("uncapped", mode) && mode == FrameRateMode::kUncapped, "parse uncapped");
  check(!ParseFrameRateMode("144", mode), "unknown values are rejected");
  check(!FrameRateModeUsesHostPacing(FrameRateMode::kOriginal) &&
            FrameRateModeUsesHostPacing(FrameRateMode::k60),
        "only original keeps the title's own pacing");

  auto p = ResolveFrameRatePolicy(FrameRateMode::kOriginal, 0, true, 144);
  check(!p.limit_fps && !p.display_paced && p.target_fps == 0, "original never caps");
  p = ResolveFrameRatePolicy(FrameRateMode::kUncapped, 0, false, 144);
  check(!p.limit_fps && p.target_fps == 0, "uncapped never caps");

  p = ResolveFrameRatePolicy(FrameRateMode::k60, 0, true, 60);
  check(p.display_paced && p.vsync_interval == 1 && !p.limit_fps, "60 on 60 Hz is display-paced");
  p = ResolveFrameRatePolicy(FrameRateMode::k60, 0, true, 59);  // 59.94 Hz reported as 59
  check(p.display_paced && p.vsync_interval == 1, "fractional 59.94 Hz still divides 60");
  p = ResolveFrameRatePolicy(FrameRateMode::k60, 0, true, 120);
  check(p.display_paced && p.vsync_interval == 2, "60 on 120 Hz uses every second refresh");
  p = ResolveFrameRatePolicy(FrameRateMode::k60, 0, true, 144);
  check(!p.display_paced && p.uneven_without_vrr && p.limit_fps == 60,
        "60 on 144 Hz is limiter-paced and labeled uneven");

  p = ResolveFrameRatePolicy(FrameRateMode::kRefresh, 0, true, 144);
  check(p.display_paced && p.vsync_interval == 1 && p.target_fps == 144, "refresh on 144 Hz");
  p = ResolveFrameRatePolicy(FrameRateMode::kHalfRefresh, 0, true, 144);
  check(p.display_paced && p.vsync_interval == 2 && p.target_fps == 72, "half refresh on 144 Hz");
  p = ResolveFrameRatePolicy(FrameRateMode::kHalfRefresh, 0, true, 165);
  check(p.display_paced && p.vsync_interval == 2, "half refresh on 165 Hz (82.5)");
  p = ResolveFrameRatePolicy(FrameRateMode::kHalfRefresh, 0, true, 240);
  check(p.display_paced && p.vsync_interval == 2 && p.target_fps == 120, "half refresh on 240 Hz");

  p = ResolveFrameRatePolicy(FrameRateMode::kCustom, 120, true, 144);
  check(!p.display_paced && p.uneven_without_vrr && p.limit_fps == 120,
        "120 on 144 Hz with VSync is uneven and limiter-paced");
  p = ResolveFrameRatePolicy(FrameRateMode::kCustom, 120, true, 240);
  check(p.display_paced && p.vsync_interval == 2, "120 on 240 Hz divides evenly");
  p = ResolveFrameRatePolicy(FrameRateMode::kCustom, 48, true, 144);
  check(p.display_paced && p.vsync_interval == 3, "48 on 144 Hz uses every third refresh");
  p = ResolveFrameRatePolicy(FrameRateMode::kCustom, 30, true, 144);
  check(!p.display_paced && p.limit_fps == 30, "intervals above 4 fall back to the limiter");
  p = ResolveFrameRatePolicy(FrameRateMode::kCustom, 0, true, 144);
  check(!p.limit_fps && p.target_fps == 0, "custom without a cap is uncapped");

  p = ResolveFrameRatePolicy(FrameRateMode::kCustom, 120, false, 144);
  check(!p.display_paced && !p.uneven_without_vrr && p.limit_fps == 120,
        "immediate/VRR presentation always uses the limiter");
  p = ResolveFrameRatePolicy(FrameRateMode::kRefresh, 0, true, 0);
  check(p.limit_fps == 60 && !p.display_paced, "unknown refresh falls back to a 60 FPS limiter");

  // V330 plain numbers: whole-number caps are custom limits in the value.
  uint32_t limit = 0;
  check(ParseFrameRate("120", mode, limit) && mode == FrameRateMode::kCustom && limit == 120,
        "a whole number is a custom cap");
  check(ParseFrameRate("60", mode, limit) && mode == FrameRateMode::k60, "60 keeps its mode");
  check(ParseFrameRate("refresh", mode, limit) && mode == FrameRateMode::kRefresh,
        "symbolic values still parse");
  for (const char* bad : {"19", "1001", "0120", "12a", "", "-60", "60.5"}) {
    check(!IsFrameRateValue(bad), "caps outside 20..1000 or not plain digits are rejected");
  }

  // Player list for a 144 Hz display: 30 (Original), 60, 72, 120, 144, Uncapped.
  auto options = FrameRateOptionsForDisplay(144.0);
  check(options.size() == 6 && options[0].value == "original" && options[1].value == "60" &&
            options[2].value == "half_refresh" && options[3].value == "120" &&
            options[4].value == "refresh" && options[5].value == "uncapped",
        "144 Hz list is 30, 60, 72, 120, 144, Uncapped");
  check(options.size() == 6 && options[4].recommended && !options[3].recommended &&
            !options[2].recommended,
        "144 Hz recommends 144");
  check(options.size() == 6 && options[0].uneven && options[1].uneven && !options[2].uneven &&
            options[3].uneven && !options[4].uneven,
        "non-divisors of 144 are marked uneven");
  check(options.size() == 6 && FrameRateOptionLabel(options[4]) == "144 - Recommended" &&
            FrameRateOptionLabel(options[3]) == "120 - may stutter without G-SYNC/FreeSync" &&
            FrameRateOptionLabel(options[2]) == "72" &&
            FrameRateOptionLabel(options[0]) ==
                "30 (Original) - may stutter without G-SYNC/FreeSync",
        "labels say the number, the recommendation and the stutter hint");
  options = FrameRateOptionsForDisplay(240.0);
  bool recommended120 = false;
  for (const auto& option : options) {
    if (option.recommended) recommended120 = option.fps == 120.0 && option.value == "half_refresh";
  }
  check(recommended120, "240 Hz recommends 120 (half the refresh)");
  options = FrameRateOptionsForDisplay(60.0);
  check(options.size() == 3 && options[0].value == "original" && !options[0].uneven &&
            options[1].value == "refresh" && options[1].recommended &&
            options[2].value == "uncapped",
        "60 Hz list is 30, 60 (recommended), Uncapped");
  options = FrameRateOptionsForDisplay(0.0);
  check(options.size() == 5 && options[4].value == "uncapped", "unknown refresh keeps the modes");

  // Menus: a whole divisor of the refresh at or below 60 with VSync.
  const FrameRatePolicy gameplay144 = ResolveFrameRatePolicy(FrameRateMode::kRefresh, 0, true, 144);
  p = MenuFrameRatePolicy(gameplay144, true, 144.0);
  check(p.display_paced && p.vsync_interval == 3 && !p.limit_fps && p.target_fps == 48.0,
        "menus on 144 Hz run at 48 (every third refresh)");
  p = MenuFrameRatePolicy(ResolveFrameRatePolicy(FrameRateMode::kRefresh, 0, true, 60), true,
                          60.0);
  check(p.vsync_interval == 1 && p.target_fps == 60.0, "menus on 60 Hz stay at 60");
  p = MenuFrameRatePolicy(ResolveFrameRatePolicy(FrameRateMode::kRefresh, 0, true, 240), true,
                          240.0);
  check(p.vsync_interval == 4 && p.target_fps == 60.0, "menus on 240 Hz run at 60");
  p = MenuFrameRatePolicy(ResolveFrameRatePolicy(FrameRateMode::kUncapped, 0, false, 144), false,
                          144.0);
  check(p.limit_fps == 60, "menus without VSync are capped at 60");
  p = MenuFrameRatePolicy(ResolveFrameRatePolicy(FrameRateMode::kCustom, 40, false, 144), false,
                          144.0);
  check(p.limit_fps == 40, "menus never run faster than gameplay");

  if (passed) std::cout << "Frame-rate policy tests passed\n";
  return passed ? 0 : 1;
}
