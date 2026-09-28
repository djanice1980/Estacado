#include <rex/input/mnk/mouse_button_policy.h>

#include <iostream>

int main() {
  using rex::input::mnk::mouse_button_policy::ToVirtualKey;
  using Button = rex::ui::MouseEvent::Button;
  using Key = rex::ui::VirtualKey;
  bool passed = true;
  const auto check = [&passed](bool condition, const char* message) {
    if (!condition) {
      std::cerr << message << '\n';
      passed = false;
    }
  };

  check(ToVirtualKey(Button::kLeft) == Key::kLButton, "left mouse mapping changed");
  check(ToVirtualKey(Button::kRight) == Key::kRButton, "right mouse mapping changed");
  check(ToVirtualKey(Button::kMiddle) == Key::kMButton, "middle mouse mapping changed");
  check(ToVirtualKey(Button::kX1) == Key::kXButton1, "Mouse 4 is not bindable");
  check(ToVirtualKey(Button::kX2) == Key::kXButton2, "Mouse 5 is not bindable");
  check(ToVirtualKey(Button::kNone) == Key::kNone, "unknown mouse button was invented");

  if (passed) std::cout << "Keyboard/mouse physical-button mapping passed\n";
  return passed ? 0 : 1;
}
