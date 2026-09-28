#include "pc_settings_utility_model.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {
struct KnownPreset {
    const wchar_t* name;
    const wchar_t* label;
    int order;
    PcPresetAudience audience;
};

// V330: players see three quality levels (their output follows the monitor);
// Steam Deck is offered on a Deck; the output-path, validation and developer
// presets stay packaged for the advanced view.
constexpr std::array<KnownPreset, 12> kKnownPresets{{
    {L"enhanced", L"Enhanced (recommended)", 0, PcPresetAudience::Player},
    {L"performance", L"Performance (for weaker PCs)", 1, PcPresetAudience::Player},
    {L"original", L"Original (console look)", 2, PcPresetAudience::Player},
    {L"steam_deck_800p_compatibility", L"Steam Deck", 3, PcPresetAudience::SteamDeck},
    {L"original_720p", L"Original, 1280 x 720 output (Xbox-compatible)", 10,
     PcPresetAudience::Developer},
    {L"native_desktop_original", L"Native desktop, original rendering", 11,
     PcPresetAudience::Developer},
    {L"performance_1080p_fxaa", L"Performance, 1080p output / FXAA", 12,
     PcPresetAudience::Developer},
    {L"balanced_1080p", L"Balanced, 1080p output", 13, PcPresetAudience::Developer},
    {L"quality_1440p", L"1440p output / 105 FOV", 14, PcPresetAudience::Developer},
    {L"quality_4k", L"4K output-path validation", 15, PcPresetAudience::Developer},
    {L"validation_1440p_internal_2x", L"VALIDATION: 1440p / internal 2x", 16,
     PcPresetAudience::Developer},
    {L"validation_4k_windowed_internal_2x",
     L"VALIDATION: 4K window / internal 2x (not physical 4K)", 17,
     PcPresetAudience::Developer},
}};

bool IsRegularNonReparseFile(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           !(attributes & FILE_ATTRIBUTE_DIRECTORY) &&
           !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

const KnownPreset* FindKnownPreset(std::wstring_view name) {
    const auto it = std::find_if(
        kKnownPresets.begin(), kKnownPresets.end(),
        [&](const KnownPreset& preset) { return name == preset.name; });
    return it == kKnownPresets.end() ? nullptr : &*it;
}

std::wstring GenericPresetLabel(std::wstring name) {
    bool capitalize = true;
    for (wchar_t& character : name) {
        if (character == L'_') {
            character = L' ';
            capitalize = true;
        } else if (capitalize && character >= L'a' && character <= L'z') {
            character = static_cast<wchar_t>(character - L'a' + L'A');
            capitalize = false;
        } else {
            capitalize = false;
        }
    }
    return name;
}

bool FilesEqual(const std::filesystem::path& left,
                const std::filesystem::path& right) {
    if (!IsRegularNonReparseFile(left) || !IsRegularNonReparseFile(right)) {
        return false;
    }
    std::error_code error;
    const auto leftSize = std::filesystem::file_size(left, error);
    if (error) return false;
    const auto rightSize = std::filesystem::file_size(right, error);
    if (error || leftSize != rightSize) return false;

    std::ifstream leftFile(left, std::ios::binary);
    std::ifstream rightFile(right, std::ios::binary);
    if (!leftFile || !rightFile) return false;
    std::array<char, 4096> leftBytes{};
    std::array<char, 4096> rightBytes{};
    while (leftFile && rightFile) {
        leftFile.read(leftBytes.data(), leftBytes.size());
        rightFile.read(rightBytes.data(), rightBytes.size());
        const auto leftCount = leftFile.gcount();
        if (leftCount != rightFile.gcount() ||
            !std::equal(leftBytes.begin(), leftBytes.begin() + leftCount,
                        rightBytes.begin())) {
            return false;
        }
    }
    return true;
}

int HexDigit(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    return -1;
}

std::optional<std::string> PercentDecode(std::string_view encoded) {
    std::string decoded;
    decoded.reserve(encoded.size());
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        if (encoded[index] != '%') {
            decoded.push_back(encoded[index]);
            continue;
        }
        if (index + 2 >= encoded.size()) return std::nullopt;
        const int high = HexDigit(encoded[index + 1]);
        const int low = HexDigit(encoded[index + 2]);
        if (high < 0 || low < 0) return std::nullopt;
        decoded.push_back(static_cast<char>((high << 4) | low));
        index += 2;
    }
    return decoded;
}

const PcConfigInspectionEntry* FindInspectionEntry(
    const std::vector<PcConfigInspectionEntry>& entries,
    std::string_view key) {
    const auto iterator = std::find_if(
        entries.begin(), entries.end(), [&](const auto& entry) {
            return entry.key == key;
        });
    return iterator == entries.end() ? nullptr : &*iterator;
}

std::string InspectionValue(
    const std::vector<PcConfigInspectionEntry>& entries,
    std::string_view key, std::string_view fallback = "?") {
    const auto* entry = FindInspectionEntry(entries, key);
    return entry ? entry->value : std::string(fallback);
}

std::string OnOff(std::string value) {
    if (value == "true") return "On";
    if (value == "false") return "Off";
    return value;
}

std::string AnisotropicLabel(std::string value) {
    if (value == "-1") return "Title-selected";
    if (value == "0") return "Off";
    if (value == "1") return "1x";
    if (value == "2") return "2x";
    if (value == "3") return "4x";
    if (value == "4") return "8x";
    if (value == "5") return "16x";
    return value;
}
}  // namespace

bool IsRunningOnSteamDeck() {
    wchar_t value[8]{};
    const DWORD length = GetEnvironmentVariableW(L"SteamDeck", value, DWORD(std::size(value)));
    return length == 1 && value[0] == L'1';
}

std::vector<PcPresetOption> EnumeratePcPresetOptions(
    const std::filesystem::path& presetDirectory) {
    std::error_code error;
    if (!std::filesystem::is_directory(presetDirectory, error) || error) {
        return {};
    }

    std::vector<PcPresetOption> result;
    for (std::filesystem::directory_iterator iterator(presetDirectory, error), end;
         !error && iterator != end; iterator.increment(error)) {
        const auto path = iterator->path();
        if (path.extension() != L".toml" || !IsRegularNonReparseFile(path)) {
            continue;
        }
        const std::wstring name = path.stem().wstring();
        const KnownPreset* known = FindKnownPreset(name);
        result.push_back({path.filename(), name,
                          known ? std::wstring(known->label)
                                : GenericPresetLabel(name),
                          known ? known->audience : PcPresetAudience::Developer});
    }
    if (error) {
        throw std::runtime_error("unable to enumerate packaged PC presets");
    }

    std::sort(result.begin(), result.end(),
              [](const PcPresetOption& left, const PcPresetOption& right) {
                  const KnownPreset* leftKnown =
                      FindKnownPreset(left.presetName);
                  const KnownPreset* rightKnown =
                      FindKnownPreset(right.presetName);
                  const int leftOrder = leftKnown ? leftKnown->order : 1000;
                  const int rightOrder = rightKnown ? rightKnown->order : 1000;
                  if (leftOrder != rightOrder) return leftOrder < rightOrder;
                  return left.filename.native() < right.filename.native();
              });
    return result;
}

std::optional<std::size_t> FindExactPcPresetMatch(
    const std::filesystem::path& activeConfig,
    const std::filesystem::path& presetDirectory,
    const std::vector<PcPresetOption>& presets) {
    for (std::size_t index = 0; index < presets.size(); ++index) {
        if (FilesEqual(activeConfig, presetDirectory / presets[index].filename)) {
            return index;
        }
    }
    return std::nullopt;
}

std::wstring QuoteWindowsCommandLineArgument(std::wstring_view argument) {
    if (!argument.empty() &&
        argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(argument);
    }

    std::wstring quoted;
    quoted.push_back(L'\"');
    std::size_t backslashes = 0;
    for (wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(character);
        }
        backslashes = 0;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

std::wstring BuildPcPresetInstallCommandLine(
    const std::filesystem::path& runtimeExecutable,
    std::wstring_view presetName, bool overwrite,
    const std::vector<PcConfigOverrideArgument>& overrides) {
    std::wstring command =
        QuoteWindowsCommandLineArgument(runtimeExecutable.wstring());
    if (presetName.empty()) {
        // Empty selection is reserved for the UI's installed-config entry.
        command += L" --edit-config";
    } else {
        command += L" --install-preset ";
        command += QuoteWindowsCommandLineArgument(presetName);
    }
    for (const PcConfigOverrideArgument& override : overrides) {
        command += L" --set-config ";
        command += QuoteWindowsCommandLineArgument(
            override.key + L"=" + override.value);
    }
    if (overwrite) command += L" --overwrite-config";
    return command;
}

std::wstring BuildPcConfigInspectionCommandLine(
    const std::filesystem::path& runtimeExecutable,
    const std::filesystem::path& configPath) {
    std::wstring command =
        QuoteWindowsCommandLineArgument(runtimeExecutable.wstring());
    command += L" --inspect-config --pc-config ";
    command += QuoteWindowsCommandLineArgument(configPath.wstring());
    return command;
}

std::wstring BuildPcDefaultLaunchCommandLine(
    const std::filesystem::path& runtimeExecutable) {
    return QuoteWindowsCommandLineArgument(runtimeExecutable.wstring());
}

std::wstring BuildPcSafeModeLaunchCommandLine(
    const std::filesystem::path& runtimeExecutable) {
    std::wstring command =
        QuoteWindowsCommandLineArgument(runtimeExecutable.wstring());
    command += L" --preset original_720p";
    return command;
}

std::vector<PcConfigInspectionEntry> ParsePcConfigInspection(
    std::string_view output) {
    struct PendingEntry {
        std::optional<std::string> key;
        std::optional<std::string> type;
        std::optional<std::string> value;
        std::optional<bool> restartRequired;
    };
    std::map<std::size_t, PendingEntry> pending;
    std::istringstream lines{std::string(output)};
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        constexpr std::string_view prefix = "PC_CONFIG_ENTRY_";
        if (line.rfind(prefix, 0) != 0) continue;
        const std::size_t fieldSeparator = line.find('_', prefix.size());
        const std::size_t valueSeparator = line.find('=', fieldSeparator);
        if (fieldSeparator == std::string::npos ||
            valueSeparator == std::string::npos) {
            continue;
        }
        std::size_t entryIndex = 0;
        try {
            const std::string indexText = line.substr(
                prefix.size(), fieldSeparator - prefix.size());
            std::size_t consumed = 0;
            entryIndex = std::stoull(indexText, &consumed);
            if (consumed != indexText.size()) continue;
        } catch (...) {
            continue;
        }
        const std::string field = line.substr(
            fieldSeparator + 1, valueSeparator - fieldSeparator - 1);
        const auto value = PercentDecode(
            std::string_view(line).substr(valueSeparator + 1));
        if (!value) continue;
        auto& entry = pending[entryIndex];
        if (field == "KEY") entry.key = *value;
        else if (field == "TYPE") entry.type = *value;
        else if (field == "VALUE") entry.value = *value;
        else if (field == "RESTART_REQUIRED") {
            if (*value == "1") entry.restartRequired = true;
            else if (*value == "0") entry.restartRequired = false;
        }
    }

    std::vector<PcConfigInspectionEntry> entries;
    for (auto& [index, entry] : pending) {
        (void)index;
        if (!entry.key || !entry.type || !entry.value ||
            !entry.restartRequired.has_value()) {
            continue;
        }
        entries.push_back({std::move(*entry.key), std::move(*entry.type),
                           std::move(*entry.value),
                           *entry.restartRequired});
    }
    return entries;
}

std::string FormatPcConfigInspectionSummary(
    const std::vector<PcConfigInspectionEntry>& entries) {
    if (entries.empty()) return "Effective settings unavailable.";
    std::ostringstream summary;
    summary << "Output: " << InspectionValue(entries, "output_resolution")
            << "    Window: " << InspectionValue(entries, "window_mode")
            << "    Monitor: " << InspectionValue(entries, "monitor") << '\n';
    summary << "Internal scale: " << InspectionValue(entries, "resolution_scale")
            << "x    AA: " << InspectionValue(entries, "swap_post_effect")
            << "    Anisotropic: "
            << AnisotropicLabel(InspectionValue(entries, "anisotropic_override"))
            << '\n';
    const auto nativeGrid = std::find_if(entries.begin(), entries.end(),
        [](const PcConfigInspectionEntry& entry) {
            return entry.key == "draw_resolution_scale_native_grid_rules";
        });
    if (nativeGrid != entries.end() && !nativeGrid->value.empty()) {
        summary << "Data-table grid: native ("
                << 1 + std::count(nativeGrid->value.begin(), nativeGrid->value.end(), ';')
                << " annotated passes; other rendering follows internal scale)\n";
    }
    if (InspectionValue(entries, "native_resolve_region_sampling") == "true" &&
        InspectionValue(entries, "native_resolve_region_tracking") == "true") {
        summary << "Native-region reconstruction: enabled for eligible 2x regions "
                   "(1x unchanged; restart required)\n";
    }
    summary << "Present: "
            << InspectionValue(entries, "display.present_mode")
            << "    Queue: "
            << InspectionValue(entries, "display.max_frame_latency")
            << "    Host cap: "
            << InspectionValue(entries, "display.frame_limit") << '\n';
    summary << "Field of view: "
            << InspectionValue(entries, "camera.field_of_view")
            << "    Motion Blur: "
            << OnOff(InspectionValue(entries, "graphics.motion_blur")) << '\n';
    summary << "Keyboard/mouse: "
            << OnOff(InspectionValue(entries, "input.keyboard_mouse"))
            << "    Controller: "
            << InspectionValue(entries, "input.controller_sensitivity")
            << " / invert Y "
            << OnOff(InspectionValue(entries, "input.controller_invert_y"))
            << '\n';
    summary << "Mouse look: " << InspectionValue(entries, "input.mouse_look", "native")
            << "    Sensitivity: " << InspectionValue(entries, "input.mouse_sensitivity")
            << " / invert Y " << OnOff(InspectionValue(entries, "input.mouse_invert_y"))
            << '\n';
    summary << "Keyboard/mouse slot: "
            << InspectionValue(entries, "input.keyboard_mouse_user_index")
            << "    Stick bridge: acceleration "
            << InspectionValue(entries, "input.mouse_acceleration")
            << " / smoothing " << InspectionValue(entries, "input.mouse_smoothing") << '\n';
    summary << "Audio volume: "
            << InspectionValue(entries, "audio.master_volume")
            << "    Language: "
            << InspectionValue(entries, "general.language") << '\n';
    summary << "All listed settings are applied at the next game start.";
    return summary.str();
}
