// Saves and settings per user (0.9.1): see runtime_user_data.h.
#include "runtime_user_data.h"

#include "product_name.h"

#include <Windows.h>
#include <KnownFolders.h>
#include <ShlObj.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <system_error>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace {
namespace fs = std::filesystem;

constexpr wchar_t kConfigName[] = L"TheDarkness.pc.toml";
constexpr wchar_t kCalibrationName[] = L"gpu_calibration.toml";
constexpr wchar_t kLockName[] = L"Local\\" DARKNESS_PRODUCT_NAME ".UserData";

bool IsFile(const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(path, error) && !error;
}

std::string Utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    std::string result(size_t(length > 0 ? length : 0), '\0');
    if (length > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), result.data(), length,
                            nullptr, nullptr);
    }
    return result;
}

std::string Utf8(const fs::path& path) { return Utf8(std::wstring_view(path.native())); }

std::string WindowsErrorText(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text = length && buffer ? std::wstring(buffer, length) : std::wstring{};
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ' ||
                             text.back() == L'.')) {
        text.pop_back();
    }
    return (text.empty() ? std::string("error") : Utf8(std::wstring_view(text))) + " (" + std::to_string(code) + ")";
}

std::string ErrorText(const std::error_code& error) {
    return error.category() == std::system_category() ? WindowsErrorText(DWORD(error.value()))
                                                       : error.message();
}

// Where a folder keeps its user data.
struct Places {
    fs::path content;
    fs::path screenshots;
    fs::path config;
    fs::path calibration;
};

Places GameFolderPlaces(const fs::path& gameFolder) {
    const fs::path data = gameFolder / L"runtime_data";
    return {data / L"content", data / L"screenshots", gameFolder / kConfigName,
            data / kCalibrationName};
}

Places LayoutPlaces(const RuntimeUserDataLayout& layout) {
    return {layout.root / L"content", layout.root / L"screenshots", layout.configPath,
            layout.root / kCalibrationName};
}

bool HoldsData(const Places& places) {
    return RuntimeFolderHasFiles(places.content) || IsFile(places.config) ||
           RuntimeFolderHasFiles(places.screenshots) || IsFile(places.calibration);
}

RuntimeUserDataLayout PerUserLayout(const fs::path& gameFolder, const fs::path& perUserRoot,
                                    const fs::path& localAppData) {
    RuntimeUserDataLayout layout;
    layout.mode = RuntimeUserDataMode::kPerUser;
    layout.gameFolder = gameFolder;
    layout.perUserRoot = perUserRoot;
    layout.root = perUserRoot;
    layout.configPath = perUserRoot / kConfigName;
    layout.localData =
        localAppData.empty() ? gameFolder : localAppData / (L"" DARKNESS_PRODUCT_NAME);
    return layout;
}

fs::path KnownFolder(const KNOWNFOLDERID& id) {
    PWSTR path = nullptr;
    fs::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &path)) && path && *path) {
        result = path;
    }
    CoTaskMemFree(path);
    return result;
}

// %LOCALAPPDATA% (isolated tests: DARKNESS_TEST_LOCAL_APP_DATA).
fs::path LocalAppDataFolder() {
    if (const wchar_t* test = _wgetenv(L"DARKNESS_TEST_LOCAL_APP_DATA"); test && *test) {
        return fs::path(test);
    }
    return KnownFolder(FOLDERID_LocalAppData);
}

fs::path SavedGamesFolder() {
    if (const wchar_t* test = _wgetenv(L"DARKNESS_TEST_SAVED_GAMES"); test && *test) {
        return fs::path(test);
    }
    PWSTR path = nullptr;
    fs::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, KF_FLAG_DONT_VERIFY, nullptr, &path)) &&
        path && *path) {
        result = path;
    }
    CoTaskMemFree(path);
    return result;
}

// Serializes copies between the game and the launcher.
class UserDataLock {
public:
    UserDataLock() : handle_(CreateMutexW(nullptr, FALSE, kLockName)) {
        if (!handle_) return;
        const DWORD wait = WaitForSingleObject(handle_, 60000);
        acquired_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
    }
    ~UserDataLock() {
        if (acquired_) ReleaseMutex(handle_);
        if (handle_) CloseHandle(handle_);
    }
    UserDataLock(const UserDataLock&) = delete;
    UserDataLock& operator=(const UserDataLock&) = delete;
    bool acquired() const { return acquired_; }

private:
    HANDLE handle_ = nullptr;
    bool acquired_ = false;
};

bool SameBytes(const fs::path& left, const fs::path& right) {
    std::ifstream a(left, std::ios::binary);
    std::ifstream b(right, std::ios::binary);
    if (!a || !b) return false;
    std::vector<char> x(size_t(1) << 20);
    std::vector<char> y(x.size());
    for (;;) {
        a.read(x.data(), std::streamsize(x.size()));
        b.read(y.data(), std::streamsize(y.size()));
        const std::streamsize count = a.gcount();
        if (count != b.gcount()) return false;
        if (count == 0) return a.eof() && b.eof();
        if (std::memcmp(x.data(), y.data(), size_t(count)) != 0) return false;
    }
}

bool Fail(RuntimeUserDataCopy& copy, std::string error) {
    copy.ok = false;
    if (copy.error.empty()) copy.error = std::move(error);
    return false;
}

// Copies one file, never over an existing one, and compares the copy.
bool CopyChecked(const fs::path& from, const fs::path& to, RuntimeUserDataCopy& copy) {
    std::error_code error;
    fs::create_directories(to.parent_path(), error);
    if (!CopyFileW(from.c_str(), to.c_str(), TRUE)) {
        return Fail(copy, "could not copy " + Utf8(from) + ": " + WindowsErrorText(GetLastError()));
    }
    // A read-only original must not leave the game a copy it cannot save to.
    const DWORD attributes = GetFileAttributesW(to.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY)) {
        SetFileAttributesW(to.c_str(), attributes & ~DWORD(FILE_ATTRIBUTE_READONLY));
    }
    if (!SameBytes(from, to)) {
        return Fail(copy, "the copy of " + Utf8(from) + " differs from the original");
    }
    ++copy.files;
    copy.bytes += fs::file_size(from, error);
    return true;
}

// Copies every file below `from` to the same place below `to`, leaving out
// the files that `existing` (the final folder) already has.
bool CopyTreeChecked(const fs::path& from, const fs::path& to, RuntimeUserDataCopy& copy,
                     const fs::path& existing = {}) {
    std::error_code error;
    fs::recursive_directory_iterator entry(from, error);
    for (; !error && entry != fs::recursive_directory_iterator(); entry.increment(error)) {
        std::error_code typeError;
        if (entry->is_symlink(typeError)) {
            if (entry->is_directory(typeError)) entry.disable_recursion_pending();
            copy.lines.push_back("left out link " + Utf8(entry->path()));
            continue;
        }
        if (!entry->is_regular_file(typeError)) continue;  // folders come with their files
        const fs::path relative = entry->path().lexically_relative(from);
        if (!existing.empty() && fs::exists(existing / relative, typeError)) continue;
        if (!CopyChecked(entry->path(), to / relative, copy)) return false;
    }
    if (error) return Fail(copy, "could not read " + Utf8(from) + ": " + ErrorText(error));
    return true;
}

// Renames within one volume, never over an existing file or folder.
bool Place(const fs::path& from, const fs::path& to, RuntimeUserDataCopy& copy) {
    std::error_code error;
    fs::create_directories(to.parent_path(), error);
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
    return Fail(copy, "could not place " + Utf8(to) + ": " + WindowsErrorText(GetLastError()));
}

// Places every staged file below `from` at the same place below `to`.
bool PlaceTree(const fs::path& from, const fs::path& to, RuntimeUserDataCopy& copy) {
    std::error_code error;
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator entry(from, error);
         !error && entry != fs::recursive_directory_iterator(); entry.increment(error)) {
        std::error_code typeError;
        if (entry->is_regular_file(typeError)) files.push_back(entry->path());
    }
    if (error) return Fail(copy, "could not read " + Utf8(from) + ": " + ErrorText(error));
    for (const fs::path& file : files) {
        if (!Place(file, to / file.lexically_relative(from), copy)) return false;
    }
    return true;
}

// Removes a folder that holds no files (only empty folders).
bool RemoveEmptyTree(const fs::path& folder) {
    std::error_code error;
    if (!fs::is_directory(folder, error) || error || RuntimeFolderHasFiles(folder)) return false;
    fs::remove_all(folder, error);
    return !error && !fs::exists(folder, error);
}

fs::path StagingFolder(const fs::path& root, const wchar_t* kind) {
    return root / (std::wstring(L".") + kind + L"-" + std::to_wstring(GetCurrentProcessId()) +
                   L"-" + std::to_wstring(GetTickCount64()));
}

std::wstring TimeStamp() {
    SYSTEMTIME local{};
    GetLocalTime(&local);
    wchar_t text[32]{};
    swprintf_s(text, L"%04u%02u%02u-%02u%02u%02u", local.wYear, local.wMonth, local.wDay,
               local.wHour, local.wMinute, local.wSecond);
    return text;
}

// The note in the game folder: its saves were copied (or, kept, left as
// they are); either way they are not copied again.
bool WriteNote(const fs::path& gameFolder, const fs::path& perUserRoot, bool copied,
               RuntimeUserDataCopy& copy) {
    const fs::path note = gameFolder / kRuntimeSavesMovedNote;
    if (IsFile(note)) return true;
    SYSTEMTIME local{};
    GetLocalTime(&local);
    char date[16]{};
    std::snprintf(date, sizeof(date), "%04u-%02u-%02u", local.wYear, local.wMonth, local.wDay);
    std::ofstream out(note, std::ios::binary);
    out << "\xEF\xBB\xBF" DARKNESS_PRODUCT_NAME
           " keeps your saves and settings in your Saved Games folder:\r\n"
        << "  " << Utf8(perUserRoot) << "\r\n\r\n";
    if (copied) {
        out << "On " << date
            << " the saves, settings and screenshots of this folder were copied\r\n"
               "there, every copied file was compared with its original, and the game uses\r\n"
               "that copy from now on.\r\n\r\n"
               "The files here (runtime_data and TheDarkness.pc.toml) were left unchanged as a\r\n"
               "backup. Once your saves load in the game, you may delete them.\r\n";
    } else {
        out << "On " << date
            << " you chose to keep the saves that were already there. The saves of\r\n"
               "this folder (runtime_data) were not copied and are left unchanged here; the\r\n"
               "settings launcher can still import them (About, Import saves).\r\n";
    }
    out.close();
    copy.lines.push_back((out ? "note " : "note not written: ") + Utf8(note));
    return bool(out);
}

}  // namespace

bool RuntimeFolderHasFiles(const fs::path& folder) {
    std::error_code error;
    if (!fs::is_directory(folder, error) || error) return false;
    for (fs::recursive_directory_iterator entry(
             folder, fs::directory_options::skip_permission_denied, error);
         !error && entry != fs::recursive_directory_iterator(); entry.increment(error)) {
        std::error_code typeError;
        if (entry->is_regular_file(typeError)) return true;
    }
    return false;
}

const char* RuntimeUserDataModeName(RuntimeUserDataMode mode) {
    switch (mode) {
        case RuntimeUserDataMode::kPerUser:
            return "per_user";
        case RuntimeUserDataMode::kPortable:
            return "portable";
        case RuntimeUserDataMode::kGameFolder:
            return "game_folder";
    }
    return "unknown";
}

std::string RuntimeUserDataLine(const RuntimeUserDataLayout& layout) {
    std::string line = std::string("USER_DATA mode=") + RuntimeUserDataModeName(layout.mode) +
                       " pending=" + (layout.migrationPending ? "1" : "0") +
                       " left_behind=" + (layout.gameFolderSavesLeftBehind ? "1" : "0");
    if (!layout.reason.empty()) line += " reason=\"" + layout.reason + "\"";
    return line + " root=" + Utf8(layout.root) + " config=" + Utf8(layout.configPath) +
           " local=" + Utf8(layout.localData);
}

RuntimeUserDataLayout RuntimeGameFolderUserDataLayout(const fs::path& gameFolder,
                                                      RuntimeUserDataMode mode,
                                                      std::string reason) {
    RuntimeUserDataLayout layout;
    layout.mode = mode;
    layout.gameFolder = gameFolder;
    layout.root = gameFolder / L"runtime_data";
    layout.configPath = gameFolder / kConfigName;
    layout.localData = gameFolder;
    layout.reason = std::move(reason);
    return layout;
}

RuntimeUserDataLayout RuntimeUserDataLayoutFor(const fs::path& gameFolder,
                                               const fs::path& savedGames,
                                               const fs::path& localAppData) {
    if (IsFile(gameFolder / kRuntimePortableMarker)) {
        return RuntimeGameFolderUserDataLayout(gameFolder, RuntimeUserDataMode::kPortable,
                                               "portable.txt");
    }
    if (savedGames.empty()) {
        return RuntimeGameFolderUserDataLayout(gameFolder, RuntimeUserDataMode::kGameFolder,
                                               "no Saved Games folder");
    }
    RuntimeUserDataLayout layout =
        PerUserLayout(gameFolder, savedGames / (L"" DARKNESS_PRODUCT_NAME), localAppData);
    const Places old = GameFolderPlaces(gameFolder);
    if (IsFile(gameFolder / kRuntimeSavesMovedNote) || !HoldsData(old)) return layout;
    if (!RuntimeFolderHasFiles(layout.root / L"content")) {
        // 0.9.0 data to copy: until then the game folder stays in use.
        RuntimeUserDataLayout pending = RuntimeGameFolderUserDataLayout(
            gameFolder, RuntimeUserDataMode::kGameFolder, "not yet copied to Saved Games");
        pending.perUserRoot = layout.perUserRoot;
        pending.migrationPending = true;
        return pending;
    }
    layout.gameFolderSavesLeftBehind = RuntimeFolderHasFiles(old.content);
    return layout;
}

RuntimeUserDataLayout RuntimeDetectUserDataLayout(const fs::path& gameFolder) {
    return RuntimeUserDataLayoutFor(gameFolder, SavedGamesFolder(), LocalAppDataFolder());
}

RuntimeUserDataLayout RuntimeMigrateGameFolderUserData(const RuntimeUserDataLayout& layout,
                                                       RuntimeUserDataCopy& copy) {
    if (!layout.migrationPending || layout.perUserRoot.empty()) return layout;
    UserDataLock lock;
    if (!lock.acquired()) {
        Fail(copy, "another " DARKNESS_PRODUCT_NAME " program is copying the saves");
        return layout;
    }
    // The launcher or the game may have finished the copy meanwhile.
    const RuntimeUserDataLayout current =
        RuntimeUserDataLayoutFor(layout.gameFolder, layout.perUserRoot.parent_path(),
                                 LocalAppDataFolder());
    if (!current.migrationPending) return current;
    const RuntimeUserDataLayout target =
        PerUserLayout(layout.gameFolder, layout.perUserRoot, LocalAppDataFolder());
    const Places from = GameFolderPlaces(layout.gameFolder);
    const Places to = LayoutPlaces(target);

    std::error_code error;
    if (fs::exists(to.content, error) && !fs::is_directory(to.content, error)) {
        Fail(copy, Utf8(to.content) + " is a file, not the saves folder");
        return layout;
    }
    fs::create_directories(target.root, error);
    const fs::path staging = StagingFolder(target.root, L"copying");
    if (!fs::create_directory(staging, error) || error) {
        Fail(copy, "could not create " + Utf8(staging) + ": " + ErrorText(error));
        return layout;
    }
    // Everything is copied and compared first. The saves folder is placed
    // last: it is the switch, so a failure leaves the game folder in use.
    const bool saves = RuntimeFolderHasFiles(from.content);
    const bool config = IsFile(from.config) && !IsFile(to.config);
    const bool calibration = IsFile(from.calibration) && !IsFile(to.calibration);
    const bool screenshots = RuntimeFolderHasFiles(from.screenshots);
    if (IsFile(from.config) && !config) copy.lines.push_back("kept the existing " + Utf8(to.config));
    bool ok = (!saves || CopyTreeChecked(from.content, staging / L"content", copy)) &&
              (!config || CopyChecked(from.config, staging / kConfigName, copy)) &&
              (!calibration || CopyChecked(from.calibration, staging / kCalibrationName, copy)) &&
              (!screenshots ||
               CopyTreeChecked(from.screenshots, staging / L"screenshots", copy, to.screenshots));
    ok = ok && (!config || Place(staging / kConfigName, to.config, copy)) &&
         (!calibration || Place(staging / kCalibrationName, to.calibration, copy)) &&
         (!screenshots || !fs::exists(staging / L"screenshots", error) ||
          PlaceTree(staging / L"screenshots", to.screenshots, copy));
    if (ok && saves) {
        if (fs::exists(to.content, error) && !RemoveEmptyTree(to.content)) {
            ok = Fail(copy, "could not replace the empty folder " + Utf8(to.content));
        } else {
            ok = Place(staging / L"content", to.content, copy);
        }
    }
    // Only copies (of files that are still in the game folder) remain here.
    fs::remove_all(staging, error);
    if (!ok) return layout;
    copy.lines.push_back("copied " + std::to_string(copy.files) + " files (" +
                         std::to_string(copy.bytes) + " bytes) from " + Utf8(layout.gameFolder) +
                         " to " + Utf8(target.root));
    WriteNote(layout.gameFolder, target.root, true, copy);
    return target;
}

bool RuntimeKeepGameFolderUserData(const RuntimeUserDataLayout& layout) {
    RuntimeUserDataCopy copy;
    return layout.mode == RuntimeUserDataMode::kPerUser &&
           WriteNote(layout.gameFolder, layout.root, false, copy);
}

RuntimeUserDataCopy RuntimeImportUserData(const RuntimeUserDataLayout& layout,
                                          const fs::path& source) {
    RuntimeUserDataCopy copy;
    Places from;
    if (RuntimeFolderHasFiles(source / L"runtime_data" / L"content")) {
        from = GameFolderPlaces(source);
    } else if (RuntimeFolderHasFiles(source / L"content")) {
        // A runtime_data folder (its settings beside it) or a copy of a
        // Saved Games folder (settings inside).
        from = {source / L"content", source / L"screenshots",
                IsFile(source / kConfigName) ? source / kConfigName
                                             : source.parent_path() / kConfigName,
                source / kCalibrationName};
    } else {
        Fail(copy, "No saves were found in " + Utf8(source) +
                       ". Choose the folder of the older version (the folder with "
                       "TheDarkness.exe); its saves are in runtime_data\\content.");
        return copy;
    }
    const Places to = LayoutPlaces(layout);
    std::error_code error;
    if (fs::equivalent(from.content, to.content, error)) {
        Fail(copy, "These are the saves the game already uses.");
        return copy;
    }
    UserDataLock lock;
    if (!lock.acquired()) {
        Fail(copy, "another " DARKNESS_PRODUCT_NAME " program is copying the saves");
        return copy;
    }
    fs::create_directories(layout.root, error);
    const fs::path staging = StagingFolder(layout.root, L"importing");
    if (!fs::create_directory(staging, error) || error) {
        Fail(copy, "could not create " + Utf8(staging) + ": " + ErrorText(error));
        return copy;
    }
    const bool config = IsFile(from.config);
    bool ok = CopyTreeChecked(from.content, staging / L"content", copy) &&
              (!config || CopyChecked(from.config, staging / kConfigName, copy)) &&
              (!RuntimeFolderHasFiles(from.screenshots) ||
               CopyTreeChecked(from.screenshots, staging / L"screenshots", copy, to.screenshots));
    const std::wstring stamp = TimeStamp();
    if (ok) {
        // The current saves are kept under a new name, never replaced.
        fs::path kept;
        if (RuntimeFolderHasFiles(to.content)) {
            kept = to.content;
            kept += L".before-import-" + stamp;
            ok = Place(to.content, kept, copy);
        } else if (fs::exists(to.content, error) && !RemoveEmptyTree(to.content)) {
            ok = Fail(copy, "could not replace the empty folder " + Utf8(to.content));
        }
        if (ok && !Place(staging / L"content", to.content, copy)) {
            ok = false;
            if (!kept.empty()) Place(kept, to.content, copy);  // put the current saves back
            kept.clear();
        }
        if (!kept.empty()) copy.lines.push_back("previous saves kept in " + Utf8(kept));
    }
    if (ok && config) {
        fs::path kept;
        if (IsFile(to.config)) {
            kept = to.config.parent_path() / (L"TheDarkness.pc.before-import-" + stamp + L".toml");
            ok = Place(to.config, kept, copy);
        }
        if (ok && !Place(staging / kConfigName, to.config, copy)) {
            ok = false;
            if (!kept.empty()) Place(kept, to.config, copy);  // put the current settings back
            kept.clear();
        }
        if (!kept.empty()) copy.lines.push_back("previous settings kept in " + Utf8(kept));
    }
    if (ok && fs::exists(staging / L"screenshots", error)) {
        ok = PlaceTree(staging / L"screenshots", to.screenshots, copy);
    }
    fs::remove_all(staging, error);
    if (!ok) return copy;
    copy.lines.push_back("imported " + std::to_string(copy.files) + " files (" +
                         std::to_string(copy.bytes) + " bytes) from " + Utf8(source));
    // Saves left behind in this game folder are now handled.
    if (fs::equivalent(from.content, GameFolderPlaces(layout.gameFolder).content, error)) {
        WriteNote(layout.gameFolder, layout.root, true, copy);
    }
    return copy;
}
