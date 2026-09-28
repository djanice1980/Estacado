#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

// TLS is guest-visible state. The current runtime executes one guest context
// on one host thread; the thread-local value map intentionally keeps value
// ownership separate from global slot allocation for later thread support.
class GuestTls {
public:
    static constexpr uint32_t kOutOfIndexes = 0xFFFFFFFF;

    uint32_t Allocate();
    bool Free(uint32_t slot);
    uint32_t Get(uint32_t slot) const;
    bool Set(uint32_t slot, uint32_t value);
    void ResetForTests();

private:
    std::unordered_set<uint32_t> allocated_slots_;
};

GuestTls& GetGuestTls();
