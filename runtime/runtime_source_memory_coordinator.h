#pragma once
#include "runtime_source_write_epochs.h"
#include <atomic>
#include <optional>
#include "runtime_guest_physical_range.h"

// An instance is configured once, before its first guest operation. A writer
// or snapshot arriving first permanently selects disabled mode. This prevents
// enabling tracking around an already-running, unobserved memory operation.
class RuntimeSourceMemoryCoordinator {
public:
    bool TrackingEnabled() const noexcept { return mode_.load(std::memory_order_acquire) == 2; }
    // Store hot path (V285): true once the startup mode has been frozen to
    // "disabled"; ordinary stores then need neither the gate nor its scope.
    bool TrackingSettledDisabled() const noexcept {
        return mode_.load(std::memory_order_relaxed) == 1;
    }
    // Freeze first-use mode even on the disabled fast path. A later caller must
    // never enable snapshots after an unobserved write has already begun.
    bool BeginGuardedOperation() noexcept { return Enabled(); }
    bool ConfigureAtStartup(bool enabled) noexcept {
        uint8_t expected = 0;
        const uint8_t selected = enabled ? 2 : 1;
        return mode_.compare_exchange_strong(expected, selected) || expected == selected;
    }
    class Write {
    public:
        Write(RuntimeSourceMemoryCoordinator& owner, RuntimeSourceWriteEpochs::Range range,
              std::optional<uint32_t> stored_u32 = {})
            : owner_(owner), lock_(owner.payload_mutex_, std::defer_lock) {
            if (owner_.Enabled()) { lock_.lock(); id_ = owner_.epochs_.Begin(range, stored_u32); }
        }
        ~Write() { if (id_) owner_.epochs_.End(id_); }
        Write(const Write&) = delete;
        Write& operator=(const Write&) = delete;
    private:
        RuntimeSourceMemoryCoordinator& owner_;
        std::unique_lock<std::recursive_mutex> lock_;
        uint64_t id_{};
    };
    class Snapshot {
    public:
        explicit Snapshot(RuntimeSourceMemoryCoordinator& owner)
            : owner_(owner), lock_(owner.payload_mutex_, std::defer_lock) {
            if (owner_.Enabled()) lock_.lock();
        }
        RuntimeSourceWriteEpochs::Ticket Watch(RuntimeSourceWriteEpochs::Range range) {
            return lock_.owns_lock() ? owner_.epochs_.Watch(range) : RuntimeSourceWriteEpochs::Ticket{};
        }
        RuntimeSourceWriteEpochs::Ticket WatchForwardU32(
            RuntimeSourceWriteEpochs::Range range, uint32_t value, uint32_t limit) {
            return lock_.owns_lock() ? owner_.epochs_.WatchForwardU32(range, value, limit)
                                    : RuntimeSourceWriteEpochs::Ticket{};
        }
        bool Unchanged(RuntimeSourceWriteEpochs::Ticket ticket) const {
            return lock_.owns_lock() && owner_.epochs_.Unchanged(ticket);
        }
        bool Retired(RuntimeSourceWriteEpochs::Ticket ticket) const {
            return lock_.owns_lock() && owner_.epochs_.Retired(ticket);
        }
        RuntimeSourceWriteEpochs::Retirement ExplainRetirement(RuntimeSourceWriteEpochs::Ticket ticket) const {
            return lock_.owns_lock() ? owner_.epochs_.ExplainRetirement(ticket)
                                    : RuntimeSourceWriteEpochs::Retirement{};
        }
        Snapshot(const Snapshot&) = delete;
        Snapshot& operator=(const Snapshot&) = delete;
    private:
        RuntimeSourceMemoryCoordinator& owner_;
        std::unique_lock<std::recursive_mutex> lock_;
    };
private:
    bool Enabled() noexcept {
        auto selected = mode_.load(std::memory_order_acquire);
        if (!selected) {
            uint8_t expected = 0;
            if (mode_.compare_exchange_strong(expected, 1)) return false;
            selected = expected;
        }
        return selected == 2;
    }
    std::atomic<uint8_t> mode_{};
    std::recursive_mutex payload_mutex_;
    // Every access is inside Write/Snapshot payload ownership. Avoid taking
    // a second mutex on each Begin/End while preserving the payload barrier.
    RuntimeSourceWriteEpochs epochs_{RuntimeSourceWriteEpochs::ExternallySerialized{}};
};

namespace runtime_source_memory_detail {
// Constant-initialized independently of the coordinator's mutexes. Publish
// only after its original thread-safe local-static construction has completed.
inline std::atomic<RuntimeSourceMemoryCoordinator*> initialized_guest_coordinator{nullptr};

__declspec(noinline) inline RuntimeSourceMemoryCoordinator& InitializeGuestCoordinator() {
    static RuntimeSourceMemoryCoordinator coordinator;
    initialized_guest_coordinator.store(&coordinator, std::memory_order_release);
    return coordinator;
}
}  // namespace runtime_source_memory_detail

inline RuntimeSourceMemoryCoordinator& RuntimeGuestSourceCoordinator() {
    if (auto* coordinator = runtime_source_memory_detail::initialized_guest_coordinator.load(
            std::memory_order_acquire)) return *coordinator;
    return runtime_source_memory_detail::InitializeGuestCoordinator();
}

// Production remains disabled until a startup caller explicitly configures the
// coordinator before ANY guarded operation. No live enable/disable API exists.
class RuntimeGuestSourceWriteScope {
public:
    __forceinline RuntimeGuestSourceWriteScope(uint64_t address, uint64_t bytes,
                                              std::optional<uint32_t> stored_u32 = {}) {
        auto& coordinator = RuntimeGuestSourceCoordinator();
        if (!coordinator.BeginGuardedOperation()) return;
        BeginTrackedWrite(coordinator, address, bytes, stored_u32);
    }
private:
    // Keep the normal disabled path out of the large range/RAII construction
    // body. The gate still freezes first-use mode before the caller's store;
    // enabled writes retain exactly the same lock, epoch and lifetime scope.
    __declspec(noinline) void BeginTrackedWrite(RuntimeSourceMemoryCoordinator& coordinator,
                                              uint64_t address, uint64_t bytes,
                                              std::optional<uint32_t> stored_u32) {
        uint32_t physical{};
        if (address > UINT32_MAX || !bytes) return;
        if (bytes <= UINT32_MAX && RuntimeCanonicalGuestPhysicalRange(
                static_cast<uint32_t>(address), static_cast<uint32_t>(bytes), physical))
            write_.emplace(coordinator,
                RuntimeSourceWriteEpochs::Range{physical, static_cast<uint32_t>(bytes)}, stored_u32);
        else if (RuntimeCanonicalGuestPhysicalRange(static_cast<uint32_t>(address), 1, physical))
            // A malformed write starting in an alias must not silently bypass
            // validity tracking for the portion overlapping physical memory.
            write_.emplace(coordinator,
                RuntimeSourceWriteEpochs::Range{0x20000000u, 1});
    }
    std::optional<RuntimeSourceMemoryCoordinator::Write> write_;
};
