#include <cmath>
#include <iostream>
#include <string>
#include <rex/graphics/pipeline/render_target/native_shader_scale_policy.h>

int main() {
  using namespace rex::graphics::render_target::native_shader_scale_policy;
  bool passed = true;
  const auto check = [&passed](bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; passed = false; }
  };
  const std::string seed = "B29F0BF45937C4C4:FDC5E32EC6045BE1:1:324:18:6";
  Rules rules;
  check(Parse("", rules) && rules.count == 0, "empty policy must leave scaling unchanged");
  check(Parse(seed, rules) && rules.count == 1, "verified annotation must parse");
  const Rule rule = rules.entries[0];
  check(RequiresNativeRasterization(rule), "data tables retain native evaluation");
  check(Matches(rule, rule.vertex_hash, rule.pixel_hash, true, 324, 18, 6, 0, 15),
        "matching single-sample data pass must keep its native grid");
  check(!Matches(rule, 1, rule.pixel_hash, true, 324, 18, 6, 0, 15), "other VS must be isolated");
  check(!Matches(rule, rule.vertex_hash, 1, true, 324, 18, 6, 0, 15), "other PS must be isolated");
  check(!Matches(rule, rule.vertex_hash, rule.pixel_hash, true, 1280, 720, 6, 0, 15),
        "full-screen imagery using a copy shader must remain scaled");
  check(!Matches(rule, rule.vertex_hash, rule.pixel_hash, true, 324, 18, 26, 0, 15),
        "other encoding must not match the source contract");
  check(!Matches(rule, rule.vertex_hash, rule.pixel_hash, false, 324, 18, 6, 0, 15),
        "cube/array/3D resources must not match");
  check(!Matches(rule, rule.vertex_hash, rule.pixel_hash, true, 324, 18, 6, 1, 15),
        "MSAA geometry must remain outside the data-pass policy");
  check(!Matches(rule, rule.vertex_hash, rule.pixel_hash, true, 324, 18, 6, 0, 7),
        "partial-channel writes must not match");
  check(!Matches(rule, rule.vertex_hash, rule.pixel_hash, true, 324, 18, 6, 0, 255),
        "MRT writes must not match");
  for (const auto& invalid : {seed + ";", seed + ";" + seed,
      std::string("X:1:1:324:18:6"), std::string("0:1:1:324:18:6"),
      std::string("1:2:32:324:18:6"), std::string("1:2:1:0:18:6"),
      std::string("1:2:1:324:18:64"), std::string("1:2:1:324:18:6:7"),
      std::string("1:2:1:324:18"), std::string("1:2:1:324:18:6 ")}) {
    check(!Parse(invalid, rules) && rules.count == 0,
          "malformed/duplicate annotations must fail atomically");
  }
  std::string many;
  for (int i = 1; i <= 33; ++i) {
    if (i > 1) many += ';';
    many += "1:2:1:" + std::to_string(i) + ":18:6";
  }
  check(!Parse(many, rules) && rules.count == 0, "policy must enforce its pass-count bound");
  check(!Parse(std::string(4097, '1'), rules), "policy text must be bounded");

  const std::string filter = "1:2:0:1280:720:26:filter";
  check(Parse(filter, rules) && rules.count == 1 && rules.entries[0].image_filter,
        "image-filter mode requires an explicit annotation");
  const auto filter_rule = rules.entries[0];
  check(RequiresNativeRasterization(filter_rule), "old filter policy remains compatible");
  check(Parse("1:2:0:1280:720:26:filter_scaled", rules) && rules.count == 1 &&
        rules.entries[0].image_filter && rules.entries[0].scaled_filter_output,
        "scaled filter is explicit; not inferred from shader dimensions");
  check(!RequiresNativeRasterization(rules.entries[0]),
        "source footprint must be independent of image-filter output grid");
  check(Matches(rules.entries[0], 1, 2, true, 1280, 720, 26, 1, 15, 0x18700270),
        "scaled filter shares the guarded depth-disabled image contract");
  check(!Matches(rules.entries[0], 1, 2, true, 1280, 720, 26, 1, 15, 0x18700272),
        "scaled filter must not reclassify depth work");
  check(!Parse(filter + ";1:2:0:1280:720:26:filter_scaled", rules),
        "contradictory evaluation-grid rules fail atomically");
  check(!Parse("1:2:0:1280:720:26:filter_scaled:filter", rules), "stacked modes rejected");
  check(Matches(filter_rule, 1, 2, true, 1280, 720, 26, 1, 15, 0x18700270),
        "depth-disabled 2-sample image filter may retain its native working grid");
  for (uint32_t depth_bit : {1u, 2u, 4u}) {
    check(!Matches(filter_rule, 1, 2, true, 1280, 720, 26, 1, 15, 0x18700270 | depth_bit),
          "filter mode must never reclassify depth/stencil work");
  }
  check(!Matches(filter_rule, 1, 2, true, 1280, 720, 26, 2, 15),
        "unobserved 4-sample filter family stays outside the implementation");
  check(!Matches(filter_rule, 1, 2, true, 320, 180, 26, 1, 15),
        "filter mode must match its exact declared source domain");
  check(!Parse(filter + ";1:2:0:1280:720:26", rules),
        "conflicting filter/data annotations must fail atomically");
  check(!Parse("1:2:0:1280:720:26:unknown", rules), "unknown mode must not parse");
  check(FilterSamplingSupported(2, 2, true, 0, true), "verified 2x footprint supported");
  check(!FilterSamplingSupported(1, 1, true, 0, true), "native source path remains unchanged");
  check(!FilterSamplingSupported(3, 3, true, 0, true), "no guessed 3x footprint");
  check(!FilterSamplingSupported(2, 1, true, 0, true), "no guessed asymmetric footprint");
  check(!FilterSamplingSupported(2, 2, false, 0, true), "point/mixed filtering unchanged");
  check(!FilterSamplingSupported(2, 2, true, 1, true), "mipped resources unchanged");
  check(!FilterSamplingSupported(2, 2, true, 0, false), "signed/gamma domains unchanged");
  check(!FilterSamplingSupported(2, 2, true, 0, true, false), "repeat/border modes remain outside this contract");
  check(FilterInstructionSupported(true, 0, 0, false, false),
        "zero guest offset must remain eligible despite host rounding epsilon");
  check(!FilterInstructionSupported(true, 0.5f, 0, false, false), "guest X offset excluded");
  check(!FilterInstructionSupported(true, 0, -0.5f, false, false), "guest Y offset excluded");
  check(!FilterInstructionSupported(false, 0, 0, false, false), "unnormalized coordinates excluded");
  check(!FilterInstructionSupported(true, 0, 0, true, false), "explicit LOD excluded");
  check(!FilterInstructionSupported(true, 0, 0, false, true), "explicit gradients excluded");

  // Synthetic fixture for the normalized-bilinear repeat copy of a lookup table.
  // samples x=-.25,y=-.25 for the first 2x subpixel. The nonadjacent edge
  // entries contaminate black before any tone-map or presentation operation.
  // A native data grid samples exactly the original black entry instead.
  const double last_last = 1;
  const double zero_last = .5;
  const double last_zero = .75;
  const double first_first = 0;
  const double wrapped = last_last * .0625 + zero_last * .1875 +
                         last_zero * .1875 + first_first * .5625;
  check(wrapped > .2 && first_first == 0,
        "supersampling a data grid must not be mistaken for identity sampling");
  if (passed) std::cout << "Native shader data-grid policy passed\n";
  return passed ? 0 : 1;
}
