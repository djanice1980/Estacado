// GPU timing diagnostic (V304): per-category intervals of a frame are
// contiguous and sum to its period; gaps between submissions are not busy
// time; the first interval chains only to the preceding frame. Source policy:
// off by default, and only a null check on the hot paths when off.
#include <rex/graphics/d3d12/gpu_timing.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main() {
  bool passed = true;
  const auto check = [&passed](bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      passed = false;
    }
  };
  using rex::graphics::d3d12::GpuTimingCategory;
  using rex::graphics::d3d12::GpuTimingWindow;
  constexpr auto u8 = [](GpuTimingCategory c) { return uint8_t(c); };

  {
    GpuTimingWindow w;
    uint64_t previous_last = 0;
    bool have_previous = false;
    // Frame 1: gap (first, no previous), other 10, draw 50, barrier 5, draw 30,
    // end of submission (draw 5), gap 20 (next submission start), swap 10.
    const uint64_t t1[] = {1000, 1010, 1060, 1065, 1095, 1100, 1120, 1130};
    const uint8_t c1[] = {u8(GpuTimingCategory::kGap),  u8(GpuTimingCategory::kOther),
                          u8(GpuTimingCategory::kDraw), u8(GpuTimingCategory::kBarrier),
                          u8(GpuTimingCategory::kDraw), u8(GpuTimingCategory::kDraw),
                          u8(GpuTimingCategory::kGap),  u8(GpuTimingCategory::kSwap)};
    w.AddFrame(t1, c1, 8, 0, previous_last, have_previous);
    check(w.frames == 1 && w.period_ticks == 130 && w.busy_ticks == 110,
          "first frame: period from its first timestamp, gaps excluded from busy");
    check(w.category_ticks[size_t(GpuTimingCategory::kDraw)] == 85 &&
              w.category_ticks[size_t(GpuTimingCategory::kBarrier)] == 5 &&
              w.category_ticks[size_t(GpuTimingCategory::kGap)] == 20 &&
              w.category_ticks[size_t(GpuTimingCategory::kSwap)] == 10,
          "intervals attributed to the category they close");
    check(have_previous && previous_last == 1130, "last timestamp carried to the next frame");

    // Frame 2 starts 70 ticks after frame 1's last timestamp.
    const uint64_t t2[] = {1200, 1300};
    const uint8_t c2[] = {u8(GpuTimingCategory::kGap), u8(GpuTimingCategory::kTransfer)};
    w.AddFrame(t2, c2, 2, 3, previous_last, have_previous);
    check(w.frames == 2 && w.period_ticks == 130 + 170 && w.busy_ticks == 110 + 100,
          "second frame: first interval chains to the previous frame's last timestamp");
    check(w.max_period_ticks == 170 && w.max_busy_ticks == 110 && w.overflow == 3 &&
              w.marks == 10,
          "maxima, overflow and marks");
    uint64_t sum = 0;
    for (uint64_t ticks : w.category_ticks) sum += ticks;
    check(sum == w.period_ticks, "categories sum to the period");

    // Non-monotonic timestamps are skipped, not wrapped.
    const uint64_t t3[] = {1400, 1390, 1395};
    const uint8_t c3[] = {u8(GpuTimingCategory::kGap), u8(GpuTimingCategory::kDraw),
                          u8(GpuTimingCategory::kDraw)};
    w.AddFrame(t3, c3, 3, 0, previous_last, have_previous);
    check(w.non_monotonic == 1 && previous_last == 1395, "backwards timestamp rebases");

    // Out-of-range categories count as other; empty frames are ignored.
    const uint64_t t4[] = {1400};
    const uint8_t c4[] = {200};
    const uint64_t other_before = w.category_ticks[size_t(GpuTimingCategory::kOther)];
    w.AddFrame(t4, c4, 1, 0, previous_last, have_previous);
    check(w.category_ticks[size_t(GpuTimingCategory::kOther)] == other_before + 5,
          "invalid category counted as other");
    const uint64_t frames = w.frames;
    w.AddFrame(t4, c4, 0, 0, previous_last, have_previous);
    check(w.frames == frames, "empty frame ignored");
    w.Reset();
    check(w.frames == 0 && w.period_ticks == 0 && w.category_ticks[1] == 0, "reset");
  }
  {
    std::ifstream file(std::string(REXGLUE_SOURCE_ROOT) +
                       "/src/graphics/d3d12/command_processor.cpp");
    const std::string source((std::istreambuf_iterator<char>(file)), {});
    check(source.find("REXCVAR_DEFINE_BOOL(d3d12_gpu_timing, false,") != std::string::npos,
          "GPU timing is off by default");
    const auto barriers = source.find("void D3D12CommandProcessor::SubmitBarriers() {");
    const auto guarded = source.find("if (gpu_timing_) {", barriers);
    const auto next_function = source.find("\n}\n", barriers);
    check(barriers != std::string::npos && guarded < next_function,
          "barrier timing only behind the enabled check");
    const auto end_submission = source.find("bool D3D12CommandProcessor::EndSubmission(bool is_swap)");
    const auto end_timing = source.find("GpuTimingEndSubmission();", end_submission);
    const auto execute = source.find("deferred_command_list_.SwapStream(job.stream);", end_submission);
    check(end_timing != std::string::npos && end_timing < execute,
          "timestamps resolved before the submission's commands are handed off");
    std::ifstream header_file(std::string(REXGLUE_SOURCE_ROOT) +
                              "/include/rex/graphics/d3d12/command_processor.h");
    const std::string header((std::istreambuf_iterator<char>(header_file)), {});
    const std::string member_text = "std::unique_ptr<GpuTimingState> gpu_timing_;";
    const auto member = header.find(member_text);
    const auto class_end = header.find("\n};", member);
    // V338: only the GPU frame meter follows it (appended, so the layout
    // before both is unchanged).
    std::string tail = member != std::string::npos && class_end != std::string::npos
                           ? header.substr(member + member_text.size(),
                                           class_end - (member + member_text.size()))
                           : std::string{};
    tail.erase(std::remove_if(tail.begin(), tail.end(),
                              [](char c) { return c == ' ' || c == '\r' || c == '\n'; }),
               tail.end());
    check(member != std::string::npos && class_end != std::string::npos &&
              tail ==
                  "structGpuFrameMeter;voidInitializeGpuFrameMeter();"
                  "voidGpuFrameMeterBeginSubmission(boolsubmission_opened,boolis_opening_frame);"
                  "voidGpuFrameMeterEndSubmission();voidGpuFrameMeterCloseFrame();"
                  "std::unique_ptr<GpuFrameMeter>gpu_frame_meter_;",
          "timing state and the frame meter are the last members (layout before unchanged)");
  }
  {
    // V306: unclipped draws estimate their vertical extent (upstream default),
    // so screen-space passes don't claim the EDRAM to its end and trigger
    // spurious render-target ownership round trips.
    std::ifstream file(std::string(REXGLUE_SOURCE_ROOT) +
                       "/src/graphics/util/draw_extent_estimator.cpp");
    const std::string source((std::istreambuf_iterator<char>(file)), {});
    check(source.find("REXCVAR_DEFINE_BOOL(execute_unclipped_draw_vs_on_cpu, true,") !=
              std::string::npos,
          "unclipped draw extent estimated on the CPU by default");
    check(source.find("REXCVAR_DEFINE_BOOL(execute_unclipped_draw_vs_on_cpu_with_scissor, false,") !=
              std::string::npos,
          "scissored unclipped draws keep using the scissor");
  }

  if (!passed) return 1;
  std::cout << "gpu timing window: PASS\n";
  return 0;
}
