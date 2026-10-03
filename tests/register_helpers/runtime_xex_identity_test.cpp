// 0.9.1 versions of default.xex (runtime_game_setup.h): a synthetic XEX2 (no
// game data) built here, encrypted like a retail one, is decoded and its
// identity hashes react to code, pointer and protected-data changes but not
// to text changes; damaged files are reported, never read out of bounds; and
// every .rdata address the runtime reads is in the protected list.
#include "runtime_game_setup.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <string>
#include <vector>

namespace setup = darkness::game_setup;

namespace {

bool passed = true;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        passed = false;
    }
}

void PutBe32(std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
    bytes[at] = uint8_t(value >> 24);
    bytes[at + 1] = uint8_t(value >> 16);
    bytes[at + 2] = uint8_t(value >> 8);
    bytes[at + 3] = uint8_t(value);
}
void PutBe16(std::vector<uint8_t>& bytes, size_t at, uint16_t value) {
    bytes[at] = uint8_t(value >> 8);
    bytes[at + 1] = uint8_t(value);
}
void PutLe32(std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
    for (int index = 0; index < 4; ++index) bytes[at + index] = uint8_t(value >> (8 * index));
}
void PutLe16(std::vector<uint8_t>& bytes, size_t at, uint16_t value) {
    bytes[at] = uint8_t(value);
    bytes[at + 1] = uint8_t(value >> 8);
}

bool AesCbcEncrypt(const uint8_t* key, std::vector<uint8_t>& data) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE handle = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) >= 0 &&
              BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
                                reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
                                ULONG(sizeof(BCRYPT_CHAIN_MODE_CBC)), 0) >= 0 &&
              BCryptGenerateSymmetricKey(algorithm, &handle, nullptr, 0,
                                         const_cast<PUCHAR>(key), 16, 0) >= 0;
    if (ok) {
        std::vector<uint8_t> output(data.size());
        uint8_t iv[16] = {};
        ULONG done = 0;
        ok = BCryptEncrypt(handle, data.data(), ULONG(data.size()), nullptr, iv, sizeof(iv),
                           output.data(), ULONG(output.size()), &done, 0) >= 0;
        data = output;
    }
    if (handle) BCryptDestroyKey(handle);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

// A 64 KiB image at 0x82000000: PE headers, .rdata at 0x1000 (data), .text
// at 0x4000 (code).
constexpr uint32_t kLoad = 0x82000000;
constexpr uint32_t kImageBytes = 0x10000;
constexpr uint32_t kRdata = 0x1000;
constexpr uint32_t kText = 0x4000;

std::vector<uint8_t> MakeImage() {
    std::vector<uint8_t> image(kImageBytes, 0);
    image[0] = 'M';
    image[1] = 'Z';
    PutLe32(image, 0x3C, 0x80);
    std::memcpy(&image[0x80], "PE\0\0", 4);
    PutLe16(image, 0x80 + 6, 2);     // sections
    PutLe16(image, 0x80 + 20, 0xE0); // optional header size
    const size_t table = 0x80 + 24 + 0xE0;
    const auto section = [&](size_t index, const char* name, uint32_t rva, uint32_t size,
                             uint32_t characteristics) {
        const size_t at = table + index * 40;
        std::memcpy(&image[at], name, std::strlen(name));
        PutLe32(image, at + 8, size);
        PutLe32(image, at + 12, rva);
        PutLe32(image, at + 16, size);
        PutLe32(image, at + 36, characteristics);
    };
    section(0, ".rdata", kRdata, 0x2000, 0x40000040);
    section(1, ".text", kText, 0x3000, 0x60000020);
    // .rdata: a string, a vtable (pointers into .text), a float.
    std::memcpy(&image[kRdata + 0x10], "Hello, world", 12);
    PutBe32(image, kRdata + 0x40, kLoad + kText + 0x100);
    PutBe32(image, kRdata + 0x44, kLoad + kText + 0x200);
    PutBe32(image, kRdata + 0x80, 0x3FAAAAAB);
    // .text: some instructions.
    for (uint32_t at = 0; at < 0x3000; at += 4) PutBe32(image, kText + at, 0x60000000 + at);
    return image;
}

// A XEX2 around the image: basic compression in two blocks, optionally
// encrypted with a session key under the retail key.
std::vector<uint8_t> MakeXex(const std::vector<uint8_t>& image, bool encrypted) {
    constexpr uint32_t kHeaderBytes = 0x1000;
    constexpr uint32_t kSecurity = 0x100;
    constexpr uint32_t kFormat = 0x400;
    std::vector<uint8_t> xex(kHeaderBytes, 0);
    std::memcpy(xex.data(), "XEX2", 4);
    PutBe32(xex, 8, kHeaderBytes);
    PutBe32(xex, 16, kSecurity);
    PutBe32(xex, 20, 2);
    PutBe32(xex, 24, 0x000003FF);
    PutBe32(xex, 28, kFormat);
    PutBe32(xex, 32, 0x00010100);
    PutBe32(xex, 36, kLoad + kText);
    PutBe32(xex, kSecurity + 4, kImageBytes);
    PutBe32(xex, kSecurity + 0x110, kLoad);
    // Two blocks: the first 0x8000 bytes, then the rest; no zero runs.
    PutBe32(xex, kFormat, 8 + 2 * 8);
    PutBe16(xex, kFormat + 4, encrypted ? 1 : 0);
    PutBe16(xex, kFormat + 6, 1);
    PutBe32(xex, kFormat + 8, 0x8000);
    PutBe32(xex, kFormat + 12, 0);
    PutBe32(xex, kFormat + 16, kImageBytes - 0x8000);
    PutBe32(xex, kFormat + 20, 0);
    std::vector<uint8_t> payload = image;
    if (encrypted) {
        static const uint8_t kRetail[16] = {0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
                                            0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91};
        const uint8_t session[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        std::vector<uint8_t> wrapped(session, session + 16);
        AesCbcEncrypt(kRetail, wrapped);
        std::memcpy(&xex[kSecurity + 0x150], wrapped.data(), 16);
        AesCbcEncrypt(session, payload);
    }
    xex.insert(xex.end(), payload.begin(), payload.end());
    return xex;
}

setup::XexIdentity Identify(const std::vector<uint8_t>& xex) {
    return setup::IdentifyXex(xex.data(), xex.size(), true);
}

}  // namespace

int main() {
    const std::vector<uint8_t> image = MakeImage();
    const setup::XexIdentity plain = Identify(MakeXex(image, false));
    const setup::XexIdentity base = Identify(MakeXex(image, true));
    Check(plain.decoded && base.decoded && base.error.empty(), "the synthetic XEX decodes");
    Check(base.imageSha256 == plain.imageSha256, "decryption restores the image");
    Check(base.loadAddress == kLoad && base.imageSize == kImageBytes &&
              base.entryPoint == kLoad + kText,
          "load address, size and entry point");
    Check(base.sections.size() == 2 && base.sections[0].name == ".rdata" &&
              !base.sections[0].code && base.sections[1].name == ".text" && base.sections[1].code,
          "sections and their kinds");
    Check(base.match == setup::XexMatch::kDifferentCode, "not the game: different code");

    {
        // A translation: other text in .rdata.
        std::vector<uint8_t> text = image;
        std::memcpy(&text[kRdata + 0x10], "Privet, mir!", 12);
        const setup::XexIdentity other = Identify(MakeXex(text, true));
        Check(other.imageSha256 != base.imageSha256, "the translated image differs");
        Check(other.codeSha256 == base.codeSha256 && other.structureSha256 == base.structureSha256 &&
                  other.pointerSha256 == base.pointerSha256 &&
                  other.protectedSha256 == base.protectedSha256,
              "a text change keeps every identity hash");
    }
    {
        std::vector<uint8_t> pointer = image;
        PutBe32(pointer, kRdata + 0x44, kLoad + kText + 0x300);
        const setup::XexIdentity other = Identify(MakeXex(pointer, true));
        Check(other.pointerSha256 != base.pointerSha256 && other.codeSha256 == base.codeSha256,
              "a changed vtable entry changes the pointer hash");
    }
    {
        std::vector<uint8_t> code = image;
        code[kText + 0x123] ^= 0xFF;
        const setup::XexIdentity other = Identify(MakeXex(code, true));
        Check(other.codeSha256 != base.codeSha256, "a changed instruction changes the code hash");
    }
    {
        std::vector<uint8_t> structure = image;
        structure[0x30] = 0x77;  // the headers, outside .rdata
        const setup::XexIdentity other = Identify(MakeXex(structure, true));
        Check(other.structureSha256 != base.structureSha256 && other.codeSha256 == base.codeSha256,
              "a changed header changes the structure hash");
    }
    {
        // Damaged files: reported, not read out of bounds.
        const std::vector<uint8_t> good = MakeXex(image, true);
        std::vector<uint8_t> truncated(good.begin(), good.begin() + 0x2000);
        Check(Identify(truncated).match == setup::XexMatch::kNotReadable &&
                  !Identify(truncated).error.empty(),
              "a truncated file is not readable");
        std::vector<uint8_t> blocks = good;
        PutBe32(blocks, 0x400 + 8, 0x7FFFFFFF);
        Check(Identify(blocks).match == setup::XexMatch::kNotReadable, "an oversized block");
        std::vector<uint8_t> security = good;
        PutBe32(security, 16, 0xFFFFFF00);
        Check(Identify(security).match == setup::XexMatch::kNotReadable, "a security offset outside");
        std::vector<uint8_t> size = good;
        PutBe32(size, 0x100 + 4, 0xFFFFFFFF);
        Check(Identify(size).match == setup::XexMatch::kNotReadable, "an impossible image size");
        const std::vector<uint8_t> tiny = {'X', 'E', 'X', '2'};
        Check(Identify(tiny).match == setup::XexMatch::kNotReadable, "four bytes");
        const std::string text = "not an executable at all";
        Check(setup::IdentifyXex(reinterpret_cast<const uint8_t*>(text.data()), text.size()).match ==
                  setup::XexMatch::kNotReadable,
              "a text file");
        const std::string report = setup::XexIdentityReport(base, "synthetic");
        Check(report.find("code sections: DIFFERENT") != std::string::npos &&
                  report.find(".text 0x82004000") != std::string::npos,
              "the report names checks and sections");
    }
    {
        // Every .rdata address literal in the runtime is protected.
        const std::regex literal("0x(820[0-9A-Fa-f]{5})\\b");
        size_t scanned = 0;
        for (const auto& entry : std::filesystem::directory_iterator(
                 std::filesystem::path(DARKNESS_SOURCE_ROOT) / "runtime")) {
            const std::string extension = entry.path().extension().string();
            if (extension != ".cpp" && extension != ".h") continue;
            const std::string name = entry.path().filename().string();
            std::ifstream file(entry.path(), std::ios::binary);
            const std::string source((std::istreambuf_iterator<char>(file)), {});
            ++scanned;
            for (std::sregex_iterator match(source.begin(), source.end(), literal), end;
                 match != end; ++match) {
                const uint32_t address = uint32_t(std::stoul((*match)[1].str(), nullptr, 16));
                if (address < 0x82000600 || address >= 0x8209F754) continue;  // .rdata only
                bool listed = false;
                for (const uint32_t protectedAddress : setup::kRuntimeReadDataAddresses) {
                    listed = listed || protectedAddress == address;
                }
                if (!listed) {
                    std::cerr << name << ": 0x" << std::hex << address << std::dec
                              << " is not in kRuntimeReadDataAddresses\n";
                }
                Check(listed, "runtime .rdata addresses are protected");
            }
        }
        Check(scanned > 40, "the runtime sources were scanned");
    }
    if (!passed) return 1;
    std::cout << "runtime xex identity: PASS\n";
    return 0;
}
