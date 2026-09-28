#include "runtime_graphics_cache.h"
#include "runtime_pc_config_snapshot.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
class AlgorithmHandle {
public:
    ~AlgorithmHandle() {
        if (value_) BCryptCloseAlgorithmProvider(value_, 0);
    }
    BCRYPT_ALG_HANDLE* put() { return &value_; }
    BCRYPT_ALG_HANDLE get() const { return value_; }

private:
    BCRYPT_ALG_HANDLE value_{};
};

class HashHandle {
public:
    ~HashHandle() {
        if (value_) BCryptDestroyHash(value_);
    }
    BCRYPT_HASH_HANDLE* put() { return &value_; }
    BCRYPT_HASH_HANDLE get() const { return value_; }

private:
    BCRYPT_HASH_HANDLE value_{};
};

void CheckNtStatus(NTSTATUS status, const char* operation) {
    if (status < 0) {
        std::ostringstream message;
        message << operation << " failed with NTSTATUS 0x" << std::hex
                << static_cast<uint32_t>(status);
        throw std::runtime_error(message.str());
    }
}

class Sha256Builder {
public:
    Sha256Builder() {
        CheckNtStatus(BCryptOpenAlgorithmProvider(
                          algorithm_.put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0),
                      "BCryptOpenAlgorithmProvider(SHA-256)");

        DWORD bytes{};
        CheckNtStatus(BCryptGetProperty(
                          algorithm_.get(), BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectBytes_),
                          sizeof(objectBytes_), &bytes, 0),
                      "BCryptGetProperty(BCRYPT_OBJECT_LENGTH)");
        CheckNtStatus(BCryptGetProperty(
                          algorithm_.get(), BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hashBytes_),
                          sizeof(hashBytes_), &bytes, 0),
                      "BCryptGetProperty(BCRYPT_HASH_LENGTH)");
        if (hashBytes_ != 32) {
            throw std::runtime_error("Windows SHA-256 provider returned an invalid digest size");
        }

        object_.resize(objectBytes_);
        CheckNtStatus(BCryptCreateHash(
                          algorithm_.get(), hash_.put(), object_.data(),
                          static_cast<ULONG>(object_.size()), nullptr, 0, 0),
                      "BCryptCreateHash(SHA-256)");
    }

    void Update(const uint8_t* data, size_t size) {
        if (!data && size) {
            throw std::runtime_error("SHA-256 input is null");
        }
        while (size) {
            const ULONG chunk = static_cast<ULONG>(
                std::min<size_t>(size, static_cast<size_t>(ULONG_MAX)));
            CheckNtStatus(BCryptHashData(hash_.get(),
                                         const_cast<PUCHAR>(data), chunk, 0),
                          "BCryptHashData(SHA-256)");
            data += chunk;
            size -= chunk;
        }
    }

    std::string Finish() {
        std::array<uint8_t, 32> digest{};
        CheckNtStatus(BCryptFinishHash(hash_.get(), digest.data(),
                                       static_cast<ULONG>(digest.size()), 0),
                      "BCryptFinishHash(SHA-256)");
        std::ostringstream text;
        text << std::hex << std::uppercase << std::setfill('0');
        for (uint8_t byte : digest) text << std::setw(2) << unsigned(byte);
        return text.str();
    }

private:
    AlgorithmHandle algorithm_;
    HashHandle hash_;
    DWORD objectBytes_{};
    DWORD hashBytes_{};
    std::vector<uint8_t> object_;
};

std::string TitleDirectoryName(uint32_t titleId) {
    std::ostringstream text;
    text << std::hex << std::uppercase << std::setfill('0')
         << std::setw(8) << titleId;
    return text.str();
}

std::string UpperAscii(std::string_view value) {
    std::string upper(value);
    for (char& character : upper) {
        if (character >= 'a' && character <= 'z') {
            character = static_cast<char>(character - 'a' + 'A');
        }
    }
    return upper;
}

bool IsObservationOnlyRexEnvironment(std::string_view upperName) {
    // These switches only bound or format telemetry (or feed developer test
    // input, or read textures for HD texture pack dumps). They cannot alter
    // shader translation, pipeline descriptions, or presentation.
    static const std::set<std::string_view> kObservationOnly{
        "REX_DIAGNOSTICS_CAMERA_STATE",
        "REX_DISPLAY_PRESENT_DIAGNOSTICS",
        "REX_DISPLAY_PRESENT_STALL_THRESHOLD_MS",
        "REX_EMBEDDED_GAMEPLAY_CAPTURE_COUNT",
        "REX_EMBEDDED_GAMEPLAY_CAPTURE_INTERVAL_SWAPS",
        "REX_EMBEDDED_GAMEPLAY_CAPTURE_START_SWAP",
        "REX_EMBEDDED_HITCH_DIAGNOSTICS",
        "REX_EMBEDDED_CP_CADENCE_DIAGNOSTICS",
        "REX_EMBEDDED_HITCH_TRACE_THRESHOLD_MS",
        "REX_GPU_TEXTURE_DUMP",
        "REX_GPU_TEXTURE_DUMP_ROOT",
        "REX_GPU_TEXTURE_PACK_ROOT",
        "REX_GPU_TEXTURE_REPLACE",
        "REX_GPU_TEXTURE_REPLACE_UPLOAD_MB_PER_FRAME",
        "REX_INPUT_TEST_SCRIPT",
        "REX_LOG_LEVEL",
    };
    return kObservationOnly.find(upperName) != kObservationOnly.end();
}

bool StartsWithRex(std::string_view name) {
    return name.size() >= 4 && name.substr(0, 4) == "REX_";
}

std::vector<RuntimeEnvironmentOverride> CanonicalCacheOverrides(
    const std::vector<RuntimeEnvironmentOverride>& overrides) {
    std::vector<RuntimeEnvironmentOverride> canonical;
    canonical.reserve(overrides.size());
    for (const auto& original : overrides) {
        std::string name = UpperAscii(original.first);
        if (!StartsWithRex(name) || IsObservationOnlyRexEnvironment(name)) {
            continue;
        }
        canonical.emplace_back(std::move(name), original.second);
    }
    std::sort(canonical.begin(), canonical.end());
    canonical.erase(std::unique(canonical.begin(), canonical.end()),
                    canonical.end());
    return canonical;
}

void HashIdentityField(Sha256Builder& hash, std::string_view name,
                       std::string_view value) {
    hash.Update(reinterpret_cast<const uint8_t*>(name.data()), name.size());
    static constexpr uint8_t kSeparator = 0;
    hash.Update(&kSeparator, 1);
    hash.Update(reinterpret_cast<const uint8_t*>(value.data()), value.size());
    hash.Update(&kSeparator, 1);
}

std::string RuntimeGraphicsCacheModsSha256(
    const RuntimeGraphicsCacheCompatibilityInputs& inputs,
    size_t& fileCount) {
    fileCount = 0;
    if (inputs.modsManifest.empty() && inputs.modTrees.empty()) return {};

    Sha256Builder hash;
    HashIdentityField(hash, "schema",
                      "TheDarknessRecompiled-mod-cache-identity-v1");
    if (inputs.modsManifest.empty()) {
        HashIdentityField(hash, "manifest", "none");
    } else {
        HashIdentityField(hash, "manifest",
                          RuntimeFileSha256(inputs.modsManifest));
    }

    std::vector<RuntimeGraphicsCacheTreeInput> trees = inputs.modTrees;
    std::sort(trees.begin(), trees.end(), [](const auto& left,
                                             const auto& right) {
        return left.label < right.label;
    });
    for (size_t index = 0; index < trees.size(); ++index) {
        const auto& tree = trees[index];
        if (tree.label.empty() || tree.root.empty()) {
            throw std::runtime_error(
                "graphics-cache mod tree identity is incomplete");
        }
        if (index && trees[index - 1].label == tree.label) {
            throw std::runtime_error(
                "graphics-cache mod tree labels are not unique");
        }
        std::error_code error;
        if (!std::filesystem::is_directory(tree.root, error) || error) {
            throw std::runtime_error(
                "graphics-cache mod tree is not accessible: " +
                tree.root.string());
        }
        HashIdentityField(hash, "tree", tree.label);

        std::vector<std::pair<std::string, std::filesystem::path>> files;
        std::filesystem::recursive_directory_iterator iterator(
            tree.root,
            std::filesystem::directory_options::skip_permission_denied,
            error);
        const std::filesystem::recursive_directory_iterator end;
        if (error) {
            throw std::runtime_error(
                "unable to enumerate graphics-cache mod tree: " +
                tree.label);
        }
        while (iterator != end) {
            const auto status = iterator->symlink_status(error);
            if (error) {
                throw std::runtime_error(
                    "unable to inspect graphics-cache mod entry: " +
                    iterator->path().string());
            }
            if (std::filesystem::is_symlink(status)) {
                if (std::filesystem::is_directory(status)) {
                    iterator.disable_recursion_pending();
                }
                throw std::runtime_error(
                    "graphics-cache mod trees may not contain symbolic links: " +
                    iterator->path().string());
            }
            if (std::filesystem::is_regular_file(status)) {
                const auto relative = std::filesystem::relative(
                    iterator->path(), tree.root, error);
                if (error || relative.empty()) {
                    throw std::runtime_error(
                        "unable to make graphics-cache mod path relative: " +
                        iterator->path().string());
                }
                files.emplace_back(relative.generic_u8string(),
                                   iterator->path());
            }
            iterator.increment(error);
            if (error) {
                throw std::runtime_error(
                    "unable to enumerate graphics-cache mod tree: " +
                    tree.label);
            }
        }
        std::sort(files.begin(), files.end(),
                  [](const auto& left, const auto& right) {
                      return left.first < right.first;
                  });
        for (const auto& [relative, path] : files) {
            HashIdentityField(hash, "path", relative);
            HashIdentityField(hash, "contents", RuntimeFileSha256(path));
            ++fileCount;
        }
    }
    return hash.Finish();
}

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        throw std::runtime_error("unable to encode graphics-cache environment override");
    }
    std::string utf8(static_cast<size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), utf8.data(),
                            required, nullptr, nullptr) != required) {
        throw std::runtime_error("unable to encode graphics-cache environment override");
    }
    return utf8;
}
}  // namespace

std::filesystem::path RuntimeGraphicsCacheBase(
    const std::filesystem::path& executableDirectory) {
    DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (required > 1) {
        std::vector<wchar_t> value(required);
        const DWORD length = GetEnvironmentVariableW(
            L"LOCALAPPDATA", value.data(), static_cast<DWORD>(value.size()));
        if (length && length < value.size()) {
            return std::filesystem::path(value.data(), value.data() + length) /
                   L"TheDarknessRecompiled" / L"Cache";
        }
    }
    return executableDirectory / L"runtime_data" / L"cache";
}

std::string RuntimeSha256(const uint8_t* data, size_t size) {
    Sha256Builder hash;
    hash.Update(data, size);
    return hash.Finish();
}

std::string RuntimeFileSha256(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("unable to hash graphics-cache input: " +
                                 path.string());
    }
    Sha256Builder hash;
    std::array<uint8_t, 64 * 1024> buffer{};
    while (stream) {
        stream.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        const std::streamsize count = stream.gcount();
        if (count > 0) hash.Update(buffer.data(), static_cast<size_t>(count));
    }
    if (!stream.eof()) {
        throw std::runtime_error("unable to read graphics-cache input: " +
                                 path.string());
    }
    return hash.Finish();
}

std::vector<RuntimeEnvironmentOverride>
RuntimeGraphicsCacheEnvironmentOverrides() {
    std::vector<RuntimeEnvironmentOverride> overrides;
    wchar_t* const block = GetEnvironmentStringsW();
    if (!block) {
        throw std::runtime_error("unable to enumerate graphics-cache environment overrides");
    }
    try {
        for (const wchar_t* entry = block; *entry;) {
            const std::wstring_view assignment(entry);
            entry += assignment.size() + 1;
            const size_t separator = assignment.find(L'=');
            // Windows drive-current-directory entries start with '=' and are
            // not normal environment assignments.
            if (!separator || separator == std::wstring_view::npos) continue;
            std::string name = UpperAscii(WideToUtf8(assignment.substr(0, separator)));
            if (!StartsWithRex(name) ||
                IsObservationOnlyRexEnvironment(name)) {
                continue;
            }
            overrides.emplace_back(
                std::move(name), WideToUtf8(assignment.substr(separator + 1)));
        }
    } catch (...) {
        FreeEnvironmentStringsW(block);
        throw;
    }
    FreeEnvironmentStringsW(block);
    return overrides;
}

std::string RuntimeGraphicsCacheEnvironmentSha256(
    const std::vector<RuntimeEnvironmentOverride>& overrides) {
    const auto canonical = CanonicalCacheOverrides(overrides);
    if (canonical.empty()) return {};

    Sha256Builder hash;
    for (const auto& [name, value] : canonical) {
        hash.Update(reinterpret_cast<const uint8_t*>(name.data()), name.size());
        static constexpr uint8_t kSeparator = 0;
        hash.Update(&kSeparator, 1);
        hash.Update(reinterpret_cast<const uint8_t*>(value.data()), value.size());
        hash.Update(&kSeparator, 1);
    }
    return hash.Finish();
}

RuntimeGraphicsCacheIdentity BuildRuntimeGraphicsCacheIdentity(
    const std::filesystem::path& cacheBase, uint32_t titleId,
    const uint8_t* executableData, size_t executableSize,
    const std::filesystem::path& selectedSettingsPath,
    const std::filesystem::path& fallbackSettingsPath,
    const std::vector<RuntimeEnvironmentOverride>& environmentOverrides,
    const RuntimeGraphicsCacheCompatibilityInputs& compatibilityInputs) {
    const auto selected = RuntimePcConfigSnapshot::Capture(selectedSettingsPath);
    return BuildRuntimeGraphicsCacheIdentityFromSnapshots(
        cacheBase, titleId, executableData, executableSize,
        selected, RuntimePcConfigSnapshot::Capture(
            selected.present() ? std::filesystem::path{} : fallbackSettingsPath),
        environmentOverrides, compatibilityInputs);
}

RuntimeGraphicsCacheIdentity BuildRuntimeGraphicsCacheIdentityFromSnapshots(
    const std::filesystem::path& cacheBase, uint32_t titleId,
    const uint8_t* executableData, size_t executableSize,
    const RuntimePcConfigSnapshot& selectedSettings,
    const RuntimePcConfigSnapshot& fallbackSettings,
    const std::vector<RuntimeEnvironmentOverride>& environmentOverrides,
    const RuntimeGraphicsCacheCompatibilityInputs& compatibilityInputs) {
    if (cacheBase.empty() || !titleId || !executableData || !executableSize ||
        compatibilityInputs.backend.empty()) {
        throw std::runtime_error("graphics-cache identity input is incomplete");
    }

    RuntimeGraphicsCacheIdentity identity{};
    identity.executableSha256 = RuntimeSha256(executableData, executableSize);
    identity.backend = compatibilityInputs.backend;
    identity.modsSha256 = RuntimeGraphicsCacheModsSha256(
        compatibilityInputs, identity.modFileCount);
    const auto& settings = selectedSettings.present() ? selectedSettings : fallbackSettings;
    if (settings.present()) {
        identity.settingsSource = settings.origin();
        identity.settingsSha256 = RuntimeSha256(
            reinterpret_cast<const uint8_t*>(settings.contents().data()),
            settings.contents().size());
    } else {
        // This token represents the built-in schema-1 defaults only when no
        // user or packaged settings file exists. A future default change must
        // bump the token.
        static constexpr uint8_t kDefaultSettingsIdentity[] =
            "TheDarknessRecompiled-pc-settings-schema-1-defaults-v1";
        identity.settingsSha256 = RuntimeSha256(
            kDefaultSettingsIdentity, sizeof(kDefaultSettingsIdentity) - 1);
    }
    identity.environmentSha256 =
        RuntimeGraphicsCacheEnvironmentSha256(environmentOverrides);
    if (!identity.environmentSha256.empty()) {
        identity.environmentOverrideCount =
            CanonicalCacheOverrides(environmentOverrides).size();
    }

    // Keep every cache-affecting bit in one compact leaf. Adding independent
    // directory levels for environment, backend and mods would exceed legacy
    // Windows path limits in the normal LOCALAPPDATA location.
    Sha256Builder compatibilityHash;
    HashIdentityField(compatibilityHash, "schema",
                      "TheDarknessRecompiled-graphics-cache-identity-v3");
    HashIdentityField(compatibilityHash, "environment",
                      identity.environmentSha256.empty()
                          ? std::string_view("none")
                          : std::string_view(identity.environmentSha256));
    HashIdentityField(compatibilityHash, "backend", identity.backend);
    HashIdentityField(compatibilityHash, "mods",
                      identity.modsSha256.empty()
                          ? std::string_view("none")
                          : std::string_view(identity.modsSha256));
    identity.compatibilitySha256 = compatibilityHash.Finish();
    identity.root = cacheBase / TitleDirectoryName(titleId) /
                    identity.executableSha256 /
                    identity.compatibilitySha256;
    return identity;
}
