#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct RuntimePackageFileValidation {
    std::string relativePath;
    std::string expectedSha256;
    std::string actualSha256;
    bool valid{};
};

struct RuntimePackageValidation {
    std::filesystem::path manifestPath;
    bool exists{};
    bool valid{};
    std::vector<RuntimePackageFileValidation> files;
    std::vector<std::string> errors;
};

// Verifies immutable deployable files only. Writable profiles, saves, caches,
// logs, original extracted content, and mods are deliberately outside this
// manifest. A missing manifest is permitted for a developer build unless the
// caller explicitly requires one.
RuntimePackageValidation ValidateRuntimePackage(
    const std::filesystem::path& executableDirectory, bool requireManifest);

std::vector<std::string> RuntimePackageValidationLines(
    const RuntimePackageValidation& validation);

