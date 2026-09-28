#include <rex/input/mnk/test_tap_policy.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

using rex::input::mnk::test_tap_policy::BoundedTaps;
using rex::input::mnk::test_tap_policy::CameraAxis;
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
int main() {
  try {
    BoundedTaps taps;
    taps.KeyDown(13, 0, 1000);
    Require(!taps.Active(13, 1000), "disabled option retained a tap");
    taps.KeyDown(13, 150, 1000);
    Require(taps.Active(13, 1000) && taps.Active(13, 1149), "short host tap lost before bound");
    Require(!taps.Active(13, 1150) && !taps.Active(13, 5000), "tap did not expire without polling");
    Require(!taps.Active(32, 1050), "tap leaked to another key");
    taps.KeyDown(32, 250, 6000);
    taps.KeyDown(13, 150, 6010);
    taps.Reset();
    Require(!taps.Active(13, 6011) && !taps.Active(32, 6011), "focus loss retained input");
    taps.KeyDown(0x1B, 250, 7000);
    taps.KeyDown(256, 150, 7000);
    taps.KeyDown(13, 251, 7000);
    Require(!taps.Active(0x1B, 7000) && !taps.Active(256, 7000) && !taps.Active(13, 7000),
            "Escape, invalid key or excessive hold was retained");
    taps.KeyDown(13, 250, (std::numeric_limits<uint64_t>::max)() - 5);
    Require(!taps.Active(13, 0), "overflow created retained input");
    Require(CameraAxis(false, -32768, false, true) == -32768,
            "disabled test camera changed normal mouse input");
    Require(CameraAxis(true, 987, false, false) == 987 &&
            CameraAxis(true, -987, true, true) == -987,
            "neutral/opposing camera keys changed the mouse result");
    taps.Reset();
    taps.KeyDown('J', 150, 8000);
    Require(CameraAxis(true, 0, taps.Active('J', 8001), taps.Active('L', 8001)) == -32767,
            "actual bounded key did not select ordinary left look axis");
    Require(CameraAxis(true, 0, taps.Active('J', 8150), false) == 0,
            "camera axis continued after key expiry");
    taps.KeyDown('I', 150, 9000);
    Require(CameraAxis(true, 0, false, taps.Active('I', 9001)) == 32767,
            "up camera key sign incorrect");
    taps.Reset();
    Require(CameraAxis(true, 0, false, taps.Active('I', 9002)) == 0,
            "camera axis survived focus reset");
    std::ifstream file(std::string(REXGLUE_SOURCE_ROOT) + "/src/input/mnk/mnk_input_driver.cpp");
    const std::string source{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    Require(source.find("input_test_tap_hold_ms, 0,") != std::string::npos, "test input became enabled by default");
    Require(source.find("input_test_camera_keys, false,") != std::string::npos &&
            source.find("test_taps_enabled && REXCVAR_GET(input_test_camera_keys)") != std::string::npos,
            "test camera lost explicit default-off and bounded-tap gates");
    Require(source.find("test_taps_enabled ? TestTapClockMs() : 0") != std::string::npos,
            "normal polls acquired test-only clock work");
    const auto lost = source.find("void MnkInputDriver::OnLostFocus");
    const auto got = source.find("void MnkInputDriver::OnGotFocus");
    Require(source.find("test_taps_.Reset();", lost) < got, "focus loss wiring lost reset");
    Require(source.find("test_taps_.KeyDown(vk, hold_ms, TestTapClockMs())") != std::string::npos,
            "production key handler stopped using tested helper");
    Require(source.find("REX_INPUT_TEST_TAP source=window_keydown") != std::string::npos,
            "opt-in input lost source labeling");
    std::cout << "Bounded test host tap retention/default-off/expiry/focus/Escape PASS\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
