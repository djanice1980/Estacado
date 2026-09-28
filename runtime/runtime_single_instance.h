#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <string>
#include <filesystem>

// Case-insensitive canonical path identity. A hash collision only rejects an
// unrelated operation; it cannot grant concurrent ownership of the same file.
std::wstring RuntimeConfigLockName(const std::filesystem::path& path);
inline constexpr wchar_t kRuntimeTitleLockName[] =
    L"Local\\TheDarknessRecompiled-545407EE";

class RuntimeSingleInstance {
public:
    explicit RuntimeSingleInstance(const std::wstring& name);
    ~RuntimeSingleInstance();

    RuntimeSingleInstance(const RuntimeSingleInstance&) = delete;
    RuntimeSingleInstance& operator=(const RuntimeSingleInstance&) = delete;

    bool acquired() const noexcept { return handle_ != nullptr; }
    bool alreadyRunning() const noexcept { return alreadyRunning_; }
    // End startup-only config ownership after every consumer owns its bytes.
    // Title ownership remains held until the executing runtime is destroyed.
    void Release() noexcept;
    // Read-only preflight; the actual launch must still acquire its own lock.
    static bool Exists(const std::wstring& name);

private:
    HANDLE handle_{};
    bool alreadyRunning_{};
};
