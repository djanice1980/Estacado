#include "runtime_function_trace.h"
#include "runtime_memory_access.h"
#include "runtime_generated_atomic_write.h"

bool TestGeneratedConditionalStore(uint8_t* base, uint32_t address,
                                   uint32_t expected, uint32_t desired) {
    return __sync_bool_compare_and_swap(reinterpret_cast<uint32_t*>(base + address),
                                       expected, __builtin_bswap32(desired));
}

bool TestGeneratedConditionalStore64(uint8_t* base, uint32_t address,
                                     int64_t expected, uint64_t desired) {
    return __sync_bool_compare_and_swap(reinterpret_cast<uint64_t*>(base + address),
                                       expected, __builtin_bswap64(desired));
}
