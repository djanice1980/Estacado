#include <rex/graphics/memory_range.h>

#include "guest_physical_write_tracker.h"

#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

namespace {

bool Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

bool CheckConcurrentPublication() {
    GuestPhysicalWriteTracker tracker;
    // Same page word, different words in one summary, and another summary.
    constexpr std::array<uint32_t, 4> pages{1u, 2u, 65u, 4097u};
    constexpr uint32_t iterations = 4096;
    std::array<std::atomic<uint32_t>, pages.size()> payloads{};
    std::array<std::atomic<uint32_t>, pages.size()> observed{};
    std::atomic<bool> start{}, abort{};
    std::atomic<unsigned> finished{};
    const auto drain = [&] {
        tracker.Drain([&](uint32_t address, uint32_t length) noexcept {
            for (size_t i = 0; i < pages.size(); ++i) {
                const uint32_t pageAddress = pages[i] << 12;
                if (pageAddress < address || pageAddress - address >= length) continue;
                // Visibility comes from dirty publication, not a payload fence.
                const uint32_t value = payloads[i].load(std::memory_order_relaxed);
                uint32_t previous = observed[i].load(std::memory_order_relaxed);
                while (previous < value && !observed[i].compare_exchange_weak(
                           previous, value, std::memory_order_release,
                           std::memory_order_relaxed)) {}
            }
        });
    };
    std::array<std::thread, pages.size()> writers;
    for (size_t i = 0; i < pages.size(); ++i) {
        writers[i] = std::thread([&, i] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (uint32_t value = 1; value <= iterations &&
                     !abort.load(std::memory_order_relaxed); ++value) {
                payloads[i].store(value, std::memory_order_relaxed);
                tracker.Mark(pages[i] << 12, 4u);
                while (observed[i].load(std::memory_order_acquire) < value &&
                       !abort.load(std::memory_order_relaxed)) std::this_thread::yield();
            }
            finished.fetch_add(1, std::memory_order_release);
        });
    }
    // MMIO flushes may originate on different guest threads. Exercise two
    // drains as well as multiple writers without adding hooks to production.
    std::thread otherDrain([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        while (finished.load(std::memory_order_acquire) != pages.size() &&
               !abort.load(std::memory_order_relaxed)) drain();
    });
    start.store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (finished.load(std::memory_order_acquire) != pages.size()) {
        drain();
        if (std::chrono::steady_clock::now() >= deadline) {
            abort.store(true, std::memory_order_relaxed);
            break;
        }
    }
    for (auto& writer : writers) writer.join();
    otherDrain.join();
    drain();
    bool passed = Check(!abort.load(), "concurrent dirty publication stopped making progress");
    for (auto& value : observed) {
        passed &= Check(value.load() == iterations,
                        "a completed physical write was lost or its payload was not visible");
    }
    return passed;
}

}  // namespace

int main() {
    using rex::graphics::AreGuestPhysicalMemoryRangesOverlapping;

    bool passed = true;
    passed &= Check(AreGuestPhysicalMemoryRangesOverlapping(0x1000, 0x1100, 0x1000, 0x1100),
                    "identical ranges must overlap");
    passed &= Check(AreGuestPhysicalMemoryRangesOverlapping(0x1020, 0x1040, 0x1000, 0x1100),
                    "an entry contained by a write must overlap");
    passed &= Check(AreGuestPhysicalMemoryRangesOverlapping(0x1000, 0x1100, 0x1020, 0x1040),
                    "a write contained by an entry must overlap");
    passed &= Check(AreGuestPhysicalMemoryRangesOverlapping(0x0F80, 0x1020, 0x1000, 0x1100),
                    "left partial overlap must be detected");
    passed &= Check(AreGuestPhysicalMemoryRangesOverlapping(0x1080, 0x1180, 0x1000, 0x1100),
                    "right partial overlap must be detected");
    passed &= Check(!AreGuestPhysicalMemoryRangesOverlapping(0x0F00, 0x1000, 0x1000, 0x1100),
                    "touching the write start must not overlap");
    passed &= Check(!AreGuestPhysicalMemoryRangesOverlapping(0x1100, 0x1200, 0x1000, 0x1100),
                    "touching the write end must not overlap");
    passed &= Check(!AreGuestPhysicalMemoryRangesOverlapping(0x1200, 0x1300, 0x1000, 0x1100),
                    "disjoint ranges must not overlap");

    GuestPhysicalWriteTracker tracker;
    tracker.Mark(0x00001004u, 4u);
    tracker.Mark(0x00001FFCu, 8u);
    tracker.Mark(0x0003FFFFu, 2u);
    tracker.Mark(0x00100000u, 1u);
    tracker.Mark(GuestPhysicalWriteTracker::kPhysicalBytes, 1u);
    std::vector<std::pair<uint32_t, uint32_t>> dirtyRanges;
    tracker.Drain([&](uint32_t address, uint32_t length) noexcept {
        dirtyRanges.emplace_back(address, length);
    });
    passed &= Check(dirtyRanges.size() == 3u,
                    "dirty physical pages must be coalesced into three exact ranges");
    if (dirtyRanges.size() == 3u) {
        passed &= Check(dirtyRanges[0] == std::make_pair(0x00001000u, 0x00002000u),
                        "same and adjacent-page writes must coalesce exactly");
        passed &= Check(dirtyRanges[1] == std::make_pair(0x0003F000u, 0x00002000u),
                        "a cross-word-boundary write must preserve both contiguous pages");
        passed &= Check(dirtyRanges[2] == std::make_pair(0x00100000u, 0x00001000u),
                        "a disjoint dirty page must remain a separate exact range");
    }
    dirtyRanges.clear();
    tracker.Drain([&](uint32_t address, uint32_t length) noexcept {
        dirtyRanges.emplace_back(address, length);
    });
    passed &= Check(dirtyRanges.empty(), "draining must consume every reported dirty page");

    // Re-dirty a page after its summary and page word were consumed. The next
    // drain must see it even when the write occurs inside a drain callback.
    tracker.Mark(0x1000u, 4u);
    tracker.Drain([&](uint32_t, uint32_t) noexcept { tracker.Mark(0x1004u, 4u); });
    tracker.Drain([&](uint32_t address, uint32_t length) noexcept {
        dirtyRanges.emplace_back(address, length);
    });
    passed &= Check(dirtyRanges.size() == 1u &&
                        dirtyRanges[0] == std::make_pair(0x1000u, 0x1000u),
                    "a write after consumption must publish the page again");
    passed &= CheckConcurrentPublication();

    if (passed) {
        std::cout << "GPU memory-range overlap tests passed\n";
    }
    return passed ? 0 : 1;
}
