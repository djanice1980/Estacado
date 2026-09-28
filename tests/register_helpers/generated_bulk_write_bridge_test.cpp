// Match the production forced-header order, then execute the unmodified
// generator's dcbzl statement through the actual compile-time bridge.
#include "runtime_function_trace.h"
#include "runtime_memory_access.h"
#include "runtime_generated_bulk_write.h"

void TestGeneratedCacheClear(uint8_t* base, uint32_t address) {
    memset(base + ((address) & ~127), 0, 128);
}
