#include "runtime_package_validation.h"

#include "runtime_graphics_cache.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

namespace {
constexpr const wchar_t* kPackageManifestName = L"TheDarkness.package.toml";

std::string UpperAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::toupper(character));
                   });
    return value;
}

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

bool IsSha256(std::string_view value) {
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](unsigned char character) {
               return std::isxdigit(character) != 0;
           });
}

bool IsContainedRelativePath(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_path()) return false;
    const std::filesystem::path normalized = path.lexically_normal();
    if (normalized.empty() || normalized == L".") return false;
    for (const auto& component : normalized) {
        if (component == L".." || component == L".") return false;
    }
    return normalized == path;
}

std::string SingleLine(std::string value) {
    for (char& character : value) {
        if (character == '\r' || character == '\n') character = ' ';
    }
    return value;
}
}  // namespace

RuntimePackageValidation ValidateRuntimePackage(
    const std::filesystem::path& executableDirectory, bool requireManifest) {
    RuntimePackageValidation result{};
    result.manifestPath = executableDirectory / kPackageManifestName;
    std::error_code filesystemError;
    result.exists = std::filesystem::is_regular_file(
                        result.manifestPath, filesystemError) &&
                    !filesystemError;
    if (!result.exists) {
        if (requireManifest) result.errors.push_back("package manifest not found");
        result.valid = result.errors.empty();
        return result;
    }

    toml::table manifest;
    try {
        manifest = toml::parse_file(result.manifestPath.string());
    } catch (const toml::parse_error& error) {
        result.errors.push_back(
            "package manifest TOML parse error: " +
            SingleLine(std::string(error.description())));
        result.valid = false;
        return result;
    }

    const auto schema = manifest["package_schema_version"].value<int64_t>();
    if (!schema || *schema != 1) {
        result.errors.push_back("package_schema_version must be integer 1");
    }
    const toml::array* files = manifest["file"].as_array();
    if (!files || files->empty()) {
        result.errors.push_back("package manifest must contain [[file]] entries");
        result.valid = false;
        return result;
    }

    std::set<std::string> seenPaths;
    std::set<std::string> requiredCore;
    for (size_t index = 0; index < files->size(); ++index) {
        const toml::table* entry = (*files)[index].as_table();
        if (!entry) {
            result.errors.push_back("file entry " + std::to_string(index) +
                                    " must be a table");
            continue;
        }
        const auto pathValue = (*entry)["path"].value<std::string>();
        const auto hashValue = (*entry)["sha256"].value<std::string>();
        if (!pathValue || pathValue->empty()) {
            result.errors.push_back("file entry " + std::to_string(index) +
                                    " has no path");
            continue;
        }
        const std::filesystem::path relative =
            std::filesystem::u8path(*pathValue);
        if (!IsContainedRelativePath(relative)) {
            result.errors.push_back("package file path is not a normalized contained path: " +
                                    *pathValue);
            continue;
        }
        const std::string normalizedPath = relative.lexically_normal().generic_string();
        const std::string pathIdentity = LowerAscii(normalizedPath);
        if (!seenPaths.insert(pathIdentity).second) {
            result.errors.push_back("duplicate package file path: " + normalizedPath);
            continue;
        }
        if (!hashValue || !IsSha256(*hashValue)) {
            result.errors.push_back("package file has invalid SHA-256: " +
                                    normalizedPath);
            continue;
        }

        RuntimePackageFileValidation file{};
        file.relativePath = normalizedPath;
        file.expectedSha256 = UpperAscii(*hashValue);
        const std::filesystem::path fullPath = executableDirectory / relative;
        std::error_code statusError;
        if (!std::filesystem::is_regular_file(fullPath, statusError) ||
            statusError) {
            result.errors.push_back("package file not found: " + normalizedPath);
        } else {
            try {
                file.actualSha256 = RuntimeFileSha256(fullPath);
                file.valid = file.actualSha256 == file.expectedSha256;
                if (!file.valid) {
                    result.errors.push_back("package file hash mismatch: " +
                                            normalizedPath);
                }
            } catch (const std::exception& error) {
                result.errors.push_back("package file hash failed: " +
                                        normalizedPath + ": " + error.what());
            }
        }
        if (pathIdentity == "thedarkness.exe" ||
            pathIdentity == "rexruntime.dll" ||
            pathIdentity == "rexgpu-xenos.dll") {
            requiredCore.insert(pathIdentity);
        }
        result.files.push_back(std::move(file));
    }

    constexpr const char* coreFiles[] = {
        "thedarkness.exe", "rexruntime.dll", "rexgpu-xenos.dll"};
    for (const char* core : coreFiles) {
        if (!requiredCore.count(core)) {
            result.errors.push_back(std::string("required core package file is absent: ") +
                                    core);
        }
    }
    result.valid = result.errors.empty() &&
                   std::all_of(result.files.begin(), result.files.end(),
                               [](const auto& file) { return file.valid; });
    return result;
}

std::vector<std::string> RuntimePackageValidationLines(
    const RuntimePackageValidation& validation) {
    std::vector<std::string> lines;
    lines.push_back("PACKAGE_MANIFEST_PATH=" + validation.manifestPath.string());
    lines.push_back("PACKAGE_MANIFEST_EXISTS=" +
                    std::to_string(validation.exists ? 1 : 0));
    lines.push_back("PACKAGE_VALID=" +
                    std::to_string(validation.valid ? 1 : 0));
    lines.push_back("PACKAGE_FILE_COUNT=" +
                    std::to_string(validation.files.size()));
    for (const auto& file : validation.files) {
        std::ostringstream line;
        line << "PACKAGE_FILE=" << file.relativePath
             << " expected=" << file.expectedSha256
             << " actual=" << file.actualSha256
             << " valid=" << (file.valid ? 1 : 0);
        lines.push_back(line.str());
    }
    lines.push_back("PACKAGE_ERROR_COUNT=" +
                    std::to_string(validation.errors.size()));
    for (const std::string& error : validation.errors) {
        lines.push_back("PACKAGE_ERROR=" + SingleLine(error));
    }
    return lines;
}
