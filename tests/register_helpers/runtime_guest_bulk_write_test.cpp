#include "runtime_guest_bulk_write.h"
#include "runtime_guest_write_completion.h"
#include <Windows.h>
#include <future>
#include <iostream>

static uint8_t* mapping;
static uint32_t writes, last_address, last_length;
static bool complete;
static bool active_write_rejected = true;
static uint8_t expected;
void TestGeneratedCacheClear(uint8_t* base, uint32_t address);
static void PopulateAndReturn(uint32_t address, bool fail) {
    const RuntimeGuestWriteCompletion completion(address, 32);
    std::memset(mapping + address, expected, 32);
    if (fail) throw 1;
}
// V285: the store hot path reads the shared dirty-page tracker. This stub
// never marks it, so every physical store still reaches the stub below.
GuestPhysicalWriteTracker g_guest_physical_writes;

void RuntimeNotifyGuestPhysicalWrite(uint32_t address, uint32_t length,
                                     const char*, uint32_t) noexcept {
    uint32_t physical{};
    if (RuntimeCanonicalGuestPhysicalRange(address, length, physical)) {
        RuntimeSourceMemoryCoordinator::Snapshot snapshot(RuntimeGuestSourceCoordinator());
        active_write_rejected &= !snapshot.Watch({physical, length}).id;
    }
    ++writes; last_address = address; last_length = length;
    complete = true;
    for (uint32_t i = 0; i < length; ++i) complete &= mapping[address + i] == expected;
}

int main() {
    // Reserve guest address space, but commit only one page per tested alias.
    mapping = static_cast<uint8_t*>(VirtualAlloc(nullptr, size_t(1) << 32,
                                                MEM_RESERVE, PAGE_NOACCESS));
    if (!mapping) return 1;
    bool ok = RuntimeGuestSourceCoordinator().ConfigureAtStartup(true);
    for (uint32_t address : {0x10000u, 0x7F000000u, 0xA0000000u, 0xC0000000u, 0xE0000000u}) {
        if (!VirtualAlloc(mapping + address, 4096, MEM_COMMIT, PAGE_READWRITE)) {
            ok = false; break;
        }
        const bool physical = address != 0x10000;
        for (size_t bytes : {size_t(32), size_t(128)}) {
            std::memset(mapping + address, 0xA5, 256);
            writes = 0; complete = false; expected = 0;
            void* result = RuntimeGeneratedMemset(mapping, mapping + address, 0, bytes, __FILE__, __LINE__);
            ok &= result == mapping + address && writes == (physical ? 1u : 0u);
            if (physical) ok &= complete && last_address == address && last_length == bytes;
            for (size_t i = 0; i < bytes; ++i) ok &= mapping[address + i] == 0;
            ok &= mapping[address + bytes] == 0xA5;
        }
        // A completed clear invalidates the old source ticket; a fresh watch is
        // available after the actual payload write and its notification finish.
        uint32_t physical_address{};
        if (RuntimeCanonicalGuestPhysicalRange(address, 128, physical_address)) {
            RuntimeSourceWriteEpochs::Ticket prior;
            { RuntimeSourceMemoryCoordinator::Snapshot snapshot(RuntimeGuestSourceCoordinator());
              prior = snapshot.Watch({physical_address, 128}); }
            RuntimeGeneratedMemset(mapping, mapping + address, 0, 128, __FILE__, __LINE__);
            { RuntimeSourceMemoryCoordinator::Snapshot snapshot(RuntimeGuestSourceCoordinator());
              ok &= prior.id && !snapshot.Unchanged(prior) && snapshot.Watch({physical_address, 128}).id; }
        }
        // Preserve arbitrary memset values and empty-operation behavior too.
        expected = 0x34; writes = 0;
        RuntimeGeneratedMemset(mapping, mapping + address, 0x1234, 128, __FILE__, __LINE__);
        ok &= mapping[address] == expected && writes == (physical ? 1u : 0u);
        writes = 0;
        RuntimeGeneratedMemset(mapping, mapping + address, 0, 0, __FILE__, __LINE__);
        ok &= writes == 0 && mapping[address] == expected;
        writes = 0; expected = 0;
        TestGeneratedCacheClear(mapping, address + 17);
        ok &= writes == (physical ? 1u : 0u) && mapping[address] == 0;
        if (physical) ok &= complete && last_address == address && last_length == 128;
        for (bool fail : {false, true}) {
            writes = 0; complete = false; expected = 0x75;
            try { PopulateAndReturn(address, fail); } catch (int) {}
            ok &= writes == (physical ? 1u : 0u);
            if (physical) ok &= complete && last_address == address && last_length == 32;
        }
        // Handwritten import PPC stores use the same override before loading
        // ppc_recomp_shared.h; guest byte order and notifications both matter.
        uint8_t* base = mapping;
        writes = 0;
        PPC_STORE_U32(address, 0x12345678);
        ok &= writes == (physical ? 1u : 0u);
        ok &= mapping[address] == 0x12 && mapping[address + 1] == 0x34 &&
              mapping[address + 2] == 0x56 && mapping[address + 3] == 0x78;
        if (physical) ok &= last_address == address && last_length == 4;
        PPC_STORE_U8(address, 0x91);
        ok &= mapping[address] == 0x91;
        PPC_STORE_U16(address, 0x2345);
        ok &= mapping[address] == 0x23 && mapping[address + 1] == 0x45;
        PPC_STORE_U64(address, 0x123456789ABCDEF0ull);
        ok &= mapping[address] == 0x12 && mapping[address + 7] == 0xF0;
        ok &= writes == (physical ? 4u : 0u);
    }
    // Exercise actual generated-store override through every physical alias.
    for (uint32_t address : {0x7F000000u, 0xA0000000u, 0xC0000000u, 0xE0000000u}) {
        uint8_t* base = mapping;
        uint32_t physical{};
        ok &= RuntimeCanonicalGuestPhysicalRange(address, 4, physical);
        PPC_STORE_U32(address, 240);
        RuntimeSourceWriteEpochs::Ticket cursor, payload;
        { RuntimeSourceMemoryCoordinator::Snapshot snapshot(RuntimeGuestSourceCoordinator());
          cursor = snapshot.WatchForwardU32({physical, 4}, PPC_LOAD_U32(address), 4096);
          payload = snapshot.Watch({physical, 4}); }
        PPC_STORE_U32(address, 256);
        { RuntimeSourceMemoryCoordinator::Snapshot snapshot(RuntimeGuestSourceCoordinator());
          ok &= cursor.id && snapshot.Unchanged(cursor) && !snapshot.Unchanged(payload);
          ok &= PPC_LOAD_U32(address) == 256; }
        PPC_STORE_U32(address, 0);
        PPC_STORE_U32(address, 512);
        { RuntimeSourceMemoryCoordinator::Snapshot snapshot(RuntimeGuestSourceCoordinator());
          ok &= !snapshot.Unchanged(cursor);
          cursor = snapshot.WatchForwardU32({physical, 4}, 512, 4096); }
        RuntimeGeneratedMemset(mapping, mapping + address, 0, 4, __FILE__, __LINE__);
        PPC_STORE_U32(address, 768);
        { RuntimeSourceMemoryCoordinator::Snapshot snapshot(RuntimeGuestSourceCoordinator());
          ok &= !snapshot.Unchanged(cursor); }
    }
    // The scalar fast gate must not let an enabled writer publish its payload
    // while a snapshot owns the memory. Exercise the actual generated U64 macro,
    // not only the coordinator in isolation; notification must remain in scope.
    {
        uint8_t* base = mapping;
        constexpr uint32_t address = 0xC0000000u;
        PPC_STORE_U64(address, 0x1122334455667788ull);
        RuntimeSourceWriteEpochs::Ticket prior;
        std::promise<void> attempting;
        std::future<void> writer;
        {
            RuntimeSourceMemoryCoordinator::Snapshot snapshot(RuntimeGuestSourceCoordinator());
            prior = snapshot.Watch({0, 8});
            writer = std::async(std::launch::async, [&] {
                attempting.set_value();
                PPC_STORE_U64(address, 0x8877665544332211ull);
            });
            attempting.get_future().wait();
            ok &= writer.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout;
            ok &= prior.id && snapshot.Unchanged(prior);
        }
        writer.get();
        RuntimeSourceMemoryCoordinator::Snapshot snapshot(RuntimeGuestSourceCoordinator());
        ok &= !snapshot.Unchanged(prior) && snapshot.Watch({0, 8}).id;
        ok &= mapping[address] == 0x88 && mapping[address + 7] == 0x11;
    }
    writes = 0;
    { const RuntimeGuestWriteCompletion empty(0xA0000000, 0); }
    { const RuntimeGuestWriteCompletion wrapping(0xFFFFFFF0, 32); }
    { const RuntimeGuestWriteCompletion high_bits(0x1A0000000ull, 32); }
    ok &= writes == 0;
    ok &= active_write_rejected;
    VirtualFree(mapping, 0, MEM_RELEASE);
    std::cout << (ok ? "guest bulk writes PASS\n" : "guest bulk writes FAIL\n");
    return ok ? 0 : 1;
}
