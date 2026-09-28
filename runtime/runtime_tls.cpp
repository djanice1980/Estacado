#include "runtime_tls.h"

#include <limits>

namespace {
thread_local std::unordered_map<uint32_t, uint32_t> tls_values;
}

uint32_t GuestTls::Allocate() {
    for (uint32_t slot = 0; slot != kOutOfIndexes; ++slot) {
        if (allocated_slots_.insert(slot).second) {
            tls_values[slot] = 0;
            return slot;
        }
    }
    return kOutOfIndexes;
}

bool GuestTls::Free(uint32_t slot) {
    if (slot == kOutOfIndexes || !allocated_slots_.erase(slot)) return false;
    tls_values.erase(slot);
    return true;
}

uint32_t GuestTls::Get(uint32_t slot) const {
    if (!allocated_slots_.count(slot)) return 0;
    const auto it = tls_values.find(slot);
    return it == tls_values.end() ? 0 : it->second;
}

bool GuestTls::Set(uint32_t slot, uint32_t value) {
    if (!allocated_slots_.count(slot)) return false;
    tls_values[slot] = value;
    return true;
}

void GuestTls::ResetForTests() {
    allocated_slots_.clear();
    tls_values.clear();
}

GuestTls& GetGuestTls() {
    static GuestTls tls;
    return tls;
}
