#include <rex/graphics/embedded_camera_geometry_capture_policy.h>
#include <rex/graphics/embedded_geometry_readback_policy.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace p = rex::graphics::embedded_camera_geometry_capture_policy;

int main() {
  bool ok = true;
  const auto check = [&](bool value, const char* label) {
    if (!value) { std::cerr << label << '\n'; ok = false; }
  };
  p::Draw d{40, 170, UINT64_C(0x741FD5FD94E6154D), true, false,
            0x14010500, 0x00010000, 0x1C708767, 1, 1};
  check(p::IsEligible(d), "observed packed-depth draw");
  for (uint64_t shader : {UINT64_C(0x1A0DA8AEF2660AD9), UINT64_C(0x2517F03F3BDACF2F),
                          UINT64_C(0xA6EFBE22D37B8304)}) {
    auto other = d; other.vertex_shader = shader;
    check(p::IsEligible(other), "other reviewed packed position families");
  }
  auto clear = d; clear.vertex_shader = UINT64_C(0x0A6D1DD7767FDF27); clear.surface = 0x0A020280;
  check(p::IsEligible(clear), "observed preceding clear geometry");
  for (uint32_t control : {0u, 2u, 4u}) {
    auto other = d; other.depth_control = control;
    check(!p::IsEligible(other), "exclude disabled testing/writing and color-only passes");
  }
  auto other = d; other.memexport = true;
  check(!p::IsEligible(other), "exclude draw that can mutate its source");
  other = d; other.scale_y = 2;
  check(!p::IsEligible(other), "exclude scaled capture");
  other = d; other.vertex_shader = 123;
  check(!p::IsEligible(other), "no arbitrary shader selector");
  p::DrawBudget draws;
  other = d; other.selected_frame = false;
  for (unsigned i = 0; i < 1000; ++i) check(!draws.Select(other), "startup does not spend capture");
  check(draws.attempts == 0 && draws.frame == 0, "no startup frame ownership");
  for (uint32_t i = 0; i < p::DrawBudget::kMaximumDraws; ++i) {
    d.ordinal = 170 + i;
    check(draws.Select(d), "complete bounded selected-frame draws");
    check(!draws.Select(d), "same draw cannot spend another reservation");
  }
  ++d.ordinal;
  check(!draws.Select(d) && draws.dropped == 1, "overflow reported without more work");
  ++d.frame;
  check(!draws.Select(d) && draws.closed, "next frame closes capture");
  --d.frame;
  check(!draws.Select(d), "no later request can reopen capture");

  p::RangeBudget ranges;
  check(ranges.Reserve(16, 5000) == 5000, "full geometry beyond old preview limit");
  check(rex::graphics::embedded_geometry_readback_policy::CaptureBytes(16, 5000, 0) == 4096,
        "legacy preview budget preserved");
  check(!ranges.Reserve(16, UINT64_MAX), "reject overflow without truncation");
  check(!ranges.Reserve(p::RangeBudget::kMemoryBytes - 4, 8), "reject crossing physical memory end");
  check(ranges.Reserve(p::RangeBudget::kMemoryBytes - 4, 4) == 4, "exact physical range end");
  check(!ranges.Reserve(0, p::RangeBudget::kMaximumRangeBytes + 1), "reject excessive single allocation");
  p::RangeBudget total;
  for (unsigned i = 0; i < 8; ++i)
    check(total.Reserve(0, p::RangeBudget::kMaximumRangeBytes) == p::RangeBudget::kMaximumRangeBytes,
          "bounded aggregate allocation");
  check(!total.Reserve(0, 1) && total.reserved_bytes == p::RangeBudget::kMaximumTotalBytes,
        "reject aggregate overflow");
  p::RangeBudget failures;
  for (unsigned i = 0; i < p::RangeBudget::kMaximumRanges; ++i)
    check(!failures.Reserve(0, 0), "failed attempts remain bounded");
  check(!failures.Reserve(0, 1) && failures.attempts == p::RangeBudget::kMaximumRanges,
        "allocation failures cannot retry indefinitely");

  const auto read = [](const char* relative) {
    std::ifstream file(std::string(DARKNESS_SOURCE_ROOT) + relative);
    std::ostringstream out; out << file.rdbuf(); return out.str();
  };
  const auto source = read("/external/ReXGlue/src/graphics/d3d12/command_processor.cpp");
  const auto submitted = source.find("deferred_command_list_.D3DDrawIndexedInstanced(");
  const auto capture = source.find("embedded_camera_geometry_budget.Select(", submitted);
  const auto end = source.find("if (embedded_prompt_occlusion_query_index", capture);
  check(submitted < capture && capture < end && end != std::string::npos,
        "GPU source copy follows actual draw before diagnostic binding reuse");
  if (capture != std::string::npos && end != std::string::npos) {
    const auto body = source.substr(capture, end - capture);
    check(body.find("IsCurrentEmbeddedGameplayCaptureFrame()") != std::string::npos &&
          body.find("QueueCameraGeometryReadback(") != std::string::npos &&
          body.find("vertex_bindings().size() == 1") != std::string::npos,
          "production requires selected frame and actual single stream");
    check(body.find("Await") == std::string::npos && body.find("TranslatePhysical") == std::string::npos,
          "no wait or CPU memory substitute");
  }
  check(source.find("embedded_camera_geometry_capture, false") != std::string::npos, "default off");
  const auto shared = read("/external/ReXGlue/src/graphics/d3d12/shared_memory.cpp");
  check(shared.find("camera_geometry_budget_.Reserve(address, requested_bytes)") != std::string::npos &&
        shared.find("QueueBoundedReadback(address, bytes, path, draw, false, true)") != std::string::npos &&
        shared.find("else if (!camera_geometry) ++geometry_readback_count_") != std::string::npos,
        "production uses full reserved ranges and a separate counter");
  return ok ? 0 : 1;
}
