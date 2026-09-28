#pragma once
#include "runtime_owned_camera_source.h"
#include "runtime_source_memory_coordinator.h"

namespace runtime_owned_camera_source {
struct ConstantPublication {
    uint64_t id{};
    Source source{};
    uint32_t physical{};
    RuntimeSourceWriteEpochs::Ticket write{};
};
// Completed native c12..19 payloads. Retain source values across later native
// retirement; the destination write ticket governs copying into a packet.
class ConstantStore {
public:
    static constexpr size_t kCapacity = 256;
    uint64_t Publish(const Source& source, uint32_t physical,
                     RuntimeSourceWriteEpochs::Ticket write) noexcept {
        if (!source.token.publication || !write.id || !write.version ||
            uint64_t(physical) + 128 > 0x20000000ull || (physical & 15) ||
            size_ == kCapacity || sequence_ == UINT64_MAX) return 0;
        const auto id = ++sequence_;
        entries_[size_++] = {id, source, physical, write};
        return id;
    }
    bool Copy(const RuntimeSourceMemoryCoordinator::Snapshot& snapshot,
              uint32_t physical, uint32_t bytes, ConstantPublication& output) const {
        if (!bytes || (physical & 3) || (bytes & 3) ||
            uint64_t(physical) + bytes > 0x20000000ull) return false;
        const ConstantPublication* found = nullptr;
        for (size_t i = 0; i < size_; ++i) {
            const auto& entry = entries_[i];
            if (physical < entry.physical || uint64_t(physical) + bytes >
                uint64_t(entry.physical) + 128 || !snapshot.Unchanged(entry.write)) continue;
            if (found) return false;
            found = &entry;
        }
        if (!found) return false;
        output = *found; return true;
    }
    void Clear() noexcept { size_ = 0; }
    size_t Size() const noexcept { return size_; }
    size_t Retire(const RuntimeSourceMemoryCoordinator::Snapshot& snapshot) {
        const size_t before = size_;
        size_t retained = 0;
        for (size_t i = 0; i < size_; ++i) {
            if (snapshot.Retired(entries_[i].write)) continue;
            if (retained != i) entries_[retained] = entries_[i];
            ++retained;
        }
        size_ = retained;
        return before - retained;
    }
private:
    std::array<ConstantPublication, kCapacity> entries_{};
    size_t size_{};
    uint64_t sequence_{};
};
} // namespace runtime_owned_camera_source
