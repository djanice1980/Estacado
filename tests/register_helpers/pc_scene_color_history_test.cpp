#include <rex/graphics/pc_scene_color_history.h>
#include <iostream>

using rex::graphics::pc_scene_color_history::State;
int main() {
  bool ok = true;
  using rex::graphics::pc_scene_color_history::SupportedSource;
  ok &= SupportedSource(26, 9, 0, false, 1280, 720);
  ok &= SupportedSource(26, 11, 0, false, 1280, 720);
  ok &= !SupportedSource(26, 10, 0, false, 1280, 720);
  ok &= !SupportedSource(32, 10, 0, false, 1280, 720);
  ok &= !SupportedSource(26, 9, 1, false, 1280, 720);
  ok &= !SupportedSource(26, 9, 0, true, 1280, 720);
  ok &= !SupportedSource(26, 9, 0, false, 2560, 1440);
  State s;
  auto copy = [&](uint64_t frame, uint64_t submitted, uint64_t completed) {
    const int slot = s.Reserve(frame, completed);
    if (slot >= 0) s.Copied(submitted);
    return s.Finish(frame);
  };
  ok &= !s.Use(1) && s.Reserve(0, 0) == -1;
  ok &= copy(1, 1, 0) && s.current == 0 && s.previous == -1;
  ok &= copy(2, 2, 0) && s.previous == 0 && s.slots[s.current].frame == 2;
  ok &= s.Use(9) && s.slots[0].last_submission == 9;
  ok &= copy(3, 3, 0) && copy(4, 4, 0);
  // No unbounded allocation or waiting when all other slots remain in flight.
  ok &= !copy(5, 5, 0) && s.current == -1 && s.previous == -1;
  ok &= s.slots[0].last_submission == 9;
  ok &= copy(6, 10, 9) && s.previous == -1;
  ok &= copy(7, 11, 9) && s.previous >= 0;
  // A duplicate source in one frame is ambiguous even with identical bytes.
  ok &= s.Reserve(8, 11) >= 0;
  s.Copied(12);
  ok &= s.Reserve(8, 11) == -1 && !s.Finish(8);
  ok &= copy(9, 13, 12) && s.previous == -1;
  // Repeated presentation / no selected draw clears adjacency.
  ok &= !s.Finish(10) && s.current == -1;
  ok &= copy(11, 14, 13) && s.previous == -1;
  ok &= copy(13, 15, 14) && s.previous == -1;
  // Explicit load/cut/format reset retains all outstanding queue use.
  s.Use(30);
  const auto slot = s.current;
  const auto epoch = s.epoch;
  s.Invalidate();
  ok &= s.epoch == epoch + 1 && s.slots[slot].last_submission == 30;
  ok &= copy(14, 31, 15) && s.previous == -1;
  ok &= s.Reserve(15, 31) >= 0;
  s.Fail(15);
  ok &= !s.Finish(15) && !s.Use(32);
  ok &= !s.Finish(15); // stale completion cannot resurrect an older pair
  State unsubmitted;
  ok &= unsubmitted.Reserve(1, 0) == 0 && !unsubmitted.Finish(1);
  State reused;
  reused.Reserve(1, 0); reused.Copied(1); reused.Finish(1);
  reused.Invalidate();
  ok &= reused.Reserve(2, 1) == 0 && !reused.Finish(2);
  if (!ok) std::cerr << "PC scene color history ownership failed\n";
  return ok ? 0 : 1;
}
