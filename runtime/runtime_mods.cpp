#include "runtime_mods.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
constexpr int64_t kMinimumPriority = -100000;
constexpr int64_t kMaximumPriority = 100000;

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

bool IsSafeId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char character) {
        return std::isalnum(character) || character == '-' ||
               character == '_' || character == '.';
    });
}

std::string SingleLine(std::string value) {
    for (char& character : value) {
        if (character == '\r' || character == '\n') character = ' ';
    }
    return value;
}

bool IsSafeRelativePath(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name() ||
        path.has_root_directory()) {
        return false;
    }
    for (const auto& component : path) {
        if (component.empty() || component == "." || component == "..")
            return false;
    }
    return true;
}

bool EqualComponent(const std::filesystem::path& left,
                    const std::filesystem::path& right) {
    return Lower(left.string()) == Lower(right.string());
}

bool IsWithin(const std::filesystem::path& root,
              const std::filesystem::path& candidate) {
    auto rootPart = root.begin();
    auto candidatePart = candidate.begin();
    for (; rootPart != root.end(); ++rootPart, ++candidatePart) {
        if (candidatePart == candidate.end() ||
            !EqualComponent(*rootPart, *candidatePart)) {
            return false;
        }
    }
    return true;
}

void RejectLinkedComponent(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error) throw std::runtime_error("unable to inspect mod path: " + path.string());
    bool linked = std::filesystem::is_symlink(status);
#ifdef _WIN32
    // Junctions and other reparse points must not vanish during canonicalization.
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        throw std::runtime_error("unable to inspect mod path attributes: " + path.string());
    linked = linked || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#endif
    if (linked) throw std::runtime_error("mod path may not contain links or reparse points: " + path.string());
}

std::filesystem::path CanonicalDirectory(
    const std::filesystem::path& path, const char* description) {
    std::error_code error;
    const auto canonical = std::filesystem::canonical(path, error);
    if (error || !std::filesystem::is_directory(canonical, error) || error) {
        throw std::runtime_error(std::string(description) +
                                 " is not an accessible directory: " +
                                 path.string());
    }
    return canonical;
}

void CollectConflicts(RuntimeModConfiguration& configuration) {
    std::unordered_map<std::string, std::pair<std::string, std::filesystem::path>>
        winners;
    for (const auto& layer : configuration.layers) {
        std::error_code error;
        std::filesystem::recursive_directory_iterator iterator(
            layer.root, std::filesystem::directory_options::skip_permission_denied,
            error);
        const std::filesystem::recursive_directory_iterator end;
        if (error) {
            throw std::runtime_error("unable to enumerate mod layer: " +
                                     layer.id);
        }
        while (iterator != end) {
            const auto& entry = *iterator;
            const auto status = entry.symlink_status(error);
            if (error) {
                throw std::runtime_error("unable to inspect mod entry in: " +
                                         layer.id);
            }
            if (std::filesystem::is_symlink(status)) {
                if (std::filesystem::is_directory(status))
                    iterator.disable_recursion_pending();
                throw std::runtime_error(
                    "mod layers may not contain symbolic links or junctions: " +
                    entry.path().string());
            }
            if (std::filesystem::is_regular_file(status)) {
                const auto canonical = std::filesystem::canonical(entry.path(), error);
                if (error || !IsWithin(layer.root, canonical)) {
                    throw std::runtime_error("mod entry escapes its layer root: " +
                                             entry.path().string());
                }
                const auto relative =
                    std::filesystem::relative(canonical, layer.root, error);
                if (error || !IsSafeRelativePath(relative)) {
                    throw std::runtime_error("invalid mod entry path: " +
                                             entry.path().string());
                }
                const std::string key = Lower(relative.generic_string());
                const auto [winner, inserted] =
                    winners.emplace(key, std::make_pair(layer.id, relative));
                if (!inserted) {
                    configuration.conflicts.push_back(
                        {winner->second.second, winner->second.first, layer.id});
                }
            }
            iterator.increment(error);
            if (error) {
                throw std::runtime_error("unable to enumerate mod layer: " +
                                         layer.id);
            }
        }
    }
}
}

RuntimeModConfiguration LoadRuntimeModConfiguration(
    const std::filesystem::path& manifestPath,
    const std::filesystem::path& executableDirectory) {
    RuntimeModConfiguration configuration{};
    configuration.manifestPath =
        std::filesystem::absolute(manifestPath).lexically_normal();
    configuration.modsRoot =
        std::filesystem::absolute(executableDirectory / L"mods").lexically_normal();

    std::error_code error;
    const auto manifestStatus =
        std::filesystem::status(configuration.manifestPath, error);
    if (error == std::errc::no_such_file_or_directory) error.clear();
    if (error) {
        throw std::runtime_error("unable to inspect mod manifest: " +
                                 configuration.manifestPath.string());
    }
    configuration.manifestPresent =
        std::filesystem::is_regular_file(manifestStatus);
    if (std::filesystem::exists(manifestStatus) &&
        !configuration.manifestPresent) {
        throw std::runtime_error("mod manifest is not a regular file: " +
                                 configuration.manifestPath.string());
    }
    if (!configuration.manifestPresent) return configuration;

    toml::table manifest;
    try {
        manifest = toml::parse_file(configuration.manifestPath.string());
    } catch (const toml::parse_error& parseError) {
        throw std::runtime_error("invalid mod manifest: " +
                                 std::string(parseError.description()));
    }

    const auto version = manifest["mods_config_version"].value<int64_t>();
    if (!version || (*version != 1 && *version != 2)) {
        throw std::runtime_error("unsupported mod manifest version");
    }
    const auto enabled = manifest["enabled"].value<bool>();
    if (!enabled) throw std::runtime_error("mod manifest enabled must be boolean");
    configuration.enabled = *enabled;
    if (!configuration.enabled) return configuration;

    RejectLinkedComponent(configuration.modsRoot);
    configuration.modsRoot = CanonicalDirectory(configuration.modsRoot, "mods root");
    const auto* entries = manifest["mod"].as_array();
    if (!entries) throw std::runtime_error("enabled mod manifest has no [[mod]] entries");

    std::unordered_set<std::string> ids;
    for (const auto& node : *entries) {
        const auto* entry = node.as_table();
        if (!entry) throw std::runtime_error("[[mod]] entry is not a table");
        bool entryEnabled = true;
        if ((*entry).contains("enabled")) {
            const auto value = (*entry)["enabled"].value<bool>();
            if (!value)
                throw std::runtime_error("mod enabled must be boolean");
            entryEnabled = *value;
        }
        if (!entryEnabled) continue;

        const auto id = (*entry)["id"].value<std::string>();
        const auto pathText = (*entry)["path"].value<std::string>();
        int64_t priority{};
        if ((*entry).contains("priority")) {
            const auto value = (*entry)["priority"].value<int64_t>();
            if (!value)
                throw std::runtime_error("mod priority must be an integer");
            priority = *value;
        }
        if (!id || !IsSafeId(*id))
            throw std::runtime_error("mod id is missing or invalid");
        if (!pathText)
            throw std::runtime_error("mod path is missing for: " + *id);
        if (priority < kMinimumPriority || priority > kMaximumPriority)
            throw std::runtime_error("mod priority is out of range for: " + *id);
        const std::string canonicalId = Lower(*id);
        if (!ids.insert(canonicalId).second)
            throw std::runtime_error("duplicate mod id: " + *id);

        if (entry->contains("requires")) {
            if (*version != 2)
                throw std::runtime_error("mod requires needs manifest version 2: " + *id);
            const auto* required = (*entry)["requires"].as_array();
            if (!required) throw std::runtime_error("mod requires must be an array: " + *id);
            std::unordered_set<std::string> uniqueRequirements;
            for (const auto& requirement : *required) {
                const auto name = requirement.value<std::string>();
                if (!name || !IsSafeId(*name))
                    throw std::runtime_error("invalid required mod id: " + *id);
                const auto normalized = Lower(*name);
                if (normalized == canonicalId || !uniqueRequirements.insert(normalized).second)
                    throw std::runtime_error("self or duplicate mod requirement: " + *id);
                configuration.requirements.push_back({*id, normalized});
            }
        }

        const std::filesystem::path relative(*pathText);
        if (!IsSafeRelativePath(relative))
            throw std::runtime_error("mod path must stay under the mods directory: " +
                                     *id);
        auto componentPath = configuration.modsRoot;
        for (const auto& component : relative) {
            componentPath /= component;
            RejectLinkedComponent(componentPath);
        }
        const auto root = CanonicalDirectory(configuration.modsRoot / relative,
                                             "mod layer");
        if (!IsWithin(configuration.modsRoot, root))
            throw std::runtime_error("mod layer escapes the mods directory: " + *id);
        configuration.layers.push_back(
            {*id, root, static_cast<int32_t>(priority)});
    }
    if (configuration.layers.empty())
        throw std::runtime_error("enabled mod manifest has no active mod layers");

    // Requirements constrain enabled membership, not override precedence.
    // Forward references and mutually dependent data packs are valid.
    for (const auto& requirement : configuration.requirements) {
        if (ids.find(requirement.requiredId) == ids.end())
            throw std::runtime_error("missing enabled mod requirement: " +
                requirement.dependentId + " requires " + requirement.requiredId);
    }
    std::sort(configuration.requirements.begin(), configuration.requirements.end(),
        [](const auto& a, const auto& b) {
            if (Lower(a.dependentId) != Lower(b.dependentId))
                return Lower(a.dependentId) < Lower(b.dependentId);
            return a.requiredId < b.requiredId;
        });

    std::sort(configuration.layers.begin(), configuration.layers.end(),
              [](const auto& left, const auto& right) {
                  if (left.priority != right.priority)
                      return left.priority > right.priority;
                  return Lower(left.id) < Lower(right.id);
              });
    CollectConflicts(configuration);
    return configuration;
}

std::vector<std::string> RuntimeModConfigurationLines(
    const RuntimeModConfiguration& configuration) {
    std::vector<std::string> lines;
    lines.reserve(6 + configuration.layers.size() +
                  configuration.conflicts.size());
    lines.push_back("MOD_CONFIG_PATH=" +
                    SingleLine(configuration.manifestPath.string()));
    lines.push_back("MOD_CONFIG_PRESENT=" +
                    std::to_string(configuration.manifestPresent ? 1 : 0));
    lines.push_back("MOD_CONFIG_VALID=1");
    lines.push_back("MOD_CONFIG_ENABLED=" +
                    std::to_string(configuration.enabled ? 1 : 0));
    lines.push_back("MOD_LAYER_COUNT=" +
                    std::to_string(configuration.layers.size()));
    for (const auto& layer : configuration.layers) {
        std::ostringstream line;
        line << "MOD_LAYER=" << layer.id << '|' << layer.priority << '|'
             << SingleLine(layer.root.string());
        lines.push_back(line.str());
    }
    lines.push_back("MOD_CONFLICT_COUNT=" +
                    std::to_string(configuration.conflicts.size()));
    for (const auto& conflict : configuration.conflicts) {
        lines.push_back("MOD_CONFLICT=" +
                        SingleLine(conflict.relativePath.generic_string()) +
                        '|' + conflict.winnerId + '|' + conflict.shadowedId);
    }
    lines.push_back("MOD_REQUIREMENT_COUNT=" + std::to_string(configuration.requirements.size()));
    for (const auto& requirement : configuration.requirements)
        lines.push_back("MOD_REQUIREMENT=" + requirement.dependentId + '|' + requirement.requiredId);
    return lines;
}

std::vector<std::string> RuntimeModValidationErrorLines(
    const std::filesystem::path& manifestPath, const std::string& error) {
    std::error_code filesystemError;
    const bool present =
        std::filesystem::is_regular_file(manifestPath, filesystemError) &&
        !filesystemError;
    return {
        "MOD_CONFIG_PATH=" + SingleLine(
            std::filesystem::absolute(manifestPath).lexically_normal().string()),
        "MOD_CONFIG_PRESENT=" + std::to_string(present ? 1 : 0),
        "MOD_CONFIG_VALID=0",
        "MOD_CONFIG_ERROR=" + SingleLine(error),
    };
}
