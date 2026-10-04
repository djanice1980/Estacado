// The player's own copy of the game: see runtime_game_setup.h.
#include "runtime_game_setup.h"

#include <windows.h>
#include <bcrypt.h>

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
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

std::string BytesSha256Impl(const uint8_t* data, size_t size) {
    Sha256 hash;
    return hash.Update(data, size) ? hash.Finish() : std::string();
}

// --- default.xex versions (0.9.1) -----------------------------------------

uint32_t Be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}
uint16_t Be16(const uint8_t* p) { return uint16_t(uint32_t(p[0]) << 8 | uint32_t(p[1])); }
uint32_t Le32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint16_t Le16(const uint8_t* p) { return uint16_t(uint32_t(p[0]) | uint32_t(p[1]) << 8); }

// The retail XEX2 key (public, also in XenonUtils/xex.h).
constexpr uint8_t kXexRetailKey[16] = {0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
                                       0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91};
constexpr uint32_t kXexHeaderFileFormat = 0x000003FF;
constexpr uint32_t kXexHeaderResources = 0x000002FF;
constexpr uint32_t kXexHeaderEntryPoint = 0x00010100;
constexpr uint32_t kMaxImageBytes = 256u << 20;

// AES-128-CBC with a zero IV over the whole 16-byte blocks of data.
bool AesCbcDecrypt(const uint8_t (&key)[16], std::vector<uint8_t>& data) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE handle = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) >= 0 &&
              BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
                                reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
                                ULONG(sizeof(BCRYPT_CHAIN_MODE_CBC)), 0) >= 0 &&
              BCryptGenerateSymmetricKey(algorithm, &handle, nullptr, 0,
                                         const_cast<PUCHAR>(key), 16, 0) >= 0;
    const size_t whole = data.size() & ~size_t(15);
    if (ok && whole) {
        std::vector<uint8_t> output(whole);
        uint8_t iv[16] = {};
        ULONG done = 0;
        ok = BCryptDecrypt(handle, data.data(), ULONG(whole), nullptr, iv, sizeof(iv),
                           output.data(), ULONG(whole), &done, 0) >= 0 &&
             done == whole;
        if (ok) std::memcpy(data.data(), output.data(), whole);
    }
    if (handle) BCryptDestroyKey(handle);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

struct DecodedXex {
    std::vector<uint8_t> image;
    // Title resources (address, size), e.g. the XDBF with its strings.
    std::vector<std::pair<uint32_t, uint32_t>> resources;
};

// Decrypts and unpacks a XEX2 (no or basic compression), checking every
// offset against the file: a damaged or foreign file is reported, not read
// out of bounds.
bool DecodeXex(const uint8_t* data, size_t size, XexIdentity& identity, DecodedXex& out) {
    const auto fail = [&](const char* text) {
        identity.error = text;
        return false;
    };
    if (size < 24 || std::memcmp(data, "XEX2", 4) != 0) {
        return fail("not an Xbox 360 executable (no XEX2 header)");
    }
    const uint32_t headerSize = Be32(data + 8);
    const uint32_t securityOffset = Be32(data + 16);
    const uint32_t headerCount = Be32(data + 20);
    if (headerSize > size || headerCount > 4096 || 24 + uint64_t(headerCount) * 8 > headerSize ||
        uint64_t(securityOffset) + 0x180 > headerSize) {
        return fail("the executable's header is damaged");
    }
    uint32_t formatOffset = 0;
    uint32_t resourceOffset = 0;
    for (uint32_t index = 0; index < headerCount; ++index) {
        const uint32_t key = Be32(data + 24 + index * 8);
        const uint32_t value = Be32(data + 28 + index * 8);
        if (key == kXexHeaderFileFormat) formatOffset = value;
        if (key == kXexHeaderResources) resourceOffset = value;
        if (key == kXexHeaderEntryPoint) identity.entryPoint = value;
    }
    const uint8_t* security = data + securityOffset;
    identity.imageSize = Be32(security + 4);
    identity.loadAddress = Be32(security + 0x110);
    if (identity.imageSize == 0 || identity.imageSize > kMaxImageBytes) {
        return fail("the executable's image size is damaged");
    }
    if (formatOffset == 0 || uint64_t(formatOffset) + 8 > headerSize) {
        return fail("the executable has no file format information");
    }
    const uint32_t infoSize = Be32(data + formatOffset);
    identity.encryption = Be16(data + formatOffset + 4);
    identity.compression = Be16(data + formatOffset + 6);
    if (infoSize < 8 || uint64_t(formatOffset) + infoSize > headerSize) {
        return fail("the executable's file format information is damaged");
    }
    if (resourceOffset && uint64_t(resourceOffset) + 4 <= headerSize) {
        const uint32_t resourceBytes = Be32(data + resourceOffset);
        if (resourceBytes >= 4 && uint64_t(resourceOffset) + resourceBytes <= headerSize) {
            for (uint32_t at = 4; at + 16 <= resourceBytes; at += 16) {
                out.resources.emplace_back(Be32(data + resourceOffset + at + 8),
                                           Be32(data + resourceOffset + at + 12));
            }
        }
    }
    std::vector<uint8_t> payload(data + headerSize, data + size);
    if (identity.encryption == 1) {
        uint8_t sessionKey[16];
        std::vector<uint8_t> key(security + 0x150, security + 0x160);
        if (!AesCbcDecrypt(kXexRetailKey, key)) return fail("unable to decrypt (AES unavailable)");
        std::memcpy(sessionKey, key.data(), sizeof(sessionKey));
        if (!AesCbcDecrypt(sessionKey, payload)) return fail("unable to decrypt (AES unavailable)");
    } else if (identity.encryption != 0) {
        return fail("the executable uses an unknown encryption");
    }
    out.image.assign(identity.imageSize, 0);
    if (identity.compression == 0) {
        std::memcpy(out.image.data(), payload.data(), std::min<size_t>(payload.size(), out.image.size()));
    } else if (identity.compression == 1) {
        size_t from = 0;
        size_t to = 0;
        for (uint32_t at = 8; at + 8 <= infoSize; at += 8) {
            const uint32_t dataBytes = Be32(data + formatOffset + at);
            const uint32_t zeroBytes = Be32(data + formatOffset + at + 4);
            if (dataBytes > payload.size() - from || dataBytes > out.image.size() - to) {
                return fail("the executable's data blocks are damaged");
            }
            std::memcpy(out.image.data() + to, payload.data() + from, dataBytes);
            from += dataBytes;
            to += dataBytes;
            if (zeroBytes > out.image.size() - to) {
                return fail("the executable's data blocks are damaged");
            }
            to += zeroBytes;
        }
    } else {
        // LZX ("normal") compression: not read by this check yet.
        return fail("the executable is packed with LZX compression, which this check does not "
                    "read yet");
    }
    return true;
}

// The identity hashes of a decoded image (see XexIdentity).
void MeasureImage(const DecodedXex& decoded, XexIdentity& identity) {
    const std::vector<uint8_t>& image = decoded.image;
    identity.imageSha256 = BytesSha256Impl(image.data(), image.size());
    // PE sections (little-endian headers at the image start).
    uint32_t rdataBegin = 0;
    uint32_t rdataEnd = 0;
    if (image.size() >= 0x40) {
        const uint32_t nt = Le32(image.data() + 0x3C);
        if (uint64_t(nt) + 24 <= image.size() && std::memcmp(image.data() + nt, "PE\0\0", 4) == 0) {
            const uint16_t count = Le16(image.data() + nt + 6);
            const uint16_t optional = Le16(image.data() + nt + 20);
            const uint64_t table = uint64_t(nt) + 24 + optional;
            for (uint16_t index = 0; index < count && table + (index + 1) * 40ull <= image.size();
                 ++index) {
                const uint8_t* header = image.data() + table + index * 40ull;
                XexIdentity::Section section;
                section.name.assign(reinterpret_cast<const char*>(header),
                                    strnlen(reinterpret_cast<const char*>(header), 8));
                section.size = Le32(header + 8);
                const uint32_t rva = Le32(header + 12);
                const uint32_t characteristics = Le32(header + 36);
                section.address = identity.loadAddress + rva;
                section.code = (characteristics & (0x00000020u | 0x20000000u)) != 0;
                const uint64_t end = std::min<uint64_t>(uint64_t(rva) + section.size, image.size());
                const uint64_t begin = std::min<uint64_t>(rva, end);
                section.sha256 = BytesSha256Impl(image.data() + begin, size_t(end - begin));
                if (section.name == ".rdata") {
                    rdataBegin = uint32_t(begin);
                    rdataEnd = uint32_t(end);
                }
                identity.sections.push_back(std::move(section));
            }
        }
    }
    Sha256 code;
    for (const XexIdentity::Section& section : identity.sections) {
        if (!section.code) continue;
        const uint64_t rva = section.address - identity.loadAddress;
        const uint64_t end = std::min<uint64_t>(rva + section.size, image.size());
        const uint64_t begin = std::min<uint64_t>(rva, end);
        code.Update(image.data() + begin, size_t(end - begin));
    }
    identity.codeSha256 = code.Finish();
    // Structure: the image without .rdata and the title resources.
    std::vector<uint8_t> structure = image;
    if (rdataEnd > rdataBegin) {
        std::fill(structure.begin() + rdataBegin, structure.begin() + rdataEnd, uint8_t(0));
    }
    for (const auto& [address, bytes] : decoded.resources) {
        if (address < identity.loadAddress) continue;
        const uint64_t begin = std::min<uint64_t>(address - identity.loadAddress, structure.size());
        const uint64_t end = std::min<uint64_t>(begin + bytes, structure.size());
        std::fill(structure.begin() + begin, structure.begin() + end, uint8_t(0));
    }
    identity.structureSha256 = BytesSha256Impl(structure.data(), structure.size());
    // .rdata words that point into the image, with their offsets.
    Sha256 pointers;
    for (uint32_t at = (rdataBegin + 3) & ~3u; at + 4 <= rdataEnd; at += 4) {
        const uint32_t word = Be32(image.data() + at);
        if (word >= identity.loadAddress && word - identity.loadAddress < image.size()) {
            const uint8_t entry[8] = {uint8_t(at >> 24), uint8_t(at >> 16), uint8_t(at >> 8),
                                      uint8_t(at),       image[at],         image[at + 1],
                                      image[at + 2],     image[at + 3]};
            pointers.Update(entry, sizeof(entry));
        }
    }
    identity.pointerSha256 = pointers.Finish();
    Sha256 read;
    for (const uint32_t address : kRuntimeReadDataAddresses) {
        if (address < identity.loadAddress) continue;
        const uint64_t begin = std::min<uint64_t>(address - identity.loadAddress, image.size());
        const uint64_t end = std::min<uint64_t>(begin + 64, image.size());
        read.Update(image.data() + begin, size_t(end - begin));
    }
    identity.protectedSha256 = read.Finish();
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

const char* const kSupportedStructureSha256 = "5e7d8bf08d820427fea8505f596b7ee081488cf8ee2be91872869778fc22ac0c";
const char* const kSupportedPointerSha256 = "ba10f21a80104cc171dc4c9d23c95d47b18ba0b67123721b9d7dc6c89b24e6bc";
const char* const kSupportedProtectedSha256 = "d1622b7ad7cbae8699192f5531d5648517e88bab9a95b4f383af09f6e851e75a";

XexIdentity IdentifyXex(const uint8_t* data, size_t size, bool decodeSupported) {
    XexIdentity identity;
    identity.fileSha256 = BytesSha256Impl(data, size);
    const bool supported = identity.fileSha256 == kSupportedXexSha256;
    if (supported && !decodeSupported) {
        identity.match = XexMatch::kSupported;
        return identity;
    }
    DecodedXex decoded;
    if (!DecodeXex(data, size, identity, decoded)) {
        identity.match = supported ? XexMatch::kSupported : XexMatch::kNotReadable;
        return identity;
    }
    identity.decoded = true;
    MeasureImage(decoded, identity);
    if (supported) {
        identity.match = XexMatch::kSupported;
    } else if (identity.loadAddress == kSupportedLoadAddress &&
               identity.imageSize == kSupportedImageSize &&
               identity.entryPoint == kSupportedEntryPoint &&
               identity.codeSha256 == kSupportedCodeSha256 &&
               identity.structureSha256 == kSupportedStructureSha256 &&
               identity.pointerSha256 == kSupportedPointerSha256 &&
               identity.protectedSha256 == kSupportedProtectedSha256) {
        identity.match = XexMatch::kSameCode;
    } else {
        identity.match = XexMatch::kDifferentCode;
    }
    return identity;
}

XexIdentity IdentifyXexFile(const std::filesystem::path& path, bool decodeSupported) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    XexIdentity identity;
    if (!stream) {
        identity.error = "unable to open " + path.u8string();
        return identity;
    }
    const std::streamoff size = stream.tellg();
    if (size <= 0 || size > std::streamoff(kMaxImageBytes)) {
        identity.error = "default.xex has an impossible size";
        return identity;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size));
    if (!stream) {
        identity.error = "unable to read " + path.u8string();
        return identity;
    }
    return IdentifyXex(bytes.data(), bytes.size(), decodeSupported);
}

std::string XexMatchText(const XexIdentity& identity) {
    switch (identity.match) {
        case XexMatch::kSupported:
            return "The supported version of The Darkness (Xbox 360, USA/Europe disc).";
        case XexMatch::kSameCode:
            return "A version of The Darkness with the same code as the supported one (for "
                   "example a localised release): it runs; its own texts and fonts are used.";
        case XexMatch::kDifferentCode:
            return "This version of The Darkness runs different code, which this release cannot "
                   "run yet.";
        case XexMatch::kNotReadable:
            break;
    }
    return "default.xex could not be read as an Xbox 360 executable" +
           (identity.error.empty() ? std::string(".") : ": " + identity.error + ".");
}

std::string XexIdentityReport(const XexIdentity& identity, const std::string& source) {
    const auto hex = [](uint32_t value) {
        char text[16];
        std::snprintf(text, sizeof(text), "0x%08X", value);
        return std::string(text);
    };
    const auto same = [](const std::string& value, const char* expected) {
        return value == expected ? "same as the supported version" : "DIFFERENT";
    };
    std::ostringstream out;
    out << "Estacado game version report (local; names, sizes and hashes only, no game data)\n"
        << "default.xex: " << source << "\n"
        << "file SHA-256: " << identity.fileSha256 << "\n"
        << "result: " << XexMatchText(identity) << "\n";
    if (!identity.error.empty()) out << "error: " << identity.error << "\n";
    if (!identity.decoded) return out.str();
    out << "encryption: " << identity.encryption << ", compression: " << identity.compression
        << "\nload address: " << hex(identity.loadAddress)
        << (identity.loadAddress == kSupportedLoadAddress ? "" : " (DIFFERENT)")
        << "\nimage size: " << hex(identity.imageSize)
        << (identity.imageSize == kSupportedImageSize ? "" : " (DIFFERENT)")
        << "\nentry point: " << hex(identity.entryPoint)
        << (identity.entryPoint == kSupportedEntryPoint ? "" : " (DIFFERENT)")
        << "\ncode sections: " << same(identity.codeSha256, kSupportedCodeSha256)
        << "\nstructure (all but .rdata and the title resource): "
        << same(identity.structureSha256, kSupportedStructureSha256)
        << "\npointers in .rdata: " << same(identity.pointerSha256, kSupportedPointerSha256)
        << "\n.rdata the runtime reads: "
        << same(identity.protectedSha256, kSupportedProtectedSha256)
        << "\nimage SHA-256: " << identity.imageSha256 << "\nsections:\n";
    for (const XexIdentity::Section& section : identity.sections) {
        out << "  " << section.name << " " << hex(section.address) << " size " << hex(section.size)
            << (section.code ? " code" : " data") << " sha256 " << section.sha256 << "\n";
    }
    return out.str();
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
    std::error_code ignore;
    std::filesystem::create_directories(executableDirectory, ignore);
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
    return ConfiguredGameXex(executableDirectory, executableDirectory);
}

std::filesystem::path ConfiguredGameXex(const std::filesystem::path& dataFolder,
                                        const std::filesystem::path& executableDirectory) {
    std::error_code error;
    for (const std::filesystem::path& folder : {dataFolder, executableDirectory}) {
        if (folder.empty()) continue;
        if (const auto located = ReadGameLocation(folder)) {
            const std::filesystem::path xex = *located / L"default.xex";
            if (std::filesystem::is_regular_file(xex, error)) return xex;
        }
    }
    for (const std::filesystem::path& folder : {dataFolder, executableDirectory}) {
        if (folder.empty()) continue;
        const std::filesystem::path extracted = folder / kExtractedFolderName / L"default.xex";
        if (std::filesystem::is_regular_file(extracted, error)) return extracted;
    }
    return {};
}

std::filesystem::path FindGameXex(const std::filesystem::path& executableDirectory) {
    return FindGameXex(executableDirectory, executableDirectory);
}

std::filesystem::path FindGameXex(const std::filesystem::path& dataFolder,
                                  const std::filesystem::path& executableDirectory) {
    if (std::filesystem::path configured = ConfiguredGameXex(dataFolder, executableDirectory);
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
    check.identity = IdentifyXexFile(xex);
    check.sha256 = check.identity.fileSha256;
    if (check.sha256.empty()) {
        check.status = GameStatus::kUnreadable;
    } else {
        check.status = check.identity.match == XexMatch::kSupported ||
                               check.identity.match == XexMatch::kSameCode
                           ? GameStatus::kOk
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
        if (Lower(file.relative.u8string()) == "default.xex") {
            std::vector<uint8_t> bytes;
            if (file.size == 0 || file.size > kMaxImageBytes) {
                info.error = "default.xex in the disc image has an impossible size";
                return info;
            }
            bytes.resize(size_t(file.size));
            if (!image.Read(file.offset, bytes.data(), bytes.size())) {
                info.error = "unable to read default.xex from the disc image";
                return info;
            }
            info.xex = IdentifyXex(bytes.data(), bytes.size());
            info.xexSha256 = info.xex.fileSha256;
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
