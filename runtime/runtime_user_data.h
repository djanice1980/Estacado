#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Where a player's saves, settings and screenshots live (0.9.1, issue #7).
//
// 0.9.0 kept them in the game folder (TheDarkness.pc.toml and runtime_data\),
// so an update extracted to a new folder started without them. From 0.9.1 on
// they live in the player's Saved Games folder, apart from the program:
//
//   %USERPROFILE%\Saved Games\Estacado\
//       TheDarkness.pc.toml     settings
//       content\                saves (the game's content device)
//       screenshots\
//       gpu_calibration.toml    the graphics card measurement
//
// The first start of 0.9.1 in a 0.9.0 folder copies that folder's saves,
// settings, screenshots and measurement there, compares every copied byte,
// and leaves SAVES_MOVED.txt in the game folder. Nothing is moved, overwritten
// or deleted. Until such a copy has succeeded the game keeps using the game
// folder (and tries again at the next start), so progress made meanwhile is
// part of the copy. A file named portable.txt beside TheDarkness.exe keeps the
// 0.9.0 layout: everything stays in the game folder.

enum class RuntimeUserDataMode {
    kPerUser,     // Saved Games\Estacado
    kPortable,    // portable.txt: the game folder (0.9.0 layout)
    kGameFolder,  // the game folder: no Saved Games folder, or its copy is pending
};

struct RuntimeUserDataLayout {
    RuntimeUserDataMode mode = RuntimeUserDataMode::kPortable;
    std::filesystem::path gameFolder;
    // content\, screenshots\ and gpu_calibration.toml.
    std::filesystem::path root;
    std::filesystem::path configPath;
    // Saved Games\Estacado (empty when the system has no Saved Games folder).
    std::filesystem::path perUserRoot;
    // The game folder holds 0.9.0 data that is not copied yet (the layout then
    // still points at the game folder).
    bool migrationPending = false;
    // The game folder holds saves that were not copied because the per-user
    // folder already has saves (the launcher offers to import them).
    bool gameFolderSavesLeftBehind = false;
    std::string reason;
};

inline constexpr wchar_t kRuntimePortableMarker[] = L"portable.txt";
inline constexpr wchar_t kRuntimeSavesMovedNote[] = L"SAVES_MOVED.txt";

// The layout of a game folder, given the Saved Games folder (empty when the
// system has none). Reads the folders; changes nothing.
RuntimeUserDataLayout RuntimeUserDataLayoutFor(const std::filesystem::path& gameFolder,
                                               const std::filesystem::path& savedGames);
// The layout on this machine (FOLDERID_SavedGames; isolated tests replace it
// with DARKNESS_TEST_SAVED_GAMES).
RuntimeUserDataLayout RuntimeDetectUserDataLayout(const std::filesystem::path& gameFolder);
// The 0.9.0 layout: the game folder's TheDarkness.pc.toml and runtime_data.
RuntimeUserDataLayout RuntimeGameFolderUserDataLayout(const std::filesystem::path& gameFolder,
                                                      RuntimeUserDataMode mode,
                                                      std::string reason);

struct RuntimeUserDataCopy {
    bool ok = true;
    uint64_t files = 0;
    uint64_t bytes = 0;
    std::string error;
    // What was copied, kept or renamed (UTF-8, for logs and the launcher).
    std::vector<std::string> lines;
};

// Performs a pending copy (migrationPending) and returns the layout to use:
// the per-user folder after a verified copy, the game folder when it failed.
// Serialized across the game and the launcher.
RuntimeUserDataLayout RuntimeMigrateGameFolderUserData(const RuntimeUserDataLayout& layout,
                                                       RuntimeUserDataCopy& copy);

// The launcher's import: copies the saves, settings and screenshots of
// another folder (an older game folder, its runtime_data folder, or a copy of
// a Saved Games\Estacado folder) into the layout's folder. Saves and settings
// already there are first renamed to "<name>.before-import-<time>" (kept).
// The game must not be running.
RuntimeUserDataCopy RuntimeImportUserData(const RuntimeUserDataLayout& layout,
                                          const std::filesystem::path& source);

// The launcher's "keep the current saves" for saves left in the game folder:
// leaves a note there, so they are not offered again (nothing is copied).
bool RuntimeKeepGameFolderUserData(const RuntimeUserDataLayout& layout);

// Whether a folder (recursively) holds at least one file.
bool RuntimeFolderHasFiles(const std::filesystem::path& folder);

const char* RuntimeUserDataModeName(RuntimeUserDataMode mode);
// One log line: USER_DATA mode=... root=... config=...
std::string RuntimeUserDataLine(const RuntimeUserDataLayout& layout);
