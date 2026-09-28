#pragma once

#include <cstdint>

// Early wake for the title's job-dependency poll (V299, title hook).
//
// The Darkness's job workers pop a job and, while the job it depends on has
// not finished, poll with KeDelayExecutionThread(1 ms) (sub_8222CC48; the
// sleep wrapper sub_828AC000 is called from 0x8222CD2C and returns to
// 0x8222CD30). At high frame rates several workers can queue behind one
// another this way, and each hand-off waits for a whole host sleep: the
// street's periodic ~10 ms frames were this convoy (CP idle, workers asleep).
//
// For exactly that call site the delay ends as soon as the dependency the
// guest is polling for has completed (the same guest bytes the loop reads
// next), otherwise after the full requested interval, as before. The hook
// only reads guest memory; the guest re-checks the condition itself and takes
// its own branch. Nothing is written, forced or skipped, and no clock
// changes. On by default since V300 (V299 same-view A/B: uncapped street
// p99 11.5 -> 7.5 ms, +11% FPS, ~0.1 core of worker CPU);
// DARKNESS_JOB_POLL_EARLY_WAKE=0 disables it, and a developer live switch
// flips it in test processes.
namespace job_poll_wake {

// sub_828AC000 stores its caller's LR, r30 and r31 in the red zone of the
// frame it then allocates (112 bytes): at r1+104, r1+88 and r1+96 when the
// import is reached.
constexpr uint32_t kSleepWrapperReturn = 0x828AC054;  // LR inside sub_828AC000
constexpr uint32_t kDependencyPollReturn = 0x8222CD30;  // caller: sub_8222CC48 loop
constexpr uint32_t kCallerLrOffset = 104;
constexpr uint32_t kCallerR30Offset = 88;  // job slot
constexpr uint32_t kCallerR31Offset = 96;  // job queue
constexpr int64_t kPollInterval = -10000;  // relative 1 ms in 100 ns units
constexpr uint16_t kNoDependency = 0xFFFF;

// The loop's exit condition (loc_8222CCF4): no dependency, or the queue's
// completion table (*(queue + 12)) holds 0 for the dependency index.
template <typename Load8, typename Load16, typename Load32>
bool DependencyResolved(uint32_t slot, uint32_t queue, Load8&& load8, Load16&& load16,
                        Load32&& load32) {
  const uint16_t dependency = load16(slot + 12);
  if (dependency == kNoDependency) return true;
  const uint32_t table = load32(queue + 12);
  return load8(table + dependency) == 0;
}

enum class WaitOutcome : uint8_t { kSpin, kStep, kFullInterval };

// Waits until ready() or until interval_ticks have elapsed. Spins (pause)
// for spin_ticks first, then waits in steps of step_ticks via wait_step().
// Returns how the wait ended; waited_ticks receives the elapsed time.
template <typename Ready, typename Now, typename Pause, typename WaitStep>
WaitOutcome WaitForDependency(Ready&& ready, Now&& now, Pause&& pause, WaitStep&& wait_step,
                              uint64_t interval_ticks, uint64_t spin_ticks,
                              uint64_t step_ticks, uint64_t& waited_ticks) {
  const uint64_t begin = now();
  const uint64_t deadline = begin + interval_ticks;
  const uint64_t spin_end = begin + (spin_ticks < interval_ticks ? spin_ticks : interval_ticks);
  for (uint64_t t = begin; t < spin_end; t = now()) {
    if (ready()) {
      waited_ticks = now() - begin;
      return WaitOutcome::kSpin;
    }
    pause();
  }
  for (uint64_t t = now(); t < deadline; t = now()) {
    if (ready()) {
      waited_ticks = t - begin;
      return WaitOutcome::kStep;
    }
    const uint64_t left = deadline - t;
    wait_step(left < step_ticks ? left : step_ticks);
  }
  waited_ticks = now() - begin;
  return WaitOutcome::kFullInterval;
}

}  // namespace job_poll_wake

// Runtime entry points (runtime_job_poll_wake.cpp).
struct PPCContext;
// True when this KeDelayExecutionThread call is the job-dependency poll and
// the early-wake hook handled the delay; false: run the ordinary delay.
bool RuntimeJobPollEarlyWake(PPCContext& ctx, uint8_t* base, int64_t interval,
                             uint32_t alertable);
void InitializeRuntimeJobPollEarlyWake();
