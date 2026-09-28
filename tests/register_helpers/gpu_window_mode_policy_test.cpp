#include <rex/ui/window_mode_policy.h>

#include <iostream>

namespace {
bool Check(bool condition, const char* message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}
}  // namespace

int main() {
  using rex::ui::window_mode_policy::ShouldStartBorderless;
  bool passed = true;
  passed &= Check(ShouldStartBorderless("borderless", false),
                  "explicit borderless mode must override the legacy flag");
  passed &= Check(!ShouldStartBorderless("windowed", true),
                  "explicit windowed mode must override the legacy flag");
  passed &= Check(ShouldStartBorderless("auto", true),
                  "auto mode must preserve legacy fullscreen=true");
  passed &= Check(!ShouldStartBorderless("auto", false),
                  "auto mode must preserve legacy fullscreen=false");
  passed &= Check(!ShouldStartBorderless("unknown", false),
                  "unknown mode must not force borderless independently");
  if (passed) std::cout << "PC startup window-mode policy tests passed\n";
  return passed ? 0 : 1;
}

