#pragma once

// Where the game and the launcher keep the player's files.
//
// Installed (the default): the program folder may be read-only (Program
// Files), so
//   settings, game location, logs, packs, mods -> %LOCALAPPDATA%\The Darkness
//   saves, screenshots, GPU calibration        -> Saved Games\The Darkness
// Portable: everything stays next to the executables, as before. A folder is
// portable when it has a "portable.txt" marker or already holds data from an
// earlier portable run (settings, game location or runtime_data), so existing
// folders keep their saves and settings.

#include <filesystem>
#include <system_error>

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

struct RuntimeUserPaths {
    bool portable = true;
    // Settings (TheDarkness.pc.toml, .mods.toml, .game.toml), logs,
    // language_packs, texture_packs, mods and an extracted disc image.
    std::filesystem::path data;
    // The runtime's user data root: saves (content), screenshots and the GPU
    // calibration.
    std::filesystem::path saves;
};

inline std::filesystem::path RuntimeKnownFolder(const KNOWNFOLDERID& id) {
    PWSTR path = nullptr;
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &path)) && path) {
        result = path;
    }
    CoTaskMemFree(path);
    return result;
}

inline bool RuntimeFolderIsPortable(const std::filesystem::path& executableDirectory) {
    std::error_code error;
    for (const wchar_t* name : {L"portable.txt", L"TheDarkness.pc.toml",
                                L"TheDarkness.game.toml", L"runtime_data"}) {
        if (std::filesystem::exists(executableDirectory / name, error)) return true;
    }
    return false;
}

inline RuntimeUserPaths ResolveRuntimeUserPaths(
    const std::filesystem::path& executableDirectory) {
    RuntimeUserPaths paths;
    paths.data = executableDirectory;
    paths.saves = executableDirectory / L"runtime_data";
    if (RuntimeFolderIsPortable(executableDirectory)) return paths;

    const std::filesystem::path local = RuntimeKnownFolder(FOLDERID_LocalAppData);
    const std::filesystem::path saved = RuntimeKnownFolder(FOLDERID_SavedGames);
    if (local.empty() || saved.empty()) return paths;  // stay portable
    paths.portable = false;
    paths.data = local / L"The Darkness";
    paths.saves = saved / L"The Darkness";
    std::error_code error;
    std::filesystem::create_directories(paths.data, error);
    std::filesystem::create_directories(paths.saves, error);
    return paths;
}
