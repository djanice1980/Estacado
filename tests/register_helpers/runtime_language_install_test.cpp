// Language pack installs (runtime/runtime_language_install.cpp): DKPATCH1
// application with both hashes, pack paths, stored zips and a whole install
// against a player's game folder, including the refusals.
#include "runtime_game_setup.h"
#include "runtime_language_install.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace install = darkness::language_install;

namespace {

bool Check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

void Put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
}
void Put64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(uint8_t(v >> (8 * i)));
}
void Put16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(uint8_t(v));
    out.push_back(uint8_t(v >> 8));
}
std::vector<uint8_t> Bytes(const std::string& text) { return {text.begin(), text.end()}; }

void PutHash(std::vector<uint8_t>& out, const std::vector<uint8_t>& data) {
    const std::string hex = darkness::game_setup::BytesSha256(data.data(), data.size());
    for (size_t i = 0; i < 64; i += 2) out.push_back(uint8_t(std::stoi(hex.substr(i, 2), nullptr, 16)));
}

struct Op { uint8_t kind; uint64_t offset, length; };
std::vector<uint8_t> MakePatch(const std::vector<uint8_t>& source, const std::vector<uint8_t>& target,
                               const std::vector<Op>& ops, const std::vector<uint8_t>& literals) {
    std::vector<uint8_t> out = Bytes("DKPATCH1");
    Put64(out, source.size());
    PutHash(out, source);
    Put64(out, target.size());
    PutHash(out, target);
    Put32(out, uint32_t(ops.size()));
    for (const Op& op : ops) {
        out.push_back(op.kind);
        out.insert(out.end(), 3, 0);
        Put64(out, op.offset);
        Put64(out, op.length);
    }
    Put64(out, literals.size());
    out.insert(out.end(), literals.begin(), literals.end());
    return out;
}

// A zip of stored entries (CRC left 0: the reader relies on the SHA-256 list).
std::vector<uint8_t> MakeZip(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files) {
    std::vector<uint8_t> out, directory;
    for (const auto& [name, data] : files) {
        const uint32_t offset = uint32_t(out.size());
        Put32(out, 0x04034B50u);
        Put16(out, 20); Put16(out, 0); Put16(out, 0); Put16(out, 0); Put16(out, 0);
        Put32(out, 0); Put32(out, uint32_t(data.size())); Put32(out, uint32_t(data.size()));
        Put16(out, uint16_t(name.size())); Put16(out, 0);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), data.begin(), data.end());
        Put32(directory, 0x02014B50u);
        Put16(directory, 20); Put16(directory, 20); Put16(directory, 0); Put16(directory, 0);
        Put16(directory, 0); Put16(directory, 0);
        Put32(directory, 0); Put32(directory, uint32_t(data.size())); Put32(directory, uint32_t(data.size()));
        Put16(directory, uint16_t(name.size())); Put16(directory, 0); Put16(directory, 0);
        Put16(directory, 0); Put16(directory, 0); Put32(directory, 0); Put32(directory, offset);
        directory.insert(directory.end(), name.begin(), name.end());
    }
    const uint32_t directoryOffset = uint32_t(out.size());
    out.insert(out.end(), directory.begin(), directory.end());
    Put32(out, 0x06054B50u);
    Put16(out, 0); Put16(out, 0);
    Put16(out, uint16_t(files.size())); Put16(out, uint16_t(files.size()));
    Put32(out, uint32_t(directory.size())); Put32(out, directoryOffset); Put16(out, 0);
    return out;
}

void WriteFile(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(data.data()),
                                               std::streamsize(data.size()));
}
std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
std::string Hash(const std::vector<uint8_t>& data) {
    return darkness::game_setup::BytesSha256(data.data(), data.size());
}

}  // namespace

int main() {
    bool ok = true;
    // Paths.
    ok &= Check(install::IsPackPath("strings.tsv") && install::IsPackPath("content/Content/Fonts/Text_AR.xfc"),
                "plain pack paths are accepted");
    for (const char* bad : {"", "/abs", "a//b", "../x", "a/../b", "a/./b", "C:x", "a\\b", "dir/", "a b"}) {
        ok &= Check(!install::IsPackPath(bad), "unsafe pack paths are refused");
    }
    // Patches.
    const std::vector<uint8_t> source = Bytes("HELLO WORLD");
    const std::vector<uint8_t> target = Bytes("HELLO ARABIC WORLD!");
    const std::vector<uint8_t> patch = MakePatch(
        source, target, {{0, 0, 6}, {1, 0, 7}, {0, 6, 5}, {1, 7, 1}}, Bytes("ARABIC !"));
    std::vector<uint8_t> out;
    std::string error;
    ok &= Check(install::ApplyPatch(source, patch, out, error) && out == target,
                "a patch rebuilds the target from the source");
    ok &= Check(!install::ApplyPatch(Bytes("HELLO W0RLD"), patch, out, error),
                "another source is refused (hash)");
    std::vector<uint8_t> bad = patch;
    bad[bad.size() - 1] ^= 1;  // a literal byte
    ok &= Check(!install::ApplyPatch(source, bad, out, error), "a damaged patch is refused (hash)");
    const std::vector<uint8_t> overrun = MakePatch(source, target, {{0, 8, 6}}, {});
    ok &= Check(!install::ApplyPatch(source, overrun, out, error), "a copy past the source is refused");

    // A whole install.
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "darkness_language_install_test";
    std::error_code ignore;
    std::filesystem::remove_all(root, ignore);
    const std::filesystem::path game = root / "game";
    const std::filesystem::path launcher = root / "launcher";
    WriteFile(game / "Content_Eng" / "Dialogues" / "All.xcd", source);
    std::filesystem::create_directories(launcher);
    const std::vector<uint8_t> strings = Bytes("MENU_BACK\tx\n");
    const std::vector<uint8_t> packToml = Bytes("name = \"Arabic\"\nlanguage = \"arabic\"\n");
    const auto asset = [&](const std::string& stringsHash) {
        return Bytes("format = 1\n[[file]]\npath = \"pack.toml\"\nsha256 = \"" + Hash(packToml) +
                     "\"\n[[file]]\npath = \"strings.tsv\"\nsha256 = \"" + stringsHash +
                     "\"\n[[patch]]\npath = \"patches/All.xcd.dkpatch\"\nsha256 = \"" + Hash(patch) +
                     "\"\nsource = \"Content_Eng/Dialogues/All.xcd\"\n"
                     "target = \"content/Content_Eng/Dialogues/All.xcd\"\n");
    };
    const auto zip = [&](const std::vector<uint8_t>& assetToml) {
        return MakeZip({{"pack.toml", packToml}, {"asset.toml", assetToml}, {"strings.tsv", strings},
                        {"patches/All.xcd.dkpatch", patch}});
    };
    const std::filesystem::path archive = root / "pack.zip";
    WriteFile(archive, zip(asset(Hash(strings))));
    std::vector<install::ZipEntry> entries;
    ok &= Check(install::ReadStoredZip(archive, entries, error) && entries.size() == 4 &&
                    entries[2].name == "strings.tsv" && entries[2].size == strings.size(),
                "stored zip entries are listed with their sizes");
    install::InstallResult result = install::InstallPack(archive, game, launcher, nullptr);
    const std::filesystem::path pack = launcher / "language_packs" / "arabic";
    ok &= Check(result.ok && result.language == "arabic", "the pack installs");
    ok &= Check(ReadFile(pack / "strings.tsv") == strings &&
                    ReadFile(pack / "content" / "Content_Eng" / "Dialogues" / "All.xcd") == target,
                "files land in the pack and the patch is applied to the game's file");
    ok &= Check(!std::filesystem::exists(pack / "patches") &&
                    !std::filesystem::exists(launcher / "language_packs" / "arabic.partial"),
                "patches are not copied and nothing is left staged");
    // Refusals keep the installed pack.
    WriteFile(archive, zip(asset(std::string(64, '0'))));
    result = install::InstallPack(archive, game, launcher, nullptr);
    ok &= Check(!result.ok && result.message == "The language pack is damaged." &&
                    ReadFile(pack / "strings.tsv") == strings,
                "a damaged archive is refused and the installed pack stays");
    WriteFile(archive, zip(asset(Hash(strings))));
    WriteFile(game / "Content_Eng" / "Dialogues" / "All.xcd", Bytes("OTHER DATA!"));
    result = install::InstallPack(archive, game, launcher, nullptr);
    ok &= Check(!result.ok && result.message == "Your game files do not match this language pack.",
                "another game file version is refused");
    WriteFile(archive, Bytes("not a zip at all"));
    result = install::InstallPack(archive, game, launcher, nullptr);
    ok &= Check(!result.ok && result.message == "The file is not a language pack.",
                "a file that is not a pack is refused");
    std::filesystem::remove_all(root, ignore);
    std::printf(ok ? "runtime_language_install: all checks passed\n"
                   : "runtime_language_install: FAILED\n");
    return ok ? 0 : 1;
}
