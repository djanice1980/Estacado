#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <intrin.h>
#include <utility>
#include <vector>

#if defined(_WIN32)
// Same declaration as <windows.h> (kept here so the generated-code include
// chain does not pull in the Windows headers).
extern "C" __declspec(dllimport) void __stdcall FlushProcessWriteBuffers(void);
#endif

// Records CPU writes to the 512 MB Xenon physical aperture without entering
// the GPU cache on every scalar guest store. The command submission boundary
// drains exact dirty page ranges before ReXGlue can consume newly submitted
// work, preserving the single authoritative guest-memory model.
//
// V285 hot path: a store to a page that is already pending (marked and not
// yet drained) needs no read-modify-write. Skipping it is safe because Drain
// (1) consumes page bits, (2) flushes every processor's write buffer
// (FlushProcessWriteBuffers), and only then (3) publishes invalidations. A
// writer that observed its page still pending either has its payload drained
// by step 2, or its check executes after step 1, sees the bit clear and takes
// the ordinary locked path. Either way no invalidation can reach the GPU
// before the payload is globally visible.
class GuestPhysicalWriteTracker {
public:
    static constexpr uint32_t kPhysicalBytes = 0x20000000u;
    static constexpr uint32_t kPageShift = 12u;
    static constexpr uint32_t kPageBytes = 1u << kPageShift;
    static constexpr uint32_t kPageCount = kPhysicalBytes >> kPageShift;
#if defined(_WIN32)
    static constexpr bool kSkipPendingRmw = true;
#else
    // Without a process-wide write-buffer flush the skip is not provably safe.
    static constexpr bool kSkipPendingRmw = false;
#endif

    // Relaxed test of one page (caller guarantees physicalAddress is in range).
    bool PagePending(uint32_t physicalAddress) const noexcept {
        if constexpr (!kSkipPendingRmw) return false;
        const uint32_t page = physicalAddress >> kPageShift;
        return (pageWords_[page >> 6].load(std::memory_order_relaxed) >> (page & 63u)) & 1u;
    }

    void Mark(uint32_t physicalAddress, uint32_t length) noexcept {
        if (!length || physicalAddress >= kPhysicalBytes ||
            length > kPhysicalBytes - physicalAddress) {
            return;
        }
        const uint32_t firstPage = physicalAddress >> kPageShift;
        const uint32_t lastPage =
            (physicalAddress + length - 1u) >> kPageShift;
        for (uint32_t page = firstPage; page <= lastPage; ++page) {
            const uint32_t wordIndex = page >> 6;
            const uint64_t pageBit = uint64_t{1} << (page & 63u);
            // Already pending: see the class comment for why no RMW is needed.
            if (kSkipPendingRmw &&
                (pageWords_[wordIndex].load(std::memory_order_relaxed) & pageBit)) {
                continue;
            }
            pageWords_[wordIndex].fetch_or(pageBit, std::memory_order_acq_rel);
            auto& summary = dirtyWordWords_[wordIndex >> 6];
            const uint64_t wordBit = uint64_t{1} << (wordIndex & 63u);
            // Keep the page RMW unconditional once taken. If a drain consumed
            // that word first, its release exchange (or a subsequent RMW
            // release sequence) synchronizes with our acquire above. Its
            // earlier summary clear therefore happens-before this load: we
            // cannot reuse the cleared publication. Otherwise the drain
            // includes our page write, or the existing summary still
            // schedules it.
            if (!(summary.load(std::memory_order_acquire) & wordBit)) {
                summary.fetch_or(wordBit, std::memory_order_release);
            }
        }
    }

    // Consumes every pending page into coalesced [address, length) ranges,
    // then flushes all write buffers before any range is published.
    template <typename Callback>
    void Drain(Callback&& callback) noexcept {
        thread_local std::vector<std::pair<uint32_t, uint32_t>> ranges;
        ranges.clear();
        uint32_t rangeFirstPage = UINT32_MAX;
        uint32_t rangeLastPage = UINT32_MAX;
        const auto closeRange = [&]() noexcept {
            if (rangeFirstPage == UINT32_MAX) return;
            ranges.emplace_back(rangeFirstPage << kPageShift,
                                (rangeLastPage - rangeFirstPage + 1u) << kPageShift);
            rangeFirstPage = UINT32_MAX;
            rangeLastPage = UINT32_MAX;
        };

        for (uint32_t summaryIndex = 0;
             summaryIndex < dirtyWordWords_.size(); ++summaryIndex) {
            uint64_t dirtyWords = dirtyWordWords_[summaryIndex].exchange(
                0, std::memory_order_acq_rel);
            while (dirtyWords) {
                unsigned long wordBit{};
                _BitScanForward64(&wordBit, dirtyWords);
                dirtyWords &= dirtyWords - 1u;
                const uint32_t wordIndex = summaryIndex * 64u + wordBit;
                if (wordIndex >= pageWords_.size()) continue;
                uint64_t pages = pageWords_[wordIndex].exchange(
                    0, std::memory_order_acq_rel);
                while (pages) {
                    unsigned long pageBit{};
                    _BitScanForward64(&pageBit, pages);
                    pages &= pages - 1u;
                    const uint32_t page = wordIndex * 64u + pageBit;
                    if (rangeFirstPage == UINT32_MAX) {
                        rangeFirstPage = rangeLastPage = page;
                    } else if (page == rangeLastPage + 1u) {
                        rangeLastPage = page;
                    } else {
                        closeRange();
                        rangeFirstPage = rangeLastPage = page;
                    }
                }
            }
        }
        closeRange();
        if (ranges.empty()) return;
#if defined(_WIN32)
        FlushProcessWriteBuffers();
#else
        std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
        for (const auto& [address, length] : ranges) callback(address, length);
    }

private:
    static constexpr uint32_t kPageWordCount = (kPageCount + 63u) / 64u;
    static constexpr uint32_t kSummaryWordCount =
        (kPageWordCount + 63u) / 64u;
    std::array<std::atomic<uint64_t>, kPageWordCount> pageWords_{};
    std::array<std::atomic<uint64_t>, kSummaryWordCount> dirtyWordWords_{};
};
