#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <filesystem>
#include <string>
#include <stdexcept>

// Owned bytes plus their original resource origin. Missing is distinct from
// empty. A later replacement of the source cannot change this value.
class RuntimePcConfigSnapshot {
public:
    static RuntimePcConfigSnapshot Capture(const std::filesystem::path& path) {
        RuntimePcConfigSnapshot result;
        if (path.empty()) return result;
        result.origin_ = std::filesystem::absolute(path).lexically_normal();
        const HANDLE file = CreateFileW(result.origin_.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return result;
            throw std::runtime_error("unable to capture startup configuration error=" + std::to_string(error));
        }
        struct Close { HANDLE handle; ~Close() { CloseHandle(handle); } } close{file};
        result.present_ = true;
        char buffer[4096];
        for (;;) {
            DWORD read{};
            if (!ReadFile(file, buffer, sizeof(buffer), &read, nullptr))
                throw std::runtime_error("unable to read startup configuration");
            if (!read) break;
            result.contents_.append(buffer, read);
        }
        return result;
    }
    // The same origin with derived bytes (title requirements added for the
    // graphics plugin); only for a present snapshot.
    RuntimePcConfigSnapshot WithContents(std::string contents) const {
        if (!present_) throw std::runtime_error("derived startup configuration needs a source");
        RuntimePcConfigSnapshot result = *this;
        result.contents_ = std::move(contents);
        return result;
    }
    bool present() const noexcept { return present_; }
    const std::filesystem::path& origin() const noexcept { return origin_; }
    const std::string& contents() const noexcept { return contents_; }
private:
    std::filesystem::path origin_;
    std::string contents_;
    bool present_{};
};
