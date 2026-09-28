#include "runtime_source_memory_coordinator.h"
#include <array>
#include <future>
#include <iostream>
#include <thread>

RuntimeSourceMemoryCoordinator* GuestCoordinatorFromPeer();
void GuestWriteFromPeer(uint32_t address);

int main(int argc, char**) {
    // Concurrent first access across separate translation units must publish
    // one fully constructed object without freezing its as-yet-unused mode.
    std::array<RuntimeSourceMemoryCoordinator*, 8> identities{};
    std::array<std::thread, 8> starters;
    std::atomic<bool> start{};
    for (size_t i = 0; i < starters.size(); ++i) {
        starters[i] = std::thread([&, i] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            identities[i] = i & 1 ? GuestCoordinatorFromPeer() : &RuntimeGuestSourceCoordinator();
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : starters) thread.join();
    for (auto* identity : identities) {
        if (identity != &RuntimeGuestSourceCoordinator()) {
            std::cerr << "guest coordinator identity differs across threads/translation units\n";
            return 1;
        }
    }
    if (argc > 1) {
        // Exercise the actual inline guest-scope gate in its own process: a
        // first write must still permanently exclude later tracking enable.
        GuestWriteFromPeer(0xC0001234u);
        auto& guest = RuntimeGuestSourceCoordinator();
        bool disabled_ok = !guest.TrackingEnabled() && !guest.ConfigureAtStartup(true);
        disabled_ok &= guest.ConfigureAtStartup(false);
        { RuntimeGuestSourceWriteScope next(0xC0001234u, 4, 18); }
        RuntimeSourceMemoryCoordinator::Snapshot copy(guest);
        disabled_ok &= !copy.Watch({0x1234u, 4}).id;
        std::cout << (disabled_ok ? "disabled guest scope PASS\n" : "disabled guest scope FAIL\n");
        return disabled_ok ? 0 : 1;
    }
    bool ok = true;
    RuntimeSourceMemoryCoordinator fast_disabled;
    ok &= !fast_disabled.BeginGuardedOperation();
    ok &= !fast_disabled.ConfigureAtStartup(true);
    ok &= fast_disabled.ConfigureAtStartup(false);
    RuntimeSourceMemoryCoordinator startup_disabled;
    ok &= startup_disabled.ConfigureAtStartup(false);
    ok &= !startup_disabled.BeginGuardedOperation();
    ok &= !startup_disabled.ConfigureAtStartup(true);
    RuntimeSourceMemoryCoordinator disabled;
    { RuntimeSourceMemoryCoordinator::Write first(disabled, {10, 4}); }
    ok &= !disabled.ConfigureAtStartup(true);
    { RuntimeSourceMemoryCoordinator::Snapshot copy(disabled); ok &= !copy.Watch({10, 4}).id; }
    RuntimeSourceMemoryCoordinator state;
    ok &= state.ConfigureAtStartup(true) && !state.ConfigureAtStartup(false);
    ok &= state.BeginGuardedOperation();
    int payload = 7;
    RuntimeSourceWriteEpochs::Ticket ticket;
    { RuntimeSourceMemoryCoordinator::Snapshot copy(state); ticket = copy.Watch({10, 4}); }
    std::promise<void> entered, finish;
    auto finished = finish.get_future();
    std::thread writer([&] {
        RuntimeSourceMemoryCoordinator::Write guard(state, {10, 4});
        payload = 9;
        entered.set_value(); finished.wait();
        payload = 11;
    });
    entered.get_future().wait();
    std::promise<void> attempting;
    auto reader = std::async(std::launch::async, [&] {
        attempting.set_value();
        RuntimeSourceMemoryCoordinator::Snapshot copy(state);
        return payload == 11 && !copy.Unchanged(ticket) && copy.Watch({10, 4}).id != 0;
    });
    attempting.get_future().wait();
    ok &= reader.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout;
    finish.set_value(); writer.join(); ok &= reader.get();
    // Nested writes on one thread are supported; unwinding releases the lock
    // and ends the tracked write before another snapshot may read the payload.
    try {
        RuntimeSourceMemoryCoordinator::Write outer(state, {10, 4});
        RuntimeSourceMemoryCoordinator::Write inner(state, {11, 1});
        throw 1;
    } catch (int) {}
    { RuntimeSourceMemoryCoordinator::Snapshot copy(state); ok &= copy.Watch({10, 4}).id != 0; }
    // A snapshot must also exclude a competing writer, including through a
    // recursive inner snapshot. Check stable payload and ticket until release.
    std::future<void> pending_write;
    std::promise<void> write_attempt;
    RuntimeSourceWriteEpochs::Ticket protected_ticket;
    {
        RuntimeSourceMemoryCoordinator::Snapshot outer(state);
        protected_ticket = outer.Watch({10, 4});
        pending_write = std::async(std::launch::async, [&] {
            write_attempt.set_value();
            RuntimeSourceMemoryCoordinator::Write write(state, {10, 4});
            payload = 99;
        });
        write_attempt.get_future().wait();
        {
            RuntimeSourceMemoryCoordinator::Snapshot inner(state);
            ok &= inner.Unchanged(protected_ticket) && payload == 11;
        }
        ok &= pending_write.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout;
        ok &= outer.Unchanged(protected_ticket) && payload == 11;
    }
    pending_write.get();
    {
        RuntimeSourceMemoryCoordinator::Snapshot copy(state);
        ok &= payload == 99 && copy.Retired(protected_ticket);
    }
    // Exercise actual coordinator writes and snapshots on competing threads.
    // Neither a torn pair nor a resurrected pre-write ticket is acceptable.
    int pair[2]{};
    std::atomic<bool> done{};
    std::thread changing([&] {
        for (int i = 1; i <= 20000; ++i) {
            RuntimeSourceMemoryCoordinator::Write write(state, {32, 8});
            pair[0] = i; pair[1] = i;
        }
        done.store(true, std::memory_order_release);
    });
    do {
        RuntimeSourceMemoryCoordinator::Snapshot copy(state);
        ok &= pair[0] == pair[1];
        auto watched = copy.Watch({32, 8});
        ok &= watched.id && copy.Unchanged(watched);
    } while (!done.load(std::memory_order_acquire));
    changing.join();
    auto& guest = RuntimeGuestSourceCoordinator();
    ok &= guest.ConfigureAtStartup(true);
    RuntimeSourceWriteEpochs::Ticket alias;
    { RuntimeSourceMemoryCoordinator::Snapshot copy(guest); alias = copy.Watch({0x1234, 4}); }
    GuestWriteFromPeer(0xC0001234u);
    { RuntimeSourceMemoryCoordinator::Snapshot copy(guest); ok &= !copy.Unchanged(alias); }
    { RuntimeGuestSourceWriteScope malformed(0xC0001234u, uint64_t(UINT32_MAX) + 1); }
    { RuntimeSourceMemoryCoordinator::Snapshot copy(guest); ok &= !copy.Watch({0x1234, 4}).id; }
    std::cout << (ok ? "source memory coordinator PASS\n" : "source memory coordinator FAIL\n");
    return ok ? 0 : 1;
}
