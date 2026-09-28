#include <cstdint>
#include <iostream>
#include <rex/graphics/embedded_target_writer_capture_policy.h>

namespace p = rex::graphics::embedded_target_writer_capture_policy;

int main() {
  const p::Config config{0x14010500, 0x000C0300, 0x01800500, 1024, 1, 256, 8};
  const p::Draw first{2841, 1054, true, true, config.surface, config.color,
                      0xF, 0, 0, config.scissor_br};
  const auto fail = [](const char* reason) {
    std::cerr << reason << '\n';
    return 1;
  };
  if (!p::IsValid(config) || !p::IsValid({})) return fail("valid configurations rejected");
  for (unsigned field = 0; field < 6; ++field) {
    auto invalid = config;
    if (field == 0) invalid.count = 9;
    if (field == 1) invalid.first = 0;
    if (field == 2) invalid.stride = 0;
    if (field == 3) invalid.surface = 0;
    if (field == 4) invalid.first = UINT32_MAX;
    if (field == 5) invalid.stride = UINT32_MAX;
    p::State state;
    if (p::IsValid(invalid) || state.Observe(invalid, first).writer)
      return fail("unsafe or overflowing configuration accepted");
  }
  p::State disabled;
  if (disabled.Observe({}, first).writer) return fail("default capture is not disabled");

  for (unsigned field = 0; field < 10; ++field) {
    auto wrong = first;
    if (field == 0) wrong.frame = 0;
    if (field == 1) wrong.selected_frame = false;
    if (field == 2) wrong.pixel_shader = false;
    if (field == 3) wrong.surface ^= 1;
    if (field == 4) wrong.color = 0x00030300;  // Probe281 normal buffer, not scene color.
    if (field == 5) wrong.color_mask = 0;
    if (field == 6) wrong.color_mask = 0xFF;
    if (field == 7) wrong.window_offset = 0xFE800000;  // Second tile is not first-tile evidence.
    if (field == 8) wrong.scissor_tl = 1;
    if (field == 9) wrong.submitted_draw = 1023;
    p::State state;
    if (state.Observe(config, wrong).writer || !state.Observe(config, first).before)
      return fail("a nonmatching draw consumed the one-frame observer");
  }
  p::State state;
  unsigned before_count = 0, after_count = 0;
  for (uint32_t ordinal = 1; ordinal <= 10000; ++ordinal) {
    auto draw = first;
    draw.submitted_draw += ordinal - 1;
    // RGB-only lit passes are real writers too.
    draw.color_mask = 7;
    auto selected = state.Observe(config, draw);
    before_count += selected.before;
    after_count += selected.after;
    if (selected.after && (selected.writer - 1) % 256 != 0)
      return fail("wrong sparse writer ordinal");
    if (state.Observe(config, draw).writer) return fail("duplicate submission counted twice");
  }
  if (before_count != 1 || after_count != 8) return fail("capture count cap violated");

  p::State incomplete;
  if (!incomplete.Observe(config, first).before) return fail("first frame did not arm");
  auto later = first;
  ++later.frame;
  later.selected_frame = false;
  incomplete.Observe(config, later);
  later.selected_frame = true;
  later.submitted_draw += 100;
  if (incomplete.Observe(config, later).writer || incomplete.Observe(config, first).writer)
    return fail("partial writer chain continued in another frame");
  auto pairs = config;
  pairs.minimum_draw = 0;
  pairs.first = 2;
  pairs.stride = 2;
  pairs.count = 4;
  pairs.pixel_shader_hash = UINT64_C(0x10DFAFC2D5BE25BE);
  pairs.capture_pairs = true;
  auto shader_draw = first;
  shader_draw.submitted_draw = 1;
  shader_draw.color_mask = 8;
  shader_draw.pixel_shader_hash = pairs.pixel_shader_hash;
  p::State paired_state;
  auto unrelated = shader_draw;
  unrelated.pixel_shader_hash ^= 1;
  if (paired_state.Observe(pairs, unrelated).writer)
    return fail("unrelated shader consumed paired observer");
  before_count = after_count = 0;
  for (unsigned i = 1; i <= 12; ++i) {
    shader_draw.submitted_draw = i;
    const auto selected = paired_state.Observe(pairs, shader_draw);
    before_count += selected.before;
    after_count += selected.after;
    if (selected.before != selected.after ||
        (selected.after && selected.writer != i))
      return fail("pair does not bracket the same exact operation");
  }
  if (before_count != 4 || after_count != 4)
    return fail("paired image cap or sparse selection violated");
  pairs.count = 5;
  if (p::IsValid(pairs)) return fail("five pairs exceed image budget");
  pairs.count = 4;
  pairs.pixel_shader_hash = 0;
  if (p::IsValid(pairs)) return fail("unqualified paired capture accepted");

  // No-input autonomous selector: a timer/frame miss never consumes the one
  // shot. Lock only a real matching draw, keep every existing target guard and
  // never stitch a partial sequence into another frame.
  pairs.pixel_shader_hash = UINT64_C(0xBE763931E2AB7D56);
  pairs.automatic_first_frame = 100;
  pairs.first = pairs.stride = 1;
  auto automatic_draw = shader_draw;
  automatic_draw.pixel_shader_hash = pairs.pixel_shader_hash;
  automatic_draw.frame = 99;
  automatic_draw.selected_frame = true;  // Cannot bypass explicit minimum.
  p::State automatic;
  if (!p::IsValid(pairs) || automatic.Observe(pairs, automatic_draw).writer ||
      automatic.locked_frame()) return fail("automatic minimum frame ignored");
  automatic_draw.frame = 100;
  automatic_draw.selected_frame = false;
  for (unsigned field = 0; field < 6; ++field) {
    auto wrong = automatic_draw;
    if (field == 0) wrong.pixel_shader_hash ^= 1;
    if (field == 1) wrong.surface ^= 1;
    if (field == 2) wrong.color ^= 1;
    if (field == 3) wrong.scissor_br ^= 1;
    if (field == 4) wrong.window_offset = 1;
    if (field == 5) wrong.pixel_shader = false;
    if (automatic.Observe(pairs, wrong).writer || automatic.locked_frame())
      return fail("wrong operation armed automatic observer");
  }
  automatic_draw.frame = 103;
  const auto auto_first = automatic.Observe(pairs, automatic_draw);
  if (!auto_first.before || !auto_first.after || automatic.locked_frame() != 103)
    return fail("exact operation did not arm without timed frame");
  if (automatic.Observe(pairs, automatic_draw).writer)
    return fail("automatic duplicate accepted");
  ++automatic_draw.frame;
  automatic_draw.submitted_draw = 1;
  if (automatic.Observe(pairs, automatic_draw).writer)
    return fail("automatic chain continued across frames");
  p::State bounded_automatic;
  before_count = after_count = 0;
  for (unsigned i = 1; i <= 20; ++i) {
    automatic_draw.submitted_draw = i;
    const auto selection = bounded_automatic.Observe(pairs, automatic_draw);
    before_count += selection.before;
    after_count += selection.after;
  }
  if (before_count != 4 || after_count != 4)
    return fail("automatic mode broke the four-pair bound");
  pairs.capture_pairs = false;
  if (p::IsValid(pairs)) return fail("unpaired automatic mode accepted");
  std::cout << "Bounded one-frame target writer capture policy passed\n";
  return 0;
}
