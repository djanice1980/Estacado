#pragma once

#include "runtime_input.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

enum class RuntimeLaunchAction {
    Run,
    Help,
    PrintPaths,
    ListPresets,
    PrintCapabilities,
    ValidateConfig,
    InspectConfig,
    ValidateMods,
    VerifyPackage,
    InstallPreset,
};

struct RuntimeLaunchOptions {
    RuntimeLaunchAction action{RuntimeLaunchAction::Run};
    std::filesystem::path xexPath;
    std::filesystem::path pcConfigPath;
    std::filesystem::path pcConfigInstallPath;
    std::filesystem::path modsConfigPath;
    std::filesystem::path userDataRoot;
    bool pcConfigExplicit{};
    bool inputDiagnostics{};
    bool frameCadenceDiagnostics{};
    bool oneVblankExperiment{};
    bool immediateDeadlineExperiment{};
    // V367: on by default (analog movement at high frame rates, see
    // runtime_movement_packet.h); --no-movement-packet-compaction for A/B.
    bool movementPacketCompaction{true};
    bool hitchDiagnostics{};
    bool audioDiagnostics{};
    bool overwritePcConfig{};
    struct PcConfigOverride {
        std::string key;
        std::string value;
    };
    std::vector<PcConfigOverride> pcConfigOverrides;
};

// Parses the runtime's title path and PC configuration selection without
// interpreting graphics settings in a second subsystem. ReXGlue remains the
// authoritative parser for graphics/display CVars.
RuntimeLaunchOptions ParseRuntimeLaunchOptions(
    int argc, const char* const* argv,
    const std::filesystem::path& executableDirectory);

std::filesystem::path RuntimeExecutableDirectory();
const char* RuntimeLaunchHelpText();
std::vector<std::filesystem::path> RuntimePresetPaths(
    const std::filesystem::path& executableDirectory);

// Returns stable, machine-readable declarations for features present in the
// current native runtime. Deferred features are reported explicitly so launchers
// do not infer support from a nearby or partially implemented subsystem.
std::vector<std::string> RuntimePcCapabilityLines();

// Queries Windows display topology without creating a title window or starting
// guest execution. A zero-display result is valid for a headless session.
std::vector<std::string> RuntimeHostDisplayLines();

struct RuntimePcConfigValidation {
    bool exists{};
    bool valid{};
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

// Performs the packaged schema/range preflight before ReXGlue consumes the
// file. Unknown leaves are warnings because advanced ReXGlue CVars may be
// legitimate; malformed or invalid declared PC settings are errors.
class RuntimePcConfigSnapshot;
RuntimePcConfigValidation ValidateRuntimePcConfig(
    const std::filesystem::path& path, bool requireFile,
    const RuntimePcConfigSnapshot* snapshot = nullptr);
std::vector<std::string> RuntimePcConfigValidationLines(
    const std::filesystem::path& path,
    const RuntimePcConfigValidation& validation);

// Reports every declared scalar in stable key order for settings front ends.
// Values are percent-encoded UTF-8, and this remains a read-only pre-guest
// action; ReXGlue is still the authoritative settings consumer at startup.
std::vector<std::string> RuntimePcConfigInspectionLines(
    const std::filesystem::path& path,
    const RuntimePcConfigValidation& validation);

// Runtime-owned non-graphics setting. Missing optional config/value preserves
// the Original identity scale of 1.0; validated explicit values are 0..1.
double RuntimeInputVibrationScaleFromPcConfig(
    const std::filesystem::path& path, const RuntimePcConfigSnapshot* snapshot = nullptr);

// Startup-only physical XInput digital-button mapping. Missing keys preserve
// identity; each guest destination accepts one named physical button or none.
RuntimeInputButtonMap RuntimeInputButtonMapFromPcConfig(
    const std::filesystem::path& path, const RuntimePcConfigSnapshot* snapshot = nullptr);

struct RuntimeControllerProfile {
    // Standard Xbox profile enum: medium=0, low=1, high=2.
    uint32_t sensitivity{};
    bool invertY{};
};

// Title-native controller preferences. The runtime exposes them only through
// the standard XAM profile values that The Darkness already consumes.
RuntimeControllerProfile RuntimeControllerProfileFromPcConfig(
    const std::filesystem::path& path, const RuntimePcConfigSnapshot* snapshot = nullptr);

// Runtime-owned host-output setting. Missing optional config/value preserves
// exact identity volume; validated explicit values are 0..1.
double RuntimeAudioMasterVolumeFromPcConfig(
    const std::filesystem::path& path, const RuntimePcConfigSnapshot* snapshot = nullptr);

// Runtime-owned Xbox language selection shared by XGetLanguage and
// ExGetXConfigSetting. Missing optional config/value preserves English (1).
// Supported named values map only to language content shipped by this title;
// "auto" follows the Windows display language like a console dashboard.
uint32_t RuntimeXboxLanguageFromPcConfig(
    const std::filesystem::path& path, const RuntimePcConfigSnapshot* snapshot = nullptr);
// Xbox language for a Windows LANGID: German 3, French 4, Spanish 5,
// Italian 6, anything else English 1.
uint32_t RuntimeXboxLanguageForWindowsLanguage(uint16_t windowsLanguage);
// The language-pack folder name for general.language (a language the game
// does not ship, currently "arabic"), or empty.
std::string RuntimeLanguagePackFromPcConfig(
    const std::filesystem::path& path, const RuntimePcConfigSnapshot* snapshot = nullptr);

// display.frame_rate (V288). "original" (default when absent) keeps the
// title's own presentation pacing; every other mode (60, half_refresh,
// refresh, custom, uncapped) selects the immediate presentation deadline and
// lets the GPU plugin pace frames. Returns true for host pacing.
bool RuntimeFrameRateUsesHostPacingFromPcConfig(
    const std::filesystem::path& path, const RuntimePcConfigSnapshot* snapshot = nullptr);

struct RuntimePcConfigInstallResult {
    std::filesystem::path sourcePath;
    std::filesystem::path destinationPath;
    bool overwritten{};
    std::size_t overrideCount{};
};

// Installs one already-validated packaged preset as the persistent adjacent PC
// configuration. The replacement is written and flushed through a same-folder
// temporary file, and existing files are replaced only when explicitly asked.
RuntimePcConfigInstallResult InstallRuntimePcPreset(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath, bool overwrite);
RuntimePcConfigInstallResult InstallRuntimePcPresetWithOverrides(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath,
    const std::vector<RuntimeLaunchOptions::PcConfigOverride>& overrides,
    bool overwrite);
// Schema settings present in a configuration file, as the text the
// inspection and --set-config use (throws when the file cannot be parsed).
std::map<std::string, std::string> RuntimePcConfigSettingValues(
    const std::filesystem::path& path);
// Title rendering annotations that internal resolution scaling requires
// (V162/V163, Probes 276-279): the three RGB lookup-table data passes keep the
// native grid and render targets narrower than 640 pixels stay native. They
// are correctness requirements of this title, not player settings: raising
// resolution_scale without them reproduces the verified atlas mis-sampling.
inline constexpr int64_t kTitleScaleThreshold = 640;
inline constexpr std::string_view kTitleNativeGridRules =
    "B29F0BF45937C4C4:FDC5E32EC6045BE1:1:324:18:6;"
    "B29F0BF45937C4C4:37AC93F53126ABB7:0:324:18:26;"
    "B29F0BF45937C4C4:54D655FC471D594A:0:324:18:26";
// The configuration text the graphics plugin receives: unchanged at internal
// scale 1 or when both annotations are present; otherwise the missing ones
// are added (explicit values in the configuration win). Throws when the text
// cannot be parsed (it was validated before).
std::string RuntimePcConfigWithTitleScaleRequirements(const std::string& contents,
                                                      const std::string& sourceName,
                                                      bool* changed = nullptr);
// resolution_scale = 0 is Automatic (V330, runtime_auto_scale.h): the GPU
// receives automaticScale instead; other values stay unchanged.
struct RuntimePcScaleTarget {
    int64_t resolutionScale = 1;
    std::string outputResolution = "native";
    int64_t monitor = 0;
    // The frame rate automatic scale should hold (display.frame_rate,
    // display.frame_limit for "custom").
    std::string frameRate = "refresh";
    uint32_t frameLimit = 0;
};
// display.widescreen (V407): fill non-16:9 screens (runtime_widescreen.h).
bool RuntimeWidescreenFromPcConfig(const std::filesystem::path& path,
                                   const RuntimePcConfigSnapshot* snapshot = nullptr);
RuntimePcScaleTarget RuntimePcScaleTargetFromContents(const std::string& contents,
                                                      const std::string& sourceName);
std::string RuntimePcConfigWithAutomaticScale(const std::string& contents,
                                              const std::string& sourceName,
                                              uint32_t automaticScale, bool* changed = nullptr);
// Shelved temporal AA (V440): graphics.temporal_aa reaches the GPU as "off"
// unless temporalAaOffered (DARKNESS_EXPERIMENTAL=temporal_aa); the other
// values stay unchanged. shelved receives the value that was turned off.
std::string RuntimePcConfigWithShelvedFeatures(const std::string& contents,
                                               const std::string& sourceName,
                                               bool temporalAaOffered,
                                               std::string* shelved = nullptr);
std::vector<std::string> RuntimePcConfigInstallLines(
    const RuntimePcConfigInstallResult& result);
std::vector<std::string> RuntimePcConfigInstallErrorLines(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath, const std::string& error);
