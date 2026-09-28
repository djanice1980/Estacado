#include "runtime_graphics_pc_config.h"
#include "runtime_graphics_cache.h"
#include "runtime_pc_settings.h"
#include "runtime_single_instance.h"
#include <rex/graphics/embedded_config.h>

#include <fstream>
#include <iostream>
#include <optional>

namespace fs = std::filesystem;
namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void Write(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
    file.close();
    Require(!file.fail(), "fixture write failed");
}
std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
bool Load(const RuntimeGraphicsPcConfig& deferred) {
    return rex::graphics::LoadEmbeddedPcConfig(deferred.snapshot().present(),
        deferred.snapshot().contents(), deferred.snapshot().origin());
}
double Fov() { return std::stod(rex::cvar::GetFlagByName("camera_field_of_view")); }
struct EnvironmentRestore {
    std::optional<std::wstring> previous;
    EnvironmentRestore() {
        DWORD length = GetEnvironmentVariableW(L"REX_CAMERA_FIELD_OF_VIEW", nullptr, 0);
        if (length) {
            std::wstring value(length, L'\0');
            value.resize(GetEnvironmentVariableW(L"REX_CAMERA_FIELD_OF_VIEW", value.data(), length));
            previous = std::move(value);
        }
        SetEnvironmentVariableW(L"REX_CAMERA_FIELD_OF_VIEW", nullptr);
    }
    ~EnvironmentRestore() {
        SetEnvironmentVariableW(L"REX_CAMERA_FIELD_OF_VIEW", previous ? previous->c_str() : nullptr);
    }
};

bool ChildSave(const fs::path& config, DWORD expected) {
    wchar_t executable[32768]{};
    if (!GetModuleFileNameW(nullptr, executable, 32768)) return false;
    std::wstring command = L"\"" + std::wstring(executable) +
        L"\" --save-next-start \"" + config.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) return false;
    const DWORD waited = WaitForSingleObject(child.hProcess, 10000);
    DWORD result = 99;
    if (waited == WAIT_OBJECT_0) GetExitCodeProcess(child.hProcess, &result);
    else { TerminateProcess(child.hProcess, 99); WaitForSingleObject(child.hProcess, 10000); }
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return waited == WAIT_OBJECT_0 && result == expected;
}
}

int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string(argv[1]) == "--save-next-start") {
        const auto config = fs::path(argv[2]);
        RuntimeSingleInstance writer(RuntimeConfigLockName(config));
        if (!writer.acquired()) return 9;
        InstallRuntimePcPresetWithOverrides(config, config,
            {{"camera.field_of_view", "103"}, {"audio.master_volume", "0.75"}}, true);
        return 0;
    }
    EnvironmentRestore environment;
    // Load only static cvar registrations. No device/window/guest is created.
    Require(LoadLibraryW(L"rexgpu-xenos.dll") != nullptr, "real GPU settings registrations unavailable");
    // Windows reuses PIDs. Reserve a new directory atomically, preserving
    // evidence from earlier runs instead of treating PID reuse as a failure.
    fs::path root;
    for (uint32_t attempt = 0; attempt < 1024 && root.empty(); ++attempt) {
        const auto candidate = fs::current_path() / ("snapshot_fixture_" +
            std::to_string(GetCurrentProcessId()) + "_" + std::to_string(attempt));
        if (fs::create_directory(candidate)) root = candidate;
    }
    Require(!root.empty(), "unable to reserve a fresh snapshot fixture");
    const auto config = root / "original origin" / "pc.toml";
    const auto example = root / "package" / "example.toml";
    const auto package = example.parent_path();
    const std::string original =
        "# preserved bytes\r\npc_config_version = 1\r\n"
        "[camera]\r\nfield_of_view = 97.0\r\n"
        "[general]\r\nlanguage = 'french'\r\n"
        "[audio]\r\nmaster_volume = 0.25\r\n"
        "[input]\r\nvibration_scale = 0.5\r\n"
        "controller_sensitivity = 'high'\r\ncontroller_invert_y = true\r\n"
        "[input.controller_bind]\r\na = 'b'\r\n";
    Write(config, original);
    Write(example, "pc_config_version = 1\n[camera]\nfield_of_view = 130.0\n");
    RuntimeGraphicsCacheCompatibilityInputs compatibility{};
    compatibility.backend = "d3d12";
    const uint8_t xex[] = {1, 2, 3};
    const auto cache = [&](const RuntimePcConfigSnapshot& selected,
                           const RuntimePcConfigSnapshot& fallback) {
        return BuildRuntimeGraphicsCacheIdentityFromSnapshots(root / "cache", 0x545407EE,
            xex, sizeof(xex), selected, fallback, {}, compatibility);
    };

    std::optional<RuntimeGraphicsPcConfig> deferred;
    RuntimeSingleInstance configStartup(RuntimeConfigLockName(config));
    Require(configStartup.acquired(), "startup config lock unavailable");
    const auto fallback = RuntimePcConfigSnapshot::Capture(example);
    std::string beforeHash;
    {
        const auto captured = RuntimePcConfigSnapshot::Capture(config);
        Require(captured.contents() == original, "capture changed source bytes");
        Require(ValidateRuntimePcConfig(config, true, &captured).valid, "captured config invalid");
        beforeHash = cache(captured, fallback).settingsSha256;
        Require(beforeHash == RuntimeFileSha256(config), "captured hash differs from original bytes");
        deferred.emplace(captured, package);
    } // The original capture's lifetime ends before deferred loading.

    // The child exercises the actual same-config serialized atomic installer.
    // Startup ownership blocks it; explicit release allows next-start saving.
    const auto preset = root / "next.toml";
    Write(preset, original);
    Require(ChildSave(config, 9), "child bypassed startup ownership");
    Require(Read(config) == original, "blocked child changed configuration");
    configStartup.Release();
    Require(ChildSave(config, 0), "next-start child save failed after ownership release");
    Require(ValidateRuntimePcConfig(config, true).valid, "saved config failed reopen validation");
    Require(RuntimeAudioMasterVolumeFromPcConfig(config) == 0.75, "saved value did not reopen");
    const auto next = RuntimePcConfigSnapshot::Capture(config);
    const auto& captured = deferred->snapshot();
    Require(cache(captured, fallback).settingsSha256 == beforeHash, "file replacement changed captured cache hash");
    Require(cache(next, fallback).settingsSha256 != beforeHash, "next-start cache failed to change");
    Require(RuntimeInputVibrationScaleFromPcConfig(config, &captured) == 0.5, "CPU vibration drifted");
    Require(RuntimeAudioMasterVolumeFromPcConfig(config, &captured) == 0.25, "CPU audio drifted");
    Require(RuntimeXboxLanguageFromPcConfig(config, &captured) == 4, "CPU language drifted");
    const auto profile = RuntimeControllerProfileFromPcConfig(config, &captured);
    Require(profile.sensitivity == 2 && profile.invertY, "CPU controller profile drifted");
    Require(RuntimeInputButtonMapFromPcConfig(config, &captured).sourceForGuest ==
            RuntimeInputButtonMapFromPcConfig(preset).sourceForGuest, "CPU button mapping drifted");
    Require(Load(*deferred) && Fov() == 97.0, "deferred GPU reopened replaced config");
    SetEnvironmentVariableW(L"REX_CAMERA_FIELD_OF_VIEW", L"111");
    Require(Load(*deferred) && Fov() == 111.0, "GPU environment override precedence changed");
    SetEnvironmentVariableW(L"REX_CAMERA_FIELD_OF_VIEW", nullptr);
    Require(Load(RuntimeGraphicsPcConfig(next, package)) && Fov() == 103.0,
            "next-start GPU did not load saved value");

    const auto relative = fs::path("shaders") / "custom.xsrp";
    Write(config.parent_path() / relative, "custom");
    Write(package / relative, "packaged");
    const auto resolve = [&](const fs::path& path) {
        return rex::graphics::ResolveEmbeddedShaderReplacementPackPath(path,
            deferred->originUtf8().c_str(), deferred->assetRootUtf8().c_str());
    };
    Require(resolve(relative) == config.parent_path() / relative, "original resource origin lost");
    const auto packagedOnly = fs::path("shaders") / "package.xsrp";
    Write(package / packagedOnly, "packaged-only");
    Require(resolve(packagedOnly) == package / packagedOnly, "package resource fallback changed");
    Require(resolve(package / relative) == package / relative, "absolute resource path changed");

    const auto missingPath = root / "missing.toml";
    const auto missing = RuntimePcConfigSnapshot::Capture(missingPath);
    Require(!missing.present() && ValidateRuntimePcConfig(missingPath, false, &missing).valid,
            "optional missing config rejected");
    Require(!ValidateRuntimePcConfig(missingPath, true, &missing).valid, "explicit missing config accepted");
    const auto fallbackHash = cache(missing, fallback).settingsSha256;
    Write(missingPath, original);
    Write(example, "pc_config_version = 1\n[camera]\nfield_of_view = 140.0\n");
    Require(cache(missing, fallback).settingsSha256 == fallbackHash, "fallback cache bytes were reopened");
    Require(RuntimeAudioMasterVolumeFromPcConfig(missingPath, &missing) == 1.0, "missing CPU config became present");
    rex::cvar::SetFlagByName("camera_field_of_view", "95");
    Require(Load(RuntimeGraphicsPcConfig(missing, package)) && Fov() == 95.0,
            "missing GPU config loaded newly-created source or example");
    const auto defaults = cache(missing, RuntimePcConfigSnapshot::Capture({}));
    Require(defaults.settingsSource.empty() && defaults.settingsSha256 != fallbackHash,
            "default token fallback changed");

    const auto emptyPath = root / "empty.toml";
    Write(emptyPath, "");
    const auto empty = RuntimePcConfigSnapshot::Capture(emptyPath);
    Require(empty.present() && empty.contents().empty(), "empty config became missing");
    Require(!ValidateRuntimePcConfig(emptyPath, true, &empty).valid, "empty PC schema unexpectedly valid");
    Require(cache(empty, fallback).settingsSha256 == RuntimeSha256(nullptr, 0), "empty config used fallback hash");
    Require(Load(RuntimeGraphicsPcConfig(empty, package)), "empty TOML GPU behavior changed");

    const auto invalidPath = root / "invalid.toml";
    Write(invalidPath, "pc_config_version = [");
    const auto invalid = RuntimePcConfigSnapshot::Capture(invalidPath);
    const auto invalidValidation = ValidateRuntimePcConfig(invalidPath, true, &invalid);
    Require(!invalidValidation.valid && invalidValidation.errors.front().find("TOML parse error") == 0,
            "invalid snapshot lost validation error");
    Write(invalidPath, original);
    Require(!Load(RuntimeGraphicsPcConfig(invalid, package)), "GPU accepted invalid captured bytes");
    Write(invalidPath, "pc_config_version = 1\n[camera]\nfield_of_view = 999.0\n");
    const auto badRange = RuntimePcConfigSnapshot::Capture(invalidPath);
    Require(!ValidateRuntimePcConfig(invalidPath, true, &badRange).valid, "range validation weakened");
    const auto savedBytes = Read(config);
    bool rejectedSave = false;
    try { InstallRuntimePcPresetWithOverrides(preset, config, {{"camera.field_of_view", "999"}}, true); }
    catch (const std::runtime_error&) { rejectedSave = true; }
    Require(rejectedSave && Read(config) == savedBytes, "invalid save changed next-start config");
    std::cout << "Owned startup CPU/cache/deferred GPU, origin, absence, errors and save/reopen PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
