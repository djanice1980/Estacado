#include "runtime_owned_packet_write_lease.h"
#include <iostream>
#include <memory>
#include <thread>
using namespace runtime_owned_camera_source;
int main() {
    bool ok = true;
    RuntimeSourceMemoryCoordinator coordinator;
    ok &= coordinator.ConfigureAtStartup(true);
    std::mutex mutex;
    auto constants = std::make_unique<ConstantStore>();
    auto packets = std::make_unique<PacketConstantStore>();
    Source source; source.token = {71, 3};
    for (uint32_t i = 0; i < 32; ++i) source.packed[i] = 0x3F800000 + i;
    {
        RuntimeSourceMemoryCoordinator::Snapshot snapshot(coordinator);
        ok &= constants->Publish(source, 0x2000, snapshot.Watch({0x2000, 128})) != 0;
    }
    PacketWriteLease lease(mutex, coordinator, *constants, *packets, 0x2000);
    ok &= lease.Resume();
    uint32_t first[17]{0x000F4030}, second[17]{0x000F4040};
    for (uint32_t i = 0; i < 16; ++i) {
        first[i + 1] = source.packed[i]; second[i + 1] = source.packed[16 + i];
    }
    unsigned publications = 0;
    auto published = [&](auto, auto, Token token, auto, auto, auto, auto) {
        ++publications; ok &= token.publication == 71;
    };
    // Simulated native stores occur inside the same lease as qualification.
    { RuntimeSourceMemoryCoordinator::Write write(coordinator, {0x3000, 68}); }
    ok &= lease.Seal(0x3000, first, 17, published) && publications == 1;
    lease.Release();
    // A submission worker needs BOTH locks. Joining it models a native GPU wait;
    // this test would deadlock (and time out) if either lease lock survived.
    std::thread submission([&] {
        std::lock_guard camera(mutex);
        RuntimeSourceMemoryCoordinator::Write write(coordinator, {0x9000, 4});
    });
    submission.join();
    ok &= !lease.Active() && lease.Resume();
    { RuntimeSourceMemoryCoordinator::Write write(coordinator, {0x4000, 68}); }
    ok &= lease.Seal(0x4000, second, 17, published) && publications == 2;
    lease.Release();
    { RuntimeSourceMemoryCoordinator::Write reuse(coordinator, {0x2000, 128}); }
    ok &= !lease.Resume() && !lease.Active();
    // Completed packet survives scratch reuse and repeated execution.
    {
        std::lock_guard camera(mutex);
        RuntimeSourceMemoryCoordinator::Snapshot snapshot(coordinator);
        PacketConstantPublication copy;
        for (int replay = 0; replay < 2; ++replay) {
            ok &= packets->Copy(snapshot, 0x3000, 68, 0x4030, 16, copy);
            ok &= packets->Copy(snapshot, 0x4000, 68, 0x4040, 16, copy);
        }
        source.token = {72, 0};
        ok &= constants->Publish(source, 0x2000, snapshot.Watch({0x2000, 128})) != 0;
    }
    ok &= lease.Resume();
    // A synchronous scratch write during the fragment revokes its provenance.
    { RuntimeSourceMemoryCoordinator::Write reuse(coordinator, {0x2000, 4}); }
    ok &= lease.Seal(0x5000, first, 17, published) && publications == 2;
    lease.Release();
    std::cout << (ok ? "packet write lease PASS\n" : "packet write lease FAIL\n");
    return ok ? 0 : 1;
}
