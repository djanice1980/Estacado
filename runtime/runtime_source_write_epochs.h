#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

// Bounded physical-range write epochs. Begin must precede the memory operation,
// End must follow it. Callers must cover every writer before trusting a ticket.
// This class does not itself intercept guest stores or certify an allocation.
class RuntimeSourceWriteEpochs {
public:
    RuntimeSourceWriteEpochs() = default;
    struct Range { uint32_t address{}, bytes{}; };
    struct Ticket { uint64_t id{}, version{}; };
    enum RetirementReason : uint32_t {
        Untyped = 1, DifferentRange = 2, OverlappingWrite = 4,
        EqualValue = 8, BackwardValue = 16, BeyondLimit = 32, TrackingFault = 64
    };
    struct Retirement {
        Ticket ticket{};
        uint32_t reasons{}, previous{}, stored{}, limit{};
        Range watched{}, write{};
    };
    // Bounded evidence only. Missing/evicted records are unknown and never
    // establish validity. Ordinary watches do not populate this cursor census.
    Retirement ExplainRetirement(Ticket ticket) const {
        auto lock = Lock();
        if (!ticket.id || !ticket.version) return {};
        if (failed_) return {ticket, TrackingFault};
        for (const auto& event : retirements_)
            if (event.ticket.id == ticket.id && event.ticket.version == ticket.version)
                return event;
        return {};
    }
    Ticket Watch(Range range) { return WatchImpl(range, 0, 0); }
    // A cursor ticket survives only exact typed U32 forward stores within capacity.
    // Any reset, identical rewrite, partial/bulk write or nested overlap revokes it.
    Ticket WatchForwardU32(Range range, uint32_t value, uint32_t limit) {
        if (range.bytes != 4 || (range.address & 3) || !value || value > limit) return {};
        return WatchImpl(range, value, limit);
    }
private:
    Ticket WatchImpl(Range range, uint32_t value, uint32_t limit) {
        auto lock = Lock();
        if (!Valid(range) || failed_) return {};
        for (const auto& write : writes_)
            if (write.id && Overlap(range, write.range)) return {};
        // Tickets describe a watched dependency, not publication identity.
        // Share an identical live dependency (including the current cursor).
        for (size_t i = 0; i < watches_used_; ++i) {
            const auto& w = watches_[i];
            if (w.id == w.version && w.range.address == range.address &&
                w.range.bytes == range.bytes && w.value == value && w.limit == limit)
                return {w.id, w.version};
        }
        if (watches_used_ == watches_.size()) {
            size_t retained = 0;
            for (size_t i = 0; i < watches_used_; ++i)
                if (watches_[i].id == watches_[i].version)
                    watches_[retained++] = watches_[i];
                else RemovePages(watches_[i].range);
            for (size_t i = retained; i < watches_used_; ++i) watches_[i] = {};
            watches_used_ = retained;
        }
        if (watches_used_ == watches_.size()) return {};
        const auto id = Next();
        if (!id) return {};
        watches_[watches_used_++] = {id, id, range, value, limit};
        AddPages(range);
        return {id, id};
    }
public:
    uint64_t Begin(Range range, std::optional<uint32_t> stored_u32 = {}) {
        auto lock = Lock();
        if (failed_) return 0;
        if (!Valid(range)) { Fail(); return 0; }
        const bool may_overlap = MayOverlapPages(range);
        bool overlapping_write = false;
        if (stored_u32 && may_overlap) for (const auto& write : writes_)
            overlapping_write |= write.id && Overlap(range, write.range);
        for (auto& write : writes_) if (!write.id) {
            const auto id = Next();
            if (!id) return 0;
            write = {id, range};
            // Keep active-write admission even when no watched page overlaps.
            if (!may_overlap) return id;
            // Remove terminally invalid watches immediately. Otherwise every
            // unrelated guest store pays for dead dependencies until the next
            // capture allocation. IDs remain monotonic; missing means retired.
            for (size_t i = 0; i < watches_used_;) {
                auto& watch = watches_[i];
                if (!Overlap(range, watch.range)) { ++i; continue; }
                const bool forward = watch.limit && stored_u32 && !overlapping_write &&
                    range.address == watch.range.address && range.bytes == 4 &&
                    *stored_u32 > watch.value && *stored_u32 <= watch.limit;
                if (forward) { watch.value = *stored_u32; ++i; }
                else {
                    if (watch.limit) {
                        uint32_t reasons = !stored_u32 ? Untyped : 0;
                        if (range.address != watch.range.address || range.bytes != 4)
                            reasons |= DifferentRange;
                        if (overlapping_write) reasons |= OverlappingWrite;
                        if (stored_u32) {
                            if (*stored_u32 == watch.value) reasons |= EqualValue;
                            if (*stored_u32 < watch.value) reasons |= BackwardValue;
                            if (*stored_u32 > watch.limit) reasons |= BeyondLimit;
                        }
                        retirements_[retirement_next_] = {{watch.id, watch.version}, reasons,
                            watch.value, stored_u32.value_or(0), watch.limit, watch.range, range};
                        retirement_next_ = (retirement_next_ + 1) % retirements_.size();
                    }
                    RemovePages(watch.range);
                    watch = watches_[--watches_used_];
                    watches_[watches_used_] = {};
                    // Recheck the swapped entry against this same write.
                }
            }
            return id;
        }
        Fail(); return 0;
    }
    void End(uint64_t id) {
        auto lock = Lock();
        if (failed_) return;
        for (auto& write : writes_) if (id && write.id == id) { write = {}; return; }
        Fail(); // Unknown/duplicate completion cannot establish quiescence.
    }
    bool Unchanged(Ticket ticket) const {
        auto lock = Lock();
        if (failed_ || !ticket.id || !ticket.version) return false;
        for (size_t i = 0; i < watches_used_; ++i) {
            const auto& watch = watches_[i];
            if (watch.id != ticket.id) continue;
            if (watch.version != ticket.version) return false;
            for (const auto& write : writes_)
                if (write.id && Overlap(watch.range, write.range)) return false;
            return true;
        }
        return false;
    }
    // Permanent retirement only. An in-flight permitted forward cursor write
    // makes Unchanged false temporarily, but must not retire the dependency.
    bool Retired(Ticket ticket) const {
        auto lock = Lock();
        if (failed_ || !ticket.id || !ticket.version) return true;
        for (size_t i = 0; i < watches_used_; ++i)
            if (watches_[i].id == ticket.id) return watches_[i].version != ticket.version;
        return true;
    }
    // Shutdown/reinitialization only: invalidates all tickets. Outstanding
    // writers forbid reset; IDs are never reused by successful resets either.
    // Faults are sticky: only destruction after external quiescence can recover.
    bool Reset() {
        auto lock = Lock();
        if (failed_) return false;
        for (const auto& write : writes_) if (write.id) return false;
        watches_ = {}; watches_used_ = 0; watched_pages_ = {};
        retirements_ = {}; retirement_next_ = 0;
        failed_ = sequence_ == UINT64_MAX;
        return !failed_;
    }
private:
    // Only the coordinator can select this mode. Its private epoch instance is
    // accessed exclusively while its payload mutex is owned, including End in
    // the Write destructor. Standalone epoch users retain their own mutex.
    friend class RuntimeSourceMemoryCoordinator;
    struct ExternallySerialized {};
    explicit RuntimeSourceWriteEpochs(ExternallySerialized) : externally_serialized_(true) {}
    std::unique_lock<std::mutex> Lock() const {
        std::unique_lock<std::mutex> lock(mutex_, std::defer_lock);
        if (!externally_serialized_) lock.lock();
        return lock;
    }
    static bool Valid(Range r) { return r.bytes && uint64_t(r.address) + r.bytes <= 0x20000000ull; }
    static bool Overlap(Range a, Range b) {
        return uint64_t(a.address) < uint64_t(b.address) + b.bytes &&
               uint64_t(b.address) < uint64_t(a.address) + a.bytes;
    }
    // Conservative 64KiB page occupancy. Counts include every page touched by
    // each live watch (max512), including both ends of a crossing range.
    // A hit still requires the original exact overlap test; only misses skip it.
    static constexpr unsigned kPageShift = 16;
    static uint32_t LastPage(Range r) { return (r.address + r.bytes - 1u) >> kPageShift; }
    void AddPages(Range r) {
        for (uint32_t p = r.address >> kPageShift; p <= LastPage(r); ++p) ++watched_pages_[p];
    }
    void RemovePages(Range r) {
        for (uint32_t p = r.address >> kPageShift; p <= LastPage(r); ++p) --watched_pages_[p];
    }
    bool MayOverlapPages(Range r) const {
        if (!watches_used_) return false;
        const auto first = r.address >> kPageShift, last = LastPage(r);
        // Large writes use the bounded exact scan rather than walking8192pages.
        if (last - first >= watches_used_) return true;
        for (uint32_t p = first; p <= last; ++p) if (watched_pages_[p]) return true;
        return false;
    }
    void Fail() { failed_ = true; }
    uint64_t Next() { if (sequence_ == UINT64_MAX) { Fail(); return 0; } return ++sequence_; }
    struct WatchEntry { uint64_t id{}, version{}; Range range{}; uint32_t value{}, limit{}; };
    struct WriteEntry { uint64_t id{}; Range range{}; };
    std::array<uint16_t, (0x20000000u >> kPageShift)> watched_pages_{};
    std::array<WatchEntry, 512> watches_{};
    std::array<WriteEntry, 64> writes_{};
    std::array<Retirement, 128> retirements_{};
    size_t retirement_next_{};
    size_t watches_used_{};
    uint64_t sequence_{};
    bool failed_{};
    const bool externally_serialized_{};
    mutable std::mutex mutex_;
};

