#pragma once

#include <cstdint>

// The title's frame-start wait (V385 experiment; V387 default: sleep then spin).
//
// Every frame (sub_820DE198) the game waits in sub_825A4278 until its frame
// item pool (game object + 108, three items) has a free item. Items return to
// the pool when the GPU has passed their fence: the release-queue pump
// sub_822360B8 checks the fence (sub_8285D8E0) and returns the item
// (sub_825A46A0). The title spins for that without a pause: every pass
// leaves the pool lock, pumps the queue, re-enters the lock, reads the time
// and checks the free list (0.5 s timeout). While the command processor limits
// the frame rate the main thread spends ~4 of every ~6 ms here (the title's
// own statistic at game + 4376) and holds a whole CPU core; with the test game
// restricted to 2 cores / 4 threads the heaviest street view fell from 169 to
// 103 FPS.
//
// The hook runs the same passes: the same guest calls with the same return
// addresses, lock order, checks, timeout and results, in the title's own
// 144-byte frame. It only waits on the host between passes (after the pump,
// before re-entering the lock). Nothing is forced, skipped or reordered; the
// guest code decides everything as before, and no clock changes.
namespace frame_wait {

constexpr uint32_t kFunction = 0x825A4278;
constexpr uint32_t kReleaseQueue = 0x82A8B380;     // r30 + 0x20000 + 6272
constexpr uint32_t kTimeoutConstant = 0x8209DCB0;  // double 0.5 (0x820A0000 - 9040)
constexpr uint32_t kEnterThunk = 0x82103DF8;       // b sub_820C85A8
constexpr uint32_t kLeaveThunk = 0x821D3700;       // b sub_820C88B0
constexpr uint32_t kPoolFreeListOffset = 8;
constexpr uint32_t kPoolFlushOffset = 36;
constexpr uint32_t kPoolLockOffset = 44;
constexpr uint32_t kFrameBytes = 144;
constexpr uint32_t kTimeOutputOffset = 80;

// Return addresses of the original's calls (the guest sees the same LR).
constexpr uint32_t kReturnFirstPump = 0x825A42A0;
constexpr uint32_t kReturnFirstEnter = 0x825A42AC;
constexpr uint32_t kReturnFlushLeave = 0x825A42D0;
constexpr uint32_t kReturnFlushPump = 0x825A42DC;
constexpr uint32_t kReturnFlushSleep = 0x825A42E4;
constexpr uint32_t kReturnFlushEnter = 0x825A42F0;
constexpr uint32_t kReturnLeaveReady = 0x825A431C;
constexpr uint32_t kReturnFirstTime = 0x825A4334;
constexpr uint32_t kReturnPassTime = 0x825A434C;
constexpr uint32_t kReturnPassLeave = 0x825A4374;
constexpr uint32_t kReturnPassPump = 0x825A4388;
constexpr uint32_t kReturnPassEnter = 0x825A4394;
constexpr uint32_t kReturnLeaveTimeout = 0x825A43A0;

// DARKNESS_FRAME_WAIT_MODE: 0 = the title's own function,
// 1 = the hook's passes without any host wait (must match 0),
// 2 = pause spin for kSpinMicroseconds, then high-resolution timer steps,
// 3 = SwitchToThread between passes,
// 4 = sleep then spin: timer steps while the predicted wait (the shortest of
//     the thread's recent waits) leaves a step plus kSpinMarginMicroseconds,
//     then pause-spin passes, so the release is seen within a pass.
// Timer steps are coarse on Windows: any request up to 500 us took ~0.51 ms
// here (measured 2026-09-27), so mode 2 notices a release up to ~0.5 ms late.
// Default since V387: mode 4. Heaviest street view, 1x uncapped, test game
// limited to 16/8/4 (no SMT)/4 (2 cores) logical CPUs: mode 0 164.6/160.6/
// 166.8/100.9 FPS, mode 2 165.3/160.6/172.0/113.2, mode 4 165.3/161.5/168.1/
// 107.0. Mode 2 frees the most CPU but starts frames up to ~0.5 ms late, and
// mouse look consumes the input gathered since the last frame start; mode 4
// sees the release within a pass and still sleeps through most of the wait.
enum class Mode : uint8_t {
    kOriginal = 0, kHostPasses = 1, kPauseThenTimer = 2, kYield = 3, kSleepThenSpin = 4
};
constexpr Mode kDefaultMode = Mode::kSleepThenSpin;
constexpr uint64_t kSpinMicroseconds = 20;
constexpr uint64_t kStepMicroseconds = 250;
constexpr uint64_t kSleepStepRequestMicroseconds = 500;
constexpr uint64_t kInitialStepEstimateMicroseconds = 600;
constexpr uint64_t kSpinMarginMicroseconds = 1000;
constexpr uint64_t kMinSleepWaitMicroseconds = 1500;
constexpr uint32_t kPredictionWaits = 8;

// Mode 4: sleep one more timer step only when it still ends a spin margin
// before the predicted release.
inline bool SleepStepFits(uint64_t elapsed_us, uint64_t predicted_us, uint64_t step_us) noexcept {
    return predicted_us >= kMinSleepWaitMicroseconds &&
           elapsed_us + step_us + kSpinMarginMicroseconds <= predicted_us;
}

// Mode 4: the thread's recent wait lengths; the prediction is the shortest.
struct RecentWaits {
    uint32_t us[kPredictionWaits]{};
    uint32_t count{};
    uint32_t next{};
    void Add(uint32_t value) noexcept {
        us[next] = value;
        next = (next + 1) % kPredictionWaits;
        if (count < kPredictionWaits) ++count;
    }
    uint32_t Shortest() const noexcept {
        uint32_t shortest = 0;
        for (uint32_t i = 0; i < count; ++i) {
            if (i == 0 || us[i] < shortest) shortest = us[i];
        }
        return shortest;
    }
};

// The loop's ready condition (loc_825A42FC / loc_825A4344): the free list's
// first node is not the tagged list end.
inline bool FreeItemReady(uint32_t first_node_link) noexcept { return (first_node_link & 1u) == 0; }

}  // namespace frame_wait

void InitializeRuntimeFrameWait();
