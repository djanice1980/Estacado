#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Who a preset is offered to: players (always listed), Steam Deck owners
// (listed and preselected on a Deck), developers (advanced view only).
enum class PcPresetAudience { Player, SteamDeck, Developer };

struct PcPresetOption {
    std::filesystem::path filename;
    std::wstring presetName;
    std::wstring displayName;
    PcPresetAudience audience{PcPresetAudience::Developer};
};

// True on a Steam Deck (Steam sets SteamDeck=1 for games it starts there).
bool IsRunningOnSteamDeck();

struct PcConfigInspectionEntry {
    std::string key;
    std::string type;
    std::string value;
    bool restartRequired{};
};

struct PcConfigOverrideArgument {
    std::wstring key;
    std::wstring value;
};

// Profile selection is pending even before any individual value is edited.
inline bool PcSettingsHavePendingChanges(bool pendingProfile, size_t valueEdits) noexcept {
    return pendingProfile || valueEdits != 0;
}

// Enumerates only regular, non-reparse TOML files. The returned order is the
// stable user-facing order used by the native settings utility.
std::vector<PcPresetOption> EnumeratePcPresetOptions(
    const std::filesystem::path& presetDirectory);

// Returns the packaged preset whose bytes exactly match the adjacent active
// configuration. No semantic normalization is performed.
std::optional<std::size_t> FindExactPcPresetMatch(
    const std::filesystem::path& activeConfig,
    const std::filesystem::path& presetDirectory,
    const std::vector<PcPresetOption>& presets);

// Windows command-line quoting used only for the package-owned executable and
// a preset name obtained from EnumeratePcPresetOptions.
std::wstring QuoteWindowsCommandLineArgument(std::wstring_view argument);
// Empty presetName selects editing the adjacent installed config, not a preset.
// This requires nonempty overrides and explicit overwrite authorization.
std::wstring BuildPcPresetInstallCommandLine(
    const std::filesystem::path& runtimeExecutable,
    std::wstring_view presetName, bool overwrite,
    const std::vector<PcConfigOverrideArgument>& overrides = {});

// Commands used by the native settings front end. Inspection remains a
// read-only runtime action so the runtime's versioned schema is authoritative.
// Safe mode selects the packaged Original preset for one launch and does not
// overwrite a user's persistent configuration.
std::wstring BuildPcConfigInspectionCommandLine(
    const std::filesystem::path& runtimeExecutable,
    const std::filesystem::path& configPath);
std::wstring BuildPcDefaultLaunchCommandLine(
    const std::filesystem::path& runtimeExecutable);
std::wstring BuildPcSafeModeLaunchCommandLine(
    const std::filesystem::path& runtimeExecutable);

// Parses only the runtime's stable PC_CONFIG_ENTRY_* inspection contract.
// Percent decoding is strict; malformed or incomplete entries are omitted.
std::vector<PcConfigInspectionEntry> ParsePcConfigInspection(
    std::string_view output);
std::string FormatPcConfigInspectionSummary(
    const std::vector<PcConfigInspectionEntry>& entries);
