#pragma once
#include <cstdint>

namespace darkness::experiments {
// Raw XEX: 8223E39C writes interval at SP+104; 8223E534 passes SP+D0
// to 8286FD68. That path copies parameter+34 to device+3504.
// This is an experimental render deadline policy, NOT a simulation-rate policy.
constexpr bool IsTitlePresentationCreation(uint32_t entry, uint32_t caller,
    uint32_t parameters, uint32_t stack) noexcept {
    return entry == 0x8286FD68u && caller == 0x8223E538u &&
        stack <= 0xFFFFFE83u && parameters == stack + 0xD0u;
}
constexpr uint32_t ExperimentalPresentInterval(bool enabled, uint32_t original) noexcept {
    // Preserve immediate, default, interval1, interval3 and unknown values.
    return enabled && original == 2u ? 1u : original;
}
constexpr uint32_t ExperimentalImmediateInterval(bool enabled, uint32_t original) noexcept {
    // Verified title-supported immediate selector, not a host present mode.
    // Change only the known title default; preserve resets/unknown values.
    return enabled && original == 2u ? 0x80000000u : original;
}
}  // namespace darkness::experiments
