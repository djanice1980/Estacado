#pragma once

// Language packs installed by the launcher from a pack archive (an optional
// release download): a zip of stored entries with the pack's own files, and
// binary patches (scripts/binary_patch.py) that turn files of the player's
// own game into the pack's versions, so the archive holds no game data. The
// pack lands in <launcher folder>/language_packs/<language>, replacing an
// older one only when the new one is complete.
//
// Archive: pack.toml (name, language, direction), asset.toml
//   format = 1
//   [[file]]  path = "<path in the pack>", sha256 = "<hex>"
//   [[patch]] path = "<patch in the archive>", sha256 = "<hex>",
//             source = "<file in the game folder>", target = "<path in the pack>"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace darkness::language_install {

// DKPATCH1 (scripts/binary_patch.py): target from source, with both SHA-256
// hashes checked.
bool ApplyPatch(const std::vector<uint8_t>& source, const std::vector<uint8_t>& patch,
                std::vector<uint8_t>& target, std::string& error);

struct ZipEntry {
    std::string name;
    uint64_t dataOffset = 0;
    uint64_t size = 0;
};
// The entries of a zip whose entries are all stored (not compressed).
bool ReadStoredZip(const std::filesystem::path& zip, std::vector<ZipEntry>& entries,
                   std::string& error);
// A relative path inside a pack: letters, digits, '_', '-', '.', '/' only; no
// empty, "." or ".." component.
bool IsPackPath(const std::string& name);

struct InstallResult {
    bool ok = false;
    std::string message;   // for the player (English, translated by the launcher)
    std::string detail;    // technical detail
    std::string language;  // the pack's folder name
};
// progress(done, total bytes) returning false cancels.
InstallResult InstallPack(const std::filesystem::path& archive,
                          const std::filesystem::path& gameFolder,
                          const std::filesystem::path& launcherFolder,
                          const std::function<bool(uint64_t, uint64_t)>& progress);

// HTTPS download into target (WinHTTP; redirects followed); the file's
// SHA-256 must equal expectedSha256 when that is not empty.
bool DownloadFile(const std::wstring& url, const std::filesystem::path& target,
                  const std::string& expectedSha256,
                  const std::function<bool(uint64_t, uint64_t)>& progress, std::string& error);

}  // namespace darkness::language_install
