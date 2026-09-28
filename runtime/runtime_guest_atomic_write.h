#pragma once
#include "runtime_memory_access.h"

// One attempted conditional store, not the surrounding guest retry loop. Keep
// the compiler's original atomic operation and its raw guest-endian operands.
template <typename T, typename Expected, typename Desired>
inline bool RuntimeGeneratedCompareExchange(uint8_t* base, T* destination,
    Expected expected, Desired desired, const char* file, uint32_t line) {
    static_assert(sizeof(T) == 4 || sizeof(T) == 8);
    const uintptr_t origin = reinterpret_cast<uintptr_t>(base);
    const uintptr_t target = reinterpret_cast<uintptr_t>(destination);
    const uint64_t address = target >= origin ? uint64_t(target - origin) : UINT64_MAX;
    const RuntimeGuestSourceWriteScope source_write(address, sizeof(T));
    const bool exchanged = __sync_bool_compare_and_swap(destination, static_cast<T>(expected), static_cast<T>(desired));
    if (exchanged && address <= UINT32_MAX &&
        RuntimeGuestStoreMayReachPhysicalMemory(static_cast<uint32_t>(address), sizeof(T)))
        RuntimeNotifyGuestPhysicalWrite(static_cast<uint32_t>(address), sizeof(T), file, line);
    return exchanged;
}
