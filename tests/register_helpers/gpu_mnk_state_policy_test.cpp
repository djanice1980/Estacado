#include <rex/input/mnk/mnk_state_policy.h>

#include <iostream>

int main() {
  using rex::input::mnk::state_policy::ControllerState;
  using rex::input::mnk::state_policy::Publish;
  bool passed = true;
  const auto check = [&passed](bool condition, const char* message) {
    if (!condition) {
      std::cerr << message << '\n';
      passed = false;
    }
  };

  ControllerState published{};
  bool has_published = false;
  uint32_t packet = 0;
  check(Publish({}, published, has_published, packet) && packet == 1,
        "initial physical bridge state was not published");
  check(!Publish({}, published, has_published, packet) && packet == 1,
        "unchanged state advanced the XInput packet");

  ControllerState pressed{};
  pressed.buttons = 0x1000;
  pressed.thumb_rx = 12000;
  check(Publish(pressed, published, has_published, packet) && packet == 2,
        "physical press/mouse motion did not advance the packet");
  check(Publish({}, published, has_published, packet) && packet == 3,
        "release/focus-loss neutral state did not advance the packet");
  check(!Publish({}, published, has_published, packet) && packet == 3,
        "neutral state generated repeated release edges");

  if (passed) std::cout << "Keyboard/mouse XInput packet policy passed\n";
  return passed ? 0 : 1;
}
