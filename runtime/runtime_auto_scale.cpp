#include "runtime_auto_scale.h"

#include <Windows.h>
#include <dxgi1_6.h>

#include <cstdlib>
#include <vector>

namespace {

bool ParseOutputSize(std::string_view text, uint32_t& width, uint32_t& height) {
    if (text == "720p") { width = 1280; height = 720; return true; }
    if (text == "1080p") { width = 1920; height = 1080; return true; }
    if (text == "1440p") { width = 2560; height = 1440; return true; }
    if (text == "4k") { width = 3840; height = 2160; return true; }
    const size_t x = text.find('x');
    if (x == std::string_view::npos || x == 0 || x + 1 >= text.size()) return false;
    const std::string w(text.substr(0, x));
    const std::string h(text.substr(x + 1));
    char* endW = nullptr;
    char* endH = nullptr;
    const unsigned long parsedW = std::strtoul(w.c_str(), &endW, 10);
    const unsigned long parsedH = std::strtoul(h.c_str(), &endH, 10);
    if (!endW || *endW || !endH || *endH || !parsedW || !parsedH) return false;
    width = uint32_t(parsedW);
    height = uint32_t(parsedH);
    return true;
}

BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    reinterpret_cast<std::vector<HMONITOR>*>(data)->push_back(monitor);
    return TRUE;
}

bool MonitorSize(int64_t index, uint32_t& width, uint32_t& height) {
    HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (index > 0) {
        std::vector<HMONITOR> monitors;
        EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, LPARAM(&monitors));
        if (size_t(index) < monitors.size()) monitor = monitors[size_t(index)];
    }
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return false;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmPelsWidth &&
        mode.dmPelsHeight) {
        width = mode.dmPelsWidth;
        height = mode.dmPelsHeight;
        return true;
    }
    width = uint32_t(info.rcMonitor.right - info.rcMonitor.left);
    height = uint32_t(info.rcMonitor.bottom - info.rcMonitor.top);
    return width && height;
}

uint64_t HighPerformanceVideoMemory() {
    // Loaded on demand: the runtime links no DXGI import.
    HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dxgi) return 0;
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    const auto create =
        reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    uint64_t memory = 0;
    IDXGIFactory1* factory = nullptr;
    if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        IDXGIAdapter1* adapter = nullptr;
        IDXGIFactory6* factory6 = nullptr;
        if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory6),
                                              reinterpret_cast<void**>(&factory6)))) {
            factory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                 __uuidof(IDXGIAdapter1),
                                                 reinterpret_cast<void**>(&adapter));
            factory6->Release();
        }
        if (!adapter) factory->EnumAdapters1(0, &adapter);
        if (adapter) {
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                memory = uint64_t(desc.DedicatedVideoMemory);
            }
            adapter->Release();
        }
        factory->Release();
    }
    FreeLibrary(dxgi);
    return memory;
}

}  // namespace

RuntimeAutoScale RuntimeDetectAutomaticScale(std::string_view outputResolution, int64_t monitor) {
    uint32_t width = 0;
    uint32_t height = 0;
    if (outputResolution.empty() || outputResolution == "native" ||
        !ParseOutputSize(outputResolution, width, height)) {
        if (!MonitorSize(monitor, width, height)) {
            width = 0;
            height = 0;
        }
    }
    return RuntimeResolveAutomaticScale(width, height, HighPerformanceVideoMemory());
}

double RuntimeMonitorRefreshHz(int64_t monitorIndex) {
    HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (monitorIndex > 0) {
        std::vector<HMONITOR> monitors;
        EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, LPARAM(&monitors));
        if (size_t(monitorIndex) < monitors.size()) monitor = monitors[size_t(monitorIndex)];
    }
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return 0.0;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) return 0.0;
    return mode.dmDisplayFrequency > 1 ? double(mode.dmDisplayFrequency) : 0.0;
}

std::string RuntimeAutoScaleLabel(uint32_t scale) {
    return std::to_string(scale) + "x - " + std::to_string(1280 * scale) + " x " +
           std::to_string(720 * scale);
}
