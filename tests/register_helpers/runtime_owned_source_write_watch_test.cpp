#include "runtime_owned_source_write_watch.h"
#include <iostream>
#include <memory>

using namespace runtime_owned_camera_source;
using Coordinator = RuntimeSourceMemoryCoordinator;
constexpr uint32_t base = 0x1000, record = base + 192;

WriteDependencies Capture(Coordinator& owner) {
    Coordinator::Snapshot snapshot(owner);
    return WatchItem(snapshot, snapshot.Watch({base, 192}), 0xA0000000 + record);
}
bool Valid(Coordinator& owner, const WriteDependencies& value) {
    Coordinator::Snapshot snapshot(owner);
    return WritesUnchanged(snapshot, value);
}
int main() {
    bool ok = true;
    Coordinator owner;
    ok &= owner.ConfigureAtStartup(true);
    const auto original = Capture(owner);
    ok &= Valid(owner, original);
    // Registration changes only list/sort fields; retained payload stays usable.
    for (auto offset : {20u, 24u, 28u, 40u}) {
        Coordinator::Write write(owner, {record + offset, 4});
    }
    ok &= Valid(owner, original);
    auto store = std::make_unique<Store>();
    Publication publication;
    publication.job = 1; publication.render_owner = 2; publication.camera = 3;
    publication.buffer = 0xA0000000 + base;
    publication.compact = publication.buffer + 148;
    publication.count = 1;
    publication.items[0] = {0xA0000000 + record, 10, 11, 12};
    publication.writes[0] = original;
    publication.camera_current[0] = 0x80000000;
    const auto token = store->Publish(publication);
    Source copy;
    ok &= token && store->Copy({token, 0}, copy) && Valid(owner, copy.writes);
    // Every payload region invalidates before a write, even an identical rewrite.
    for (auto address : {base, base + 127, base + 128, base + 191,
                         record, record + 12, record + 16, record + 32,
                         record + 36, record + 44, record + 47}) {
        const auto watched = Capture(owner);
        ok &= Valid(owner, watched);
        {
            Coordinator::Write write(owner, {address, 1});
            ok &= !Valid(owner, watched);
        }
        ok &= !Valid(owner, watched);
        { Coordinator::Write restore(owner, {address, 1}); }
        ok &= !Valid(owner, watched);
    }
    // Old tokens still replay owned values; they cannot certify native bytes.
    ok &= store->Copy({token, 0}, copy) && copy.camera_current[0] == 0x80000000;
    ok &= copy.writes.prefix.id == original.prefix.id && !Valid(owner, copy.writes);
    {
        Coordinator::Snapshot snapshot(owner);
        ok &= !WritesUnchanged(snapshot, WatchItem(snapshot, {}, 0xA00010C0));
        ok &= !WritesUnchanged(snapshot, WatchItem(snapshot, original.prefix, 0xFFFFFFF0));
    }
    Coordinator disabled;
    ok &= disabled.ConfigureAtStartup(false) && !Valid(disabled, Capture(disabled));
    // Actual compact arena offsets: ordinary allocation advances the cursor,
    // but cursor reset/storage changes invalidate the captured native source.
    constexpr uint32_t arena_physical = 0x8000;
    const auto capture_arena = [&]() {
        Coordinator::Snapshot snapshot(owner);
        return WatchArena(snapshot, 0xA0008000, 0xA0001000, 4096, 240,
                          1, 0xC0001000, 240);
    };
    const auto arena_valid = [&](const ArenaDependency& arena) {
        Coordinator::Snapshot snapshot(owner);
        return ArenaUnchanged(snapshot, arena);
    };
    const auto arena = capture_arena();
    ok &= arena_valid(arena);
    { Coordinator::Write allocate(owner, {arena_physical + 1388, 4}, 256); }
    ok &= arena_valid(arena);
    { Coordinator::Write finish_batch(owner, {arena_physical + 1384, 4}, 2); }
    ok &= arena_valid(arena);
    publication.arena = arena;
    const auto arena_token = store->Publish(publication);
    for (auto offset : {0u, 8u, 12u, 1388u}) {
        const auto before = capture_arena();
        ok &= arena_valid(before);
        { Coordinator::Write replace(owner, {arena_physical + offset, 4}); }
        ok &= !arena_valid(before);
    }
    ok &= store->Copy({arena_token, 0}, copy) && copy.arena.batch == 1;
    ok &= copy.arena.header.id == arena.header.id && !arena_valid(copy.arena);
    {
        Coordinator::Snapshot snapshot(owner);
        ok &= !ArenaUnchanged(snapshot, WatchArena(snapshot, 0xA0008000,
            0xA0001000, 4096, 239, 1, 0xA0001000, 240));
        ok &= !ArenaUnchanged(snapshot, WatchArena(snapshot, 0xA0008000,
            0xA0001000, 4096, 4097, 1, 0xA0001000, 240));
        ok &= !ArenaUnchanged(snapshot, WatchArena(snapshot, 0xA0008000,
            0xA0001000, 4096, 240, 0, 0xA0001000, 240));
        ok &= !ArenaUnchanged(snapshot, WatchArena(snapshot, 0xA0008000,
            0xA0001000, 4096, 240, 1, 0xA0000FF0, 240));
    }
    Coordinator saturated;
    // Real selector rejects stale, ambiguous and wrong-backing candidates;
    // canonical aliases alone never confer identity.
    store->Clear();
    publication.writes[0] = Capture(owner);
    publication.arena = capture_arena();
    publication.backing = {71, base, 4096};
    const auto selected_token = store->Publish(publication);
    bool backing_enabled = true;
    SelectionStats statistics;
    const auto select = [&](uint32_t selected_record) {
        statistics = {};
        Coordinator::Snapshot snapshot(owner);
        return SelectWatchedSource(*store, snapshot, selected_record,
            [&](const BackingAllocation& value, uint32_t physical, uint32_t bytes) {
                return backing_enabled && value.generation == 71 &&
                    physical == record && bytes == 48;
            }, copy, &statistics);
    };
    ok &= select(0xC0000000 + record) && copy.token.publication == selected_token;
    ok &= statistics.calls == 1 && statistics.candidates == 1 && statistics.selected == 1;
    backing_enabled = false;
    ok &= !select(0xA0000000 + record) && copy.token.publication == selected_token;
    ok &= statistics.invalid_backing == 1 && statistics.eligible == 0;
    backing_enabled = true;
    ok &= !select(0xA0000000 + record + 48);
    ok &= store->Publish(publication) != 0;
    ok &= !select(0xA0000000 + record); // Never choose newest by timestamp.
    ok &= statistics.ambiguous == 1 && statistics.eligible == 2;
    { Coordinator::Write overwrite(owner, {base, 4}); }
    ok &= !select(0xA0000000 + record);
    ok &= statistics.changed[2] == 2 && statistics.missing[2] == 0;
    publication.writes[0] = Capture(owner);
    const auto reused = store->Publish(publication);
    ok &= select(0xA0000000 + record) && copy.token.publication == reused;
    { Coordinator::Write reset(owner, {arena_physical + 1388, 4}, 0); }
    ok &= !select(0xA0000000 + record) && copy.token.publication == reused;
    ok &= statistics.changed[1] == 3;
    ok &= statistics.cursor_reasons[4] == 3 && statistics.cursor_reason_unknown == 0;
    ok &= statistics.last_cursor_retirement.reasons == RuntimeSourceWriteEpochs::BackwardValue &&
        statistics.last_cursor_retirement.stored == 0 &&
        statistics.last_cursor_retirement.watched.address == arena_physical + 1388;
    ok &= saturated.ConfigureAtStartup(true);
    {
        Coordinator::Snapshot snapshot(saturated);
        // Distinct live dependencies fill capacity; identical watches now share.
        for (uint32_t i = 0; i < 510; ++i) ok &= snapshot.Watch({0x10000 + i, 1}).id != 0;
        const auto prefix = snapshot.Watch({base, 192});
        const auto partial = WatchItem(snapshot, prefix, 0xA0000000 + record);
        ok &= partial.record_head.id != 0 && partial.record_mode.id == 0;
        ok &= !WritesUnchanged(snapshot, partial);
    }
    std::cout << (ok ? "owned source write watch PASS\n" : "owned source write watch FAIL\n");
    return ok ? 0 : 1;
}
