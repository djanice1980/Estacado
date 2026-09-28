// Measured automatic settings (V338): the graphics card's identity and the
// per-card record, shared by the game and the settings launcher.
#include "runtime_gpu_calibration.h"

#include <Windows.h>
#include <dxgi1_6.h>

#include <fstream>
#include <sstream>

RuntimeGpuIdentity RuntimeDetectGpuIdentity() {
    RuntimeGpuIdentity identity;
    // Loaded on demand: the runtime links no DXGI import.
    HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dxgi) return identity;
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    const auto create =
        reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
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
                identity.vendorId = desc.VendorId;
                identity.deviceId = desc.DeviceId;
                identity.videoMemoryBytes = uint64_t(desc.DedicatedVideoMemory);
                const int size = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0,
                                                     nullptr, nullptr);
                if (size > 1) {
                    std::string text(size_t(size), '\0');
                    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, text.data(), size,
                                        nullptr, nullptr);
                    text.resize(size_t(size - 1));
                    identity.description = std::move(text);
                }
            }
            adapter->Release();
        }
        factory->Release();
    }
    FreeLibrary(dxgi);
    return identity;
}

std::filesystem::path RuntimeGpuCalibrationPath(const std::filesystem::path& userDataRoot) {
    return userDataRoot / L"gpu_calibration.toml";
}

std::optional<RuntimeGpuCalibrationRecord> RuntimeLoadGpuCalibration(
    const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    std::ostringstream text;
    text << file.rdbuf();
    return RuntimeGpuCalibrationFromToml(text.str());
}
