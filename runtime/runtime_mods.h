#pragma once

#include "runtime_filesystem.h"

#include <filesystem>
#include <string>
#include <vector>

struct RuntimeModConflict {
    std::filesystem::path relativePath;
    std::string winnerId;
    std::string shadowedId;
};

struct RuntimeModRequirement {
    std::string dependentId;
    std::string requiredId;
};

struct RuntimeModConfiguration {
    bool manifestPresent{};
    bool enabled{};
    std::filesystem::path manifestPath;
    std::filesystem::path modsRoot;
    std::vector<GuestContentOverlay> layers;
    std::vector<RuntimeModConflict> conflicts;
    std::vector<RuntimeModRequirement> requirements;
};

// Loads the runtime-owned content override manifest. Graphics/display settings
// remain under ReXGlue's authoritative PC configuration parser; this manifest
// controls only read-only loose title content under <executable>/mods.
RuntimeModConfiguration LoadRuntimeModConfiguration(
    const std::filesystem::path& manifestPath,
    const std::filesystem::path& executableDirectory);

// Stable, single-line output for launchers and settings tools. Validation is
// deliberately read-only and never mounts an overlay or starts guest code.
std::vector<std::string> RuntimeModConfigurationLines(
    const RuntimeModConfiguration& configuration);
std::vector<std::string> RuntimeModValidationErrorLines(
    const std::filesystem::path& manifestPath, const std::string& error);
