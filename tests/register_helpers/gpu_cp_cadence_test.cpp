#include <rex/graphics/cp_cadence_diagnostic.h>
#include <rex/graphics/cp_interrupt_timing.h>
#include <rex/graphics/cp_interrupt_wakeup.h>
#include <rex/graphics/swap_interval_diagnostic.h>
#include <thread>
#include <atomic>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <cstdlib>
#include <chrono>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

int main() {
  using rex::graphics::CpCadenceDiagnostic;
  auto require = [](bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
  };
  rex::graphics::CpInterruptTiming timing;
  rex::graphics::CpInterruptWakeup wake;
  const auto beforeRead = wake.Snapshot();
  require(!wake.Wait(beforeRead, std::chrono::milliseconds(0)),
          "no event must not indicate completion");
  wake.Notify(); // callback between memory read and sleeping
  require(wake.Wait(beforeRead, std::chrono::milliseconds(0)),
          "notification before wait must not be lost");
  const auto beforeConcurrent = wake.Snapshot();
  std::thread notifier([&] { wake.Notify(); });
  require(wake.Wait(beforeConcurrent, std::chrono::milliseconds(1000)),
          "concurrent advisory notification");
  notifier.join();
  require(!wake.Wait(wake.Snapshot(), std::chrono::milliseconds(0)),
          "consumed generation must not repeatedly wake");
  // A notification must never replace the caller's authoritative comparison.
  const unsigned guestValue = 4;
  const auto unrelated = wake.Snapshot();
  wake.Notify();
  require(wake.Wait(unrelated, std::chrono::milliseconds(0)) && guestValue != 0,
          "advisory wake must not fabricate guest acknowledgment");
  const auto sharedGeneration = wake.Snapshot();
  std::atomic<unsigned> observedWakeCount{0};
  std::thread waiterA([&] { if (wake.Wait(sharedGeneration, std::chrono::milliseconds(1000))) ++observedWakeCount; });
  std::thread waiterB([&] { if (wake.Wait(sharedGeneration, std::chrono::milliseconds(1000))) ++observedWakeCount; });
  wake.Notify();
  wake.Notify(); // repeated advisory notifications may coalesce
  waiterA.join(); waiterB.join();
  require(observedWakeCount == 2, "completion must wake all relevant concurrent waiters");
  require(!wake.Wait(wake.Snapshot(), std::chrono::milliseconds(0)),
          "repeated notifications cannot leave a permanently signaled wait");
  // CP cancellation is checked by the caller after bounded timeout/recheck;
  // cancellation must not be confused with successful guest completion.
  std::atomic<bool> running{true};
  std::atomic<unsigned> exits{0};
  std::thread cancelled([&] {
    while (running.load()) {
      const auto generation = wake.Snapshot();
      if (guestValue == 0) { std::exit(1); }
      wake.Wait(generation, std::chrono::milliseconds(1));
    }
    ++exits;
  });
  running = false; // no notification: fallback still makes cancellation visible
  cancelled.join();
  require(exits == 1 && guestValue == 4, "cancellation cannot fabricate predicate success");
  timing.Add(1, {});
  require(timing.Finish().count == 0, "disabled correlation must be inert");
  timing.Begin(4096);
  require(timing.Token() == 4096, "correlation token");
  std::thread producer([&] { for (unsigned i=0; i<70; ++i)
    timing.Add(4096, {1, 1, 0xBF36F000, 4, 0, i, i+1, i+2}); });
  producer.join();
  const auto batch = timing.Finish();
  require(batch.count == 64 && batch.overflow == 6 && timing.Token() == 0,
          "bounded cross-thread correlation");
  require(batch.records[0].dispatch == 1 && batch.records[63].returned == 65,
          "correlation timestamps preserved");
  timing.Begin(8192);
  timing.Add(4096, {});
  timing.Add(8192, {2, 0x13, 0x1F36F000, 0, ~0u, 100, 0, 120});
  const auto next = timing.Finish();
  require(next.count == 1 && next.overflow == 0 && next.records[0].kind == 2,
          "late callback cannot contaminate another window");
  unsigned samples = 0;
  for (uint64_t i = 0; i < 1000000; ++i)
    samples += CpCadenceDiagnostic::ShouldSample(i);
  require(samples == 128, "diagnostic lifetime budget must be 128 windows");
  CpCadenceDiagnostic d;
  d.DrawWorkTime(99);
  require(d.draw_work.calls == 0, "disabled whole-draw timing is inert");
  d.PacketWorkTime(0, 99);
  require(d.packet_work[0].calls == 0, "disabled packet timing is inert");
  d.OcclusionWorkTime(0, 99);
  require(d.occlusion_work[0].calls == 0, "disabled ZPD timing is inert");
  d.DrawStageTime(0, 99);
  require(d.draw_stages[0].calls == 0, "disabled draw timing is inert");
  d.Begin(true, 4096, 100);
  d.DrawStageTime(0, 7); d.DrawStageTime(0, 3);
  d.DrawWorkTime(12); d.DrawWorkTime(5);
  d.PacketWorkTime(0, 7); d.PacketWorkTime(0, 3);
  d.PacketWorkTime(3, 9, 0x46); d.PacketWorkTime(3, 4, 0x47);
  d.PacketWorkTime(4, 999);
  d.OcclusionWorkTime(0, 12); d.OcclusionWorkTime(1, 8);
  d.OcclusionWorkTime(2, 7); d.OcclusionWorkTime(3, 999);
  d.DrawStageTime(1, 2); d.DrawStageTime(2, 9); d.DrawStageTime(3, 999);
  require(d.draw_stages[0].calls == 2 && d.draw_stages[0].us == 10 &&
          d.draw_stages[0].max_us == 7 && d.draw_stages[1].us == 2 &&
          d.draw_stages[2].us == 9 && d.wait_ticks == 0,
          "draw stages separate from PM4 wait accounting");
  require(d.draw_work.calls == 2 && d.draw_work.ticks == 17 &&
          d.draw_work.max_ticks == 12,
          "whole-draw timing retains its own bounded aggregate");
  require(d.packet_work[0].calls == 2 && d.packet_work[0].ticks == 10 &&
          d.packet_work[0].max_ticks == 7 && d.packet_work[3].ticks == 13 &&
          d.type3_opcode_work[0x46].calls == 1 &&
          d.type3_opcode_work[0x46].ticks == 9 &&
          d.type3_opcode_work[0x47].ticks == 4,
          "packet categories aggregate without overflow");
  uint64_t opcode_calls = 0, opcode_ticks = 0;
  for (const auto& work : d.type3_opcode_work) {
    opcode_calls += work.calls;
    opcode_ticks += work.ticks;
  }
  require(opcode_calls == d.packet_work[3].calls &&
          opcode_ticks == d.packet_work[3].ticks,
          "type-3 opcode attribution must conserve category work");
  require(d.occlusion_work[0].ticks == 12 && d.occlusion_work[1].ticks == 8 &&
          d.occlusion_work[2].ticks == 7,
          "ZPD nested stages retain distinct durations");
  d.Begin(true, 4097, 200);
  require(d.draw_stages[0].us == 0 && d.draw_work.calls == 0 &&
          d.packet_work[0].calls == 0 &&
          d.type3_opcode_work[0x46].calls == 0 &&
          d.occlusion_work[0].calls == 0,
          "draw timing resets between sparse windows");
  d.Begin(false, 1, 100);
  d.Wait(20, 1, 2, 3, 4, 5);
  require(!d.active && d.waits == 0 && d.begin_tick == 0, "default-off must be inert");
  d.Begin(true, 4096, 100);
  d.Wait(20, 1, 2, 3, 4, 5);
  d.Wait(10, 6, 7, 8, 9, 10);
  require(d.waits == 2 && d.wait_ticks == 30 && d.max_wait_ticks == 20 &&
          d.max_wait_address == 2 && d.max_wait_ref == 3 && d.max_wait_mask == 4,
          "whole-window aggregation must retain maximum wait identity");
  d.Begin(true, 4097, 200);
  require(!d.active && d.waits == 0 && d.max_wait_ticks == 0,
          "unsampled window must reset without stale attribution");
  d.Begin(true, 8192, 300);
  require(d.active && d.begin_tick == 300 && d.wait_ticks == 0, "new window reset");
  for (unsigned i = 0; i < 10; ++i) d.Wait(i + 1, 0x13, i, 0, ~0u, 256);
  d.Wait(20, 0x13, 0, 0, ~0u, 256);
  require(d.wait_group_count == 8 && d.wait_groups[0].count == 2 &&
          d.wait_groups[0].ticks == 21 && d.overflow_waits == 2 &&
          d.overflow_ticks == 19, "bounded grouping and overflow accounting");
  uint64_t sum = d.overflow_ticks, count = d.overflow_waits;
  for (const auto& g : d.wait_groups) { sum += g.ticks; count += g.count; }
  require(sum == d.wait_ticks && count == d.waits, "group totals must conserve waits");
  d.Begin(true, 8192, 301);
  d.Wait(1, 0x13, 4, 0, ~0u, 256);
  d.Wait(1, 0x13, 4, 1, ~0u, 256);
  d.Wait(1, 0x13, 4, 0, 0xFFFF, 256);
  d.Wait(1, 0x13, 4, 0, ~0u, 0);
  d.Wait(1, 0x14, 4, 0, ~0u, 256);
  require(d.wait_group_count == 5 && d.overflow_waits == 0,
          "full comparison identity must be retained and reset");
  d.Begin(false, 8192, 302);
  d.Wait(100, 1, 2, 3, 4, 5);
  require(d.wait_group_count == 0 && d.overflow_ticks == 0,
          "disabled grouping must remain inert");
  d.Begin(true, 4096, 400);
  d.Wait(100, 0x13, 4, 0, ~0u, 256, 2, 80);
  d.Wait(50, 0x13, 4, 0, ~0u, 256, 1, 40);
  require(d.wait_ticks == 150 && d.wait_groups[0].sleep_ticks == 120 &&
          d.wait_groups[0].sleeps == 3,
          "sleep accounting is nested, not added to wait duration");
  for (unsigned i = 1; i <= 8; ++i) d.Wait(10, 0x13, 4+i, 0, ~0u, 256, 1, 9);
  require(d.overflow_sleeps == 1 && d.overflow_sleep_ticks == 9,
          "overflow preserves sleep accounting");
  d.Begin(false, 4096, 500);
  d.Wait(10, 1, 2, 3, 4, 5, 1, 9);
  require(d.overflow_sleeps == 0 && d.wait_groups[0].sleeps == 0,
          "reset and disabled state clear nested accounting");
  std::ifstream file(std::string(DARKNESS_SOURCE_ROOT) +
      "/external/ReXGlue/src/graphics/command_processor.cpp");
  const std::string source((std::istreambuf_iterator<char>(file)), {});
  // V285 promoted the wake to the default: the accepted V283 player result
  // always ran with it, and without it the CP polls WAIT_REG_MEM with Sleep.
  require(source.find("REXCVAR_DEFINE_BOOL(embedded_interrupt_wait_wakeup, true") != std::string::npos,
          "advisory wake must default on (accepted player configuration)");
  const auto wakeSnapshot = source.find("interrupt_wakeup ? cp_interrupt_wakeup.Snapshot()");
  const auto memoryObservation = source.find("raw_value =", wakeSnapshot);
  const auto advisoryWait = source.find("cp_interrupt_wakeup.Wait(wake_generation", memoryObservation);
  require(wakeSnapshot != std::string::npos && memoryObservation > wakeSnapshot &&
          advisoryWait > memoryObservation && source.find("} while (!matched);", advisoryWait) != std::string::npos,
          "wake must register before read and always retry real predicate");
  std::ifstream runtimeFile(std::string(DARKNESS_SOURCE_ROOT) + "/runtime/runtime_graphics.cpp");
  const std::string runtimeSource((std::istreambuf_iterator<char>(runtimeFile)), {});
  const auto callback = runtimeSource.find("routine(context, base);");
  const auto notification = runtimeSource.find("adapter.interrupt_completed(adapter.gpu)", callback);
  require(callback != std::string::npos && notification > callback,
          "advisory notification must follow actual callback");
  require(source.find("REXCVAR_DEFINE_BOOL(embedded_cp_cadence_diagnostics, false") !=
              std::string::npos, "opt-in flag missing");
  require(source.find("REXCVAR_DEFINE_BOOL(embedded_swap_interval_diagnostics, false") !=
              std::string::npos, "swap distribution must default off");
  require(source.find("REXCVAR_DEFINE_INT32(embedded_swap_long_frame_us, 0,") !=
              std::string::npos, "per-frame long-frame capture must default off");
  {
    // Long-frame records are written with the window log (one file open per
    // window), never from the per-swap path.
    const auto window_log = source.find("void CommandProcessor::LogSwapIntervalWindow(");
    const auto long_write = source.find("TraceSwapLongFrames(swap_intervals_, frequency, tsc_hz);");
    const auto swap_handler = source.find("bool CommandProcessor::ExecutePacketType3_XE_SWAP(");
    require(window_log != std::string::npos && long_write > window_log &&
                swap_handler > long_write &&
                source.find("TraceSwapLongFrames(", swap_handler) == std::string::npos,
            "long-frame records must be written only by the window log");
  }
  require(source.find("!kernel_state_ && REXCVAR_GET(embedded_cp_cadence_diagnostics)") !=
              std::string::npos, "embedded-only gate missing");
  const auto store = source.find("memory::store(memory_->TranslatePhysical(address), data_value);");
  const auto marker = source.find("++cp_cadence_.markers;", store);
  require(store != std::string::npos && marker > store && marker - store < 140,
          "marker observation must follow authoritative store");
  require(source.find("accounting_valid=%u") != std::string::npos,
          "overlap must not silently masquerade as zero other work");
  require(source.find("const uint64_t next_ordinal = cadence_swap_ordinal;") !=
              std::string::npos &&
          source.find("guest_frame_count_.load(std::memory_order_relaxed)") !=
              std::string::npos,
          "VBlank counter must not select or label real-swap windows");
  require(source.find("host_submission_valid=%u") != std::string::npos,
          "host fence availability must be explicit");
  const auto wait_observe = source.find("cp_cadence_.Wait(");
  const auto wait_observe_end = source.find('}', wait_observe);
  require(wait_observe != std::string::npos && wait_observe_end != std::string::npos,
          "wait observer missing");
  const auto observation = source.substr(wait_observe, wait_observe_end - wait_observe);
  for (const char* forbidden : {"store", "Sleep", "TraceEmbedded", "fprintf", "WriteRegister"})
    require(observation.find(forbidden) == std::string::npos,
            "per-wait observation must not log, sleep or alter guest state");
  rex::graphics::SwapIntervalDiagnostic intervals;
  intervals.AddIdle(10); intervals.AddWait(20); intervals.AddIssueSwap(30);
  intervals.AddDraw(); intervals.AddType0(4); intervals.AddZpd(50);
  require(intervals.frame_idle_ticks == 0 && intervals.frame_wait_ticks == 0 &&
          intervals.frame_issue_swap_ticks == 0 && intervals.frame_draws == 0 &&
          intervals.frame_type0_words == 0 && intervals.frame_zpd_ticks == 0,
          "disabled work observer must be inert");
  require(!intervals.Observe(false, 2, 100, 200, 10'000) && intervals.count == 0,
          "disabled interval observer must be inert");
  rex::graphics::SwapIntervalDiagnostic drawStages;
  drawStages.AddSampledDraw(1, 2, 3, 1);
  require(drawStages.frame_sampled_draws == 0 && !drawStages.ShouldSampleDraw(),
          "sampled draw timing must default off");
  drawStages.active = true;
  for (unsigned i = 0; i < 63; ++i) {
    drawStages.AddDraw();
    require(!drawStages.ShouldSampleDraw(), "draw sampling is bounded to every 64th draw");
  }
  drawStages.AddDraw();
  require(drawStages.ShouldSampleDraw(), "64th draw must be sampled");
  const uint64_t preTextureParts[rex::graphics::SwapIntervalDiagnostic::kPreTextureParts] =
      {4, 5, 6, 7, 8, 9, 10};
  drawStages.AddSampledDraw(40, 30, 20, 10, preTextureParts,
                            rex::graphics::SwapIntervalDiagnostic::kPreTextureParts);
  drawStages.Observe(true, 2, 1000, 167000, 16'600);
  require(drawStages.one_refresh.sampled_draws == 1 &&
          drawStages.one_refresh.sampled_draw_ticks[0] == 40 &&
          drawStages.one_refresh.sampled_draw_ticks[1] == 30 &&
          drawStages.one_refresh.sampled_draw_ticks[2] == 20 &&
          drawStages.one_refresh.sampled_binding_ticks == 10 &&
          drawStages.one_refresh.sampled_pre_texture_part_ticks[3] == 7 &&
          drawStages.one_refresh.sampled_pre_texture_part_counts[3] == 1 &&
          drawStages.one_refresh.draws == 64 && !drawStages.ShouldSampleDraw(),
          "sampled draw stages must follow the observed interval and reset");
  drawStages.AddSampledDraw(9, 8, 7, 6, preTextureParts, 2);
  drawStages.Observe(true, 3, 167000, 500000, 33'300);
  require(drawStages.doubled.sampled_pre_texture_part_counts[0] == 1 &&
          drawStages.doubled.sampled_pre_texture_part_ticks[1] == 5 &&
          drawStages.doubled.sampled_pre_texture_part_counts[2] == 0 &&
          drawStages.one_refresh.sampled_pre_texture_part_counts[6] == 1 &&
          drawStages.frame_sampled_pre_texture_part_counts[0] == 0,
          "partial sampled draws must preserve reached parts and frame grouping");
  intervals.active = true;
  uint64_t thread_cpu = 100;
  intervals.SetThreadCpuBaseline(thread_cpu, true);
  intervals.SetThreadCycleBaseline(thread_cpu * 3, true);
  uint64_t tick = 1000;
  for (uint64_t i = 0; i < intervals.kWindowSize; ++i) {
    const uint64_t us = i == 0 ? 16'667 : i == 1 ? 16'668 :
                        i == 2 ? 33'334 : i == 3 ? 33'335 :
                        i == 4 ? 65'000 : 16'000;
    const uint64_t next_tick = tick + us * 10;
    thread_cpu += us * 5;
    intervals.AddIdle(100); intervals.AddWait(200); intervals.AddIssueSwap(300);
    if (i == 0 || i == 2) {
      intervals.AddDraw(); intervals.AddType0(i == 0 ? 4 : 8);
      intervals.AddZpd(i == 0 ? 50 : 70);
      intervals.AddUpload(i == 0 ? 4096 : 8192);
      intervals.AddUpload(4096);
      intervals.AddRenderTargetUpdateReuse();
    }
    const bool ready = intervals.Observe(true, i + 2, tick, next_tick, us,
                                         thread_cpu, true, thread_cpu * 3, true);
    require(ready == (i + 1 == intervals.kWindowSize),
            "interval window must emit only after kWindowSize samples");
    tick = next_tick;
  }
  require(intervals.count == intervals.kWindowSize &&
          intervals.first_swap == 2 && intervals.last_swap == intervals.kWindowSize + 1 &&
          intervals.over_16667_us == 4 && intervals.over_33334_us == 2 &&
          intervals.histogram_ms[16] == intervals.kWindowSize - 3 &&
          intervals.histogram_ms[33] == 2 &&
          intervals.histogram_ms[64] == 1 &&
          intervals.min_us == 16'000 && intervals.max_us == 65'000,
          "swap histogram boundaries and exact deadline counts");
  require(intervals.one_refresh.count == intervals.kWindowSize - 3 &&
          intervals.doubled.count == 3 &&
          intervals.one_refresh.waits == intervals.kWindowSize - 3 &&
          intervals.doubled.waits == 3 &&
          intervals.one_refresh.accounting_invalid == 0 &&
          intervals.doubled.accounting_invalid == 0 &&
          intervals.one_refresh.elapsed_ticks ==
              intervals.one_refresh.idle_ticks + intervals.one_refresh.wait_ticks +
              intervals.one_refresh.issue_swap_ticks + intervals.one_refresh.remaining_ticks &&
          intervals.doubled.elapsed_ticks ==
              intervals.doubled.idle_ticks + intervals.doubled.wait_ticks +
          intervals.doubled.issue_swap_ticks + intervals.doubled.remaining_ticks,
          "work groups must distinguish doubled intervals and conserve elapsed time");
  require(intervals.one_refresh.thread_cpu_samples == intervals.kWindowSize - 3 &&
          intervals.doubled.thread_cpu_samples == 3 &&
          intervals.one_refresh.thread_cycle_samples == intervals.kWindowSize - 3 &&
          intervals.doubled.thread_cycle_samples == 3 &&
          intervals.one_refresh.thread_cycles == intervals.one_refresh.thread_cpu_100ns * 3 &&
          intervals.doubled.thread_cycles == intervals.doubled.thread_cpu_100ns * 3 &&
          intervals.max_doubled_run == 3 && intervals.doubled_run == 0 &&
          intervals.one_refresh.thread_cpu_100ns * 2 ==
              intervals.one_refresh.elapsed_ticks &&
          intervals.doubled.thread_cpu_100ns * 2 ==
              intervals.doubled.elapsed_ticks,
           "thread CPU samples must follow the classified swap intervals");
  require(intervals.one_refresh.draws == 1 && intervals.doubled.draws == 1 &&
          intervals.one_refresh.type0_packets == 1 &&
          intervals.doubled.type0_packets == 1 &&
          intervals.one_refresh.type0_words == 4 &&
          intervals.doubled.type0_words == 8 &&
          intervals.one_refresh.zpd_calls == 1 &&
          intervals.doubled.zpd_calls == 1 &&
          intervals.one_refresh.zpd_ticks == 50 &&
          intervals.doubled.zpd_ticks == 70,
          "draw, register and ZPD work must follow the classified interval");
  require(intervals.one_refresh.upload_bytes == 8192 &&
          intervals.doubled.upload_bytes == 12288 &&
          intervals.one_refresh.upload_ranges == 2 && intervals.doubled.upload_ranges == 2 &&
          intervals.one_refresh.max_frame_upload_bytes == 8192 &&
          intervals.doubled.max_frame_upload_bytes == 12288 &&
          intervals.one_refresh.render_target_update_reuses == 1 &&
          intervals.doubled.render_target_update_reuses == 1 &&
          intervals.frame_upload_bytes == 0 && intervals.frame_upload_ranges == 0 &&
          intervals.frame_render_target_update_reuses == 0,
          "upload and render-target reuse counts must follow the classified interval");
  intervals.ZpdEnded(3);
  intervals.ZpdEnded(4);
  intervals.ZpdPublished(1, 3);
  intervals.ZpdPublished(5, 2);
  intervals.ZpdAwait(0, false);
  intervals.ZpdAwait(1, true);
  intervals.ZpdAwait(2, false);
  require(intervals.zpd.ended == 2 && intervals.zpd.published == 2 &&
          intervals.zpd.max_depth == 4 && intervals.zpd.lag_swaps[1] == 1 &&
          intervals.zpd.lag_swaps[3] == 1 && intervals.zpd.slot_awaits == 1 &&
          intervals.zpd.write_awaits == 1 && intervals.zpd.index_awaits == 1 &&
          intervals.zpd.blocking_awaits == 1 && intervals.zpd_pending_now == 2,
          "deferred ZPD counters must follow ended/published/awaited reports");
  intervals.ResetWindow();
  require(intervals.zpd.ended == 0 && intervals.zpd.published == 0 &&
          intervals.zpd_pending_now == 2 && intervals.last_thread_cycles_valid &&
          intervals.last_thread_cycles == thread_cpu * 3,
          "window reset keeps the pending depth and cycle baseline only");
  require(intervals.count == 0 && intervals.histogram_ms[16] == 0 &&
          intervals.min_us == std::numeric_limits<uint64_t>::max() &&
           intervals.active && intervals.one_refresh.count == 0 &&
           intervals.frame_wait_ticks == 0 && intervals.one_refresh.draws == 0 &&
           intervals.one_refresh.zpd_ticks == 0 &&
           intervals.one_refresh.upload_bytes == 0 &&
           intervals.doubled.max_frame_upload_bytes == 0 &&
           intervals.doubled.render_target_update_reuses == 0 &&
          intervals.last_thread_cpu_valid &&
          intervals.last_thread_cpu_100ns == thread_cpu,
          "interval window reset must not leak old samples");
  intervals.Observe(true, intervals.kWindowSize + 2, tick, tick + 160'000, 16'000,
                    thread_cpu + 80'000, true);
  require(intervals.one_refresh.thread_cpu_samples == 1 &&
          intervals.one_refresh.thread_cpu_100ns == 80'000,
          "thread CPU baseline must survive a completed window");
  intervals.ResetWindow();
  require(!intervals.Observe(true, 1 + intervals.kWindowSize * intervals.kMaxWindows + 1,
                             tick, tick + 10, 1) && intervals.count == 0,
          "interval capture must stop at its lifetime limit");
  // V297 long-frame capture: a lowered threshold regroups intervals and keeps
  // each long frame with the frame before it; fence waits and full syncs
  // follow the classified interval; the window reset keeps the settings.
  {
    rex::graphics::SwapIntervalDiagnostic longs;
    longs.AddFenceWait(5); longs.AddFullSync();
    require(longs.frame_fence_wait_ticks == 0 && longs.frame_full_syncs == 0,
            "inactive fence observer must be inert");
    require(longs.long_interval_us == rex::graphics::SwapIntervalDiagnostic::kDoubledIntervalUs &&
                !longs.capture_long_frames,
            "default grouping stays two 60 Hz refreshes without per-frame capture");
    std::vector<rex::graphics::SwapIntervalDiagnostic::LongFrame> storage(
        rex::graphics::SwapIntervalDiagnostic::kLongFrameCapacity);
    require(sizeof(rex::graphics::SwapIntervalDiagnostic) < 4096,
            "capture storage must live outside the observer (compact command processor)");
    longs.active = true;
    longs.long_interval_us = 9'000;
    longs.capture_long_frames = true;
    longs.long_frames = storage.data();
    longs.long_frame_capacity = uint32_t(storage.size());
    longs.SetThreadCycleBaseline(1000, true);
    uint64_t t = 10'000;
    uint64_t cycles = 1000;
    const uint64_t frame_us[] = {5'500, 5'600, 11'000, 5'400, 12'000};
    for (uint64_t i = 0; i < 5; ++i) {
      longs.AddIdle(10 * (i + 1));
      longs.AddDraw();
      if (i == 2) { longs.AddFenceWait(4'000); longs.AddFullSync(); longs.AddUpload(64); }
      cycles += 100 * (i + 1);
      longs.Observe(true, i + 2, t, t + frame_us[i] * 10, frame_us[i], 0, false, cycles, true);
      t += frame_us[i] * 10;
    }
    require(longs.doubled.count == 2 && longs.one_refresh.count == 3,
            "threshold must regroup long intervals");
    require(longs.doubled.fence_wait_ticks == 4'000 && longs.doubled.full_syncs == 1 &&
                longs.one_refresh.fence_wait_ticks == 0 && longs.frame_fence_wait_ticks == 0,
            "fence waits and full syncs must follow the classified interval");
    require(longs.long_frame_count == 2 && longs.long_frames_dropped == 0,
            "each long frame is captured once");
    const auto& first = longs.long_frames[0];
    require(first.frame.swap == 4 && first.frame.interval_us == 11'000 &&
                first.frame.idle_ticks == 30 && first.frame.fence_wait_ticks == 4'000 &&
                first.frame.full_syncs == 1 && first.frame.upload_bytes == 64 &&
                first.frame.draws == 1 && first.frame.thread_cycles_valid &&
                first.frame.thread_cycles == 300,
            "captured frame carries its own work");
    require(first.previous.swap == 3 && first.previous.interval_us == 5'600 &&
                first.previous.idle_ticks == 20 && first.previous.thread_cycles == 200,
            "captured frame carries the frame before it");
    require(longs.long_frames[1].frame.swap == 6 && longs.long_frames[1].previous.swap == 5,
            "second long frame and its predecessor");
    longs.ResetWindow();
    require(longs.long_frame_count == 0 && longs.long_interval_us == 9'000 &&
                longs.capture_long_frames && longs.last_frame.swap == 6 &&
                longs.long_frames == storage.data() &&
                longs.long_frame_capacity == storage.size() && longs.doubled.count == 0,
            "window reset clears captures but keeps threshold, capture, storage and last frame");
    for (uint32_t i = 0; i < rex::graphics::SwapIntervalDiagnostic::kLongFrameCapacity + 3; ++i) {
      longs.Observe(true, 7 + i, t, t + 100'000, 10'000);
      t += 100'000;
    }
    require(longs.long_frame_count == rex::graphics::SwapIntervalDiagnostic::kLongFrameCapacity &&
                longs.long_frames_dropped == 3,
            "capture is bounded per window and counts drops");
    rex::graphics::SwapIntervalDiagnostic unowned;
    unowned.active = true;
    unowned.capture_long_frames = true;
    unowned.long_interval_us = 1;
    unowned.Observe(true, 2, 10, 20, 5);
    require(unowned.long_frame_count == 0 && unowned.long_frames_dropped == 1,
            "capture without storage only counts drops");
  }
  // Local cost estimate, not a gameplay A/B result or a hardware-independent limit.
  // Includes two QPC reads and aggregation, mirroring a sampled WAIT boundary.
  constexpr unsigned iterations = 200000;
  d.Begin(true, 4096, 1);
  const auto start = std::chrono::steady_clock::now();
  for (unsigned i = 0; i < iterations; ++i) {
    LARGE_INTEGER begin{}, end{};
    QueryPerformanceCounter(&begin);
    QueryPerformanceCounter(&end);
    d.Wait(uint64_t(end.QuadPart - begin.QuadPart), 3, i, 1, 0xFFFFFFFF, 0);
  }
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - start).count();
  require(d.waits == iterations, "benchmark aggregation was not retained");
  std::cout << "LOCAL_OBSERVER_COST iterations=" << iterations
            << " ns_per_sampled_wait=" << double(ns) / iterations
            << " aggregate_ticks=" << d.wait_ticks
            << " gameplay_overhead_validation=pending\n";
  std::cout << "CP cadence sampling/accounting policy PASS (not live timing validation)\n";
}
