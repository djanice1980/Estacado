#include "runtime_mmio.h"
#include "runtime_guest_physical_range.h"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

// The GPU page tracker (runtime_graphics.cpp; unit tests provide a stub).
void RuntimeNotifyGuestPhysicalWrite(uint32_t guest_address, uint32_t length,
                                     const char* source_file, uint32_t source_line) noexcept;

namespace {
struct MmioRange {
    uint32_t first;
    uint32_t last;
    void* context;
    RuntimeMmioRead32 read;
    RuntimeMmioWrite32 write;
};

std::mutex mmio_mutex;
std::vector<MmioRange> mmio_ranges;

bool FindRange(uint32_t address, MmioRange& result) {
    std::lock_guard<std::mutex> lock(mmio_mutex);
    const auto found = std::find_if(mmio_ranges.begin(), mmio_ranges.end(),
        [address](const MmioRange& range) {
            return address >= range.first && address <= range.last;
        });
    if (found == mmio_ranges.end()) return false;
    result = *found;
    return true;
}

[[noreturn]] void UnsupportedWidth(uint32_t address, uint32_t width) {
    throw std::runtime_error("unsupported MMIO width=" + std::to_string(width) +
                             " address=" + std::to_string(address));
}
}

bool RegisterRuntimeMmioRange(uint32_t first, uint32_t last, void* context,
                              RuntimeMmioRead32 read, RuntimeMmioWrite32 write) {
    if (first > last || !context || !read || !write) return false;
    std::lock_guard<std::mutex> lock(mmio_mutex);
    for (const auto& range : mmio_ranges) {
        if (first <= range.last && last >= range.first) return false;
    }
    mmio_ranges.push_back({first, last, context, read, write});
    return true;
}

void ResetRuntimeMmioRangesForTests() {
    std::lock_guard<std::mutex> lock(mmio_mutex);
    mmio_ranges.clear();
}

bool IsRuntimeMmioAddress(uint32_t address) {
    MmioRange range{};
    return FindRange(address, range);
}

uint8_t RuntimeMmioLoadU8(uint8_t* base, uint32_t address) {
    MmioRange range{};
    if (FindRange(address, range)) UnsupportedWidth(address, 1);
    return *reinterpret_cast<volatile uint8_t*>(base + address);
}

uint16_t RuntimeMmioLoadU16(uint8_t* base, uint32_t address) {
    MmioRange range{};
    if (FindRange(address, range)) UnsupportedWidth(address, 2);
    return __builtin_bswap16(*reinterpret_cast<volatile uint16_t*>(base + address));
}

uint32_t RuntimeMmioLoadU32(uint8_t* base, uint32_t address) {
    MmioRange range{};
    if (FindRange(address, range)) {
        if (address & 3u) throw std::runtime_error("unaligned 32-bit MMIO read");
        return range.read(range.context, address);
    }
    return __builtin_bswap32(*reinterpret_cast<volatile uint32_t*>(base + address));
}

uint64_t RuntimeMmioLoadU64(uint8_t* base, uint32_t address) {
    MmioRange range{};
    if (FindRange(address, range)) UnsupportedWidth(address, 8);
    return __builtin_bswap64(*reinterpret_cast<volatile uint64_t*>(base + address));
}

// A statically tagged MMIO store (PPC_MM_STORE_*, 12 sites in the title's D3D
// library) that lands in guest RAM is an ordinary guest store. Like every
// generated store it must reach the GPU page tracker after the payload, or a
// GPU copy of the page (D3D command-buffer memory also holds inline vertex
// data) keeps stale bytes.
static void NotifyRamStore(uint32_t address, uint32_t bytes) {
    uint32_t physical = 0;
    if (RuntimeCanonicalGuestPhysicalRange(address, bytes, physical)) {
        RuntimeNotifyGuestPhysicalWrite(address, bytes, nullptr, 0);
    }
}

void RuntimeMmioStoreU8(uint8_t* base, uint32_t address, uint8_t value) {
    MmioRange range{};
    if (FindRange(address, range)) UnsupportedWidth(address, 1);
    *reinterpret_cast<volatile uint8_t*>(base + address) = value;
    NotifyRamStore(address, 1u);
}

void RuntimeMmioStoreU16(uint8_t* base, uint32_t address, uint16_t value) {
    MmioRange range{};
    if (FindRange(address, range)) UnsupportedWidth(address, 2);
    *reinterpret_cast<volatile uint16_t*>(base + address) = __builtin_bswap16(value);
    NotifyRamStore(address, 2u);
}

void RuntimeMmioStoreU32(uint8_t* base, uint32_t address, uint32_t value) {
    MmioRange range{};
    if (FindRange(address, range)) {
        if (address & 3u) throw std::runtime_error("unaligned 32-bit MMIO write");
        range.write(range.context, address, value);
        return;
    }
    *reinterpret_cast<volatile uint32_t*>(base + address) = __builtin_bswap32(value);
    NotifyRamStore(address, 4u);
}

void RuntimeMmioStoreU64(uint8_t* base, uint32_t address, uint64_t value) {
    MmioRange range{};
    if (FindRange(address, range)) UnsupportedWidth(address, 8);
    *reinterpret_cast<volatile uint64_t*>(base + address) = __builtin_bswap64(value);
    NotifyRamStore(address, 8u);
}
