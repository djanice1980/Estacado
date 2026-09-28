#include <rex/graphics/embedded_depth_resolve_capture_policy.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace p = rex::graphics::embedded_depth_resolve_capture_policy;

int main() {
  unsigned failures = 0;
  const auto check = [&](bool value, const char* label) {
    if (!value) { std::cerr << label << '\n'; ++failures; }
  };
  // Actual V202 hallway frame3206: two native ranges, no depth texture binding.
  p::Resolve first{3206, 2, true, true, false, 1, 1, 0x4, 0x14010500,
      0x00010000, 0x00004302, 0x02D00500, 0x1118A000, 0x1118A000, 1966080};
  auto second = first;
  second.ordinal = 5;
  second.destination_base = second.written_address = 0x1136A000;
  second.written_bytes = 1802240;
  check(p::IsEligible(first) && p::IsEligible(second), "observed depth ranges");
  p::State state;
  auto unselected = first;
  unselected.selected_frame = false;
  check(state.Select(unselected) == 0, "no selected complete frame");
  check(state.Select(first) == 1, "first range");
  check(state.Select(first) == 0, "duplicate resolve");
  check(state.Select(second) == 2, "second range preserved independently");
  auto next = first;
  next.frame++;
  next.selected_frame = false;
  check(state.Select(next) == 0, "unselected next frame closes capture");
  next.selected_frame = true;
  check(state.Select(next) == 0, "later request cannot mix frames");
  check(state.Select(first) == 0, "closed frame cannot reopen");

  p::State limited;
  for (uint32_t i = 1; i <= 6; ++i) {
    auto attempt = first;
    attempt.ordinal = i;
    check(limited.Select(attempt) == (i <= 4 ? i : 0), "four attempt bound");
    // No allocation/completion acknowledgment: failed copies also consume budget.
  }

  const auto reject = [&](p::Resolve r, const char* label) {
    p::State fresh;
    check(!p::IsEligible(r) && fresh.Select(r) == 0, label);
    check(fresh.Select(first) == 1, "invalid metadata does not consume capture");
  };
  auto bad = first; bad.succeeded = false; reject(bad, "failed resolve");
  bad = first; bad.scaled = true; reject(bad, "scaled GPU range");
  bad = first; bad.scale_x = 2; reject(bad, "internal scale2x");
  bad = first; bad.scale_y = 0; reject(bad, "invalid scale");
  bad = first; bad.control = 0x00100140; reject(bad, "color resolve");
  bad = first; bad.surface = 0x14000500; reject(bad, "other sample layout");
  bad = first; bad.depth = 0; reject(bad, "other depth encoding");
  bad = first; bad.destination_info ^= 1; reject(bad, "other destination encoding");
  bad = first; bad.destination_pitch = 0x03200320; reject(bad, "800px shadow surface");
  bad = first; bad.written_address += 4; reject(bad, "partial offset unclassified");
  bad = first; bad.written_bytes = 0; reject(bad, "empty range");
  bad = first; bad.written_bytes = 8u * 1024 * 1024 + 4; reject(bad, "copy size bound");
  bad = first; bad.written_bytes--; reject(bad, "partial packed pixel");
  bad = first; bad.written_address = bad.destination_base = 0x1FFFFFFC;
  reject(bad, "physical memory end overflow");
  bad = first; bad.written_address = bad.destination_base = UINT32_MAX;
  reject(bad, "address wrap");
  bad = first; bad.written_address = bad.destination_base = 0;
  reject(bad, "unowned zero range");
  bad = first; bad.frame = 0; reject(bad, "no frame identity");
  bad = first; bad.ordinal = 0; reject(bad, "no resolve identity");

  std::ifstream file(std::string(DARKNESS_SOURCE_ROOT) +
      "/external/ReXGlue/src/graphics/d3d12/command_processor.cpp");
  std::ostringstream contents; contents << file.rdbuf();
  const std::string source = contents.str();
  const auto resolve = source.find("const bool resolved = render_target_cache_->Resolve(");
  const auto capture = source.find("if (!kernel_state_ && REXCVAR_GET(embedded_temporal_depth_resolve_capture))", resolve);
  const auto end = source.find("if (!kernel_state_ && IsEmbeddedFrameDiagnosticsEnabled())", capture);
  check(resolve != std::string::npos && capture != std::string::npos &&
      end != std::string::npos && resolve < capture && capture < end, "production post-resolve boundary");
  if (capture != std::string::npos && end != std::string::npos) {
    const auto body = source.substr(capture, end - capture);
    check(body.find("QueueTextureSourceReadback(") != std::string::npos &&
        body.find("IsCurrentEmbeddedGameplayCaptureFrame()") != std::string::npos,
        "production frame gate and fenced GPU copy");
    check(body.find("AwaitAllQueueOperationsCompletion") == std::string::npos &&
        body.find("TranslatePhysical") == std::string::npos,
        "no diagnostic wait or CPU substitute");
  }
  check(source.find("embedded_temporal_depth_resolve_capture, false") != std::string::npos,
      "default off");
  std::ifstream rt_file(std::string(DARKNESS_SOURCE_ROOT) +
      "/external/ReXGlue/src/graphics/d3d12/render_target_cache.cpp");
  std::ostringstream rt_contents; rt_contents << rt_file.rdbuf();
  const auto rt_source = rt_contents.str();
  const auto layout_gate = rt_source.find(
      "(REXCVAR_GET(embedded_temporal_depth_resolve_capture) &&");
  const auto layout_record = rt_source.find("REX_EMBEDDED_RESOLVE_GRID ordinal=", layout_gate);
  check(layout_gate != std::string::npos && layout_record != std::string::npos,
      "depth export includes production resolve layout provenance");
  if (layout_gate != std::string::npos && layout_record != std::string::npos) {
    const auto gate = rt_source.substr(layout_gate, layout_record - layout_gate);
    check(gate.find("resolve_info.IsCopyingDepth()") != std::string::npos &&
        gate.find("IsCurrentEmbeddedGameplayCaptureFrame()") != std::string::npos &&
        gate.find("resolve_grid_trace_count < 64") != std::string::npos,
        "layout observation is depth-only, selected-frame and bounded");
  }
  return failures ? 1 : 0;
}
