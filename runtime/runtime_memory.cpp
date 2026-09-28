#include "runtime_memory.h"
#include "runtime_source_memory_coordinator.h"

#include <algorithm>
#include <limits>

void GuestMemoryAccounting::Reset() {
    RuntimeSourceMemoryCoordinator::Write retirement(RuntimeGuestSourceCoordinator(), {0, kPhysicalBytes});
    std::lock_guard lock(mutex_);
    std::fill(reserved_pages_.begin(), reserved_pages_.end(), uint8_t{0});
    allocations_.clear();
    used_pages_ = 0;
}

bool GuestMemoryAccounting::ReservePhysicalPages(uint32_t pageCount,
                                                  uint32_t alignmentPages,
                                                  uint32_t minimumPage,
                                                  uint32_t maximumPage,
                                                  uint32_t protection,
                                                  uint32_t* firstPage) {
    if (!firstPage || !pageCount || pageCount > kPhysicalPages) return false;
    if (!alignmentPages) alignmentPages = 1;
    if (minimumPage >= kPhysicalPages) return false;
    maximumPage = std::min(maximumPage, kPhysicalPages - 1);
    if (minimumPage > maximumPage || pageCount > maximumPage - minimumPage + 1) return false;
    RuntimeSourceMemoryCoordinator::Snapshot lifecycle(RuntimeGuestSourceCoordinator());
    std::lock_guard lock(mutex_);

    const uint64_t lastStart = uint64_t(maximumPage) - pageCount + 1;
    for (uint64_t candidate = minimumPage; candidate <= lastStart; ++candidate) {
        if (candidate % alignmentPages) continue;
        bool free = true;
        for (uint32_t page = 0; page < pageCount; ++page) {
            if (reserved_pages_[candidate + page]) {
                free = false;
                candidate += page;
                break;
            }
        }
        if (!free) continue;

        RuntimeSourceMemoryCoordinator::Write reuse(RuntimeGuestSourceCoordinator(),
            {static_cast<uint32_t>(candidate) * kPageSize, pageCount * kPageSize});
        for (uint32_t page = 0; page < pageCount; ++page) reserved_pages_[candidate + page] = 1;
        const uint32_t start = static_cast<uint32_t>(candidate);
        // ID exhaustion disables identity metadata, never guest allocation.
        const uint64_t generation = allocation_sequence_ == UINT64_MAX
            ? 0 : ++allocation_sequence_;
        allocations_.emplace(start, Allocation{start, pageCount, protection, generation});
        used_pages_ += pageCount;
        *firstPage = start;
        return true;
    }
    return false;
}

bool GuestMemoryAccounting::ReleasePhysicalPages(uint32_t firstPage) {
    // Acquire payload before accounting, matching source snapshot lock order.
    RuntimeSourceMemoryCoordinator::Snapshot lifecycle(RuntimeGuestSourceCoordinator());
    std::lock_guard lock(mutex_);
    const auto it = allocations_.find(firstPage);
    if (it == allocations_.end()) return false;
    RuntimeSourceMemoryCoordinator::Write retirement(RuntimeGuestSourceCoordinator(),
        {firstPage * kPageSize, it->second.pageCount * kPageSize});
    for (uint32_t page = 0; page < it->second.pageCount; ++page) {
        reserved_pages_[firstPage + page] = 0;
    }
    used_pages_ -= it->second.pageCount;
    allocations_.erase(it);
    return true;
}

uint32_t GuestMemoryAccounting::AllocationSizeAtBasePage(uint32_t firstPage) const {
    std::lock_guard lock(mutex_);
    const auto it = allocations_.find(firstPage);
    if (it == allocations_.end()) return 0;
    return it->second.pageCount * kPageSize;
}

uint32_t GuestMemoryAccounting::AllocationProtectionAtPage(uint32_t page) const {
    std::lock_guard lock(mutex_);
    for (const auto& [firstPage, allocation] : allocations_) {
        if (page >= firstPage && page - firstPage < allocation.pageCount) {
            return allocation.protection;
        }
    }
    return 0;
}

GuestMemoryAccounting& GetGuestMemoryAccounting() {
    static GuestMemoryAccounting accounting;
    return accounting;
}

uint32_t GuestMemoryAccounting::used_physical_pages() const {
    std::lock_guard lock(mutex_);
    return used_pages_;
}

GuestMemoryAccounting::AllocationIdentity GuestMemoryAccounting::IdentityForRange(
    uint32_t physical, uint32_t bytes) const {
    if (!bytes || uint64_t(physical) + bytes > kPhysicalBytes) return {};
    std::lock_guard lock(mutex_);
    for (const auto& [page, allocation] : allocations_) {
        const uint32_t first = page * kPageSize;
        const uint32_t size = allocation.pageCount * kPageSize;
        if (physical >= first && uint64_t(physical) + bytes <= uint64_t(first) + size)
            return {allocation.generation, first, size};
    }
    return {};
}

bool GuestMemoryAccounting::IdentityMatches(AllocationIdentity identity,
    uint32_t physical, uint32_t bytes) const {
    if (!identity.generation) return false;
    const auto current = IdentityForRange(physical, bytes);
    return current.generation == identity.generation &&
        current.physical_base == identity.physical_base && current.bytes == identity.bytes;
}
