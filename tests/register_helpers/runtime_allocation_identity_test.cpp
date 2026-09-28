#include "runtime_memory.h"
#include "runtime_source_memory_coordinator.h"
#include <future>
#include <atomic>
#include <iostream>
#include <thread>
#include <vector>

int main() {
    GuestMemoryAccounting memory;
    bool ok = RuntimeGuestSourceCoordinator().ConfigureAtStartup(true);
    uint32_t first = 999;
    ok &= memory.ReservePhysicalPages(2, 1, 8, 9, 4, &first) && first == 8;
    const auto old = memory.IdentityForRange(8 * 4096 + 7, 5000);
    ok &= old.generation != 0 && old.physical_base == 8 * 4096 && old.bytes == 8192;
    ok &= memory.IdentityMatches(old, 9 * 4096, 4096);
    ok &= !memory.IdentityForRange(9 * 4096, 4097).generation;
    ok &= !memory.IdentityForRange(8 * 4096, 0).generation;
    ok &= !memory.IdentityForRange(0xFFFFFFF0, 32).generation;
    RuntimeSourceWriteEpochs::Ticket watched;
    std::future<bool> release;
    std::promise<void> attempting;
    {
        RuntimeSourceMemoryCoordinator::Snapshot snapshot(RuntimeGuestSourceCoordinator());
        watched = snapshot.Watch({8 * 4096, 8192});
        release = std::async(std::launch::async, [&] {
            attempting.set_value();
            return memory.ReleasePhysicalPages(first);
        });
        attempting.get_future().wait();
        ok &= release.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout;
        ok &= memory.IdentityMatches(old, 8 * 4096, 1);
    }
    ok &= release.get() && !memory.IdentityMatches(old, 8 * 4096, 1);
    { RuntimeSourceMemoryCoordinator::Snapshot snapshot(RuntimeGuestSourceCoordinator());
      ok &= watched.id && !snapshot.Unchanged(watched); }
    ok &= memory.ReservePhysicalPages(2, 1, 8, 9, 4, &first);
    const auto reused = memory.IdentityForRange(8 * 4096, 8192);
    ok &= reused.generation > old.generation && !memory.IdentityMatches(old, 8 * 4096, 1);
    memory.Reset();
    ok &= memory.ReservePhysicalPages(1, 1, 8, 9, 4, &first);
    const auto reset = memory.IdentityForRange(8 * 4096, 1);
    ok &= reset.generation > reused.generation && !memory.IdentityMatches(reused, 8 * 4096, 1);
    ok &= memory.ReservePhysicalPages(1, 1, 9, 9, 4, &first);
    ok &= !memory.IdentityForRange(8 * 4096, 8192).generation; // Two adjacent allocations.
    memory.Reset();
    std::atomic<bool> concurrent_ok{true};
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) workers.emplace_back([&] {
        for (int i = 0; i < 100; ++i) {
            uint32_t page{};
            if (!memory.ReservePhysicalPages(1, 1, 0, 63, 4, &page)) {
                concurrent_ok = false; continue;
            }
            auto id = memory.IdentityForRange(page * 4096, 4096);
            if (!id.generation || !memory.IdentityMatches(id, page * 4096, 4096) ||
                !memory.ReleasePhysicalPages(page) || memory.IdentityMatches(id, page * 4096, 1))
                concurrent_ok = false;
        }
    });
    for (auto& worker : workers) worker.join();
    ok &= concurrent_ok.load() && memory.used_physical_pages() == 0;
    std::cout << (ok ? "allocation identity PASS\n" : "allocation identity FAIL\n");
    return ok ? 0 : 1;
}
