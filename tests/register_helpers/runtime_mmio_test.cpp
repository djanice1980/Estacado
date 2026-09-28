#include "runtime_mmio.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {
struct Device {
    uint32_t read_value = 0x11223344u;
    uint32_t read_address{};
    uint32_t write_address{};
    uint32_t write_value{};
};

uint32_t Read(void* context, uint32_t address) {
    auto& device = *static_cast<Device*>(context);
    device.read_address = address;
    return device.read_value;
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

uint32_t physical_notifications{};
uint32_t last_notified_address{};
uint32_t last_notified_length{};
}

// The GPU page tracker: tagged MMIO stores that land in RAM notify it.
void RuntimeNotifyGuestPhysicalWrite(uint32_t address, uint32_t length, const char*,
                                     uint32_t) noexcept {
    ++physical_notifications;
    last_notified_address = address;
    last_notified_length = length;
}

int main() {
    alignas(8) uint8_t memory[0x300]{};
    Device device{};
    ResetRuntimeMmioRangesForTests();

    RuntimeMmioStoreU32(memory, 4, 0xA1B2C3D4u);
    if (!Check(memory[4] == 0xA1 && memory[5] == 0xB2 && memory[6] == 0xC3 &&
                   memory[7] == 0xD4 && RuntimeMmioLoadU32(memory, 4) == 0xA1B2C3D4u,
               "unregistered RAM path changed value or byte order")) return 1;

    if (!Check(RegisterRuntimeMmioRange(0x100, 0x1FF, &device, Read, Write),
               "MMIO range registration failed") ||
        !Check(!RegisterRuntimeMmioRange(0x180, 0x200, &device, Read, Write),
               "overlapping MMIO range was accepted") ||
        !Check(IsRuntimeMmioAddress(0x100) && IsRuntimeMmioAddress(0x1FF) &&
                   !IsRuntimeMmioAddress(0x0FF) && !IsRuntimeMmioAddress(0x200),
               "MMIO range boundaries are incorrect")) return 1;

    RuntimeMmioStoreU32(memory, 0x100, 0x89ABCDEFu);
    if (!Check(device.write_address == 0x100 && device.write_value == 0x89ABCDEFu,
               "MMIO write did not preserve host-order register value") ||
        !Check(RuntimeMmioLoadU32(memory, 0x1FC) == 0x11223344u &&
                   device.read_address == 0x1FC,
               "MMIO read did not route through the device")) return 1;

    bool rejected_width = false;
    try {
        RuntimeMmioStoreU16(memory, 0x100, 0x1234);
    } catch (const std::runtime_error&) {
        rejected_width = true;
    }
    if (!Check(rejected_width, "unsupported MMIO width was silently accepted")) return 1;

    RuntimeMmioStoreU32(memory, 0x200, 0x01020304u);
    if (!Check(memory[0x200] == 1 && memory[0x203] == 4,
               "unregistered address adjacent to MMIO did not retain RAM semantics")) return 1;
    if (!Check(physical_notifications == 0,
               "ordinary (non-physical) RAM through a tagged store notified the GPU tracker"))
        return 1;

    // A tagged store into a physical alias (0xA0000000 = guest physical 0) is
    // guest RAM the GPU may hold a copy of: it must notify the page tracker,
    // after the payload, with the guest address and width.
    alignas(8) uint8_t physical_ram[16]{};
    uint8_t* aliased_base = physical_ram - 0xA0000000u;
    RuntimeMmioStoreU32(aliased_base, 0xA0000004u, 0x0A0B0C0Du);
    RuntimeMmioStoreU16(aliased_base, 0xA0000008u, 0x1122u);
    if (!Check(physical_ram[4] == 0x0A && physical_ram[7] == 0x0D &&
                   physical_ram[8] == 0x11 && physical_ram[9] == 0x22 &&
                   physical_notifications == 2 && last_notified_address == 0xA0000008u &&
                   last_notified_length == 2,
               "tagged store into physical-alias RAM did not notify the GPU page tracker"))
        return 1;

    std::cout << "runtime MMIO tests passed\n";
    return 0;
}
