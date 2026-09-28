#include "runtime_source_memory_coordinator.h"

RuntimeSourceMemoryCoordinator* GuestCoordinatorFromPeer() {
    return &RuntimeGuestSourceCoordinator();
}

void GuestWriteFromPeer(uint32_t address) {
    RuntimeGuestSourceWriteScope write(address, 4, 17);
}
