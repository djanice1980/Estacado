#include <rex/memory/host_write_scope.h>
#include <iostream>
#include "runtime_host_write_callbacks.h"
#include <future>
#include <vector>

struct State { std::vector<unsigned> events; unsigned active{}; bool ok = true; };
static void* Begin(void* context, uint32_t physical, uint32_t bytes) noexcept {
    auto& state = *static_cast<State*>(context);
    state.ok &= physical == 128 && bytes == 16;
    state.events.push_back(++state.active);
    return nullptr;
}
static void End(void* context, void* token) noexcept {
    auto& state = *static_cast<State*>(context);
    state.ok &= token == nullptr && state.active != 0;
    state.events.push_back(10 + state.active--);
}
int main() {
    State state;
    rex::memory::HostWriteCallbacks callbacks{&state, Begin, End};
    bool ok = callbacks.Valid();
    try {
        rex::memory::HostWriteScope outer(callbacks, 128, 16);
        { rex::memory::HostWriteScope empty(callbacks, 128, 0); }
        rex::memory::HostWriteScope inner(callbacks, 128, 16);
        throw 1;
    } catch (int) {}
    ok &= state.ok && !state.active && state.events == std::vector<unsigned>{1, 2, 12, 11};
    { rex::memory::HostWriteScope disabled({}, 128, 16); }
    callbacks.end = nullptr;
    ok &= !callbacks.Valid(); // Installation must reject incomplete pairs.
    auto& coordinator = RuntimeGuestSourceCoordinator();
    ok &= coordinator.ConfigureAtStartup(true);
    rex::memory::HostWriteCallbacks real{nullptr, runtime_host_writes::Begin, runtime_host_writes::End};
    int payload = 0;
    RuntimeSourceWriteEpochs::Ticket ticket;
    std::future<void> writer;
    std::promise<void> attempting;
    {
        RuntimeSourceMemoryCoordinator::Snapshot snapshot(coordinator);
        ticket = snapshot.Watch({128, 16});
        writer = std::async(std::launch::async, [&] {
            attempting.set_value();
            rex::memory::HostWriteScope write(real, 128, 16);
            payload = 1;
        });
        attempting.get_future().wait();
        ok &= writer.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout;
        ok &= payload == 0;
    }
    writer.get();
    {
        RuntimeSourceMemoryCoordinator::Snapshot snapshot(coordinator);
        ok &= payload == 1 && ticket.id && !snapshot.Unchanged(ticket);
    }
    try {
        rex::memory::HostWriteScope outer(real, 128, 16);
        rex::memory::HostWriteScope inner(real, 128, 16);
        throw 1;
    } catch (int) {}
    ok &= runtime_host_writes::Current().depth == 0;
    { RuntimeSourceMemoryCoordinator::Snapshot snapshot(coordinator);
      ok &= snapshot.Watch({128, 16}).id != 0; }
    std::cout << (ok ? "host write scope PASS\n" : "host write scope FAIL\n");
    return ok ? 0 : 1;
}
