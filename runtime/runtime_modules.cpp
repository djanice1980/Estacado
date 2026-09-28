#include "runtime_modules.h"
#include "runtime_guest_write_completion.h"

#include "image.h"
#include "xex.h"
#include "xdbf.h"
#include "xdbf_wrapper.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {
uint32_t systemFlags{};
std::array<uint8_t, kExecutableExecutionInfoBytes> executionInfo{};
bool hasExecutionInfo{};
uint32_t executionInfoGuestAddress{};
std::vector<RuntimeAchievement> executableAchievements;

uint32_t ReadBigEndian32(const uint8_t* value) {
    return (uint32_t(value[0]) << 24) | (uint32_t(value[1]) << 16) |
           (uint32_t(value[2]) << 8) | uint32_t(value[3]);
}

bool ContainsBlock(const uint8_t* begin, size_t bytes, const uint8_t* block,
                   size_t blockBytes) {
    if (!begin || !block || block < begin) return false;
    const size_t offset = static_cast<size_t>(block - begin);
    return offset <= bytes && blockBytes <= bytes - offset;
}
}

void ConfigureExecutableSystemFlags(const uint8_t* xex, size_t size) {
    systemFlags = 0;
    executionInfo.fill(0);
    hasExecutionInfo = false;
    executionInfoGuestAddress = 0;
    executableAchievements.clear();
    if (!xex || size < sizeof(Xex2Header)) return;
    const auto* header = reinterpret_cast<const Xex2Header*>(xex);
    const size_t count = header->headerCount;
    if (count > (size - sizeof(Xex2Header)) / sizeof(Xex2OptHeader)) return;
    const auto* options = reinterpret_cast<const Xex2OptHeader*>(xex + sizeof(Xex2Header));
    for (size_t i = 0; i < count; ++i) {
        if (options[i].key == XEX_HEADER_SYSTEM_FLAGS) {
            systemFlags = options[i].value;
        } else if (options[i].key == XEX_HEADER_EXECUTION_INFO) {
            const size_t offset = options[i].offset;
            if (offset <= size && kExecutableExecutionInfoBytes <= size - offset) {
                std::memcpy(executionInfo.data(), xex + offset, executionInfo.size());
                hasExecutionInfo = true;
            }
        }
    }
}

bool ConfigureExecutableTitleMetadata(const uint8_t* xex, size_t size,
                                      const Image& image) {
    executableAchievements.clear();
    if (!xex || size < sizeof(Xex2Header) || !image.data || !hasExecutionInfo)
        return false;

    const auto* header = reinterpret_cast<const Xex2Header*>(xex);
    const size_t optionCount = header->headerCount;
    if (optionCount > (size - sizeof(Xex2Header)) / sizeof(Xex2OptHeader))
        return false;
    const auto* options = reinterpret_cast<const Xex2OptHeader*>(
        xex + sizeof(Xex2Header));
    size_t resourceHeaderOffset = SIZE_MAX;
    for (size_t i = 0; i < optionCount; ++i) {
        if (options[i].key == XEX_HEADER_RESOURCE_INFO) {
            resourceHeaderOffset = options[i].offset;
            break;
        }
    }
    if (resourceHeaderOffset > size || 4 > size - resourceHeaderOffset)
        return false;
    const uint8_t* resourceHeader = xex + resourceHeaderOffset;
    const uint32_t resourceHeaderBytes = ReadBigEndian32(resourceHeader);
    if (resourceHeaderBytes < 4 || (resourceHeaderBytes - 4) % 16 ||
        resourceHeaderBytes > size - resourceHeaderOffset)
        return false;

    const uint32_t titleId = ReadBigEndian32(executionInfo.data() + 0x0C);
    char expectedName[9]{};
    std::snprintf(expectedName, sizeof(expectedName), "%08X", titleId);
    uint32_t resourceAddress{};
    uint32_t resourceBytes{};
    const uint32_t resourceCount = (resourceHeaderBytes - 4) / 16;
    for (uint32_t i = 0; i < resourceCount; ++i) {
        const uint8_t* descriptor = resourceHeader + 4 + i * 16;
        if (!std::memcmp(descriptor, expectedName, 8)) {
            resourceAddress = ReadBigEndian32(descriptor + 8);
            resourceBytes = ReadBigEndian32(descriptor + 12);
            break;
        }
    }
    if (!resourceAddress || !resourceBytes || resourceAddress < image.base)
        return false;
    const uint64_t resourceRva = uint64_t(resourceAddress) - image.base;
    if (resourceRva > image.size || resourceBytes > image.size - resourceRva)
        return false;
    const uint8_t* resourceData = image.data.get() + resourceRva;

    XDBFWrapper database(resourceData, resourceBytes);
    if (!database.pBuffer) return false;
    const XDBFBlock achievementBlock = database.GetResource(
        XDBF_SPA_NAMESPACE_METADATA, XACH_SIGNATURE);
    if (!achievementBlock ||
        !ContainsBlock(resourceData, resourceBytes, achievementBlock.pBuffer,
                       achievementBlock.BufferSize) ||
        achievementBlock.BufferSize < sizeof(XACHHeader))
        return false;
    const auto* achievementHeader =
        reinterpret_cast<const XACHHeader*>(achievementBlock.pBuffer);
    const uint32_t achievementCount = achievementHeader->AchievementCount;
    if (achievementHeader->Signature != XACH_SIGNATURE ||
        achievementCount >
            (achievementBlock.BufferSize - sizeof(XACHHeader)) / sizeof(XACHEntry))
        return false;

    EXDBFLanguage language = XDBF_LANGUAGE_ENGLISH;
    const XDBFBlock titleConfiguration = database.GetResource(
        XDBF_SPA_NAMESPACE_METADATA, 0x58535443u);
    if (titleConfiguration &&
        ContainsBlock(resourceData, resourceBytes, titleConfiguration.pBuffer,
                      titleConfiguration.BufferSize) &&
        titleConfiguration.BufferSize >= sizeof(XSTCHeader)) {
        const auto* configuration =
            reinterpret_cast<const XSTCHeader*>(titleConfiguration.pBuffer);
        if (configuration->Signature == 0x58535443u &&
            configuration->Language > XDBF_LANGUAGE_UNKNOWN &&
            configuration->Language < XDBF_LANGUAGE_MAX) {
            language = configuration->Language;
        }
    }

    const auto* entries = reinterpret_cast<const XACHEntry*>(
        achievementBlock.pBuffer + sizeof(XACHHeader));
    executableAchievements.reserve(achievementCount);
    for (uint32_t i = 0; i < achievementCount; ++i) {
        const auto& entry = entries[i];
        executableAchievements.push_back(RuntimeAchievement{
            entry.AchievementID,
            database.GetString(language, entry.NameID),
            database.GetString(language, entry.UnlockedDescID),
            database.GetString(language, entry.LockedDescID),
            entry.ImageID,
            entry.Gamerscore,
            entry.Flags,
        });
    }
    return true;
}

const std::vector<RuntimeAchievement>& ExecutableAchievements() {
    return executableAchievements;
}

void ConfigureExecutableAchievementsForTests(
    std::vector<RuntimeAchievement> achievements) {
    executableAchievements = std::move(achievements);
}

uint32_t ExecutableTitleId() {
    return hasExecutionInfo ? ReadBigEndian32(executionInfo.data() + 0x0C) : 0;
}

bool PublishExecutableExecutionInfo(uint8_t* base, uint32_t guestAddress) {
    executionInfoGuestAddress = 0;
    if (!base || !hasExecutionInfo || (guestAddress & 3u) ||
        guestAddress > UINT32_MAX - executionInfo.size()) {
        return false;
    }
    {
        const RuntimeGuestWriteCompletion completion(guestAddress,
            static_cast<uint32_t>(executionInfo.size()));
        std::memcpy(base + guestAddress, executionInfo.data(), executionInfo.size());
    }
    executionInfoGuestAddress = guestAddress;
    return true;
}

uint32_t ExecutableExecutionInfoAddress() {
    return executionInfoGuestAddress;
}

bool ExecutableHasPrivilege(uint32_t privilege) {
    return privilege < 32 && (systemFlags & (uint32_t(1) << privilege)) != 0;
}
