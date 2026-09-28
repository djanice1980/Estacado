#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>

using Microsoft::WRL::ComPtr;

int main() {
  ComPtr<IDXGIFactory6> factory;
  if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
    std::fprintf(stderr, "CreateDXGIFactory2 failed\n");
    return 1;
  }

  for (UINT index = 0;; ++index) {
    ComPtr<IDXGIAdapter1> adapter;
    const HRESULT enumerate_result = factory->EnumAdapterByGpuPreference(
        index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    if (enumerate_result == DXGI_ERROR_NOT_FOUND) break;
    if (FAILED(enumerate_result)) return 2;

    DXGI_ADAPTER_DESC1 description{};
    adapter->GetDesc1(&description);
    if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

    ComPtr<ID3D12Device> device;
    const HRESULT device_result =
        D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(device_result)) continue;

    D3D12_FEATURE_DATA_D3D12_OPTIONS8 options8{};
    const HRESULT options_result = device->CheckFeatureSupport(
        D3D12_FEATURE_D3D12_OPTIONS8, &options8, sizeof(options8));
    ::wprintf(L"adapter=%ls vendor=0x%04X device=0x%04X options8=0x%08X "
              L"unaligned_block_textures=%u\n",
              description.Description, description.VendorId, description.DeviceId,
              static_cast<unsigned>(options_result),
              SUCCEEDED(options_result) && options8.UnalignedBlockTexturesSupported ? 1u : 0u);
  }
  return 0;
}
