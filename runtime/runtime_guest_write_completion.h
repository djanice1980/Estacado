#pragma once

#include "runtime_memory_access.h"
#include "runtime_source_memory_coordinator.h"

// For import callbacks that populate a validated output span through host
// pointers. Notify on every exit, including errors after partial output. This
// reports completed writes only; it does not make guest memory race-free.
class RuntimeGuestWriteCompletion {
public:
    RuntimeGuestWriteCompletion(uint64_t address, uint32_t bytes)
        : address_(address), bytes_(bytes), write_scope_(address, bytes) {}
    RuntimeGuestWriteCompletion(const RuntimeGuestWriteCompletion&) = delete;
    RuntimeGuestWriteCompletion& operator=(const RuntimeGuestWriteCompletion&) = delete;
    ~RuntimeGuestWriteCompletion() {
        if (address_ <= UINT32_MAX && bytes_ <= (uint64_t(1) << 32) - address_ &&
            RuntimeGuestStoreMayReachPhysicalMemory(static_cast<uint32_t>(address_), bytes_))
            RuntimeNotifyGuestPhysicalWrite(static_cast<uint32_t>(address_), bytes_, __FILE__, __LINE__);
    }
private:
    uint64_t address_;
    uint32_t bytes_;
    RuntimeGuestSourceWriteScope write_scope_;
};
