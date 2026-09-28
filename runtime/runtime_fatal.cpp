// Fatal stops of the title: report, dump and player message (V380; see
// runtime_fatal.h). Separate from main.cpp so every target that links the
// import layer (and the tests) has it.
#include "runtime_fatal.h"
#include "product_name.h"
#include "runtime_function_trace.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <DbgHelp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace {
using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                          PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);
MiniDumpWriteDumpFn g_miniDumpWriteDump{};
wchar_t g_crashDirectory[MAX_PATH]{};  // "<executable directory>\\logs\\"
}  // namespace

void RuntimeFatalConfigure(const wchar_t* crashDirectory, void* miniDumpWriteDump) noexcept {
    if (crashDirectory) wcscpy_s(g_crashDirectory, crashDirectory);
    g_miniDumpWriteDump = reinterpret_cast<MiniDumpWriteDumpFn>(miniDumpWriteDump);
}

namespace {
std::mutex g_fatalMutex;
std::string g_fatalDetail;
std::atomic<bool> g_fatalDumpWritten{};
std::atomic<bool> g_fatalReported{};

void AppendCrashLogLine(const std::string& line) noexcept {
    wchar_t logPath[MAX_PATH]{};
    if (g_crashDirectory[0]) {
        CreateDirectoryW(g_crashDirectory, nullptr);
        _snwprintf_s(logPath, _TRUNCATE, L"%lsruntime_crash.log", g_crashDirectory);
    } else {
        CreateDirectoryW(L"logs", nullptr);
        wcscpy_s(logPath, L"logs\\runtime_crash.log");
    }
    const HANDLE crashLog = CreateFileW(logPath, FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written{};
    if (crashLog != INVALID_HANDLE_VALUE) {
        WriteFile(crashLog, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        CloseHandle(crashLog);
    }
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line.data(), static_cast<DWORD>(line.size()),
              &written, nullptr);
}

std::string FatalUtcStamp() {
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    char stamp[40];
    _snprintf_s(stamp, _TRUNCATE, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", utc.wYear, utc.wMonth,
                utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds);
    return stamp;
}

std::wstring WideFromUtf8(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring wide(size_t(std::max(size, 0)), L'\0');
    if (size > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), size);
    return wide;
}
}  // namespace

void RuntimeFatalRecordDetail(const std::string& detail) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_fatalMutex);
        if (!g_fatalDetail.empty()) g_fatalDetail += " | ";
        g_fatalDetail += detail;
        if (g_fatalDetail.size() > 4000) g_fatalDetail.resize(4000);
    } catch (...) {
    }
}

void RuntimeFatalWriteDump(const char* tag) noexcept {
    if (!g_miniDumpWriteDump || !g_crashDirectory[0]) return;
    if (g_fatalDumpWritten.exchange(true)) return;
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    CreateDirectoryW(g_crashDirectory, nullptr);
    wchar_t dumpPath[MAX_PATH]{};
    _snwprintf_s(dumpPath, _TRUNCATE, L"%lsTheDarkness_fatal_%04u%02u%02u_%02u%02u%02u_%lu.dmp",
                 g_crashDirectory, utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute,
                 utc.wSecond, GetCurrentProcessId());
    const HANDLE dump = CreateFileW(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    BOOL dumped = FALSE;
    if (dump != INVALID_HANDLE_VALUE) {
        // No exception record: the stopping thread's own stack is in the dump
        // (taken at the stop, before the error unwinds to main).
        dumped = g_miniDumpWriteDump(
            GetCurrentProcess(), GetCurrentProcessId(), dump,
            static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory |
                                       MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
            nullptr, nullptr, nullptr);
        CloseHandle(dump);
    }
    char line[MAX_PATH * 2 + 128];
    _snprintf_s(line, _TRUNCATE, "RUNTIME_FATAL_DUMP utc=%s tag=%s tid=%lu written=%d path=%ls\r\n",
                FatalUtcStamp().c_str(), tag ? tag : "", GetCurrentThreadId(), dumped ? 1 : 0,
                dumpPath);
    AppendCrashLogLine(line);
}

void RuntimeFatalReport(const std::string& reason) noexcept {
    if (g_fatalReported.exchange(true)) return;
    try {
        std::string detail;
        {
            std::lock_guard<std::mutex> lock(g_fatalMutex);
            detail = g_fatalDetail;
        }
        std::string line = "RUNTIME_FATAL_EXIT utc=" + FatalUtcStamp() + " pid=" +
                           std::to_string(GetCurrentProcessId()) + " reason=" + reason;
        if (!detail.empty()) line += " detail=" + detail;
        line += "\r\n";
        AppendCrashLogLine(line);
        RuntimeWriteThreadSnapshot("fatal-exit");
    } catch (...) {
    }
    RuntimeFatalWriteDump("fatal-exit");
}

namespace {
std::atomic<long long> g_lastSwapMs{};  // steady clock; 0 = no swap yet
std::atomic<bool> g_stallWatchStarted{};
std::atomic<bool> g_stallWatchStop{};
std::atomic<bool> g_stallDumpWritten{};

long long SteadyMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void WriteStallDump() noexcept {
    if (!g_miniDumpWriteDump || !g_crashDirectory[0]) return;
    if (g_stallDumpWritten.exchange(true)) return;
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    CreateDirectoryW(g_crashDirectory, nullptr);
    wchar_t dumpPath[MAX_PATH]{};
    _snwprintf_s(dumpPath, _TRUNCATE, L"%lsTheDarkness_stall_%04u%02u%02u_%02u%02u%02u_%lu.dmp",
                 g_crashDirectory, utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute,
                 utc.wSecond, GetCurrentProcessId());
    const HANDLE dump = CreateFileW(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    BOOL dumped = FALSE;
    if (dump != INVALID_HANDLE_VALUE) {
        dumped = g_miniDumpWriteDump(
            GetCurrentProcess(), GetCurrentProcessId(), dump,
            static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory |
                                       MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
            nullptr, nullptr, nullptr);
        CloseHandle(dump);
    }
    char line[MAX_PATH * 2 + 96];
    _snprintf_s(line, _TRUNCATE, "RUNTIME_STALL_DUMP utc=%s written=%d path=%ls\r\n",
                FatalUtcStamp().c_str(), dumped ? 1 : 0, dumpPath);
    AppendCrashLogLine(line);
}
}  // namespace

std::atomic<uint64_t> g_swapCount{};

void RuntimeFatalNoteSwap() noexcept {
    g_lastSwapMs.store(SteadyMs(), std::memory_order_relaxed);
    g_swapCount.fetch_add(1, std::memory_order_relaxed);
}

uint64_t RuntimeSwapCount() noexcept { return g_swapCount.load(std::memory_order_relaxed); }

void RuntimeFatalStartStallWatch(bool (*closeRequested)() noexcept) noexcept {
    if (g_stallWatchStarted.exchange(true)) return;
    try {
        std::thread([closeRequested] {
            long long stalledSince = 0;  // last swap of the stall being reported
            unsigned reports = 0;
            while (!g_stallWatchStop.load(std::memory_order_acquire)) {
                Sleep(500);
                const long long last = g_lastSwapMs.load(std::memory_order_relaxed);
                if (!last || g_fatalReported.load() ||
                    g_stallWatchStop.load(std::memory_order_acquire)) {
                    continue;
                }
                const long long gap = SteadyMs() - last;
                if (stalledSince) {
                    if (last != stalledSince && reports <= 8) {
                        char line[160];
                        _snprintf_s(line, _TRUNCATE, "RUNTIME_STALL_END utc=%s seconds=%.1f\r\n",
                                    FatalUtcStamp().c_str(), (last - stalledSince) / 1000.0);
                        AppendCrashLogLine(line);
                    }
                    if (last != stalledSince) stalledSince = 0;
                    continue;
                }
                const bool closing = closeRequested && closeRequested();
                if (gap < kRuntimeStallReportMs && !(closing && gap >= kRuntimeStallAtCloseMs)) {
                    continue;
                }
                stalledSince = last;
                if (++reports > 8) continue;
                char line[200];
                _snprintf_s(line, _TRUNCATE,
                            "RUNTIME_STALL utc=%s pid=%lu seconds_without_frame=%.1f close_requested=%d\r\n",
                            FatalUtcStamp().c_str(), GetCurrentProcessId(), gap / 1000.0,
                            closing ? 1 : 0);
                AppendCrashLogLine(line);
                RuntimeWriteThreadSnapshot("stall");
                WriteStallDump();
            }
        }).detach();
    } catch (...) {
    }
}

void RuntimeFatalStopStallWatch() noexcept {
    g_stallWatchStop.store(true, std::memory_order_release);
}

void RuntimeFatalShowDialog(const std::string& reason) noexcept {
    if (std::getenv("DARKNESS_NO_FATAL_DIALOG")) return;
    try {
        std::string shown = reason.size() > 400 ? reason.substr(0, 400) + "..." : reason;
        std::wstring text =
            L"The game stopped because of an error it could not recover from:\n\n" +
            WideFromUtf8(shown) +
            L"\n\nA crash report (runtime_crash.log and a .dmp file) was saved in:\n" +
            std::wstring(g_crashDirectory[0] ? g_crashDirectory : L"logs\\") +
            L"\n\nPlease send those files to the developer.";
        MessageBoxW(nullptr, text.c_str(), L"" DARKNESS_PRODUCT_NAME,
                    MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
    } catch (...) {
    }
}

