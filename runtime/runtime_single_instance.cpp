#include "runtime_single_instance.h"

#include <sstream>
#include <stdexcept>
#include <cstdint>

std::wstring RuntimeConfigLockName(const std::filesystem::path& path) {
    if (path.empty()) throw std::runtime_error("configuration lock path is empty");
    auto key = std::filesystem::weakly_canonical(std::filesystem::absolute(path)).wstring();
    // Windows invariant casing, not a user-locale-dependent mapping.
    const int count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE,
        key.data(), static_cast<int>(key.size()), nullptr, 0, nullptr, nullptr, 0);
    if (!count) throw std::runtime_error("unable to normalize configuration lock path");
    std::wstring normalized(count, L'\0');
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE,
        key.data(), static_cast<int>(key.size()), normalized.data(), count, nullptr, nullptr, 0))
        throw std::runtime_error("unable to normalize configuration lock path");
    uint64_t hash = 14695981039346656037ull;
    for (const wchar_t value : normalized) {
        hash = (hash ^ (value & 255)) * 1099511628211ull;
        hash = (hash ^ (value >> 8)) * 1099511628211ull;
    }
    return L"Local\\TheDarknessRecompiled-Config-" + std::to_wstring(hash);
}

RuntimeSingleInstance::RuntimeSingleInstance(const std::wstring& name) {
    if (name.empty()) {
        throw std::runtime_error("runtime instance lock name is empty");
    }
    SetLastError(ERROR_SUCCESS);
    HANDLE handle = CreateMutexW(nullptr, TRUE, name.c_str());
    const DWORD error = GetLastError();
    if (!handle) {
        std::ostringstream message;
        message << "unable to create runtime instance lock error=" << error;
        throw std::runtime_error(message.str());
    }
    if (error == ERROR_ALREADY_EXISTS) {
        alreadyRunning_ = true;
        CloseHandle(handle);
        return;
    }
    handle_ = handle;
}

RuntimeSingleInstance::~RuntimeSingleInstance() {
    Release();
}

void RuntimeSingleInstance::Release() noexcept {
    if (handle_) {
        ReleaseMutex(handle_);
        CloseHandle(handle_);
        handle_ = nullptr;
    }
}

bool RuntimeSingleInstance::Exists(const std::wstring& name) {
    if (name.empty()) throw std::runtime_error("runtime instance lock name is empty");
    HANDLE handle = OpenMutexW(SYNCHRONIZE, FALSE, name.c_str());
    if (handle) {
        CloseHandle(handle);
        return true;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND) return false;
    // Another process may own a lock we cannot open. Do not launch into it.
    if (error == ERROR_ACCESS_DENIED) return true;
    throw std::runtime_error("unable to check running game state");
}
