#include <rex/ui/guest_aspect_policy.h>

#include <cstdint>
#include <iostream>

namespace {

bool Expect(uint32_t surface_width, uint32_t surface_height, int32_t expected_x,
            int32_t expected_y, uint32_t expected_width, uint32_t expected_height) {
  const auto actual = rex::ui::guest_aspect_policy::ResolveContained(
      surface_width, surface_height, 16, 9);
  if (actual.x == expected_x && actual.y == expected_y &&
      actual.width == expected_width && actual.height == expected_height) {
    return true;
  }
  std::cerr << surface_width << 'x' << surface_height << " produced "
            << actual.x << ',' << actual.y << ' ' << actual.width << 'x'
            << actual.height << ", expected " << expected_x << ','
            << expected_y << ' ' << expected_width << 'x' << expected_height
            << '\n';
  return false;
}

}  // namespace

int main() {
  bool ok = true;
  ok &= Expect(1280, 720, 0, 0, 1280, 720);
  ok &= Expect(1920, 1080, 0, 0, 1920, 1080);
  ok &= Expect(1280, 800, 0, 40, 1280, 720);      // 16:10.
  ok &= Expect(3440, 1440, 440, 0, 2560, 1440);  // 21:9.
  ok &= Expect(5120, 1440, 1280, 0, 2560, 1440); // 32:9.
  ok &= Expect(1024, 768, 0, 96, 1024, 576);     // 4:3.

  const auto invalid = rex::ui::guest_aspect_policy::ResolveContained(0, 720, 16, 9);
  ok &= invalid.width == 0 && invalid.height == 0;
  return ok ? 0 : 1;
}
