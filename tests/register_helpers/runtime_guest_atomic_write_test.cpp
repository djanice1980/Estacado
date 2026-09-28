#include "runtime_guest_atomic_write.h"
#include <Windows.h>
#include <iostream>
#include <future>

bool TestGeneratedConditionalStore(uint8_t*, uint32_t, uint32_t, uint32_t);
bool TestGeneratedConditionalStore64(uint8_t*, uint32_t, int64_t, uint64_t);
static uint32_t notifications;
static bool guarded = true;
// V285: the store hot path reads the shared dirty-page tracker. This stub
// never marks it, so every physical store still reaches the stub below.
GuestPhysicalWriteTracker g_guest_physical_writes;

void RuntimeNotifyGuestPhysicalWrite(uint32_t address, uint32_t length,
                                     const char*, uint32_t) noexcept {
    ++notifications;
    uint32_t physical{};
    guarded &= RuntimeCanonicalGuestPhysicalRange(address, length, physical);
    RuntimeSourceMemoryCoordinator::Snapshot snapshot(RuntimeGuestSourceCoordinator());
    guarded &= !snapshot.Watch({physical, length}).id;
}

int main() {
    auto* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, size_t(1) << 32,
                                                   MEM_RESERVE, PAGE_NOACCESS));
    if (!base) return 1;
    bool ok = RuntimeGuestSourceCoordinator().ConfigureAtStartup(true);
    for (uint32_t address : {0x10000u, 0x7F000000u, 0xA0000000u, 0xC0000000u, 0xE0000000u}) {
        if (!VirtualAlloc(base + address, 4096, MEM_COMMIT, PAGE_READWRITE)) { ok = false; break; }
        auto* destination = reinterpret_cast<uint32_t*>(base + address);
        *destination = __builtin_bswap32(0x11223344u);
        uint32_t physical{};
        const bool is_physical = RuntimeCanonicalGuestPhysicalRange(address, 4, physical);
        RuntimeSourceWriteEpochs::Ticket ticket;
        if (is_physical) {
            RuntimeSourceMemoryCoordinator::Snapshot snapshot(RuntimeGuestSourceCoordinator());
            ticket = snapshot.Watch({physical, 4});
        }
        notifications = 0;
        ok &= TestGeneratedConditionalStore(base, address, *destination, 0x55667788u);
        ok &= base[address] == 0x55 && base[address + 3] == 0x88;
        ok &= notifications == (is_physical ? 1u : 0u);
        if (is_physical) {
            RuntimeSourceMemoryCoordinator::Snapshot snapshot(RuntimeGuestSourceCoordinator());
            ok &= ticket.id && !snapshot.Unchanged(ticket);
            ticket = snapshot.Watch({physical, 4});
        }
        // Failure preserves bytes/result and sends no completed-write notice.
        notifications = 0;
        ok &= !TestGeneratedConditionalStore(base, address, 0, 0xDEADBEEFu);
        ok &= *destination == __builtin_bswap32(0x55667788u) && notifications == 0;
        if (is_physical) {
            RuntimeSourceMemoryCoordinator::Snapshot snapshot(RuntimeGuestSourceCoordinator());
            // Attempted writes conservatively invalidate; never certify stale data.
            ok &= ticket.id && !snapshot.Unchanged(ticket) && snapshot.Watch({physical, 4}).id;
        }
        auto* wide = reinterpret_cast<uint64_t*>(base + address);
        *wide = __builtin_bswap64(0xFEDCBA9876543210ull);
        notifications = 0;
        ok &= TestGeneratedConditionalStore64(base, address, static_cast<int64_t>(*wide),
                                              0x8877665544332211ull);
        ok &= base[address] == 0x88 && base[address + 7] == 0x11;
        ok &= notifications == (is_physical ? 1u : 0u);
        notifications = 0;
        ok &= !TestGeneratedConditionalStore64(base, address, 0, 1);
        ok &= *wide == __builtin_bswap64(0x8877665544332211ull) && notifications == 0;
    }
    // A real generated atomic attempt must wait before touching the payload
    // while a snapshot owns it, then retain the original conditional result.
    std::future<bool> writer;
    std::promise<void> attempting;
    const uint32_t address = 0xC0000000u;
    if (ok) {
        {
            RuntimeSourceMemoryCoordinator::Snapshot snapshot(RuntimeGuestSourceCoordinator());
            *reinterpret_cast<uint32_t*>(base + address) = 0;
            writer = std::async(std::launch::async, [&] {
                attempting.set_value();
                return TestGeneratedConditionalStore(base, address, 0, 0x12345678u);
            });
            attempting.get_future().wait();
            ok &= writer.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout;
            ok &= *reinterpret_cast<uint32_t*>(base + address) == 0;
        }
        ok &= writer.get() && base[address] == 0x12 && base[address + 3] == 0x78;
    }
    ok &= guarded;
    VirtualFree(base, 0, MEM_RELEASE);
    std::cout << (ok ? "guest atomic writes PASS\n" : "guest atomic writes FAIL\n");
    return ok ? 0 : 1;
}
