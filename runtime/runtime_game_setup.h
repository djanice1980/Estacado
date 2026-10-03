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
#include <vector>

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

// Versions of default.xex (0.9.1). The recompiled code is the supported
// executable's; another copy runs when it has the same code. Its decrypted,
// unpacked image must match the supported one in the code sections (with the
// jump tables in them), in its structure (everything but .rdata and the
// title's resource strings), in every .rdata word that points into the image
// (vtables, function tables) and in the .rdata the runtime reads by address.
// Text, fonts and the resource strings may differ, so a localisation works.
// The program carries hashes of the supported image, never game data.
enum class XexMatch {
    kSupported,      // the supported file itself
    kSameCode,       // another file with the same code: runs
    kDifferentCode,  // another build of the game: does not run
    kNotReadable,    // not a readable Xbox 360 executable
};
struct XexIdentity {
    XexMatch match = XexMatch::kNotReadable;
    std::string fileSha256;
    // The decoded image (not decoded for the supported file unless asked).
    bool decoded = false;
    std::string error;
    uint16_t encryption = 0;
    uint16_t compression = 0;
    uint32_t loadAddress = 0;
    uint32_t imageSize = 0;
    uint32_t entryPoint = 0;
    std::string imageSha256;
    std::string codeSha256;
    std::string structureSha256;
    std::string pointerSha256;
    std::string protectedSha256;
    struct Section {
        std::string name;
        uint32_t address = 0;
        uint32_t size = 0;
        bool code = false;
        std::string sha256;
    };
    std::vector<Section> sections;
};
// The .rdata the runtime reads by address (64 bytes from each).
inline constexpr uint32_t kRuntimeReadDataAddresses[] = {
    0x8205BE44, 0x82077FB8, 0x82078228, 0x82079088, 0x82095798, 0x8209DCB0,
    0x8209DE58, 0x8209E058, 0x8209E500, 0x8209E8A0, 0x8209F124,
};
// The supported image's identity.
inline constexpr uint32_t kSupportedLoadAddress = 0x82000000;
inline constexpr uint32_t kSupportedImageSize = 0x00B10000;
inline constexpr uint32_t kSupportedEntryPoint = 0x828AA3E8;
inline constexpr char kSupportedCodeSha256[] =
    "c084f775f88ec4b7186abd9ad1c4444cc2623d66637a1420e6c41ac3c2fe5d5a";
extern const char* const kSupportedStructureSha256;
extern const char* const kSupportedPointerSha256;
extern const char* const kSupportedProtectedSha256;

// Identifies default.xex bytes; decodeSupported also decodes the supported
// file (reports, measuring the reference values).
XexIdentity IdentifyXex(const uint8_t* data, size_t size, bool decodeSupported = false);
XexIdentity IdentifyXexFile(const std::filesystem::path& path, bool decodeSupported = false);
// Plain-text report of an identification: names, sizes and hashes only.
std::string XexIdentityReport(const XexIdentity& identity, const std::string& source);
// One line for players: what the identification means.
std::string XexMatchText(const XexIdentity& identity);

enum class GameStatus { kOk, kMissing, kUnreadable, kWrongVersion };
struct GameCheck {
    GameStatus status = GameStatus::kMissing;
    std::string sha256;  // of default.xex, when it could be read
    XexIdentity identity;
};
// default.xex in the folder compared with the supported version (kOk for
// the supported file and for copies with the same code).
GameCheck CheckGameFolder(const std::filesystem::path& folder);

// Xbox 360 disc images (GDF): the files and the default.xex hash.
struct DiscImageInfo {
    bool ok = false;
    std::string error;
    uint64_t files = 0;
    uint64_t bytes = 0;
    std::string xexSha256;  // empty without a default.xex
    XexIdentity xex;        // of that default.xex
};
DiscImageInfo InspectDiscImage(const std::filesystem::path& image);

// Extracts every file into destination (created). progress(done, total)
// returning false cancels. False with a message on failure or cancel.
bool ExtractDiscImage(const std::filesystem::path& image,
                      const std::filesystem::path& destination,
                      const std::function<bool(uint64_t, uint64_t)>& progress,
                      std::string* error);

}  // namespace darkness::game_setup
