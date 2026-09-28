#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

// This is the runtime's model of guest physical memory, not a claim about the
// Xbox kernel's private allocations. Physical aliases in main.cpp expose this
// same 512 MiB backing store at the guest-visible physical-memory ranges.
class GuestMemoryAccounting {
public:
    static constexpr uint32_t kPageSize = 4096;
    static constexpr uint32_t kPhysicalBytes = 0x20000000;
    static constexpr uint32_t kPhysicalPages = kPhysicalBytes / kPageSize;

    void Reset();

    uint32_t total_physical_pages() const { return kPhysicalPages; }
    uint32_t used_physical_pages() const;
    uint32_t available_physical_pages() const { return kPhysicalPages - used_physical_pages(); }
    uint32_t highest_physical_page() const { return kPhysicalPages - 1; }

    // Reserves a contiguous page range in the physical backing model. This is
    // intentionally allocation-policy-neutral so MmAllocatePhysicalMemoryEx
    // can supply its verified range/alignment semantics when it is reached.
    bool ReservePhysicalPages(uint32_t pageCount, uint32_t alignmentPages,
                              uint32_t minimumPage, uint32_t maximumPage,
                              uint32_t protection, uint32_t* firstPage);
    bool ReleasePhysicalPages(uint32_t firstPage);
    // The currently evidenced MmQueryAllocationSize path asks about the base
    // address returned by MmAllocatePhysicalMemoryEx. Return zero for an
    // untracked or interior address rather than inventing heap semantics.
    uint32_t AllocationSizeAtBasePage(uint32_t firstPage) const;
    // MmQueryAddressProtect accepts any address owned by an allocation, not
    // only its base. Return zero for an untracked physical page.
    uint32_t AllocationProtectionAtPage(uint32_t page) const;

    struct AllocationIdentity {
        uint64_t generation{};
        uint32_t physical_base{}, bytes{};
    };
    // Physical backing allocation identity only. A native arena may reuse its
    // interior without releasing these pages; this is not a write version or
    // a lock held across a caller's later memory access.
    AllocationIdentity IdentityForRange(uint32_t physical, uint32_t bytes) const;
    bool IdentityMatches(AllocationIdentity identity, uint32_t physical,
                         uint32_t bytes) const;

private:
    struct Allocation {
        uint32_t firstPage;
        uint32_t pageCount;
        uint32_t protection;
        uint64_t generation;
    };

    std::vector<uint8_t> reserved_pages_ = std::vector<uint8_t>(kPhysicalPages, 0);
    std::unordered_map<uint32_t, Allocation> allocations_;
    uint32_t used_pages_{};
    uint64_t allocation_sequence_{}; // Never reset or reused, even by Reset.
    mutable std::mutex mutex_;
};

GuestMemoryAccounting& GetGuestMemoryAccounting();
