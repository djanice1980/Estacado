#pragma once
#include "runtime_owned_camera_packets.h"

namespace runtime_owned_camera_source {
// A lease covers one uninterrupted native writer fragment. Both locks must be
// released before the native buffer flush, then the scratch source is selected
// again before copying the next fragment. No guest execution occurs here.
class PacketWriteLease {
public:
    PacketWriteLease(std::mutex& mutex, RuntimeSourceMemoryCoordinator& coordinator,
                     const ConstantStore& constants, PacketConstantStore& packets,
                     uint32_t scratch)
        : lock_(mutex, std::defer_lock), coordinator_(coordinator),
          constants_(constants), packets_(packets), scratch_(scratch) {}
    bool Resume() {
        Release();
        lock_.lock();
        snapshot_.emplace(coordinator_);
        if (!constants_.Copy(*snapshot_, scratch_, 128, constant_)) {
            Release(); return false;
        }
        return true;
    }
    void Release() {
        snapshot_.reset();
        if (lock_.owns_lock()) lock_.unlock();
    }
    bool Active() const { return snapshot_.has_value(); }
    template<class Published>
    bool Seal(uint32_t physical, const uint32_t* words, uint32_t count,
              Published&& published) {
        if (!Active() || (physical & 3) ||
            uint64_t(physical) + uint64_t(count) * 4 > 0x20000000ull) return false;
        return VisitNativeConstantPackets(words, count,
            [&](uint32_t offset, uint32_t first, uint32_t size) {
                const uint32_t begin = first < 0x4030 ? 0x4030 : first;
                const uint32_t end = first + size > 0x4050 ? 0x4050 : first + size;
                if (end <= begin) return;
                const auto id = packets_.Publish(*snapshot_, constant_, begin - 0x4030,
                    physical + offset * 4, words + offset, size + 1,
                    begin - first + 1, end - begin);
                if (id) published(id, constant_.id, constant_.source.token,
                    physical + offset * 4, size + 1, begin, end - begin);
            });
    }
private:
    // Destruction releases the snapshot before the camera lock.
    std::unique_lock<std::mutex> lock_;
    RuntimeSourceMemoryCoordinator& coordinator_;
    const ConstantStore& constants_;
    PacketConstantStore& packets_;
    uint32_t scratch_{};
    ConstantPublication constant_{};
    std::optional<RuntimeSourceMemoryCoordinator::Snapshot> snapshot_;
};
} // namespace runtime_owned_camera_source
