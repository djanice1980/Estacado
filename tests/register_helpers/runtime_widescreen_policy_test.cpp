// Widescreen guest mode sizes (runtime/runtime_widescreen_policy.h).
#include "runtime_widescreen_policy.h"

#include <iostream>
#include <string>

namespace {
bool Check(bool condition, const std::string& message) {
  if (!condition) std::cerr << "FAIL: " << message << '\n';
  return condition;
}
bool Is(RuntimeGuestDisplaySize size, uint32_t width, uint32_t height) {
  return size.width == width && size.height == height;
}
}  // namespace

int main() {
  bool ok = true;
  ok &= Check(Is(RuntimeGuestDisplaySizeForOutput(2560, 1440), 1280, 720) &&
                  !RuntimeGuestDisplaySizeForOutput(1920, 1080).Widescreen() &&
                  !RuntimeGuestDisplaySizeForOutput(1920, 1079).Widescreen(),
              "16:9 screens keep 1280 x 720");
  ok &= Check(Is(RuntimeGuestDisplaySizeForOutput(2520, 1080), 1680, 720), "21:9 window");
  ok &= Check(Is(RuntimeGuestDisplaySizeForOutput(3440, 1440), 1728, 720), "3440 x 1440");
  ok &= Check(Is(RuntimeGuestDisplaySizeForOutput(2560, 1080), 1712, 720), "2560 x 1080");
  ok &= Check(Is(RuntimeGuestDisplaySizeForOutput(5120, 1440), 2560, 720), "32:9");
  ok &= Check(Is(RuntimeGuestDisplaySizeForOutput(1920, 1200), 1280, 800), "16:10");
  ok &= Check(Is(RuntimeGuestDisplaySizeForOutput(1024, 768), 1280, 960), "4:3");
  ok &= Check(Is(RuntimeGuestDisplaySizeForOutput(0, 1080), 1280, 720), "unknown output");
  ok &= Check(RuntimeGuestDisplaySizeForOutput(20000, 1000).width == 3840, "very wide clamps");
  for (uint32_t w : {2520u, 3440u, 2560u, 5120u, 1920u, 1024u}) {
    const auto size = RuntimeGuestDisplaySizeForOutput(w, w == 1920 ? 1200 : 1440);
    ok &= Check(size.width % 16 == 0 && size.height % 16 == 0, "multiples of 16");
  }
  // Internal-scale class boundary: half the guest width in 80-pixel tiles.
  ok &= Check(RuntimeWidescreenScaleThreshold(1280, 640) == 640 &&
                  RuntimeWidescreenScaleThreshold(1280, 0) == 0,
              "1280 wide keeps the configured threshold");
  ok &= Check(RuntimeWidescreenScaleThreshold(1680, 640) == 880, "21:9 half-width pitch 880");
  ok &= Check(RuntimeWidescreenScaleThreshold(1712, 640) == 880, "2560 x 1080");
  ok &= Check(RuntimeWidescreenScaleThreshold(2560, 640) == 1280, "32:9");
  ok &= Check(RuntimeWidescreenScaleThreshold(3840, 640) == 1920, "48:9");
  ok &= Check(RuntimeWidescreenScaleThreshold(1680, 0) == 0 &&
                  RuntimeWidescreenScaleThreshold(1680, 512) == 512,
              "disabled or custom thresholds are kept");
  for (uint32_t w : {1296u, 1680u, 1712u, 1728u, 2560u, 3840u}) {
    const uint32_t t = RuntimeWidescreenScaleThreshold(w, 640);
    ok &= Check(t % 80 == 0 && t >= w / 2 && t < w, "half-width buffers native, full width scaled");
  }
  if (ok) std::cout << "runtime_widescreen_policy: ok\n";
  return ok ? 0 : 1;
}
