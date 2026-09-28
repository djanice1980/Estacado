// Developer test input script (input_test_script): parsing, timing, tap
// visibility, exact mouse spreading, stall catch-up, and yield/arm.
#include <rex/input/mnk/test_script_policy.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace tsp = rex::input::mnk::test_script_policy;

uint16_t Key(std::string_view name) {
  if (name == "Space") return 0x20;
  if (name == "W") return 0x57;
  if (name == "Return") return 0x0D;
  if (name == "LMB") return 0x01;
  return 0;
}

struct Harness {
  tsp::Runner runner;
  tsp::Runner::Keys keys{};
  int32_t dx = 0;
  int32_t dy = 0;
  std::vector<std::string> started;
  std::vector<std::string> errors;

  void Append(std::string_view text) { runner.Append(text, &Key, errors); }
  void Poll(uint64_t now) {
    runner.Advance(now, keys, dx, dy, [this](const tsp::Command& command) {
      started.push_back(std::string(tsp::OpName(command.op)) + ":" +
                        std::to_string(command.line));
    });
  }
};

}  // namespace

int main() {
  bool passed = true;
  const auto check = [&passed](bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      passed = false;
    }
  };

  {
    // Parsing: comments, blanks, CRLF, BOM, lines split across reads, errors.
    Harness h;
    h.Append("\xEF\xBB");
    h.Append("\xBF# route\r\ntap Space 150\r\nwait 20");
    h.Append("0\nmouse 100 -50 40 # sweep\n\nmark street arrival\nbogus 1\ntap Nope 10\n");
    h.Append("mouse 1\ndown W");
    check(h.errors.size() == 3, "three invalid lines are reported");
    Harness host;
    host.Append("press Return\nclick 640 360\nclick -1 5\npress Nope\n");
    host.Poll(0);
    check(host.errors.size() == 2 && host.started.size() == 2 &&
              host.started[0] == "press:1" && host.started[1] == "click:2" &&
              !host.keys[0x0D],
          "press/click are host events and never hold a game key");
    h.Poll(1000);
    check(h.started.size() == 1 && h.started[0] == "tap:2" && h.keys[0x20],
          "tap starts at the first poll after BOM/comment lines");
    check(h.runner.idle() == false, "commands remain queued");
  }
  {
    // Tap holds for its time and is released at a later poll than the press,
    // even when the poll comes much later.
    Harness h;
    h.Append("tap Space 100\n");
    h.Poll(1000);
    check(h.keys[0x20], "tap pressed");
    h.Poll(1050);
    check(h.keys[0x20], "tap still held before its time");
    h.Poll(1100);
    check(!h.keys[0x20], "tap released at its time");
    Harness late;
    late.Append("wait 10\ntap Space 10\n");
    late.Poll(0);
    late.Poll(9000);  // the tap is long overdue by this poll
    check(late.keys[0x20], "an overdue tap is still visible to one poll");
    late.Poll(9016);
    check(!late.keys[0x20], "and released at the next poll");
  }
  {
    // Waits chain without drift; after a stall commands resume at most 50 ms
    // late instead of bursting through the queue.
    Harness h;
    h.Append("wait 100\ntap W 100\nwait 100\ntap Space 100\n");
    h.Poll(0);
    h.Poll(99);
    check(!h.keys[0x57], "wait holds the next command");
    h.Poll(100);
    check(h.keys[0x57], "the next command starts exactly at the wait's end");
    h.Poll(200);
    check(!h.keys[0x57], "tap ends on schedule");
    h.Poll(10000);  // long stall inside the wait
    check(h.keys[0x20] && h.started.size() == 4, "stalled script does not skip commands");
    h.Poll(10000);
    const bool pressedAfterStall = h.keys[0x20];
    h.Poll(10040);
    check(pressedAfterStall && h.keys[0x20], "after a stall the tap keeps (most of) its hold time");
    h.Poll(10200);
    check(!h.keys[0x20] && h.runner.idle(), "script completes");
  }
  {
    // Mouse counts spread evenly and sum exactly.
    Harness h;
    h.Append("mouse 1001 -333 100\nmouse 7 3\n");
    int32_t total_x = 0;
    int32_t total_y = 0;
    for (uint64_t t = 0; t <= 120; t += 7) {
      h.dx = 0;
      h.dy = 0;
      h.Poll(1000 + t);
      total_x += h.dx;
      total_y += h.dy;
    }
    check(total_x == 1008 && total_y == -330, "spread counts sum exactly (plus an instant move)");
  }
  {
    // Yield: keys released, queue dropped, later lines ignored until `arm`.
    Harness h;
    h.Append("down W\nwait 1000\ntap Space 50\n");
    h.Poll(0);
    check(h.keys[0x57], "down holds");
    h.runner.YieldToLocalInput(h.keys);
    check(!h.keys[0x57] && h.runner.yielded(), "yield releases keys");
    h.Append("tap Space 50\n");
    h.Poll(100);
    check(!h.keys[0x20] && h.runner.idle(), "commands after a yield are ignored");
    h.Append("arm\ntap Return 50\n");
    h.Poll(200);
    check(!h.runner.yielded() && h.keys[0x0D], "arm resumes with the following commands");
  }
  {
    // Driver wiring (source): off by default, only while keyboard/mouse is
    // enabled, yields to local input, drives state without focus, and the
    // switch is cache-neutral.
    const std::string rex_root = REXGLUE_SOURCE_ROOT;
    std::ifstream file(rex_root + "/src/input/mnk/mnk_input_driver.cpp", std::ios::binary);
    const std::string source((std::istreambuf_iterator<char>(file)), {});
    check(source.find("REXCVAR_DEFINE_STRING(input_test_script, \"\",") != std::string::npos,
          "test script is off by default");
    check(source.find("if (IsEnabled() && !REXCVAR_GET(input_test_script).empty())") !=
              std::string::npos,
          "test script needs keyboard/mouse enabled");
    check(source.find("GetLastInputInfo(&info)") != std::string::npos &&
              source.find("test_script_.YieldToLocalInput(script_keys_);") != std::string::npos,
          "local input yields the script");
    check(source.find("if (!is_active() || (!has_focus_ && !scripted))") != std::string::npos,
          "scripted state does not need focus");
    check(source.find("stick_now_ms - mouse_stick_ms_ >= kMouseStickHoldMs") != std::string::npos &&
              source.find("mouse_stick = mouse_stick_;") != std::string::npos,
          "stick bridge holds one deflection for every pad poll in a frame");
    std::ifstream cache(std::string(DARKNESS_SOURCE_ROOT) + "/runtime/runtime_graphics_cache.cpp",
                        std::ios::binary);
    const std::string cache_source((std::istreambuf_iterator<char>(cache)), {});
    check(cache_source.find("\"REX_INPUT_TEST_SCRIPT\",") != std::string::npos,
          "test input does not change the graphics cache namespace");
  }

  {
    // Scripted thumbsticks: held until changed, jitter bounded per poll,
    // invalid forms rejected, a held stick keeps the script active, a yield
    // releases it.
    Harness h;
    h.Append("stick L 0 32767\nstick R -16000 100 0\nstick X 1 1\nstick L 40000 0\n"
             "stick L 1 1 5000\nstick L 5\n");
    check(h.errors.size() == 4, "four invalid stick lines are reported");
    h.Poll(1000);
    const auto& s = h.runner.sticks();
    check(s[0] == 0 && s[1] == 32767 && s[2] == -16000 && s[3] == 100,
          "stick commands set left and right deflections");
    check(h.started.size() == 2 && h.started[0] == "stick:1" && h.started[1] == "stick:2",
          "stick commands start in order");
    check(!h.runner.idle(), "a held stick keeps the script active");
    h.Poll(1010);
    check(h.runner.sticks()[1] == 32767, "a stick holds between polls");
    h.Append("stick L 1000 -2000 300\n");
    bool varied = false;
    bool bounded = true;
    int16_t first = 0;
    for (int poll = 0; poll < 50; ++poll) {
      h.Poll(1020 + poll);
      const auto& j = h.runner.sticks();
      if (poll == 0) first = j[0];
      varied = varied || j[0] != first;
      bounded = bounded && j[0] >= 700 && j[0] <= 1300 && j[1] >= -2300 && j[1] <= -1700;
    }
    check(varied && bounded, "jitter changes every poll within its bound");
    h.Append("stick L 0 0\nstick R 0 0\n");
    h.Poll(2000);
    check(h.runner.sticks()[0] == 0 && h.runner.sticks()[3] == 0 && h.runner.idle(),
          "0 0 releases a stick");
    h.Append("stick R 20000 0\n");
    h.Poll(2010);
    h.runner.YieldToLocalInput(h.keys);
    check(h.runner.sticks()[2] == 0 && h.runner.yielded(), "a yield releases scripted sticks");
  }

  if (!passed) return 1;
  std::cout << "MnK test script policy: PASS\n";
  return 0;
}
