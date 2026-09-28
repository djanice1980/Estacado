#include <rex/input/mnk/mouse_wheel_policy.h>

#include <iostream>

int main() {
  using rex::input::mnk::mouse_wheel_policy::Direction;
  using rex::input::mnk::mouse_wheel_policy::PulseQueue;
  bool passed = true;
  auto check = [&](bool condition, const char* message) {
    if (!condition) {
      std::cerr << message << '\n';
      passed = false;
    }
  };

  PulseQueue queue;
  queue.PushScrollY(0);
  check(queue.Pop() == Direction::kNone, "zero scroll invented a pulse");
  queue.PushScrollY(120);
  queue.PushScrollY(-120);
  check(queue.Pop() == Direction::kUp, "up detent lost or reordered");
  check(queue.Pop() == Direction::kDown, "down detent lost or reordered");
  check(queue.Pop() == Direction::kNone, "consumed detent stayed held");

  queue.PushScrollY(360);
  check(queue.size() == 3, "multi-detent scroll count changed");
  check(queue.Pop() == Direction::kUp, "first multi-detent pulse changed");
  check(queue.Pop() == Direction::kUp, "second multi-detent pulse changed");
  check(queue.Pop() == Direction::kUp, "third multi-detent pulse changed");

  for (size_t i = 0; i < PulseQueue::kCapacity + 8; ++i) {
    queue.PushScrollY(-120);
  }
  check(queue.size() == PulseQueue::kCapacity,
        "physical wheel queue exceeded its bound");
  queue.Clear();
  check(queue.Pop() == Direction::kNone, "focus-loss clear retained input");

  if (passed) std::cout << "Keyboard/mouse wheel pulse policy passed\n";
  return passed ? 0 : 1;
}
