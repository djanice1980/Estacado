#include "runtime_frame_wait.h"

#include "runtime_function_trace.h"
#include "runtime_memory_access.h"
#include "ppc_recomp_shared.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <immintrin.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" PPC_FUNC(__imp__sub_825A4278);

namespace {
std::atomic<uint8_t> frame_wait_mode{static_cast<uint8_t>(frame_wait::kDefaultMode)};

struct Counters {
    std::atomic<uint64_t> calls{};
    std::atomic<uint64_t> waits{};
    std::atomic<uint64_t> passes{};
    std::atomic<uint64_t> timer_steps{};
    std::atomic<uint64_t> waited_us{};
};
Counters counters;

uint64_t QpcNow() {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return uint64_t(value.QuadPart);
}

uint64_t QpcFrequency() {
    static const uint64_t frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return uint64_t(value.QuadPart);
    }();
    return frequency;
}

HANDLE ThreadStepTimer() {
    thread_local HANDLE timer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    return timer;
}

void WaitStep(uint64_t microseconds) {
    const HANDLE timer = ThreadStepTimer();
    LARGE_INTEGER due;
    due.QuadPart = -int64_t(microseconds * 10);  // relative, 100 ns units
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(timer, INFINITE);
    } else {
        SwitchToThread();
    }
}

double DoubleFromBits(uint64_t bits) noexcept {
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void ReportCounters(uint64_t calls) {
    const uint64_t waits = counters.waits.load(std::memory_order_relaxed);
    std::printf("RUNTIME_FRAME_WAIT calls=%llu waits=%llu passes=%llu timer_steps=%llu "
                "mean_wait_us=%.1f\n",
                static_cast<unsigned long long>(calls), static_cast<unsigned long long>(waits),
                static_cast<unsigned long long>(counters.passes.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(counters.timer_steps.load(std::memory_order_relaxed)),
                waits ? double(counters.waited_us.load(std::memory_order_relaxed)) / double(waits)
                      : 0.0);
    std::fflush(stdout);
}
}  // namespace

void InitializeRuntimeFrameWait() {
    const char* value = std::getenv("DARKNESS_FRAME_WAIT_MODE");
    if (value && value[0] >= '0' && value[0] <= '4' && value[1] == '\0') {
        frame_wait_mode.store(static_cast<uint8_t>(value[0] - '0'), std::memory_order_relaxed);
    }
    std::printf("RUNTIME_FRAME_WAIT_POLICY mode=%u source=%s\n",
                unsigned(frame_wait_mode.load(std::memory_order_relaxed)),
                value ? "environment" : "default");
    std::fflush(stdout);
}

// sub_825A4278(pool): see runtime_frame_wait.h. The passes below follow the
// generated function statement by statement; only the host wait between the
// pump and the lock re-entry is new.
PPC_FUNC(sub_825A4278) {
    using namespace frame_wait;
    const auto mode = static_cast<Mode>(frame_wait_mode.load(std::memory_order_relaxed));
    if (mode == Mode::kOriginal) {
        __imp__sub_825A4278(ctx, base);
        return;
    }
    PPC_FUNC_PROLOGUE();
    PPC_RUNTIME_FUNCTION_ENTER(kFunction, ctx, base);
    const uint64_t entry_lr = ctx.lr;
    const uint32_t pool = ctx.r3.u32;
    const uint32_t lock = pool + kPoolLockOffset;
    // The title's frame (stwu r1,-144(r1)); the time is written at r1 + 80.
    const uint32_t caller_stack = ctx.r1.u32;
    const uint32_t frame = caller_stack - kFrameBytes;
    PPC_STORE_U32(frame, caller_stack);
    ctx.r1.u64 = frame;
    const auto call = [&](auto&& function, uint32_t r3, uint32_t return_address) {
        ctx.r3.u64 = r3;
        ctx.lr = return_address;
        function(ctx, base);
    };
    const auto call_thunk = [&](auto&& function, uint32_t thunk, uint32_t r3,
                                uint32_t return_address) {
        ctx.ctr.u64 = thunk;  // mtctr rN; bctrl
        call(function, r3, return_address);
    };
    const auto ready = [&] {
        const uint32_t first = PPC_LOAD_U32(pool + kPoolFreeListOffset);
        return FreeItemReady(PPC_LOAD_U32(first + 4));
    };
    const auto now = [&](uint32_t return_address) {
        call(sub_820CB6C0, frame + kTimeOutputOffset, return_address);
        ctx.fpscr.disableFlushMode();
        return DoubleFromBits(PPC_LOAD_U64(ctx.r3.u32));
    };
    const auto finish = [&](uint32_t result) {
        ctx.r1.u64 = caller_stack;
        ctx.r3.u64 = result;
        ctx.lr = entry_lr;
    };

    call(sub_822360B8, kReleaseQueue, kReturnFirstPump);
    call(sub_820C85A8, lock, kReturnFirstEnter);
    while (PPC_LOAD_U32(pool + kPoolFlushOffset) != 0) {
        call(sub_820C88B0, lock, kReturnFlushLeave);
        call(sub_822360B8, kReleaseQueue, kReturnFlushPump);
        call(sub_828A7CF0, 10, kReturnFlushSleep);
        call_thunk(sub_82103DF8, kEnterThunk, lock, kReturnFlushEnter);
    }
    const uint64_t calls = counters.calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((calls & 0xFFFF) == 0) ReportCounters(calls);
    if (ready()) {
        call_thunk(sub_821D3700, kLeaveThunk, lock, kReturnLeaveReady);
        finish(1);
        return;
    }
    const double deadline = now(kReturnFirstTime) + DoubleFromBits(PPC_LOAD_U64(kTimeoutConstant));
    const uint64_t frequency = QpcFrequency();
    const uint64_t begin = QpcNow();
    const uint64_t spin_end = begin + frequency * kSpinMicroseconds / 1'000'000;
    thread_local RecentWaits recent;
    thread_local uint64_t step_estimate_us = kInitialStepEstimateMicroseconds;
    const uint64_t predicted_us = recent.Shortest();
    uint64_t passes = 0;
    uint64_t timer_steps = 0;
    uint32_t result = 0;
    for (;;) {
        const double time = now(kReturnPassTime);
        if (!(time < deadline)) {  // fcmpu + bge: at or past the deadline, or unordered
            call_thunk(sub_821D3700, kLeaveThunk, lock, kReturnLeaveTimeout);
            result = 0;
            break;
        }
        if (ready()) {
            call_thunk(sub_821D3700, kLeaveThunk, lock, kReturnLeaveReady);
            result = 1;
            break;
        }
        call(sub_820C88B0, lock, kReturnPassLeave);
        call(sub_822360B8, kReleaseQueue, kReturnPassPump);
        ++passes;
        // The only change: a host wait between passes, with the pool lock free.
        if (mode == Mode::kPauseThenTimer) {
            if (QpcNow() < spin_end) {
                _mm_pause();
            } else {
                WaitStep(kStepMicroseconds);
                ++timer_steps;
            }
        } else if (mode == Mode::kYield) {
            SwitchToThread();
        } else if (mode == Mode::kSleepThenSpin) {
            const uint64_t elapsed_us = (QpcNow() - begin) * 1'000'000 / frequency;
            if (SleepStepFits(elapsed_us, predicted_us, step_estimate_us)) {
                const uint64_t step_begin = QpcNow();
                WaitStep(kSleepStepRequestMicroseconds);
                const uint64_t actual_us = (QpcNow() - step_begin) * 1'000'000 / frequency;
                step_estimate_us = (step_estimate_us * 3 + actual_us) / 4;
                ++timer_steps;
            } else {
                _mm_pause();
            }
        }
        call_thunk(sub_82103DF8, kEnterThunk, lock, kReturnPassEnter);
    }
    const uint64_t waited_us = (QpcNow() - begin) * 1'000'000 / frequency;
    if (result) recent.Add(uint32_t(waited_us < UINT32_MAX ? waited_us : UINT32_MAX));
    counters.waits.fetch_add(1, std::memory_order_relaxed);
    counters.passes.fetch_add(passes, std::memory_order_relaxed);
    counters.timer_steps.fetch_add(timer_steps, std::memory_order_relaxed);
    counters.waited_us.fetch_add(waited_us, std::memory_order_relaxed);
    finish(result);
}
