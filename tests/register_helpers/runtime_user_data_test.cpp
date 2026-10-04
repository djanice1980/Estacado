// 0.9.1 saves and settings per user (runtime_user_data.h): layouts, the
// verified one-time copy from a 0.9.0 game folder, imports, explicit roots,
// and paths beyond the system code page (the folder names below contain
// Latin, Chinese and Cyrillic letters).
#include "runtime_pc_settings.h"
#include "runtime_user_data.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

namespace {

bool passed = true;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        passed = false;
    }
}

void Write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << bytes;
}

std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), {});
}

fs::path Normal(const fs::path& path) { return fs::absolute(path).lexically_normal(); }

// A 0.9.0 game folder: saves, settings, a screenshot and the measurement.
void MakeOldFolder(const fs::path& game) {
    Write(game / L"TheDarkness.exe", "exe");
    Write(game / L"TheDarkness.pc.toml", "[display]\nframe_rate = \"refresh\"\n");
    Write(game / L"runtime_data" / L"content" / L"545407EE" / L"00000001" / L"DEFAULT" /
              L"Checkpoint",
          std::string(70000, 'c'));
    Write(game / L"runtime_data" / L"content" / L"545407EE" / L"Profile" / L"achievements.txt",
          "a1\n");
    Write(game / L"runtime_data" / L"screenshots" / L"shot1.png", "png1");
    Write(game / L"runtime_data" / L"gpu_calibration.toml", "calibration\n");
}

const fs::path kCheckpoint =
    fs::path(L"content") / L"545407EE" / L"00000001" / L"DEFAULT" / L"Checkpoint";

}  // namespace

int main() {
    const fs::path base =
        fs::temp_directory_path() / L"estacado user data é李Ж";
    std::error_code error;
    fs::remove_all(base, error);
    const fs::path savedGames = base / L"Saved Games";
    const fs::path root = savedGames / L"Estacado";

    {
        // A new folder: Saved Games\Estacado, nothing to copy.
        const fs::path game = base / L"fresh";
        fs::create_directories(game);
        const RuntimeUserDataLayout layout = RuntimeUserDataLayoutFor(game, savedGames);
        Check(layout.mode == RuntimeUserDataMode::kPerUser && !layout.migrationPending &&
                  !layout.gameFolderSavesLeftBehind,
              "a new folder uses the per-user folder");
        Check(layout.root == root && layout.configPath == root / L"TheDarkness.pc.toml",
              "saves and settings in Saved Games\\Estacado");
    }
    {
        // portable.txt keeps the 0.9.0 layout; no Saved Games: the game folder.
        const fs::path game = base / L"portable";
        MakeOldFolder(game);
        Write(game / L"portable.txt", "");
        const RuntimeUserDataLayout layout = RuntimeUserDataLayoutFor(game, savedGames);
        Check(layout.mode == RuntimeUserDataMode::kPortable &&
                  layout.root == game / L"runtime_data" &&
                  layout.configPath == game / L"TheDarkness.pc.toml" && !layout.migrationPending,
              "portable.txt keeps everything in the game folder");
        const RuntimeUserDataLayout none = RuntimeUserDataLayoutFor(base / L"fresh", {});
        Check(none.mode == RuntimeUserDataMode::kGameFolder &&
                  none.root == base / L"fresh" / L"runtime_data",
              "without a Saved Games folder the game folder is used");
    }
    {
        // Logs, language packs and an extracted disc image: the local
        // application data folder in per-user mode, the game folder otherwise.
        const fs::path local = base / L"Local";
        const fs::path game = base / L"installed";
        fs::create_directories(game);
        const RuntimeUserDataLayout perUser = RuntimeUserDataLayoutFor(game, savedGames, local);
        Check(perUser.mode == RuntimeUserDataMode::kPerUser &&
                  perUser.localData == local / L"Estacado",
              "per-user mode keeps logs and packs in LocalAppData\\Estacado");
        Check(RuntimeUserDataLayoutFor(game, savedGames).localData == game,
              "without a local application data folder the game folder holds them");
        Check(RuntimeUserDataLayoutFor(game, {}, local).localData == game,
              "without a Saved Games folder the game folder holds them");
        Write(game / L"portable.txt", "");
        Check(RuntimeUserDataLayoutFor(game, savedGames, local).localData == game,
              "portable.txt keeps them in the game folder");
    }
    {
        // An update from 0.9.0: the game folder stays in use until the copy.
        const fs::path game = base / L"old";
        MakeOldFolder(game);
        const RuntimeUserDataLayout pending = RuntimeUserDataLayoutFor(game, savedGames);
        Check(pending.migrationPending && pending.mode == RuntimeUserDataMode::kGameFolder &&
                  pending.root == game / L"runtime_data" &&
                  pending.configPath == game / L"TheDarkness.pc.toml" &&
                  pending.perUserRoot == root,
              "0.9.0 data: the game folder stays in use until it is copied");
        RuntimeUserDataCopy copy;
        const RuntimeUserDataLayout layout = RuntimeMigrateGameFolderUserData(pending, copy);
        Check(copy.ok && copy.error.empty() && copy.files == 5, "five files copied");
        Check(layout.mode == RuntimeUserDataMode::kPerUser && layout.root == root &&
                  !layout.migrationPending,
              "the per-user folder after the copy");
        Check(Read(root / kCheckpoint) == std::string(70000, 'c'), "the save is copied byte for byte");
        Check(Read(root / L"TheDarkness.pc.toml") == Read(game / L"TheDarkness.pc.toml"),
              "the settings are copied");
        Check(Read(root / L"screenshots" / L"shot1.png") == "png1" &&
                  Read(root / L"gpu_calibration.toml") == "calibration\n",
              "screenshots and the measurement are copied");
        Check(Read(game / L"runtime_data" / kCheckpoint) == std::string(70000, 'c') &&
                  fs::is_regular_file(game / L"TheDarkness.pc.toml"),
              "the originals stay in the game folder");
        Check(fs::is_regular_file(game / L"SAVES_MOVED.txt"), "a note in the game folder");
        bool staging = false;
        for (const auto& entry : fs::directory_iterator(root)) {
            staging = staging || entry.path().filename().native().rfind(L".", 0) == 0;
        }
        Check(!staging, "no staging folder is left");
        const RuntimeUserDataLayout again = RuntimeUserDataLayoutFor(game, savedGames);
        Check(again.mode == RuntimeUserDataMode::kPerUser && !again.migrationPending &&
                  !again.gameFolderSavesLeftBehind,
              "copied once: settled");
        RuntimeUserDataCopy none;
        const RuntimeUserDataLayout unchanged = RuntimeMigrateGameFolderUserData(again, none);
        Check(none.ok && none.files == 0 && unchanged.root == root, "nothing to copy twice");
    }
    {
        // A second 0.9.0 folder while per-user saves exist: never merged, offered.
        const fs::path game = base / L"second";
        MakeOldFolder(game);
        Write(game / L"runtime_data" / kCheckpoint, "second");
        Write(game / L"runtime_data" / L"screenshots" / L"shot1.png", "other png");
        const RuntimeUserDataLayout layout = RuntimeUserDataLayoutFor(game, savedGames);
        Check(layout.mode == RuntimeUserDataMode::kPerUser && !layout.migrationPending &&
                  layout.gameFolderSavesLeftBehind,
              "existing per-user saves stay in use; the folder's saves are offered");
        // Importing them keeps the current saves and settings under new names.
        const RuntimeUserDataCopy imported = RuntimeImportUserData(layout, game);
        Check(imported.ok, "the import succeeds");
        Check(Read(root / kCheckpoint) == "second", "the imported save is in use");
        bool keptSaves = false;
        bool keptSettings = false;
        for (const auto& entry : fs::directory_iterator(root)) {
            const std::wstring name = entry.path().filename().native();
            keptSaves = keptSaves || (name.rfind(L"content.before-import-", 0) == 0 &&
                                      Read(entry.path() / L"545407EE" / L"00000001" /
                                           L"DEFAULT" / L"Checkpoint") ==
                                          std::string(70000, 'c'));
            keptSettings = keptSettings || name.rfind(L"TheDarkness.pc.before-import-", 0) == 0;
        }
        Check(keptSaves && keptSettings, "the previous saves and settings are kept");
        Check(Read(root / L"screenshots" / L"shot1.png") == "png1",
              "an existing screenshot is not replaced");
        Check(fs::is_regular_file(game / L"SAVES_MOVED.txt") &&
                  !RuntimeUserDataLayoutFor(game, savedGames).gameFolderSavesLeftBehind,
              "the imported folder is not offered again");
    }
    {
        // Keeping the current saves instead.
        const fs::path game = base / L"third";
        MakeOldFolder(game);
        const RuntimeUserDataLayout layout = RuntimeUserDataLayoutFor(game, savedGames);
        Check(layout.gameFolderSavesLeftBehind, "the third folder is offered");
        Check(RuntimeKeepGameFolderUserData(layout) &&
                  !RuntimeUserDataLayoutFor(game, savedGames).gameFolderSavesLeftBehind,
              "kept: not offered again");
        Check(Read(game / L"runtime_data" / kCheckpoint) == std::string(70000, 'c'),
              "kept saves are left as they are");
    }
    {
        // Imports refuse folders without saves and the saves in use.
        const RuntimeUserDataLayout layout = RuntimeUserDataLayoutFor(base / L"fresh", savedGames);
        Check(!RuntimeImportUserData(layout, base / L"fresh").ok, "a folder without saves");
        Check(!RuntimeImportUserData(layout, root).ok, "the saves in use");
    }
    {
        // An existing per-user settings file is never replaced by the copy.
        const fs::path saved = base / L"Saved Games 2";
        Write(saved / L"Estacado" / L"TheDarkness.pc.toml", "mine\n");
        const fs::path game = base / L"old2";
        MakeOldFolder(game);
        RuntimeUserDataCopy copy;
        const RuntimeUserDataLayout layout =
            RuntimeMigrateGameFolderUserData(RuntimeUserDataLayoutFor(game, saved), copy);
        Check(copy.ok && layout.mode == RuntimeUserDataMode::kPerUser, "the copy succeeds");
        Check(Read(saved / L"Estacado" / L"TheDarkness.pc.toml") == "mine\n",
              "the existing settings are kept");
        Check(Read(saved / L"Estacado" / kCheckpoint) == std::string(70000, 'c'),
              "the saves are copied");
    }
    {
        // A copy that cannot be placed keeps the game folder in use and
        // replaces nothing (a file stands where the saves folder goes).
        const fs::path saved = base / L"Saved Games 3";
        Write(saved / L"Estacado" / L"content", "a file");
        const fs::path game = base / L"old3";
        MakeOldFolder(game);
        RuntimeUserDataCopy copy;
        const RuntimeUserDataLayout layout =
            RuntimeMigrateGameFolderUserData(RuntimeUserDataLayoutFor(game, saved), copy);
        Check(!copy.ok && !copy.error.empty() && layout.mode == RuntimeUserDataMode::kGameFolder &&
                  layout.migrationPending && layout.root == game / L"runtime_data",
              "a failed copy keeps the game folder in use");
        Check(Read(saved / L"Estacado" / L"content") == "a file" &&
                  !fs::exists(saved / L"Estacado" / L"TheDarkness.pc.toml"),
              "nothing is placed or replaced");
        Check(!fs::exists(game / L"SAVES_MOVED.txt"), "no note after a failed copy");
    }
    {
        // Launch options: the layout's defaults; an explicit root keeps its
        // own settings; arguments are UTF-8.
        const fs::path game = base / L"fresh";
        const RuntimeUserDataLayout layout = RuntimeUserDataLayoutFor(game, savedGames);
        const char* plain[] = {"TheDarkness.exe"};
        const RuntimeLaunchOptions options = ParseRuntimeLaunchOptions(1, plain, game, layout);
        Check(options.pcConfigPath == Normal(layout.configPath) &&
                  options.pcConfigInstallPath == Normal(layout.configPath) &&
                  options.userDataRoot == Normal(layout.root) && !options.userDataRootExplicit,
              "defaults come from the layout");
        const fs::path isolated = base / L"isolated 李";
        const std::string isolatedUtf8 = isolated.u8string();
        const std::string rootArgument = "--user-data-root=" + isolatedUtf8;
        const char* rooted[] = {"TheDarkness.exe", rootArgument.c_str()};
        const RuntimeLaunchOptions rootedOptions = ParseRuntimeLaunchOptions(2, rooted, game, layout);
        Check(rootedOptions.userDataRootExplicit && rootedOptions.userDataRoot == Normal(isolated),
              "a UTF-8 root argument");
        Check(rootedOptions.pcConfigPath == Normal(isolated / L"TheDarkness.pc.toml") &&
                  rootedOptions.pcConfigInstallPath == Normal(isolated / L"TheDarkness.pc.toml"),
              "an explicit root keeps its own settings");
        const std::string config = (base / L"other Ж.toml").u8string();
        const char* both[] = {"TheDarkness.exe", "--user-data-root", isolatedUtf8.c_str(),
                              "--pc-config", config.c_str()};
        const RuntimeLaunchOptions bothOptions = ParseRuntimeLaunchOptions(5, both, game, layout);
        Check(bothOptions.pcConfigPath == Normal(base / L"other Ж.toml"),
              "--pc-config names the settings file");
    }
    fs::remove_all(base, error);
    if (!passed) return 1;
    std::cout << "runtime user data: PASS\n";
    return 0;
}
