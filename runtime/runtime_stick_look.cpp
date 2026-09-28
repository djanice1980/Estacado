// Right-stick look at high frame rates (V390): see runtime_stick_look_policy.h.
#include "runtime_stick_look.h"
#include "runtime_stick_look_policy.h"

#include "runtime_memory_access.h"
#include "ppc_recomp_shared.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" PPC_FUNC(__imp__sub_823FC650);
extern "C" PPC_FUNC(__imp__sub_823FCF68);

namespace {
std::atomic<bool> g_enabled{true};

// Game-thread state: the emitter and look() run on the title's game thread.
stick_look::Tracker g_tracker;
bool g_inEmitter = false;
int64_t g_frequency = 0;

struct Counters {
    uint64_t calls = 0;
    uint64_t sends = 0;
    uint64_t cadenceSends = 0;
    uint64_t hfrCalls = 0;
    uint64_t carriedLooks = 0;
};
Counters g_counters;

double LoadDouble(uint8_t* base, uint32_t address) {
    const uint64_t bits = PPC_LOAD_U64(address);
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

float LoadFloat(uint8_t* base, uint32_t address) {
    const uint32_t bits = PPC_LOAD_U32(address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool ClientInRange(uint32_t client) {
    return client >= 0x10000u && uint64_t(client) + stick_look::kSensYOffset + 4u <= 0x100000000ull;
}

void Report() {
    if ((g_counters.calls & 0x3FFF) != 1) return;
    std::printf("RUNTIME_STICK_LOOK calls=%llu hfr_calls=%llu sends=%llu cadence_sends=%llu "
                "carried_looks=%llu frame_ms=%.2f\n",
                static_cast<unsigned long long>(g_counters.calls),
                static_cast<unsigned long long>(g_counters.hfrCalls),
                static_cast<unsigned long long>(g_counters.sends),
                static_cast<unsigned long long>(g_counters.cadenceSends),
                static_cast<unsigned long long>(g_counters.carriedLooks),
                g_tracker.frameSeconds * 1000.0);
    std::fflush(stdout);
}
}  // namespace

void InitializeRuntimeStickLook() {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    g_frequency = frequency.QuadPart;
    const char* value = std::getenv("DARKNESS_STICK_LOOK_HFR");
    if (value && value[0] == '0') g_enabled.store(false, std::memory_order_relaxed);
    std::printf("RUNTIME_STICK_LOOK_POLICY enabled=%u interval_ms=%.3f min_frame_ms=%.3f "
                "source=%s\n",
                g_enabled.load(std::memory_order_relaxed) ? 1u : 0u,
                double(stick_look::kHfrInterval) * 1000.0,
                stick_look::kMinHfrFrameSeconds * 1000.0, value ? "environment" : "default");
    std::fflush(stdout);
}

// The timed look emitter (client, interval): the title's own function runs
// in every case; per-frame calls get the 1/300 s interval while frames are at
// least 1/250 s apart, and the cadence clock follows each stored look time.
PPC_FUNC(sub_823FC650) {
    using namespace stick_look;
    const uint32_t client = ctx.r3.u32;
    if (!g_enabled.load(std::memory_order_relaxed) || !ClientInRange(client)) {
        __imp__sub_823FC650(ctx, base);
        return;
    }
    if (client != g_tracker.client) ResetFor(g_tracker, client);
    const float interval = float(ctx.f1.f64);
    const double before = LoadDouble(base, client + kLastLookTimeOffset);
    if (!IsTitleInterval(interval)) {
        // Another caller's own interval (sub_823FC818, 0.05 s): unchanged; a
        // send there is one the title makes too.
        __imp__sub_823FC650(ctx, base);
        const double after = LoadDouble(base, client + kLastLookTimeOffset);
        if (after != before) {
            g_tracker.haveCadence = true;
            g_tracker.cadenceTime = after;
            g_tracker.carryYaw = 0.0;
            g_tracker.carryPitch = 0.0;
        }
        return;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    NoteFrame(g_tracker, now.QuadPart, g_frequency);
    SyncCadence(g_tracker, before);
    const bool hfr = UseHfrInterval(g_tracker);
    ++g_counters.calls;
    if (hfr) {
        ++g_counters.hfrCalls;
        ctx.f1.f64 = double(kHfrInterval);
    }
    g_inEmitter = true;
    __imp__sub_823FC650(ctx, base);
    g_inEmitter = false;
    const double after = LoadDouble(base, client + kLastLookTimeOffset);
    if (after != before) {
        ++g_counters.sends;
        if (AfterSend(g_tracker, after, LoadFloat(base, client + kTimeScaleOffset), interval)) {
            ++g_counters.cadenceSends;
        }
    }
    Report();
}

// look(client, yaw, pitch): the emitter's call carries sub-unit remainders
// (runtime_stick_look_policy.h); every other caller (bound mouse moves, the
// native mouse look) reaches the title's function unchanged.
PPC_FUNC(sub_823FCF68) {
    using namespace stick_look;
    const uint32_t client = ctx.r3.u32;
    if (g_inEmitter && uint32_t(ctx.lr) == kLookReturn && client == g_tracker.client) {
        const Look look = CarryLook(g_tracker, float(ctx.f1.f64), float(ctx.f2.f64),
                                    LoadFloat(base, client + kSensXOffset),
                                    LoadFloat(base, client + kSensYOffset));
        if (FloatBits(look.yaw.argument) != FloatBits(float(ctx.f1.f64)) ||
            FloatBits(look.pitch.argument) != FloatBits(float(ctx.f2.f64))) {
            ++g_counters.carriedLooks;
            ctx.f1.f64 = double(look.yaw.argument);
            ctx.f2.f64 = double(look.pitch.argument);
        }
    }
    __imp__sub_823FCF68(ctx, base);
}
