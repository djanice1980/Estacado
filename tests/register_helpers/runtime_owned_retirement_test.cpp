#include "runtime_owned_camera_packets.h"
#include "runtime_owned_source_write_watch.h"
#include <iostream>
#include <memory>
using namespace runtime_owned_camera_source;
using Coordinator = RuntimeSourceMemoryCoordinator;

int main() {
    bool ok = true;
    RuntimeSourceWriteEpochs epochs;
    const auto header = epochs.Watch({0x100, 16});
    const auto cursor = epochs.WatchForwardU32({0x200, 4}, 4, 4096);
    const auto pending = epochs.Begin({0x200, 4}, 8);
    ok &= !epochs.Unchanged(cursor) && !epochs.Retired(cursor);
    // Several ledger capacities of retired tickets never evict live dependencies.
    RuntimeSourceWriteEpochs::Ticket oldest;
    for (unsigned i = 0; i < 2048; ++i) {
        const auto t = epochs.Watch({0x300, 4});
        if (!i) oldest = t;
        ok &= t.id && epochs.Unchanged(t) && !epochs.Retired(t);
        const auto w = epochs.Begin({0x300, 4}); epochs.End(w);
        ok &= epochs.Retired(t) && !epochs.Unchanged(t);
    }
    ok &= epochs.Retired(oldest) && !epochs.Retired(cursor) && epochs.Unchanged(header);
    epochs.End(pending);
    ok &= epochs.Unchanged(cursor);
    ok &= epochs.WatchForwardU32({0x200, 4}, 8, 4096).id == cursor.id;
    ok &= epochs.Watch({0x100, 16}).id == header.id;
    const auto reset = epochs.Begin({0x200, 4}, 0); epochs.End(reset);
    ok &= epochs.Retired(cursor);
    // One write must invalidate every intersecting entry even when compaction
    // swaps a later entry into the current position. Neighbors remain live.
    RuntimeSourceWriteEpochs bulk;
    RuntimeSourceWriteEpochs::Ticket many[512];
    for (uint32_t i = 0; i < 512; ++i) many[i] = bulk.Watch({0x1000 + i * 4, 4});
    const auto spanning = bulk.Begin({0x1004, 510 * 4});
    for (uint32_t i = 1; i < 511; ++i)
        ok &= bulk.Retired(many[i]) && !bulk.Unchanged(many[i]);
    ok &= bulk.Unchanged(many[0]) && bulk.Unchanged(many[511]);
    ok &= !bulk.Watch({0x1004, 4}).id; // Still cannot watch an active write.
    bulk.End(spanning);
    ok &= bulk.Watch({0x1004, 4}).id > many[511].id;

    Coordinator owner; ok &= owner.ConfigureAtStartup(true);
    auto constants = std::make_unique<ConstantStore>();
    auto packets = std::make_unique<PacketConstantStore>();
    Source source; source.token = {1, 0}; source.camera_current[0] = 0x3F800000;
    uint32_t words[33]{}; words[0] = 0x001F4030;
    ConstantPublication scratch;
    PacketConstantPublication copy;
    uint64_t first_packet = 0, latest_packet = 0;
    for (unsigned i = 0; i < 1800; ++i) {
        // Scratch is reused every time; packet0 remains replayable throughout.
        { Coordinator::Write write(owner, {0x2000, 128}); }
        if (i > 1) { Coordinator::Write write(owner, {0x4000, 132}); }
        Coordinator::Snapshot snapshot(owner);
        if (constants->Size() == ConstantStore::kCapacity) constants->Retire(snapshot);
        const auto id = constants->Publish(source, 0x2000, snapshot.Watch({0x2000, 128}));
        ok &= id && constants->Copy(snapshot, 0x2000, 128, scratch);
        latest_packet = packets->Publish(snapshot, scratch, 0, i ? 0x4000 : 0x3000,
                                         words, 33, 1, 32);
        if (!i) first_packet = latest_packet;
        ok &= latest_packet && packets->Copy(snapshot, 0x3000, 132, 0x4030, 32, copy);
        ok &= copy.id == first_packet && copy.source.camera_current[0] == 0x3F800000;
        ok &= constants->Size() <= ConstantStore::kCapacity && packets->Size() <= PacketConstantStore::kCapacity;
    }
    ok &= latest_packet > first_packet + PacketConstantStore::kCapacity;
    {
        Coordinator::Snapshot snapshot(owner);
        ok &= packets->Copy(snapshot, 0x4000, 132, 0x4030, 32, copy) && copy.id == latest_packet;
    }
    { Coordinator::Write write(owner, {0x3000, 264}); }
    {
        Coordinator::Snapshot snapshot(owner);
        packets->Retire(snapshot);
        ok &= !packets->Copy(snapshot, 0x3000, 132, 0x4030, 32, copy);
        // The caller's already-owned values survive retirement unchanged.
        ok &= copy.id == latest_packet && copy.source.camera_current[0] == 0x3F800000;
    }

    auto sources = std::make_unique<Store>();
    Publication p; p.job = 1; p.render_owner = 2; p.camera = 3;
    p.buffer = 0xA0001000; p.compact = p.buffer + 148; p.count = 2;
    p.items[0] = {p.buffer + 192, 1, 2, 3}; p.items[1] = {p.buffer + 240, 4, 5, 6};
    {
        Coordinator::Snapshot snapshot(owner);
        p.arena.header = snapshot.Watch({0x5000, 16});
        p.arena.cursor_write = snapshot.WatchForwardU32({0x6000, 4}, 512, 4096);
        const auto prefix = snapshot.Watch({0x1000, 192});
        for (unsigned i = 0; i < 2; ++i) p.writes[i] = WatchItem(snapshot, prefix, p.items[i].record);
    }
    const auto token = sources->Publish(p);
    { Coordinator::Write write(owner, {0x10C0, 20}); }
    {
        Coordinator::Snapshot snapshot(owner);
        ok &= !PublicationRetired(snapshot, p); // Other item still usable.
        ok &= sources->RetireIf([&](const auto& v) { return PublicationRetired(snapshot, v); }) == 0;
    }
    { Coordinator::Write write(owner, {0x10F0, 20}); }
    {
        Coordinator::Snapshot snapshot(owner);
        ok &= sources->RetireIf([&](const auto& v) { return PublicationRetired(snapshot, v); }) == 1;
    }
    Source old;
    ok &= !sources->Copy({token, 0}, old) && sources->Publish(p) > token;
    Coordinator disabled; disabled.ConfigureAtStartup(false);
    { Coordinator::Snapshot snapshot(disabled); ok &= !snapshot.Retired(p.arena.header); }
    std::cout << (ok ? "owned retirement PASS\n" : "owned retirement FAIL\n");
    return ok ? 0 : 1;
}
