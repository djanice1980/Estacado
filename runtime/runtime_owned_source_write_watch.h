#pragma once
#include "runtime_owned_camera_source.h"
#include "runtime_source_memory_coordinator.h"

namespace runtime_owned_camera_source {
inline ArenaDependency WatchArena(RuntimeSourceMemoryCoordinator::Snapshot& snapshot,
    uint32_t allocator, uint32_t storage, uint32_t capacity, uint32_t cursor,
    uint32_t batch, uint32_t buffer, uint32_t bytes) {
    uint32_t physical{}, storage_physical{}, buffer_physical{};
    if (!batch || cursor > capacity ||
        !RuntimeCanonicalGuestPhysicalRange(allocator, 1392, physical) ||
        !RuntimeCanonicalGuestPhysicalRange(storage, capacity, storage_physical) ||
        !RuntimeCanonicalGuestPhysicalRange(buffer, bytes, buffer_physical) ||
        buffer_physical < storage_physical ||
        uint64_t(buffer_physical) + bytes > uint64_t(storage_physical) + cursor) return {};
    // 8259DFA8 advances batch1384 before consumption; it is not retirement.
    // Track cursor1388 instead: only typed, strictly forward stores survive.
    // Reset/rewrite/unknown writes revoke permanently, even after reallocation.
    // Header watches still cover reconstruction and storage/capacity changes.
    return {allocator, storage, capacity, batch, snapshot.Watch({physical, 16}),
        snapshot.WatchForwardU32({physical + 1388, 4}, cursor, capacity)};
}
inline bool ArenaUnchanged(const RuntimeSourceMemoryCoordinator::Snapshot& snapshot,
                           const ArenaDependency& value) {
    return value.allocator && value.batch && snapshot.Unchanged(value.header) &&
        snapshot.Unchanged(value.cursor_write);
}
inline WriteDependencies WatchItem(RuntimeSourceMemoryCoordinator::Snapshot& snapshot,
    RuntimeSourceWriteEpochs::Ticket prefix, uint32_t record) {
    uint32_t physical{};
    if (!prefix.id || !RuntimeCanonicalGuestPhysicalRange(record, 48, physical)) return {};
    return {prefix, snapshot.Watch({physical, 20}),
        snapshot.Watch({physical + 32, 8}), snapshot.Watch({physical + 44, 4})};
}
inline bool WritesUnchanged(const RuntimeSourceMemoryCoordinator::Snapshot& snapshot,
                            const WriteDependencies& value) {
    return snapshot.Unchanged(value.prefix) && snapshot.Unchanged(value.record_head) &&
        snapshot.Unchanged(value.record_mode) && snapshot.Unchanged(value.record_optional);
}
inline bool PublicationRetired(const RuntimeSourceMemoryCoordinator::Snapshot& snapshot,
                               const Publication& value) {
    if (snapshot.Retired(value.arena.header) || snapshot.Retired(value.arena.cursor_write))
        return true;
    for (uint32_t i = 0; i < value.count; ++i) {
        const auto& w = value.writes[i];
        if (!snapshot.Retired(w.prefix) && !snapshot.Retired(w.record_head) &&
            !snapshot.Retired(w.record_mode) && !snapshot.Retired(w.record_optional))
            return false;
    }
    return true;
}
// Record equality narrows candidates; watched dependencies and backing identity
// qualify them. Duplicate eligible publications are ambiguous, even if identical.
struct SelectionStats {
    uint64_t calls{}, invalid_record{}, candidates{}, eligible{}, ambiguous{}, selected{};
    uint64_t invalid_arena{}, invalid_backing{};
    // arena header,cursor; source prefix,record head,mode,optional.
    std::array<uint64_t, 6> missing{}, changed{};
    // Bitwise reasons may overlap. Count unmatched/evicted evidence separately.
    std::array<uint64_t, 7> cursor_reasons{};
    uint64_t cursor_reason_unknown{};
    RuntimeSourceWriteEpochs::Retirement last_cursor_retirement{};
};
template<class BackingValidator>
bool SelectWatchedSource(const Store& store,
    const RuntimeSourceMemoryCoordinator::Snapshot& snapshot, uint32_t record,
    BackingValidator&& backing_valid, Source& output, SelectionStats* stats = nullptr) {
    if (stats) ++stats->calls;
    uint32_t selected_physical{};
    if (!RuntimeCanonicalGuestPhysicalRange(record, 48, selected_physical)) {
        if (stats) ++stats->invalid_record;
        return false;
    }
    Token selected{};
    bool ambiguous = false;
    store.VisitItems([&](Token token, const Publication& value, uint32_t item) {
        uint32_t physical{};
        if (!RuntimeCanonicalGuestPhysicalRange(value.items[item].record, 48, physical) ||
            physical != selected_physical) return;
        if (stats) ++stats->candidates;
        const auto& writes = value.writes[item];
        const RuntimeSourceWriteEpochs::Ticket tickets[] = {value.arena.header,
            value.arena.cursor_write, writes.prefix, writes.record_head,
            writes.record_mode, writes.record_optional};
        bool valid = value.arena.allocator && value.arena.batch;
        if (!valid && stats) ++stats->invalid_arena;
        for (size_t i = 0; i < 6; ++i) {
            if (!tickets[i].id || !tickets[i].version) {
                valid = false; if (stats) ++stats->missing[i];
            } else if (!snapshot.Unchanged(tickets[i])) {
                valid = false; if (stats) ++stats->changed[i];
                if (stats && i == 1) {
                    const auto event = snapshot.ExplainRetirement(tickets[i]);
                    if (!event.reasons) ++stats->cursor_reason_unknown;
                    else {
                        stats->last_cursor_retirement = event;
                        for (size_t bit = 0; bit < stats->cursor_reasons.size(); ++bit)
                            if (event.reasons & (1u << bit)) ++stats->cursor_reasons[bit];
                    }
                }
            }
        }
        if (!backing_valid(value.backing, physical, 48u)) {
            valid = false; if (stats) ++stats->invalid_backing;
        }
        if (!valid) return;
        if (stats) ++stats->eligible;
        if (selected.publication) ambiguous = true;
        selected = token;
    });
    if (ambiguous) { if (stats) ++stats->ambiguous; return false; }
    const bool copied = selected.publication && store.Copy(selected, output);
    if (copied && stats) ++stats->selected;
    return copied;
}
} // namespace runtime_owned_camera_source
