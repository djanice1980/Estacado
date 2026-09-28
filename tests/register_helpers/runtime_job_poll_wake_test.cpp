// Job-dependency poll early wake (runtime_job_poll_wake.h): the delay ends
// early only when the exact condition the guest loop checks next is true, and
// otherwise lasts the full requested interval. Source policy: scoped to the
// one call site, reads guest memory only, off by default.
#include "runtime_job_poll_wake.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>

namespace {
struct FakeGuest {
    std::map<uint32_t, uint8_t> bytes;
    void Store16(uint32_t address, uint16_t value) {
        bytes[address] = uint8_t(value >> 8);
        bytes[address + 1] = uint8_t(value);
    }
    void Store32(uint32_t address, uint32_t value) {
        for (int i = 0; i < 4; ++i) bytes[address + i] = uint8_t(value >> (24 - 8 * i));
    }
    uint8_t Load8(uint32_t address) { return bytes[address]; }
    uint16_t Load16(uint32_t address) { return uint16_t(Load8(address) << 8 | Load8(address + 1)); }
    uint32_t Load32(uint32_t address) {
        return uint32_t(Load16(address)) << 16 | Load16(address + 2);
    }
};
}  // namespace

int main() {
    bool passed = true;
    const auto check = [&passed](bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            passed = false;
        }
    };
    using namespace job_poll_wake;

    {
        // Exit condition mirrors loc_8222CCF4.
        FakeGuest guest;
        const uint32_t slot = 0x40001000, queue = 0x40002000, table = 0x40003000;
        guest.Store32(queue + 12, table);
        const auto resolved = [&] {
            return DependencyResolved(
                slot, queue, [&](uint32_t a) { return guest.Load8(a); },
                [&](uint32_t a) { return guest.Load16(a); }, [&](uint32_t a) { return guest.Load32(a); });
        };
        guest.Store16(slot + 12, kNoDependency);
        check(resolved(), "no dependency (0xFFFF) exits");
        guest.Store16(slot + 12, 7);
        guest.bytes[table + 7] = 3;
        check(!resolved(), "pending dependency (nonzero completion byte) keeps waiting");
        guest.bytes[table + 7] = 0;
        check(resolved(), "completed dependency (zero byte) exits");
    }
    {
        // Wait outcomes with a fake clock: 1000 ticks = the requested interval.
        uint64_t clock = 0;
        uint64_t steps = 0;
        const auto now = [&] { return clock; };
        const auto pause = [&] { clock += 1; };
        const auto step = [&](uint64_t ticks) { clock += ticks; ++steps; };
        uint64_t waited = 0;
        uint64_t ready_at = 5;
        auto outcome = WaitForDependency([&] { return clock >= ready_at; }, now, pause, step,
                                         1000, 20, 200, waited);
        check(outcome == WaitOutcome::kSpin && waited == 5 && steps == 0,
              "dependency resolving within the spin returns without sleeping");
        clock = 0; steps = 0; ready_at = 450;
        outcome = WaitForDependency([&] { return clock >= ready_at; }, now, pause, step, 1000,
                                    20, 200, waited);
        check(outcome == WaitOutcome::kStep && waited >= 450 && waited < 1000 && steps == 3,
              "dependency resolving later ends at the next step, before the interval");
        clock = 0; steps = 0; ready_at = 5000;
        outcome = WaitForDependency([&] { return clock >= ready_at; }, now, pause, step, 1000,
                                    20, 200, waited);
        check(outcome == WaitOutcome::kFullInterval && waited == 1000,
              "unresolved dependency waits exactly the full requested interval");
    }
    {
        // Source policy.
        std::ifstream importsFile(std::string(DARKNESS_SOURCE_ROOT) + "/runtime/verified_imports.cpp");
        const std::string imports((std::istreambuf_iterator<char>(importsFile)), {});
        const auto hook = imports.find("if (!RuntimeJobPollEarlyWake(ctx, base, interval, alertable)) {");
        const auto delay = imports.find("DelayForGuestInterval(interval);", hook);
        check(hook != std::string::npos && delay != std::string::npos && delay - hook < 120,
              "ordinary delay runs whenever the hook does not handle the call");
        std::ifstream wakeFile(std::string(DARKNESS_SOURCE_ROOT) + "/runtime/runtime_job_poll_wake.cpp");
        const std::string wake((std::istreambuf_iterator<char>(wakeFile)), {});
        check(wake.find("std::atomic<bool> runtimeJobPollEarlyWake{true};") != std::string::npos &&
                  wake.find("DARKNESS_JOB_POLL_EARLY_WAKE") != std::string::npos,
              "early wake on by default since V300, reversible through the environment");
        check(wake.find("PPC_STORE") == std::string::npos,
              "the hook never writes guest memory");
        check(wake.find("PPC_LOAD_U32(ctx.r1.u32 + kCallerLrOffset) != kDependencyPollReturn") !=
                      std::string::npos &&
                  wake.find("uint32_t(ctx.lr) != kSleepWrapperReturn") != std::string::npos &&
                  wake.find("interval != kPollInterval") != std::string::npos,
              "scoped to the job-dependency call site and its 1 ms interval");
    }

    if (!passed) return 1;
    std::cout << "runtime job poll early wake: PASS\n";
    return 0;
}
