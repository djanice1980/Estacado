#include "runtime_memory_access.h"

#include <cstdint>
#include <iostream>

namespace {
constexpr uint32_t kXenosStatusAddress = 0x7FC86544u;
constexpr uint32_t kXmaContextArrayAddress = 0x7FEA1800u;
uint32_t physical_notifications{};

struct Device {
    uint32_t read_address{};
    uint32_t write_address{};
    uint32_t write_value{};
};

uint32_t Read(void* context, uint32_t address) {
    static_cast<Device*>(context)->read_address = address;
    return 1;
}

void Write(void* context, uint32_t address, uint32_t value) {
    auto& device = *static_cast<Device*>(context);
    device.write_address = address;
    device.write_value = value;
}

bool Check(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}
}  // namespace

// V285: the store hot path reads the shared dirty-page tracker. This stub
// never marks it, so every physical store still reaches the stub below.
GuestPhysicalWriteTracker g_guest_physical_writes;

void RuntimeNotifyGuestPhysicalWrite(uint32_t, uint32_t, const char*,
                                     uint32_t) noexcept { ++physical_notifications; }

int main() {
    alignas(8) uint8_t memory[16]{};
    uint8_t* base = memory;
    Device device{};

    if (!Check(RuntimeGuestStoreMayReachPhysicalMemory(0x7F000000u, 1u) &&
                   RuntimeGuestStoreMayReachPhysicalMemory(0x7FFFFFFFu, 16u) &&
                   !RuntimeGuestStoreMayReachPhysicalMemory(0x80000000u, 4u) &&
                   !RuntimeGuestStoreMayReachPhysicalMemory(0x9FFFFFFFu, 4u) &&
                   RuntimeGuestStoreMayReachPhysicalMemory(0xA0000000u, 16u) &&
                   RuntimeGuestStoreMayReachPhysicalMemory(0xDFFFFFFFu, 1u) &&
                   !RuntimeGuestStoreMayReachPhysicalMemory(0xDFFFFFFFu, 2u) &&
                   RuntimeGuestStoreMayReachPhysicalMemory(0xE0000000u, 16u) &&
                   !RuntimeGuestStoreMayReachPhysicalMemory(0xFFD00000u, 1u),
               "guest physical-alias store prefilter changed its boundary contract")) {
        return 1;
    }

    ResetRuntimeMmioRangesForTests();
    if (!Check(RegisterRuntimeMmioRange(0x7FC80000u, 0x7FCFFFFFu, &device, Read, Write),
               "Xenos MMIO range registration failed")) {
        return 1;
    }
    if (!Check(RegisterRuntimeMmioRange(0x7FEA0000u, 0x7FEAFFFFu, &device, Read, Write),
               "XMA MMIO range registration failed")) {
        return 1;
    }

    // These deliberately use the ordinary generated-code macros, not the
    // explicitly tagged PPC_MM_* variants.
    if (!Check(PPC_LOAD_U32(kXenosStatusAddress) == 1 &&
                   device.read_address == kXenosStatusAddress,
               "untagged Xenos load bypassed the MMIO device")) {
        return 1;
    }

    PPC_STORE_U32(0x7FC80714u, 0x12345678u);
    if (!Check(device.write_address == 0x7FC80714u && device.write_value == 0x12345678u,
               "untagged Xenos store bypassed the MMIO device")) {
        return 1;
    }

    if (!Check(PPC_LOAD_U32(kXmaContextArrayAddress) == 1 &&
                   device.read_address == kXmaContextArrayAddress,
               "untagged XMA load bypassed the MMIO device")) {
        return 1;
    }

    PPC_STORE_U32(4, 0xA1B2C3D4u);
    if (!Check(memory[4] == 0xA1 && memory[7] == 0xD4 &&
                   PPC_LOAD_U32(4) == 0xA1B2C3D4u,
               "ordinary RAM semantics changed")) {
        return 1;
    }

    if (!Check(!RuntimeGuestSourceCoordinator().ConfigureAtStartup(true),
               "ordinary scalar store did not freeze disabled tracking before writing")) {
        return 1;
    }
    uint32_t address_evaluations = 0, value_evaluations = 0;
    auto address = [&](uint32_t value) { ++address_evaluations; return value; };
    auto value = [&](uint64_t bits) { ++value_evaluations; return bits; };
    PPC_STORE_U8(address(0), value(0x9A));
    PPC_STORE_U16(address(2), value(0x1234));
    PPC_STORE_U32(address(4), value(0x56789ABCu));
    PPC_STORE_U64(address(8), value(0x0123456789ABCDEFull));
    if (!Check(address_evaluations == 4 && value_evaluations == 4 &&
                   memory[0] == 0x9A && memory[1] == 0 &&
                   memory[2] == 0x12 && memory[3] == 0x34 &&
                   PPC_LOAD_U32(4) == 0x56789ABCu &&
                   memory[8] == 0x01 && memory[15] == 0xEF &&
                   physical_notifications == 0,
               "disabled scalar stores changed width, byte order, argument evaluation or alias filtering")) {
        return 1;
    }

    std::cout << "runtime memory-access tests passed\n";
    return 0;
}
