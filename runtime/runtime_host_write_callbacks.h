#pragma once
#include "runtime_source_memory_coordinator.h"
#include <array>
#include <exception>

// Plugin callback tokens are thread-local RAII slots, avoiding allocation for
// every writeback. Overflow, cross-thread or out-of-order completion cannot
// silently continue with unguarded data. No exception crosses the plugin ABI.
namespace runtime_host_writes {
struct Stack {
    std::array<std::optional<RuntimeSourceMemoryCoordinator::Write>, 64> slots;
    size_t depth{};
};
inline Stack& Current() { thread_local Stack stack; return stack; }
inline void* Begin(void*, uint32_t physical, uint32_t bytes) noexcept {
    auto& stack = Current();
    if (stack.depth == stack.slots.size()) std::terminate();
    auto& slot = stack.slots[stack.depth];
    slot.emplace(RuntimeGuestSourceCoordinator(), RuntimeSourceWriteEpochs::Range{physical, bytes});
    ++stack.depth;
    return &slot;
}
inline void End(void*, void* token) noexcept {
    auto& stack = Current();
    if (!stack.depth || token != &stack.slots[stack.depth - 1]) std::terminate();
    stack.slots[--stack.depth].reset();
}
} // namespace runtime_host_writes
