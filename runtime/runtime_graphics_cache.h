#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using RuntimeEnvironmentOverride = std::pair<std::string, std::string>;
class RuntimePcConfigSnapshot;

struct RuntimeGraphicsCacheTreeInput {
    std::string label;
    std::filesystem::path root;
};

struct RuntimeGraphicsCacheCompatibilityInputs {
    std::string backend;
    std::filesystem::path modsManifest;
    std::vector<RuntimeGraphicsCacheTreeInput> modTrees;
};

struct RuntimeGraphicsCacheIdentity {
    std::filesystem::path root;
    std::string executableSha256;
    // Provenance only (logged, not part of the key): the stored records are
    // settings-independent.
    std::string settingsSha256;
    std::string environmentSha256;
    std::string modsSha256;
    std::string compatibilitySha256;
    std::string backend;
    size_t environmentOverrideCount{};
    size_t modFileCount{};
    std::filesystem::path settingsSource;
};

// Returns the per-user writable cache location. The executable-directory
// fallback remains inside runtime_data and never writes into extracted title
// content.
std::filesystem::path RuntimeGraphicsCacheBase(
    const std::filesystem::path& executableDirectory);

std::string RuntimeSha256(const uint8_t* data, size_t size);
std::string RuntimeFileSha256(const std::filesystem::path& path);

// Returns non-observational REX_* process overrides. Values are never logged;
// callers expose only the canonical count and digest.
std::vector<RuntimeEnvironmentOverride>
RuntimeGraphicsCacheEnvironmentOverrides();

// Canonicalizes names case-insensitively (matching Windows environment
// semantics), sorts the assignments, and hashes an unambiguous serialization.
// Returns an empty string when no cache-relevant override is present.
std::string RuntimeGraphicsCacheEnvironmentSha256(
    const std::vector<RuntimeEnvironmentOverride>& overrides);

// Separates persistent graphics storage by title, exact XEX contents,
// cache-affecting environment overrides, backend and active mod contents.
// ReXGlue additionally folds the resolved replacement-shader pack into the
// effective storage leaf.
//
// V290 (identity v3): runtime/GPU binaries and PC settings are no longer part
// of the key, so the store survives updates and settings changes. The store
// holds only guest microcode (content-addressed) and pipeline descriptions
// (hash-checked); both are retranslated/recreated by the current code at every
// load, and the storage files reject format changes through their magic, API
// and version headers (PipelineDescription / Modification / ShaderStoredHeader
// kVersion). A change that must discard every store bumps the schema token.
RuntimeGraphicsCacheIdentity BuildRuntimeGraphicsCacheIdentityFromSnapshots(
    const std::filesystem::path& cacheBase, uint32_t titleId,
    const uint8_t* executableData, size_t executableSize,
    const RuntimePcConfigSnapshot& selectedSettings,
    const RuntimePcConfigSnapshot& fallbackSettings,
    const std::vector<RuntimeEnvironmentOverride>& environmentOverrides,
    const RuntimeGraphicsCacheCompatibilityInputs& compatibilityInputs);

// Convenience for offline callers. Running titles pass their startup snapshots.
RuntimeGraphicsCacheIdentity BuildRuntimeGraphicsCacheIdentity(
    const std::filesystem::path& cacheBase, uint32_t titleId,
    const uint8_t* executableData, size_t executableSize,
    const std::filesystem::path& selectedSettingsPath,
    const std::filesystem::path& fallbackSettingsPath,
    const std::vector<RuntimeEnvironmentOverride>& environmentOverrides,
    const RuntimeGraphicsCacheCompatibilityInputs& compatibilityInputs);
