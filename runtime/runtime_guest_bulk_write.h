#pragma once

#include "runtime_memory_access.h"
#include <cstddef>
#include <cstring>

// Guard the payload before clearing and retain the completed-write notification.
// Only readers using the same coordinator participate in this serialization.
inline void* RuntimeGeneratedMemset(uint8_t* base, void* destination, int value,
                                    size_t bytes, const char* file, uint32_t line) {
    const uintptr_t origin = reinterpret_cast<uintptr_t>(base);
    const uintptr_t target = reinterpret_cast<uintptr_t>(destination);
    const RuntimeGuestSourceWriteScope source_write(
        target >= origin ? uint64_t(target - origin) : UINT64_MAX, bytes);
    void* result = std::memset(destination, value, bytes);
    if (target >= origin && target - origin <= UINT32_MAX &&
        bytes <= UINT32_MAX && bytes <= (uint64_t(1) << 32) - (target - origin)) {
        const auto address = static_cast<uint32_t>(target - origin);
        const auto length = static_cast<uint32_t>(bytes);
        if (RuntimeGuestStoreMayReachPhysicalMemory(address, length))
            RuntimeNotifyGuestPhysicalWrite(address, length, file, line);
    }
    return result;
}
