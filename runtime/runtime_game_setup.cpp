// The player's own copy of the game: see runtime_game_setup.h.
#include "runtime_game_setup.h"

#include <windows.h>
#include <bcrypt.h>

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

namespace darkness::game_setup {

namespace {

constexpr uint64_t kSectorBytes = 2048;
constexpr char kGdfMagic[] = "MICROSOFT*XBOX*MEDIA";
constexpr size_t kGdfMagicBytes = 20;
// Where the game partition may start (plain game partition, XGD1, XGD2,
// XGD3 layouts); its volume descriptor is sector 32 of the partition.
constexpr uint64_t kPartitionOffsets[] = {0x00000000, 0x0000FB20, 0x00020600, 0x02080000,
                                          0x0FD90000};
constexpr uint8_t kAttributeDirectory = 0x10;

class Sha256 {
public:
    Sha256() {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
            algorithm_ = nullptr;
            return;
        }
        DWORD objectBytes = 0, got = 0;
        if (BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH,
                              reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &got,
                              0) < 0) {
            return;
        }
        object_.resize(objectBytes);
        if (BCryptCreateHash(algorithm_, &hash_, object_.data(), ULONG(object_.size()), nullptr,
                             0, 0) < 0) {
            hash_ = nullptr;
        }
    }
    ~Sha256() {
        if (hash_) BCryptDestroyHash(hash_);
        if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    bool Update(const uint8_t* data, size_t size) {
        return hash_ && BCryptHashData(hash_, const_cast<PUCHAR>(data), ULONG(size), 0) >= 0;
    }
    std::string Finish() {
        std::array<uint8_t, 32> digest{};
        if (!hash_ || BCryptFinishHash(hash_, digest.data(), ULONG(digest.size()), 0) < 0) {
            return {};
        }
        static constexpr char kHex[] = "0123456789abcdef";
        std::string out;
        for (uint8_t byte : digest) {
            out.push_back(kHex[byte >> 4]);
            out.push_back(kHex[byte & 15]);
        }
        return out;
    }

private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    std::vector<uint8_t> object_;
};

std::string Lower(std::string text) {
    for (char& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// A disc image opened for reading (64-bit offsets).
class Image {
public:
    explicit Image(const std::filesystem::path& path) {
        file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        LARGE_INTEGER size{};
        if (file_ != INVALID_HANDLE_VALUE && GetFileSizeEx(file_, &size)) {
            size_ = uint64_t(size.QuadPart);
        }
    }
    ~Image() {
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    }
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    bool open() const { return file_ != INVALID_HANDLE_VALUE; }
    uint64_t size() const { return size_; }
    bool Read(uint64_t offset, void* out, size_t bytes) const {
        if (!open() || offset > size_ || bytes > size_ - offset) return false;
        uint8_t* dest = static_cast<uint8_t*>(out);
        while (bytes) {
            OVERLAPPED overlapped{};
            overlapped.Offset = DWORD(offset & 0xFFFFFFFFu);
            overlapped.OffsetHigh = DWORD(offset >> 32);
            const DWORD chunk = DWORD(std::min<size_t>(bytes, 16u << 20));
            DWORD got = 0;
            if (!ReadFile(file_, dest, chunk, &got, &overlapped) || got != chunk) return false;
            dest += got;
            offset += got;
            bytes -= got;
        }
        return true;
    }

private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    uint64_t size_ = 0;
};

struct DiscFile {
    std::filesystem::path relative;  // from the image root
    bool directory = false;
    uint64_t offset = 0;             // in the image (files)
    uint64_t size = 0;
};

// A file or folder name from the image: plain names only (no separators,
// no "." or ".." and no characters Windows rejects).
bool SafeName(const std::string& name) {
    if (name.empty() || name == "." || name == "..") return false;
    for (unsigned char c : name) {
        if (c < 32 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|') {
            return false;
        }
    }
    return true;
}

// Walks the image's directory trees (GDF: each directory table is a binary
// tree of entries addressed in 4-byte units).
bool ListDisc(const Image& image, std::vector<DiscFile>& files, std::string* error) {
    const auto fail = [&](const char* text) {
        if (error) *error = text;
        return false;
    };
    uint64_t partition = 0;
    bool found = false;
    char magic[kGdfMagicBytes];
    for (uint64_t offset : kPartitionOffsets) {
        if (image.Read(offset + 32 * kSectorBytes, magic, sizeof(magic)) &&
            std::memcmp(magic, kGdfMagic, kGdfMagicBytes) == 0) {
            partition = offset;
            found = true;
            break;
        }
    }
    if (!found) return fail("not an Xbox 360 disc image (no game partition found)");
    uint32_t root[2];
    if (!image.Read(partition + 32 * kSectorBytes + kGdfMagicBytes, root, sizeof(root))) {
        return fail("the disc image is truncated");
    }
    struct Table {
        std::filesystem::path folder;
        uint64_t sector;
        uint64_t bytes;
    };
    std::vector<Table> tables{{{}, root[0], root[1]}};
    constexpr size_t kMaxEntries = 1u << 20;
    constexpr uint64_t kMaxTableBytes = 64ull << 20;
    for (size_t t = 0; t < tables.size(); ++t) {
        const Table table = tables[t];
        if (table.bytes == 0) continue;
        if (table.bytes > kMaxTableBytes || tables.size() > kMaxEntries) {
            return fail("the disc image's directory tables are damaged");
        }
        std::vector<uint8_t> buffer(size_t(table.bytes));
        if (!image.Read(partition + table.sector * kSectorBytes, buffer.data(), buffer.size())) {
            return fail("the disc image is truncated");
        }
        std::vector<uint32_t> stack{0};
        std::vector<bool> seen(buffer.size() / 4 + 1, false);
        while (!stack.empty()) {
            const uint32_t ordinal = stack.back();
            stack.pop_back();
            const size_t at = size_t(ordinal) * 4;
            if (at + 14 > buffer.size() || seen[ordinal]) continue;
            seen[ordinal] = true;
            uint16_t left, right;
            uint32_t sector, length;
            std::memcpy(&left, &buffer[at], 2);
            std::memcpy(&right, &buffer[at + 2], 2);
            std::memcpy(&sector, &buffer[at + 4], 4);
            std::memcpy(&length, &buffer[at + 8], 4);
            if (left == 0xFFFF && right == 0xFFFF) continue;  // padding
            const uint8_t attributes = buffer[at + 12];
            const size_t nameBytes = buffer[at + 13];
            if (at + 14 + nameBytes > buffer.size()) {
                return fail("the disc image's directory tables are damaged");
            }
            if (left && left != 0xFFFF) stack.push_back(left);
            if (right && right != 0xFFFF) stack.push_back(right);
            const std::string name(reinterpret_cast<const char*>(&buffer[at + 14]), nameBytes);
            if (!SafeName(name)) return fail("the disc image contains an invalid file name");
            DiscFile file;
            file.relative = table.folder / std::filesystem::u8path(name);
            file.directory = (attributes & kAttributeDirectory) != 0;
            file.size = file.directory ? 0 : length;
            file.offset = partition + uint64_t(sector) * kSectorBytes;
            if (!file.directory && (file.offset > image.size() || file.size > image.size() - file.offset)) {
                return fail("the disc image is truncated");
            }
            if (file.directory) tables.push_back({file.relative, sector, length});
            files.push_back(std::move(file));
            if (files.size() > kMaxEntries) return fail("the disc image lists too many files");
        }
    }
    return true;
}

bool HashRange(const Image& image, uint64_t offset, uint64_t size, std::string& out) {
    Sha256 hash;
    std::vector<uint8_t> buffer(1u << 20);
    while (size) {
        const size_t chunk = size_t(std::min<uint64_t>(size, buffer.size()));
        if (!image.Read(offset, buffer.data(), chunk) || !hash.Update(buffer.data(), chunk)) {
            return false;
        }
        offset += chunk;
        size -= chunk;
    }
    out = hash.Finish();
    return !out.empty();
}

}  // namespace

std::string FileSha256(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    Sha256 hash;
    std::vector<char> buffer(1u << 20);
    while (stream) {
        stream.read(buffer.data(), std::streamsize(buffer.size()));
        const std::streamsize got = stream.gcount();
        if (got > 0 && !hash.Update(reinterpret_cast<const uint8_t*>(buffer.data()), size_t(got))) {
            return {};
        }
    }
    if (!stream.eof()) return {};
    return hash.Finish();
}

std::string BytesSha256(const uint8_t* data, size_t size) {
    Sha256 hash;
    return hash.Update(data, size) ? hash.Finish() : std::string();
}

std::optional<std::filesystem::path> ReadGameLocation(
    const std::filesystem::path& executableDirectory) {
    const std::filesystem::path file = executableDirectory / kLocationFileName;
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error)) return std::nullopt;
    try {
        const toml::table table = toml::parse_file(file.u8string());
        if (const auto folder = table["game_folder"].value<std::string>(); folder && !folder->empty()) {
            return std::filesystem::u8path(*folder);
        }
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

bool WriteGameLocation(const std::filesystem::path& executableDirectory,
                       const std::filesystem::path& folder, std::string* error) {
    const auto text = folder.u8string();
    std::string escaped;
    for (const auto c : text) {
        if (c == '\\' || c == '"') escaped.push_back('\\');
        escaped.push_back(char(c));
    }
    const std::filesystem::path file = executableDirectory / kLocationFileName;
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream << "# Where the game files are (written by the settings launcher's game setup).\n"
           << "game_folder = \"" << escaped << "\"\n";
    stream.close();
    if (!stream) {
        if (error) *error = "unable to write " + file.u8string();
        return false;
    }
    return true;
}

std::filesystem::path ConfiguredGameXex(const std::filesystem::path& executableDirectory) {
    std::error_code error;
    if (const auto folder = ReadGameLocation(executableDirectory)) {
        const std::filesystem::path xex = *folder / L"default.xex";
        if (std::filesystem::is_regular_file(xex, error)) return xex;
    }
    const std::filesystem::path extracted =
        executableDirectory / kExtractedFolderName / L"default.xex";
    if (std::filesystem::is_regular_file(extracted, error)) return extracted;
    return {};
}

std::filesystem::path FindGameXex(const std::filesystem::path& executableDirectory) {
    if (std::filesystem::path configured = ConfiguredGameXex(executableDirectory);
        !configured.empty()) {
        return configured;
    }
    std::error_code error;
    std::filesystem::path directory = std::filesystem::absolute(executableDirectory, error);
    if (error) return {};
    for (;;) {
        const std::filesystem::path candidate = directory / kDiscDumpFolderName / L"default.xex";
        if (std::filesystem::is_regular_file(candidate, error)) return candidate;
        const std::filesystem::path parent = directory.parent_path();
        if (parent.empty() || parent == directory) break;
        directory = parent;
    }
    return {};
}

GameCheck CheckGameFolder(const std::filesystem::path& folder) {
    GameCheck check;
    const std::filesystem::path xex = folder / L"default.xex";
    std::error_code error;
    if (!std::filesystem::is_regular_file(xex, error)) {
        check.status = GameStatus::kMissing;
        return check;
    }
    check.sha256 = FileSha256(xex);
    if (check.sha256.empty()) {
        check.status = GameStatus::kUnreadable;
    } else {
        check.status = check.sha256 == kSupportedXexSha256 ? GameStatus::kOk
                                                           : GameStatus::kWrongVersion;
    }
    return check;
}

DiscImageInfo InspectDiscImage(const std::filesystem::path& imagePath) {
    DiscImageInfo info;
    Image image(imagePath);
    if (!image.open()) {
        info.error = "unable to open the disc image";
        return info;
    }
    std::vector<DiscFile> files;
    if (!ListDisc(image, files, &info.error)) return info;
    for (const DiscFile& file : files) {
        if (file.directory) continue;
        ++info.files;
        info.bytes += file.size;
        if (Lower(file.relative.u8string()) == "default.xex" &&
            !HashRange(image, file.offset, file.size, info.xexSha256)) {
            info.error = "unable to read default.xex from the disc image";
            return info;
        }
    }
    info.ok = true;
    return info;
}

bool ExtractDiscImage(const std::filesystem::path& imagePath,
                      const std::filesystem::path& destination,
                      const std::function<bool(uint64_t, uint64_t)>& progress,
                      std::string* error) {
    const auto fail = [&](std::string text) {
        if (error) *error = std::move(text);
        return false;
    };
    Image image(imagePath);
    if (!image.open()) return fail("unable to open the disc image");
    std::vector<DiscFile> files;
    std::string listError;
    if (!ListDisc(image, files, &listError)) return fail(listError);
    uint64_t total = 0;
    for (const DiscFile& file : files) total += file.size;
    std::error_code fsError;
    std::filesystem::create_directories(destination, fsError);
    if (fsError) return fail("unable to create " + destination.u8string());
    std::vector<uint8_t> buffer(4u << 20);
    uint64_t done = 0;
    if (progress && !progress(0, total)) return fail("cancelled");
    for (const DiscFile& file : files) {
        const std::filesystem::path target = destination / file.relative;
        if (file.directory) {
            std::filesystem::create_directories(target, fsError);
            if (fsError) return fail("unable to create " + target.u8string());
            continue;
        }
        std::filesystem::create_directories(target.parent_path(), fsError);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) return fail("unable to write " + target.u8string());
        uint64_t offset = file.offset;
        uint64_t left = file.size;
        while (left) {
            const size_t chunk = size_t(std::min<uint64_t>(left, buffer.size()));
            if (!image.Read(offset, buffer.data(), chunk)) {
                return fail("unable to read " + file.relative.u8string() + " from the disc image");
            }
            out.write(reinterpret_cast<const char*>(buffer.data()), std::streamsize(chunk));
            if (!out) return fail("unable to write " + target.u8string() + " (disk full?)");
            offset += chunk;
            left -= chunk;
            done += chunk;
            if (progress && !progress(done, total)) return fail("cancelled");
        }
    }
    return true;
}

}  // namespace darkness::game_setup
