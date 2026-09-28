#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct Image;

// Runtime-private guest storage for the title's immutable 0x18-byte
// XEX_HEADER_EXECUTION_INFO record. This lives in the page already reserved
// for small runtime-owned guest wrappers, below PCR/TEB storage and above the
// bounded guest kernel-object range.
inline constexpr uint32_t kExecutableExecutionInfoGuestAddress = 0x7D0FFE00u;
inline constexpr size_t kExecutableExecutionInfoBytes = 0x18;

void ConfigureExecutableSystemFlags(const uint8_t* xex, size_t size);
struct RuntimeAchievement {
    uint32_t id{};
    std::string label;
    std::string description;
    std::string unachievedDescription;
    uint32_t imageId{};
    uint32_t gamerscore{};
    uint32_t flags{};
};

bool ConfigureExecutableTitleMetadata(const uint8_t* xex, size_t size,
                                      const Image& image);
const std::vector<RuntimeAchievement>& ExecutableAchievements();
void ConfigureExecutableAchievementsForTests(std::vector<RuntimeAchievement> achievements);
uint32_t ExecutableTitleId();
bool PublishExecutableExecutionInfo(uint8_t* base, uint32_t guestAddress);
uint32_t ExecutableExecutionInfoAddress();
bool ExecutableHasPrivilege(uint32_t privilege);
