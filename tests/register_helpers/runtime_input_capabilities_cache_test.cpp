// Controller-capabilities cache hot-plug contract (runtime_input_capabilities_cache.h):
// the speed gain must never hide a connection change.
#include "runtime_input_capabilities_cache.h"

#include <cstdint>
#include <iostream>

namespace {
constexpr uint32_t kSuccess = 0;
constexpr uint32_t kNotConnected = 1167;  // ERROR_DEVICE_NOT_CONNECTED

struct Capabilities {
    uint32_t type = 0;
};

struct FakeDevice {
    bool connected[4]{};
    uint32_t calls = 0;
    uint32_t operator()(uint32_t slot, uint32_t, Capabilities* out) {
        ++calls;
        if (!connected[slot]) return kNotConnected;
        out->type = 1;
        return kSuccess;
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
    using Cache = RuntimeInputCapabilitiesCache<Capabilities>;

    {
        // Pad connected after launch: an empty slot is re-probed within 1 s.
        Cache cache;
        FakeDevice device;
        Capabilities caps;
        auto query = [&](uint32_t s, uint32_t f, Capabilities* c) { return device(s, f, c); };
        check(cache.Get(0, 1, 0, kSuccess, caps, query) == kNotConnected, "empty slot at launch");
        device.connected[0] = true;
        check(cache.Get(0, 1, 500, kSuccess, caps, query) == kNotConnected,
              "empty slot answer is reused briefly (the per-frame calls stay cheap)");
        check(cache.Get(0, 1, 1000, kSuccess, caps, query) == kSuccess && caps.type == 1,
              "a pad connected after launch is seen within one second");
        const uint32_t calls = device.calls;
        for (uint64_t t = 1000; t < 5900; t += 16) cache.Get(0, 1, t, kSuccess, caps, query);
        check(device.calls == calls, "a connected pad is not re-queried every frame");
        cache.Get(0, 1, 6000, kSuccess, caps, query);
        check(device.calls == calls + 1, "a connected pad is re-validated every 5 s");
    }
    {
        // Mid-game disconnect / reconnect reported by the state poll.
        Cache cache;
        FakeDevice device;
        device.connected[1] = true;
        Capabilities caps;
        auto query = [&](uint32_t s, uint32_t f, Capabilities* c) { return device(s, f, c); };
        cache.NoteStateResult(1, true);
        check(cache.Get(1, 1, 0, kSuccess, caps, query) == kSuccess, "connected at start");
        device.connected[1] = false;
        cache.NoteStateResult(1, false);  // XInputGetState fails on the next poll
        check(cache.Get(1, 1, 16, kSuccess, caps, query) == kNotConnected,
              "a disconnect is visible on the very next capabilities query");
        device.connected[1] = true;
        cache.NoteStateResult(1, true);   // XInputGetState succeeds again
        check(cache.Get(1, 1, 32, kSuccess, caps, query) == kSuccess,
              "a reconnect is visible on the very next capabilities query");
        const uint32_t calls = device.calls;
        cache.NoteStateResult(1, true);   // no transition: cache stays valid
        cache.Get(1, 1, 48, kSuccess, caps, query);
        check(device.calls == calls, "steady state polls do not invalidate the cache");
    }
    {
        // Different flags are distinct questions; out-of-range slots pass through.
        Cache cache;
        FakeDevice device;
        device.connected[2] = true;
        Capabilities caps;
        auto query = [&](uint32_t s, uint32_t f, Capabilities* c) { return device(s, f, c); };
        cache.Get(2, 1, 0, kSuccess, caps, query);
        cache.Get(2, 0, 1, kSuccess, caps, query);
        check(device.calls == 2, "a flags change re-queries");
        cache.Get(7, 1, 2, kSuccess, caps, query);
        check(device.calls == 3, "slots outside the cache always query");
    }

    if (passed) std::cout << "Input capabilities cache hot-plug tests passed\n";
    return passed ? 0 : 1;
}
