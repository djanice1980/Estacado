#include "runtime_language_install.h"

#include "runtime_game_setup.h"

#include <windows.h>
#include <winhttp.h>

#include <toml++/toml.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string_view>

namespace darkness::language_install {
namespace {

uint16_t Get16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t Get32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t Get64(const uint8_t* p) { return uint64_t(Get32(p)) | (uint64_t(Get32(p + 4)) << 32); }

std::string Hex(const uint8_t* bytes, size_t size) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < size; ++i) {
        out.push_back(kHex[bytes[i] >> 4]);
        out.push_back(kHex[bytes[i] & 15]);
    }
    return out;
}

bool ReadFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& bytes) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) return false;
    const std::streamoff size = stream.tellg();
    if (size < 0) return false;
    bytes.resize(size_t(size));
    stream.seekg(0);
    return stream.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size())) ||
           bytes.empty();
}

bool WriteFileBytes(const std::filesystem::path& path, const uint8_t* data, size_t size) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    stream.write(reinterpret_cast<const char*>(data), std::streamsize(size));
    return bool(stream);
}

// Bytes of one stored entry.
bool ReadEntry(const std::filesystem::path& zip, const ZipEntry& entry, std::vector<uint8_t>& out) {
    std::ifstream stream(zip, std::ios::binary);
    if (!stream) return false;
    out.resize(size_t(entry.size));
    stream.seekg(std::streamoff(entry.dataOffset));
    return stream.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size())) ||
           out.empty();
}

InstallResult Fail(std::string message, std::string detail) {
    InstallResult result;
    result.message = std::move(message);
    result.detail = std::move(detail);
    return result;
}

}  // namespace

bool ApplyPatch(const std::vector<uint8_t>& source, const std::vector<uint8_t>& patch,
                std::vector<uint8_t>& target, std::string& error) {
    constexpr size_t kHeader = 8 + 8 + 32 + 8 + 32 + 4;
    if (patch.size() < kHeader || std::memcmp(patch.data(), "DKPATCH1", 8) != 0) {
        error = "not a DKPATCH1 patch";
        return false;
    }
    const uint8_t* p = patch.data() + 8;
    const uint64_t sourceSize = Get64(p);
    const std::string sourceHash = Hex(p + 8, 32);
    const uint64_t targetSize = Get64(p + 40);
    const std::string targetHash = Hex(p + 48, 32);
    const uint32_t count = Get32(p + 80);
    size_t pos = kHeader;
    if (source.size() != sourceSize ||
        game_setup::BytesSha256(source.data(), source.size()) != sourceHash) {
        error = "the original file is not the expected version";
        return false;
    }
    if (patch.size() < pos + size_t(count) * 20 + 8) {
        error = "truncated patch";
        return false;
    }
    const size_t opsAt = pos;
    pos += size_t(count) * 20;
    const uint64_t literalSize = Get64(patch.data() + pos);
    pos += 8;
    if (patch.size() - pos < literalSize) {
        error = "truncated patch";
        return false;
    }
    const uint8_t* literals = patch.data() + pos;
    target.clear();
    target.reserve(size_t(targetSize));
    for (uint32_t k = 0; k < count; ++k) {
        const uint8_t* op = patch.data() + opsAt + size_t(k) * 20;
        const uint8_t kind = op[0];
        const uint64_t offset = Get64(op + 4);
        const uint64_t length = Get64(op + 12);
        const uint8_t* from = kind == 0 ? source.data() : literals;
        const uint64_t limit = kind == 0 ? source.size() : literalSize;
        if (kind > 1 || offset > limit || length > limit - offset ||
            target.size() + length > targetSize) {
            error = "invalid patch operation";
            return false;
        }
        target.insert(target.end(), from + offset, from + offset + length);
    }
    if (target.size() != targetSize ||
        game_setup::BytesSha256(target.data(), target.size()) != targetHash) {
        error = "the patched file does not match";
        return false;
    }
    return true;
}

bool IsPackPath(const std::string& name) {
    if (name.empty() || name.size() > 200 || name.front() == '/' || name.back() == '/') {
        return false;
    }
    size_t start = 0;
    while (start <= name.size()) {
        size_t end = name.find('/', start);
        if (end == std::string::npos) end = name.size();
        const std::string_view part(name.data() + start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        for (char c : part) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
            if (!ok) return false;
        }
        start = end + 1;
    }
    return true;
}

bool ReadStoredZip(const std::filesystem::path& zip, std::vector<ZipEntry>& entries,
                   std::string& error) {
    entries.clear();
    std::ifstream stream(zip, std::ios::binary | std::ios::ate);
    if (!stream) {
        error = "unreadable file";
        return false;
    }
    const uint64_t fileSize = uint64_t(stream.tellg());
    // End of central directory: in the last 64 KB + 22 bytes.
    const uint64_t tail = std::min<uint64_t>(fileSize, 65557);
    std::vector<uint8_t> end(static_cast<size_t>(tail));
    stream.seekg(std::streamoff(fileSize - tail));
    if (!stream.read(reinterpret_cast<char*>(end.data()), std::streamsize(end.size()))) {
        error = "unreadable file";
        return false;
    }
    size_t eocd = std::string::npos;
    for (size_t i = end.size() >= 22 ? end.size() - 22 + 1 : 0; i-- > 0;) {
        if (Get32(end.data() + i) == 0x06054B50u) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos) {
        error = "not a zip file";
        return false;
    }
    const uint16_t count = Get16(end.data() + eocd + 10);
    const uint32_t directorySize = Get32(end.data() + eocd + 12);
    const uint32_t directoryOffset = Get32(end.data() + eocd + 16);
    if (uint64_t(directoryOffset) + directorySize > fileSize) {
        error = "damaged zip file";
        return false;
    }
    std::vector<uint8_t> directory(directorySize);
    stream.seekg(directoryOffset);
    if (!stream.read(reinterpret_cast<char*>(directory.data()), std::streamsize(directory.size()))) {
        error = "damaged zip file";
        return false;
    }
    size_t pos = 0;
    for (uint16_t n = 0; n < count; ++n) {
        if (pos + 46 > directory.size() || Get32(directory.data() + pos) != 0x02014B50u) {
            error = "damaged zip file";
            return false;
        }
        const uint8_t* h = directory.data() + pos;
        const uint16_t method = Get16(h + 10);
        const uint32_t packed = Get32(h + 20);
        const uint32_t size = Get32(h + 24);
        const uint16_t nameLength = Get16(h + 28);
        const uint16_t extraLength = Get16(h + 30);
        const uint16_t commentLength = Get16(h + 32);
        const uint32_t localOffset = Get32(h + 42);
        if (pos + 46 + nameLength > directory.size()) {
            error = "damaged zip file";
            return false;
        }
        ZipEntry entry;
        entry.name.assign(reinterpret_cast<const char*>(h + 46), nameLength);
        pos += 46 + size_t(nameLength) + extraLength + commentLength;
        if (!entry.name.empty() && entry.name.back() == '/') continue;  // a folder
        if (method != 0 || packed != size) {
            error = "compressed zip entries are not supported: " + entry.name;
            return false;
        }
        uint8_t local[30];
        stream.seekg(localOffset);
        if (!stream.read(reinterpret_cast<char*>(local), sizeof(local)) ||
            Get32(local) != 0x04034B50u) {
            error = "damaged zip file";
            return false;
        }
        entry.dataOffset = uint64_t(localOffset) + 30 + Get16(local + 26) + Get16(local + 28);
        entry.size = size;
        if (entry.dataOffset + entry.size > fileSize) {
            error = "damaged zip file";
            return false;
        }
        entries.push_back(std::move(entry));
    }
    return true;
}

InstallResult InstallPack(const std::filesystem::path& archive,
                          const std::filesystem::path& gameFolder,
                          const std::filesystem::path& launcherFolder,
                          const std::function<bool(uint64_t, uint64_t)>& progress) {
    std::vector<ZipEntry> entries;
    std::string error;
    if (!ReadStoredZip(archive, entries, error)) {
        return Fail("The file is not a language pack.", error);
    }
    const auto find = [&](const std::string& name) -> const ZipEntry* {
        for (const ZipEntry& entry : entries) {
            if (entry.name == name) return &entry;
        }
        return nullptr;
    };
    const ZipEntry* assetEntry = find("asset.toml");
    const ZipEntry* packEntry = find("pack.toml");
    std::vector<uint8_t> bytes;
    if (!assetEntry || !packEntry) {
        return Fail("The file is not a language pack.", "asset.toml or pack.toml missing");
    }
    toml::table asset, pack;
    try {
        if (!ReadEntry(archive, *assetEntry, bytes)) throw std::runtime_error("unreadable");
        asset = toml::parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        if (!ReadEntry(archive, *packEntry, bytes)) throw std::runtime_error("unreadable");
        pack = toml::parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    } catch (const std::exception& exception) {
        return Fail("The file is not a language pack.", exception.what());
    }
    const std::string language = pack["language"].value_or(std::string{});
    bool languageOk = language.size() >= 2 && language.size() <= 32;
    for (char c : language) languageOk = languageOk && c >= 'a' && c <= 'z';
    if (asset["format"].value_or(int64_t(0)) != 1 || !languageOk) {
        return Fail("The file is not a language pack.", "unsupported asset.toml or language");
    }
    struct FileItem { const ZipEntry* entry; std::string path, sha256; };
    struct PatchItem { const ZipEntry* entry; std::string sha256, source, target; };
    std::vector<FileItem> files;
    std::vector<PatchItem> patches;
    uint64_t total = 0;
    if (const toml::array* list = asset["file"].as_array()) {
        for (const toml::node& node : *list) {
            const toml::table* item = node.as_table();
            if (!item) continue;
            FileItem file{nullptr, (*item)["path"].value_or(std::string{}),
                          (*item)["sha256"].value_or(std::string{})};
            file.entry = find(file.path);
            if (!IsPackPath(file.path) || !file.entry || file.sha256.size() != 64) {
                return Fail("The language pack is damaged.", "bad file entry: " + file.path);
            }
            total += file.entry->size;
            files.push_back(std::move(file));
        }
    }
    if (const toml::array* list = asset["patch"].as_array()) {
        for (const toml::node& node : *list) {
            const toml::table* item = node.as_table();
            if (!item) continue;
            const std::string path = (*item)["path"].value_or(std::string{});
            PatchItem patch{find(path), (*item)["sha256"].value_or(std::string{}),
                            (*item)["source"].value_or(std::string{}),
                            (*item)["target"].value_or(std::string{})};
            if (!IsPackPath(path) || !patch.entry || patch.sha256.size() != 64 ||
                !IsPackPath(patch.source) || !IsPackPath(patch.target)) {
                return Fail("The language pack is damaged.", "bad patch entry: " + path);
            }
            total += patch.entry->size;
            patches.push_back(std::move(patch));
        }
    }
    if (files.empty()) {
        return Fail("The language pack is damaged.", "no files");
    }
    const std::filesystem::path packs = launcherFolder / L"language_packs";
    const std::filesystem::path staging = packs / (language + ".partial");
    const std::filesystem::path installed = packs / language;
    std::error_code fsError;
    std::filesystem::remove_all(staging, fsError);
    std::filesystem::create_directories(staging, fsError);
    uint64_t done = 0;
    const auto step = [&](uint64_t bytesDone) {
        done += bytesDone;
        return !progress || progress(done, total);
    };
    const auto abandon = [&](InstallResult result) {
        std::error_code ignore;
        std::filesystem::remove_all(staging, ignore);
        return result;
    };
    for (const FileItem& file : files) {
        if (!ReadEntry(archive, *file.entry, bytes)) {
            return abandon(Fail("The language pack is damaged.", "unreadable " + file.path));
        }
        if (game_setup::BytesSha256(bytes.data(), bytes.size()) != file.sha256) {
            return abandon(Fail("The language pack is damaged.", "hash mismatch " + file.path));
        }
        if (!WriteFileBytes(staging / std::filesystem::u8path(file.path), bytes.data(), bytes.size())) {
            return abandon(Fail("Unable to write the language pack.", file.path));
        }
        if (!step(file.entry->size)) {
            return abandon(Fail("Installation cancelled.", {}));
        }
    }
    for (const PatchItem& patch : patches) {
        std::vector<uint8_t> source, target;
        if (!ReadEntry(archive, *patch.entry, bytes) ||
            game_setup::BytesSha256(bytes.data(), bytes.size()) != patch.sha256) {
            return abandon(Fail("The language pack is damaged.", "patch " + patch.target));
        }
        if (!ReadFileBytes(gameFolder / std::filesystem::u8path(patch.source), source)) {
            return abandon(Fail("Your game files do not match this language pack.",
                                "missing " + patch.source));
        }
        if (!ApplyPatch(source, bytes, target, error)) {
            return abandon(Fail("Your game files do not match this language pack.",
                                patch.source + ": " + error));
        }
        if (!WriteFileBytes(staging / std::filesystem::u8path(patch.target), target.data(),
                            target.size())) {
            return abandon(Fail("Unable to write the language pack.", patch.target));
        }
        if (!step(patch.entry->size)) {
            return abandon(Fail("Installation cancelled.", {}));
        }
    }
    // The complete pack replaces an older one.
    const std::filesystem::path previous = packs / (language + ".previous");
    std::filesystem::remove_all(previous, fsError);
    fsError.clear();
    if (std::filesystem::exists(installed, fsError)) {
        std::filesystem::rename(installed, previous, fsError);
        if (fsError) {
            return abandon(Fail("Unable to write the language pack.",
                                "the installed pack is in use: " + fsError.message()));
        }
    }
    std::filesystem::rename(staging, installed, fsError);
    if (fsError) {
        std::error_code ignore;
        std::filesystem::rename(previous, installed, ignore);
        return abandon(Fail("Unable to write the language pack.", fsError.message()));
    }
    std::filesystem::remove_all(previous, fsError);
    InstallResult result;
    result.ok = true;
    result.language = language;
    result.message = "Language pack installed.";
    return result;
}

bool DownloadFile(const std::wstring& url, const std::filesystem::path& target,
                  const std::string& expectedSha256,
                  const std::function<bool(uint64_t, uint64_t)>& progress, std::string& error) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256]{}, path[2048]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = DWORD(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = DWORD(std::size(path));
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
        error = "not an https URL";
        return false;
    }
    struct Handle {
        HINTERNET value = nullptr;
        ~Handle() { if (value) WinHttpCloseHandle(value); }
    } session, connection, request;
    session.value = WinHttpOpen(L"TheDarknessSettings/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.value) {
        session.value = WinHttpOpen(L"TheDarknessSettings/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (session.value) connection.value = WinHttpConnect(session.value, host, parts.nPort, 0);
    if (connection.value) {
        request.value = WinHttpOpenRequest(connection.value, L"GET", path, nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE);
    }
    if (!request.value ||
        !WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.value, nullptr)) {
        error = "connection failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    DWORD status = 0, statusSize = sizeof(status);
    WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        error = "server answered " + std::to_string(status);
        return false;
    }
    DWORD length = 0, lengthSize = sizeof(length);
    WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &length, &lengthSize, WINHTTP_NO_HEADER_INDEX);
    std::error_code fsError;
    std::filesystem::create_directories(target.parent_path(), fsError);
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "unable to write " + target.u8string();
        return false;
    }
    std::vector<char> buffer(1u << 16);
    uint64_t received = 0;
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(request.value, buffer.data(), DWORD(buffer.size()), &got)) {
            error = "download interrupted (" + std::to_string(GetLastError()) + ")";
            return false;
        }
        if (!got) break;
        out.write(buffer.data(), got);
        received += got;
        if (progress && !progress(received, length)) {
            error = "cancelled";
            return false;
        }
    }
    out.close();
    if (!out) {
        error = "unable to write " + target.u8string();
        return false;
    }
    if (!expectedSha256.empty() && game_setup::FileSha256(target) != expectedSha256) {
        error = "the download does not match the expected file";
        return false;
    }
    return true;
}

}  // namespace darkness::language_install
