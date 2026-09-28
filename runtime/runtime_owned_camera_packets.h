#pragma once
#include "runtime_owned_camera_constants.h"

namespace runtime_owned_camera_source {
// Host-endian snapshot of one completed writer segment. Accept only the forms
// emitted by828714A0/82870AB0: exact Type2 padding and sequential Type0 constants.
// Validate the ENTIRE segment before emitting any provenance callback.
template<class Visitor>
bool VisitNativeConstantPackets(const uint32_t* words, uint32_t count, Visitor&& visit) {
    if (!words || !count || count > 65536) return false;
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (uint32_t offset = 0; offset < count;) {
            const uint32_t header = words[offset];
            if (header == 0x80000000u) { ++offset; continue; }
            const uint32_t size = ((header >> 16) & 0x3FFFu) + 1;
            const uint32_t first = header & 0x7FFFu;
            if ((header & 0xC0008000u) || first < 0x4000 ||
                first >= 0x4400 || (first & 15) || (size & 15) ||
                size > 0x4400 - first || size >= count - offset) return false;
            if (pass) visit(offset, first, size);
            offset += size + 1;
        }
    }
    return true;
}

struct PacketConstantPublication {
    uint64_t id{}, constant_id{};
    Source source{};
    uint32_t packet_physical{}, packet_bytes{}, payload_offset{};
    uint32_t first_register{}, source_word{}, words{};
    RuntimeSourceWriteEpochs::Ticket write{};
};
// Caller holds the SAME payload Snapshot from source qualification through
// actual native packet copy and Publish. Equality supplements that ownership;
// it never discovers a source. No native/GPU calls or waits occur in this store.
class PacketConstantStore {
public:
    static constexpr size_t kCapacity = 512;
    uint64_t Publish(RuntimeSourceMemoryCoordinator::Snapshot& snapshot,
                     const ConstantPublication& constant, uint32_t source_word,
                     uint32_t packet_physical, const uint32_t* packet,
                     uint32_t packet_words, uint32_t payload_offset,
                     uint32_t words) {
        if (!constant.id || !constant.source.token.publication || !packet ||
            !snapshot.Unchanged(constant.write) || (packet_physical & 3) ||
            packet_words < 2 || packet_words > 1025 ||
            uint64_t(packet_physical) + uint64_t(packet_words) * 4 > 0x20000000ull ||
            !words || source_word >= 32 || words > 32 - source_word ||
            !payload_offset || payload_offset >= packet_words ||
            words > packet_words - payload_offset ||
            sequence_ == UINT64_MAX) return 0;
        const uint32_t header = packet[0];
        const uint32_t first = header & 0x7FFFu;
        const uint32_t declared = ((header >> 16) & 0x3FFFu) + 1;
        if ((header & 0xC0008000u) || declared + 1 != packet_words ||
            first < 0x4000 || first >= 0x4400 || declared > 0x4400 - first ||
            (first & 15) || (declared & 15) ||
            first + payload_offset - 1 != 0x4030u + source_word) return 0;
        for (uint32_t i = 0; i < words; ++i)
            if (packet[payload_offset + i] != constant.source.packed[source_word + i]) return 0;
        if (size_ == kCapacity) Retire(snapshot);
        if (size_ == kCapacity) return 0;
        const auto ticket = snapshot.Watch({packet_physical, packet_words * 4});
        if (!ticket.id) return 0;
        const auto id = ++sequence_;
        entries_[size_++] = {id, constant.id, constant.source, packet_physical,
            packet_words * 4, payload_offset * 4, first + payload_offset - 1,
            source_word, words, ticket};
        return id;
    }
    // Exact packet and requested register slice; retained for repeated execution.
    // A matching physical address alone never confers identity.
    bool Copy(const RuntimeSourceMemoryCoordinator::Snapshot& snapshot,
              uint32_t packet_physical, uint32_t packet_bytes,
              uint32_t first_register, uint32_t words,
              PacketConstantPublication& output) const {
        if (!words || words > 32) return false;
        const PacketConstantPublication* found = nullptr;
        for (size_t i = 0; i < size_; ++i) {
            const auto& p = entries_[i];
            if (p.packet_physical != packet_physical || p.packet_bytes != packet_bytes ||
                first_register < p.first_register ||
                uint64_t(first_register) + words > uint64_t(p.first_register) + p.words ||
                !snapshot.Unchanged(p.write)) continue;
            if (found) return false;
            found = &p;
        }
        if (!found) return false;
        output = *found;
        const auto delta = first_register - output.first_register;
        output.payload_offset += delta * 4; output.source_word += delta;
        output.first_register = first_register; output.words = words;
        return true;
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
    std::array<PacketConstantPublication, kCapacity> entries_{};
    size_t size_{};
    uint64_t sequence_{};
};
} // namespace runtime_owned_camera_source
