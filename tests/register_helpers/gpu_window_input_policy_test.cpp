#include <rex/ui/window_input_policy.h>

#include <iostream>

namespace {
bool Check(bool condition, const char* message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}
}  // namespace

int main() {
  using rex::ui::VirtualKey;
  using rex::ui::window_input_policy::ShouldToggleFullscreen;
  bool passed = true;
  passed &= Check(ShouldToggleFullscreen(VirtualKey::kReturn, true, false, true),
                  "fresh Alt+Enter must toggle fullscreen");
  passed &= Check(!ShouldToggleFullscreen(VirtualKey::kReturn, true, true, true),
                  "key repeat must not repeatedly toggle fullscreen");
  passed &= Check(!ShouldToggleFullscreen(VirtualKey::kReturn, true, false, false),
                  "plain Enter must remain available to the application");
  passed &= Check(!ShouldToggleFullscreen(VirtualKey::kReturn, false, false, true),
                  "Alt+Enter key-up must not toggle fullscreen");
  passed &= Check(!ShouldToggleFullscreen(VirtualKey::kSpace, true, false, true),
                  "Alt with another key must not toggle fullscreen");
  if (passed) std::cout << "PC window input policy tests passed\n";
  return passed ? 0 : 1;
}
