#pragma once

// The player's own copy of the game (first-run setup in the launcher, game
// location in the runtime). The recompiled code matches exactly one
// default.xex: The Darkness, Xbox 360, USA/Europe disc (title 545407EE). A
// player points the launcher at an extracted game folder (remembered in
// TheDarkness.game.toml next to the executables) or at a disc image, which is
// extracted into the "game" folder next to the executables.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace darkness::game_setup {

inline constexpr char kSupportedXexSha256[] =
    "aace35a8f9bcdc7f28aeab9ff8cf3bdf200353f5c83705f6284487347acb3c5f";
inline constexpr wchar_t kLocationFileName[] = L"TheDarkness.game.toml";
inline constexpr wchar_t kExtractedFolderName[] = L"game";
// The folder name of a disc dump (searched next to the executables and in
// every folder above them).
inline constexpr wchar_t kDiscDumpFolderName[] = L"Darkness, The (USA, Europe) (En,Fr,De,Es,It)";

// SHA-256 of a file as lower-case hex; empty when it cannot be read.
std::string FileSha256(const std::filesystem::path& path);
// SHA-256 of bytes as lower-case hex.
std::string BytesSha256(const uint8_t* data, size_t size);

// The game folder named in <executable directory>/TheDarkness.game.toml
// (game_folder = "<UTF-8 path>"), if the file names one.
std::optional<std::filesystem::path> ReadGameLocation(
    const std::filesystem::path& executableDirectory);
bool WriteGameLocation(const std::filesystem::path& executableDirectory,
                       const std::filesystem::path& folder, std::string* error);

// default.xex of the configured game: the location file's folder, else the
// "game" folder next to the executables; empty when neither has one.
std::filesystem::path ConfiguredGameXex(const std::filesystem::path& executableDirectory);
// The default.xex the game starts with: ConfiguredGameXex, else
// <executable directory or a folder above>/<kDiscDumpFolderName>/default.xex;
// empty when there is none.
std::filesystem::path FindGameXex(const std::filesystem::path& executableDirectory);

enum class GameStatus { kOk, kMissing, kUnreadable, kWrongVersion };
struct GameCheck {
    GameStatus status = GameStatus::kMissing;
    std::string sha256;  // of default.xex, when it could be read
};
// default.xex in the folder compared with the supported version.
GameCheck CheckGameFolder(const std::filesystem::path& folder);

// Xbox 360 disc images (GDF): the files and the default.xex hash.
struct DiscImageInfo {
    bool ok = false;
    std::string error;
    uint64_t files = 0;
    uint64_t bytes = 0;
    std::string xexSha256;  // empty without a default.xex
};
DiscImageInfo InspectDiscImage(const std::filesystem::path& image);

// Extracts every file into destination (created). progress(done, total)
// returning false cancels. False with a message on failure or cancel.
bool ExtractDiscImage(const std::filesystem::path& image,
                      const std::filesystem::path& destination,
                      const std::function<bool(uint64_t, uint64_t)>& progress,
                      std::string* error);

}  // namespace darkness::game_setup
