#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include "runtime_source_write_epochs.h"

// CPU-owned snapshots at the verified compact registration endpoint. IDs name
// immutable host data, NOT guest allocations, CPU updates or rendered frames.
// The caller serializes access. No guest address lookup or consuming read exists.
namespace runtime_owned_camera_source {
struct Item {
    uint32_t record{}, primary{}, geometry{}, shader{};
};

struct BackingAllocation {
    uint64_t generation{};
    uint32_t physical_base{}, bytes{};
};

// Write-dependency tickets, not native suballocation retirement or draw identity.
// Link/sort fields at record+20/+24/+28/+40 are deliberately not dependencies.
struct WriteDependencies {
    RuntimeSourceWriteEpochs::Ticket prefix{}, record_head{}, record_mode{}, record_optional{};
};

// Native arena header/cursor dependencies, separate from per-item payload.
// Batch is diagnostic metadata; forward allocation is not arena retirement.
struct ArenaDependency {
    uint32_t allocator{}, storage{}, capacity{}, batch{};
    RuntimeSourceWriteEpochs::Ticket header{}, cursor_write{};
};

struct Publication {
    uint32_t job{}, render_owner{}, camera{}, buffer{}, compact{}, count{};
    std::array<uint32_t, 16> camera_current{}; // camera+1760 at registration
    std::array<uint32_t, 32> packed{}; // completed native vector pair, not PC motion
    std::array<Item, 64> items{};
    BackingAllocation backing{}; // Point observation, not native arena/write version.
    ArenaDependency arena{};
    std::array<WriteDependencies, 64> writes{};
};

struct Token {
    uint64_t publication{};
    uint32_t item{};
};

struct Source {
    Token token{};
    uint32_t job{}, render_owner{}, camera{}, buffer{}, compact{};
    Item native_item{};
    std::array<uint32_t, 16> camera_current{};
    std::array<uint32_t, 32> packed{};
    BackingAllocation backing{};
    ArenaDependency arena{};
    WriteDependencies writes{};
};

class Store {
public:
    static constexpr size_t kCapacity = 256;

    // Full/invalid stores fail closed without replacing a retained source. IDs
    // survive Clear so an old token cannot silently select a new publication.
    uint64_t Publish(const Publication& value) noexcept {
        if (!Valid(value) || size_ == kCapacity || sequence_ == UINT64_MAX) return 0;
        auto& entry = entries_[size_++];
        entry.id = ++sequence_;
        entry.value = value;
        return entry.id;
    }

    bool Copy(Token token, Source& output) const noexcept {
        if (!token.publication) return false;
        for (size_t i = 0; i < size_; ++i) {
            const auto& entry = entries_[i];
            if (entry.id != token.publication) continue;
            const auto& value = entry.value;
            if (token.item >= value.count) return false;
            output = {token, value.job, value.render_owner, value.camera,
                value.buffer, value.compact, value.items[token.item],
                value.camera_current, value.packed, value.backing, value.arena, value.writes[token.item]};
            return true;
        }
        return false; // Leave the caller's previous owned copy unchanged.
    }

    void Clear() noexcept { size_ = 0; }
    size_t Size() const noexcept { return size_; }
    // Caller must prove permanent retirement, not temporary unavailability.
    // Already-owned downstream copies remain independent of these entries.
    template<class Retired> size_t RetireIf(Retired&& retired) {
        const size_t before = size_;
        size_t retained = 0;
        for (size_t i = 0; i < size_; ++i) {
            if (retired(entries_[i].value)) continue;
            if (retained != i) entries_[retained] = entries_[i];
            ++retained;
        }
        size_ = retained;
        return before - retained;
    }

    // Enumerate producer tokens; native selection must prove dependencies.
    template<class Visitor> void VisitItems(Visitor&& visitor) const {
        for (size_t i = 0; i < size_; ++i) {
            const auto& entry = entries_[i];
            for (uint32_t item = 0; item < entry.value.count; ++item)
                visitor(Token{entry.id, item}, entry.value, item);
        }
    }

private:
    static bool Valid(const Publication& value) noexcept {
        if (!value.job || !value.render_owner || !value.camera || !value.buffer ||
            (value.buffer & 15u) || !value.count || value.count > value.items.size() ||
            uint64_t(value.buffer) + 192u + uint64_t(value.count) * 48u > (uint64_t(1) << 32) ||
            uint64_t(value.buffer) + 148u != value.compact) return false;
        for (uint32_t i = 0; i < value.count; ++i) {
            if (value.items[i].record != uint64_t(value.buffer) + 192u + uint64_t(i) * 48u ||
                !value.items[i].primary) return false;
        }
        return true;
    }
    struct Entry { uint64_t id{}; Publication value{}; };
    std::array<Entry, kCapacity> entries_{};
    size_t size_{};
    uint64_t sequence_{};
};
} // namespace runtime_owned_camera_source
