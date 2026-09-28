#include <rex/graphics/embedded_camera_draw_capture_policy.h>
#include <rex/graphics/embedded_camera_history_capture_policy.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace p = rex::graphics::embedded_camera_draw_capture_policy;
namespace h = rex::graphics::embedded_camera_history_capture_policy;

int main() {
  bool ok = true;
  const auto check = [&](bool condition, const char* label) {
    if (!condition) { std::cerr << label << '\n'; ok = false; }
  };
  uint64_t bitmap[4]{1ull << 2, 1ull << 3, 0, 1ull << 63};
  uint32_t upload[12]{0x3F800000, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  p::Constants copy;
  copy.Capture(upload, sizeof(upload), 3, bitmap);
  check(copy.valid && copy.count == 3 && copy.map[1] == bitmap[1] &&
      copy.map[3] == bitmap[3] && copy.words[11] == 12,
      "preserve sparse map and actual dense upload bytes");
  upload[0] = 0;
  check(copy.words[0] == 0x3F800000, "own upload bytes across reuse");
  copy.Capture(upload, sizeof(upload) - 1, 3, bitmap);
  check(!copy.valid && !copy.count && !copy.words[0], "reject short upload without stale data");
  copy.Capture(upload, sizeof(upload), 2, bitmap);
  check(!copy.valid, "reject map/count mismatch");
  copy.Capture(nullptr, sizeof(upload), 3, bitmap);
  check(!copy.valid, "reject missing upload");
  copy.Capture(upload, UINT32_MAX, 257, bitmap);
  check(!copy.valid, "reject excessive register count before arithmetic");
  uint64_t empty[4]{};
  copy.Capture(nullptr, 0, 0, empty);
  check(copy.valid && !copy.count, "explicit empty shader constant map");
  p::Budget budget;
  check(!budget.Select(false, 10, 1), "partial/unselected frame ignored");
  check(!budget.Select(true, 0, 1) && !budget.Select(true, 10, 0), "require actual identity");
  for (uint32_t i = 1; i <= p::Budget::kMaximumDraws; ++i)
    check(budget.Select(true, 10, i), "bounded complete frame");
  check(!budget.Select(true, 10, p::Budget::kMaximumDraws), "duplicate draw rejected");
  check(!budget.Select(true, 10, p::Budget::kMaximumDraws + 1) && budget.dropped == 1,
      "report overflow without enlarging storage");
  check(!budget.Select(false, 11, 1) && budget.closed, "next frame closes scope");
  check(!budget.Select(true, 10, 1) && !budget.Select(true, 12, 1), "no second frame/reopen");

  h::Budget history;
  check(!history.Select(11, 1, h::Budget::kMotionPixelShader) && history.Finish(11, 0) == -1,
      "history ignores draws and swaps before explicit arming");
  history.Failure(11);
  check(!history.Arm(0, 10) && history.Arm(7, 10) && !history.Arm(8, 10),
      "one nonzero manual generation per history run");
  check(history.generation == 7 && history.first_frame == 11 && !history.Active(10) &&
      history.Active(11) && !history.Active(12), "arm-only swap selects the following frame");
  check(!history.Select(10, 99, h::Budget::kMotionPixelShader) && history.Finish(10, 99) == -1 &&
      !history.aborted && !history.observed[0], "partial arm frame cannot enter history");
  check(!history.Select(11, 1, 0) && history.Select(11, 2, h::Budget::kMotionPixelShader) &&
      !history.Select(11, 3, 1), "count every actual draw while selecting only motion shader");
  check(history.Finish(11, 3) == 0 && !history.closed && !history.aborted &&
      history.observed[0] == 3 && history.captured[0] == 1 && !history.failures[0],
      "first frame finishes without closing independent history");
  check(history.Select(12, 1, h::Budget::kMotionPixelShader) && history.Finish(12, 1) == 1 &&
      history.closed && !history.aborted && history.captured[1] == 1,
      "immediate successor uses independent counters and then closes");
  check(!history.Select(13, 1, h::Budget::kMotionPixelShader) && history.Finish(13, 1) == -1 &&
      !history.Arm(9, 13), "no third frame or repeated request can reopen history");
  check(budget.closed, "two-frame history does not reopen the legacy single-frame budget");

  h::Budget overflow;
  overflow.Arm(1, 20);
  for (uint64_t frame = 21; frame <= 22; ++frame) {
    for (uint32_t i = 1; i <= h::Budget::kMaximumDraws; ++i)
      check(overflow.Select(frame, i, h::Budget::kMotionPixelShader), "history bounded selection");
    check(!overflow.Select(frame, h::Budget::kMaximumDraws + 1, h::Budget::kMotionPixelShader) &&
        !overflow.Select(frame, h::Budget::kMaximumDraws + 2, 0),
        "overflow does not grow copies or stop actual draw accounting");
    check(overflow.Finish(frame, h::Budget::kMaximumDraws + 2) == int(frame - 21),
        "overflow frame remains explicitly reportable");
  }
  check(overflow.captured[0] == h::Budget::kMaximumDraws &&
      overflow.captured[1] == h::Budget::kMaximumDraws &&
      overflow.dropped[0] == 1 && overflow.dropped[1] == 1 && !overflow.aborted,
      "per-frame overflow is explicit and cannot masquerade as complete metadata");
  for (uint32_t error = 0; error < 4; ++error) {
    h::Budget bad;
    bad.Arm(1, 10);
    if (error == 0) bad.Select(11, 0, h::Budget::kMotionPixelShader);
    if (error == 1) bad.Select(11, 2, h::Budget::kMotionPixelShader);
    if (error == 2) {
      bad.Select(11, 1, h::Budget::kMotionPixelShader);
      bad.Select(11, 1, h::Budget::kMotionPixelShader);
    }
    if (error == 3) bad.Select(12, 1, h::Budget::kMotionPixelShader);
    check(bad.aborted && !bad.Select(11, 1, h::Budget::kMotionPixelShader) &&
        bad.Finish(11, bad.observed[0]) == 0 && bad.closed,
        "zero, missed, duplicate, or out-of-frame draw invalidates capture");
  }
  h::Budget missed;
  missed.Arm(1, 10);
  check(missed.Finish(12, 0) == -2 && missed.closed && missed.aborted,
      "missing first boundary cannot be replaced by a later frame");
  h::Budget repeated;
  repeated.Arm(1, 10);
  check(repeated.Finish(11, 0) == 0 && repeated.Finish(11, 0) == -2 && repeated.closed,
      "repeated swap cannot be accepted as the second frame");
  h::Budget mismatch;
  mismatch.Arm(1, 10);
  check(mismatch.Finish(11, 1) == 0 && mismatch.closed && mismatch.aborted,
      "submitted draw mismatch makes the recorded frame invalid");
  h::Budget failed;
  failed.Arm(1, 10);
  failed.Failure(10);
  failed.Failure(11);
  check(failed.Finish(11, 0) == 0 && failed.failures[0] == 1 && !failed.failures[1],
      "draw failures are scoped to the armed frame and remain explicit");
  failed.Failure(12);
  failed.Finish(12, 0);
  failed.Failure(12);
  check(failed.failures[1] == 1, "closed frame cannot acquire later failure metadata");
  h::Budget maximum;
  check(!maximum.Arm(1, UINT64_MAX - 1) && !maximum.Arm(1, UINT64_MAX) &&
      maximum.Arm(1, UINT64_MAX - 2) && maximum.Finish(UINT64_MAX - 1, 0) == 0 &&
      maximum.Finish(UINT64_MAX, 0) == 1 && maximum.closed,
      "adjacent frame arithmetic cannot wrap");

  h::Budget extended;
  for (uint32_t frames : {0u, 1u, 3u, 5u, 7u, UINT32_MAX})
    check(!extended.Arm(1, 10, frames) && !extended.generation,
        "invalid history horizon cannot arm or mutate the capture");
  check(extended.Arm(1, 10, 6) && extended.frame_count == 6,
      "extended horizon requires explicit opt-in");
  for (uint32_t i = 0; i < 6; ++i) {
    for (uint32_t draw_index = 1; draw_index <= h::Budget::kMaximumDraws; ++draw_index)
      check(extended.Select(11 + i, draw_index, h::Budget::kMotionPixelShader),
          "each extended frame keeps the same bounded draw selection");
    check(!extended.Select(11 + i, h::Budget::kMaximumDraws + 1, h::Budget::kMotionPixelShader),
        "extended horizon cannot increase per-frame storage");
    check(extended.Finish(11 + i, h::Budget::kMaximumDraws + 1) == int(i) &&
        extended.dropped[i] == 1 && extended.closed == (i == 5),
        "six independently accounted frames close at the selected horizon");
  }
  check(!extended.Select(17, 1, h::Budget::kMotionPixelShader) && extended.Finish(17, 0) == -1,
      "no seventh frame can enter the extended capture");
  h::Budget extended_wrap;
  check(!extended_wrap.Arm(1, UINT64_MAX - 5, 6) &&
      extended_wrap.Arm(1, UINT64_MAX - 6, 6), "extended frame arithmetic cannot wrap");
  for (uint32_t i = 0; i < 6; ++i)
    check(extended_wrap.Finish(UINT64_MAX - 5 + i, 0) == int(i), "last valid six-frame window");
  h::Budget extended_lost;
  extended_lost.Arm(1, 10, 6);
  for (uint32_t i = 0; i < 4; ++i) extended_lost.Finish(11 + i, 0);
  check(extended_lost.Finish(16, 0) == -2 && extended_lost.closed && extended_lost.aborted,
      "a missing late boundary cannot be hidden by a longer horizon");

  std::ifstream file(std::string(DARKNESS_SOURCE_ROOT) +
      "/external/ReXGlue/src/graphics/d3d12/command_processor.cpp");
  std::ostringstream out; out << file.rdbuf();
  const auto source = out.str();
  const auto draw = source.find("deferred_command_list_.D3DDrawIndexedInstanced(");
  const auto capture = source.find("embedded_camera_draw_budget.Select(", draw);
  const auto end = source.find("if (embedded_prompt_occlusion_query_index", capture);
  check(draw != std::string::npos && capture != std::string::npos &&
      end != std::string::npos && draw < capture && capture < end,
      "production copy after actual draw and before diagnostic binding reuse");
  if (capture != std::string::npos && end != std::string::npos) {
    const auto body = source.substr(capture, end - capture);
    check(body.find("IsCurrentEmbeddedGameplayCaptureFrame()") != std::string::npos &&
        body.find("embedded_float_vertex_cpu_address_") != std::string::npos &&
        body.find("constant_map_vertex.float_bitmap") != std::string::npos,
        "production selected-frame capture owns actual bound constants with shader map");
    check(body.find("Await") == std::string::npos && body.find("TranslatePhysical") == std::string::npos,
        "no wait or guest-memory substitute");
    check(body.find("embedded_camera_history_budget.Select(") != std::string::npos &&
        body.find("REXCVAR_GET(embedded_hitch_diagnostics)") != std::string::npos &&
        body.find("pixel_shader ? pixel_shader->ucode_data_hash() : 0") != std::string::npos &&
        body.find("capture_camera_draw || capture_history_draw") != std::string::npos &&
        body.find("embedded_camera_scene_capture) || capture_history_draw") != std::string::npos,
        "independent history frame records actual motion shaders and both bound constant stages");
    check(body.find("embedded_float_pixel_cpu_address_") != std::string::npos &&
        body.find("history.camera = draw;") != std::string::npos &&
        body.find("GetVertexFetch(binding.fetch_constant)") != std::string::npos &&
        body.find("history.bindings_complete =") != std::string::npos &&
        body.find("history.index_address = primitive_processing_result.guest_index_base") != std::string::npos,
        "history owns upload bytes and identifies actual geometry bindings without claiming their contents");
  }
  check(source.find("embedded_camera_draw_capture, false") != std::string::npos,
      "default off");
  check(source.find("embedded_camera_history_capture, false") != std::string::npos,
      "history observer defaults off");
  const auto arm_action = source.find("manual_action == embedded_manual_capture_policy::Action::kArm");
  const auto arm = source.find("embedded_camera_history_budget.Arm(", arm_action);
  const auto finish = source.find("FinishEmbeddedCameraHistory(ordinal", arm);
  const auto reset = source.find("embedded_frame_frontier.Reset();", finish);
  check(arm_action != std::string::npos && arm != std::string::npos && finish != std::string::npos &&
      reset != std::string::npos && arm_action < arm && arm < finish && finish < reset,
      "history arms only on completed manual swap and finishes before frontier reset");
  const auto writer_start = source.find("void FinishEmbeddedCameraHistory(");
  const auto writer_end = source.find("struct EmbeddedInputTransition", writer_start);
  if (writer_start != std::string::npos && writer_end != std::string::npos) {
    const auto writer = source.substr(writer_start, writer_end - writer_start);
    check(writer.find("std::ferror(file)") != std::string::npos &&
        writer.find("std::fclose(file)") != std::string::npos &&
        writer.find("HISTORY_END") != std::string::npos &&
        writer.find("bound_inputs_not_history_acceptance") != std::string::npos &&
        writer.find("Await") == std::string::npos && writer.find("EndSubmission") == std::string::npos,
        "bounded metadata writer reports completion without a GPU wait or submission change");
  } else {
    check(false, "production history writer exists");
  }
  {
    h::Budget proof;
    check(!proof.BeginOwnedProof(1), "no proof before arm");
    check(proof.Arm(1,100,6), "arm six-frame proof window");
    for (uint32_t index=0;index<6;++index) {
      check(proof.BeginOwnedProof(101+index)==(index==2), "exact delayed proof index");
      check(!proof.BeginOwnedProof(102+index), "no proof on a mismatched boundary");
      proof.Finish(101+index,0);
    }
    check(!proof.BeginOwnedProof(103), "no proof after closure");
    h::Budget short_window;
    short_window.Arm(1,100,2);
    check(!short_window.BeginOwnedProof(101), "legacy two-frame selection stays separate");
  }
  return ok ? 0 : 1;
}
