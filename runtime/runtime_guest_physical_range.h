#pragma once
#include <cstdint>

inline bool RuntimeCanonicalGuestPhysicalRange(uint32_t address, uint32_t length,
                                               uint32_t& physical) noexcept {
    if (!length) return false;
    uint64_t result = 0;
    if (address >= 0x7F000000u && address < 0x80000000u) {
        if (length > 0x80000000u - address) return false;
        result = address - 0x7F000000u;
    } else if (address >= 0xA0000000u && address < 0xE0000000u) {
        result = address & 0x1FFFFFFFu;
    } else if (address >= 0xE0000000u && address < 0xFFD00000u) {
        if (length > 0xFFD00000u - address) return false;
        result = uint64_t(address - 0xE0000000u) + 0x1000u;
    } else return false;
    if (result >= 0x20000000ull || result + length > 0x20000000ull) return false;
    physical = static_cast<uint32_t>(result);
    return true;
}
