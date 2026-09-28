#include "runtime_job_poll_wake.h"

#include "ppc_recomp_shared.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <immintrin.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>

// Live switch (developer tooling pokes this byte in isolated test processes;
// player builds set it once from DARKNESS_JOB_POLL_EARLY_WAKE).
std::atomic<bool> runtimeJobPollEarlyWake{true};

namespace {
constexpr uint64_t kSpinMicroseconds = 20;
constexpr uint64_t kStepMicroseconds = 200;

struct Counters {
    std::atomic<uint64_t> polls{};
    std::atomic<uint64_t> spin{};
    std::atomic<uint64_t> step{};
    std::atomic<uint64_t> full{};
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

// One high-resolution waitable timer per worker thread (Windows 10 1803+).
HANDLE ThreadStepTimer() {
    thread_local HANDLE timer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    return timer;
}

void WaitStep(HANDLE timer, uint64_t ticks) {
    // Relative due time in 100 ns units.
    LARGE_INTEGER due;
    due.QuadPart = -int64_t(ticks * 10'000'000 / QpcFrequency());
    if (due.QuadPart == 0) due.QuadPart = -1;
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(timer, INFINITE);
    } else {
        SwitchToThread();
    }
}

void ReportCounters(uint64_t polls) {
    const uint64_t spin = counters.spin.load(std::memory_order_relaxed);
    const uint64_t step = counters.step.load(std::memory_order_relaxed);
    const uint64_t full = counters.full.load(std::memory_order_relaxed);
    const uint64_t waited = counters.waited_us.load(std::memory_order_relaxed);
    std::printf("RUNTIME_JOB_POLL_WAKE polls=%llu spin=%llu step=%llu full=%llu "
                "mean_wait_us=%.1f\n",
                static_cast<unsigned long long>(polls), static_cast<unsigned long long>(spin),
                static_cast<unsigned long long>(step), static_cast<unsigned long long>(full),
                polls ? double(waited) / double(polls) : 0.0);
    std::fflush(stdout);
}
}  // namespace

void InitializeRuntimeJobPollEarlyWake() {
    const char* value = std::getenv("DARKNESS_JOB_POLL_EARLY_WAKE");
    if (value && (value[0] == '0' || value[0] == '1')) {
        runtimeJobPollEarlyWake.store(value[0] == '1', std::memory_order_relaxed);
    }
    std::printf("RUNTIME_JOB_POLL_WAKE_POLICY enabled=%u source=%s\n",
                runtimeJobPollEarlyWake.load(std::memory_order_relaxed) ? 1u : 0u,
                value ? "environment" : "default");
    std::fflush(stdout);
}

bool RuntimeJobPollEarlyWake(PPCContext& ctx, uint8_t* base, int64_t interval,
                             uint32_t alertable) {
    using namespace job_poll_wake;
    if (!runtimeJobPollEarlyWake.load(std::memory_order_relaxed) || alertable ||
        interval != kPollInterval || uint32_t(ctx.lr) != kSleepWrapperReturn ||
        PPC_LOAD_U32(ctx.r1.u32 + kCallerLrOffset) != kDependencyPollReturn) {
        return false;
    }
    HANDLE timer = ThreadStepTimer();
    if (!timer) return false;
    const uint32_t slot = uint32_t(PPC_LOAD_U64(ctx.r1.u32 + kCallerR30Offset));
    const uint32_t queue = uint32_t(PPC_LOAD_U64(ctx.r1.u32 + kCallerR31Offset));
    if (!slot || !queue) return false;
    // Volatile guest reads: other workers complete the dependency concurrently.
    const auto load8 = [base](uint32_t address) -> uint8_t {
        return *reinterpret_cast<volatile uint8_t*>(base + address);
    };
    const auto load16 = [base](uint32_t address) -> uint16_t {
        const uint16_t raw = *reinterpret_cast<volatile uint16_t*>(base + address);
        return uint16_t((raw >> 8) | (raw << 8));
    };
    const auto load32 = [base](uint32_t address) -> uint32_t {
        return __builtin_bswap32(*reinterpret_cast<volatile uint32_t*>(base + address));
    };
    const uint64_t frequency = QpcFrequency();
    uint64_t waited = 0;
    const WaitOutcome outcome = WaitForDependency(
        [&] { return DependencyResolved(slot, queue, load8, load16, load32); }, QpcNow,
        [] { _mm_pause(); }, [timer](uint64_t ticks) { WaitStep(timer, ticks); },
        frequency / 1000, frequency * kSpinMicroseconds / 1'000'000,
        frequency * kStepMicroseconds / 1'000'000, waited);
    switch (outcome) {
    case WaitOutcome::kSpin: counters.spin.fetch_add(1, std::memory_order_relaxed); break;
    case WaitOutcome::kStep: counters.step.fetch_add(1, std::memory_order_relaxed); break;
    case WaitOutcome::kFullInterval: counters.full.fetch_add(1, std::memory_order_relaxed); break;
    }
    counters.waited_us.fetch_add(waited * 1'000'000 / frequency, std::memory_order_relaxed);
    const uint64_t polls = counters.polls.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((polls & 0xFFFF) == 0) ReportCounters(polls);
    return true;
}
