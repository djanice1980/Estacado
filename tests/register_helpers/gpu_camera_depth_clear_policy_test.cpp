#include <rex/graphics/embedded_camera_depth_clear_policy.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace p = rex::graphics::embedded_camera_depth_clear_policy;

int main() {
  bool ok = true;
  const auto check = [&](bool value, const char* reason) {
    if (!value) { ok = false; std::cerr << reason << '\n'; }
  };
  p::Draw clear{10, 169, UINT64_C(0x0A6D1DD7767FDF27), UINT64_C(0x2E372EA28CC404B7),
                true, false, 0x0A020280, 0x00010000, 0x00008777, 8, 3, 1, 1};
  auto alias = clear;
  alias.ordinal = 170; alias.surface = 0x14010500; alias.primitive = 4;
  alias.control = 0x1C708767; alias.vertices = 18;
  p::Budget budget;
  auto startup = clear; startup.selected = false;
  for (unsigned i = 0; i < 1000; ++i)
    check(!budget.AfterClear(startup) && !budget.AfterAlias(alias), "startup/unpaired capture excluded");
  check(!budget.frame && !budget.attempts, "no startup budget spending");
  check(budget.AfterClear(clear), "first real clear");
  check(!budget.AfterClear(clear), "no duplicate clear");
  check(budget.AfterAlias(alias) == 169, "immediate native alias retains clear identity");
  check(!budget.AfterAlias(alias), "no duplicate alias");
  clear.ordinal = 508; alias.ordinal = 509;
  check(budget.AfterClear(clear) && budget.AfterAlias(alias) == 508, "second tile is its own epoch");
  clear.ordinal = 800;
  check(!budget.AfterClear(clear) && budget.attempts == 4 && budget.pairs == 2,
        "four total attempts even if GPU allocation fails");
  ++clear.frame;
  check(!budget.AfterClear(clear) && budget.closed, "one frame closes permanently");
  --clear.frame;
  check(!budget.AfterClear(clear), "no later request can reopen");

  for (unsigned failure = 0; failure < 6; ++failure) {
    p::Budget gate; clear.ordinal = 169; alias.ordinal = 170;
    check(gate.AfterClear(clear), "clear before failed transition");
    auto wrong = alias;
    if (failure == 0) ++wrong.ordinal;
    if (failure == 1) ++wrong.frame;
    if (failure == 2) wrong.surface = 0x14000500;
    if (failure == 3) wrong.control = 4;
    if (failure == 4) wrong.memexport = true;
    if (failure == 5) wrong.scale_x = 2;
    check(!gate.AfterAlias(wrong) && gate.missed == 1 && !gate.pending_clear,
          "reject stale, wrong or scaled consumer and report lost pair");
    check(!gate.AfterAlias(alias), "failed transition cannot retry under reused draw identity");
  }
  p::Budget unknown;
  clear.pixel_shader = 0;
  check(!unknown.AfterClear(clear), "unknown clear program cannot open a pair");
  p::ReadbackBudget range;
  check(range.Reserve(640, 1024, 4) == 7864320, "4x clear captures all samples for 384 explicit rows");
  check(range.Reserve(1280, 1024, 2) == 7864320, "2x alias preserves same sample count");
  check(range.Reserve(1280, 1024, 4) == 15728640, "bounded 2x-as-4x source includes every host sample");
  check(!range.Reserve(UINT64_MAX, 1024, 4), "reject multiplication overflow before allocation");
  check(!range.Reserve(640, 1024, 4) && range.attempts == 4, "invalid reservations spend the finite budget");
  p::ReadbackBudget invalid;
  check(!invalid.Reserve(640, 383, 4), "do not silently truncate requested prefix");
  check(!invalid.Reserve(1281, 1024, 2), "no unrelated target width");
  check(!invalid.Reserve(1280, 1024, 1), "no single-sample target");
  check(!invalid.Reserve(1280, 1024, 8), "no unsupported sample count");

  const auto read = [](const char* path) {
    std::ifstream file(std::string(DARKNESS_SOURCE_ROOT) + path);
    std::ostringstream out; out << file.rdbuf(); return out.str();
  };
  const auto cp = read("/external/ReXGlue/src/graphics/d3d12/command_processor.cpp");
  const auto update = cp.find("const bool render_target_update_succeeded = render_target_cache_->Update(");
  const auto pre = cp.find("embedded_camera_depth_clear_budget.AfterAlias(", update);
  const auto pipeline = cp.find("pipeline_cache_->ConfigurePipeline(", pre);
  const auto draw = cp.find("deferred_command_list_.D3DDrawIndexedInstanced(", pipeline);
  const auto constants = cp.find("embedded_camera_draw_budget.Select(", draw);
  const auto geometry = cp.find("embedded_camera_geometry_budget.Select(", constants);
  const auto post = cp.find("embedded_camera_depth_clear_budget.AfterClear(", geometry);
  check(update < pre && pre < pipeline && pipeline < draw && draw < constants &&
        constants < geometry && geometry < post && post != std::string::npos,
        "production preserves natural transfer, guest binding/draw, constants and geometry ordering");
  check(cp.find("embedded_camera_depth_clear_capture, false") != std::string::npos,
        "clear capture defaults off");
  const auto rt = read("/external/ReXGlue/src/graphics/d3d12/render_target_cache.cpp");
  const auto begin = rt.find("bool D3D12RenderTargetCache::QueueCameraDepthClearReadback(");
  const auto end = rt.find("void D3D12RenderTargetCache::CompleteDepthSourceReadbacks()", begin);
  const auto queue = rt.substr(begin, end - begin);
  check(queue.find("Await") == std::string::npos && queue.find("->Map(") == std::string::npos &&
        queue.find("D3DDraw") == std::string::npos && queue.find("TranslatePhysical") == std::string::npos,
        "no synchronous wait, mapping, guest draw or CPU substitute in capture path");
  check(queue.find("camera_depth_readback_budget_.Reserve(") < queue.find("CreateCommittedResource("),
        "reserve before allocation");
  check(queue.find("if (!camera_frame) captured_depth_sources_.push_back") != std::string::npos,
        "legacy unique-resource counter is not consumed by camera epochs");
  check(queue.find("D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, previous") != std::string::npos &&
        queue.find("SetExternalPipeline(p.pipeline.Get())") != std::string::npos,
        "restore source state and invalidate guest pipeline after diagnostic dispatch");
  check(rt.find("pending_depth_source_readbacks_.front().submission <= completed") != std::string::npos &&
        rt.find("REX_CAMERA_DEPTH_CLEAR_COMPLETE") != std::string::npos,
        "only completed GPU samples can be published");
  return ok ? 0 : 1;
}
