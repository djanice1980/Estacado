// Developer tool: records what the display actually shows, one entry per
// presented frame, for per-frame motion analysis (camera, view model, HUD).
// Uses DXGI Desktop Duplication (read-only; no input, no window changes):
// each new desktop frame is copied into a mip chain on the GPU, a small mip
// (for example 320 x 180 from 2560 x 1440) is read back as 8-bit luma, and
// the frame's LastPresentTime (QPC) and AccumulatedFrames are kept with it.
//
// usage: screen_frame_capture --out FILE [--seconds 8] [--output 0] [--mip 3]
//                             [--color] [--start-file PATH] [--stop-file PATH]
// Output: "SFC1" header (u32 width, u32 height, u32 count, i64 qpc_frequency)
// then per frame: i64 present_qpc, i64 acquire_qpc, u32 accumulated_frames,
// width*height luma bytes. With --color the header is "SFCC" and each frame
// has width*height*3 bytes (R, G, B) instead of luma.
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace {
struct Options {
  std::string out;
  double seconds = 8.0;
  UINT output = 0;
  UINT mip = 3;
  bool color = false;
  std::string start_file;
  std::string stop_file;
};

bool Parse(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    const char* v = nullptr;
    if (a == "--out" && (v = next())) o.out = v;
    else if (a == "--seconds" && (v = next())) o.seconds = atof(v);
    else if (a == "--output" && (v = next())) o.output = UINT(atoi(v));
    else if (a == "--mip" && (v = next())) o.mip = UINT(atoi(v));
    else if (a == "--color") o.color = true;
    else if (a == "--start-file" && (v = next())) o.start_file = v;
    else if (a == "--stop-file" && (v = next())) o.stop_file = v;
    else return false;
  }
  return !o.out.empty() && o.seconds > 0 && o.mip >= 1 && o.mip <= 6;
}

bool Exists(const std::string& path) {
  return !path.empty() && GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}
}  // namespace

int main(int argc, char** argv) {
  Options o;
  if (!Parse(argc, argv, o)) {
    fprintf(stderr, "usage: screen_frame_capture --out FILE [--seconds 8] [--output 0] "
                    "[--mip 3] [--color] [--start-file PATH] [--stop-file PATH]\n");
    return 2;
  }
  // Physical pixels on scaled displays.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 1;
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<IDXGIOutput> output;
  for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
    if (SUCCEEDED(adapter->EnumOutputs(o.output, &output))) break;
    adapter.Reset();
  }
  if (!output) {
    fprintf(stderr, "output %u not found\n", o.output);
    return 1;
  }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
  if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 1,
                               D3D11_SDK_VERSION, &device, nullptr, &context))) {
    fprintf(stderr, "D3D11CreateDevice failed\n");
    return 1;
  }
  ComPtr<IDXGIOutput1> output1;
  output.As(&output1);
  ComPtr<IDXGIOutputDuplication> duplication;
  HRESULT hr = output1->DuplicateOutput(device.Get(), &duplication);
  if (FAILED(hr)) {
    fprintf(stderr, "DuplicateOutput failed 0x%08lX\n", hr);
    return 1;
  }
  DXGI_OUTDUPL_DESC desc{};
  duplication->GetDesc(&desc);
  const UINT width = desc.ModeDesc.Width, height = desc.ModeDesc.Height;
  // Mip chain source (copy target) and a staging texture for the chosen mip.
  D3D11_TEXTURE2D_DESC chain{};
  chain.Width = width;
  chain.Height = height;
  chain.MipLevels = o.mip + 1;
  chain.ArraySize = 1;
  chain.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  chain.SampleDesc.Count = 1;
  chain.Usage = D3D11_USAGE_DEFAULT;
  chain.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  chain.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
  ComPtr<ID3D11Texture2D> chain_texture;
  ComPtr<ID3D11ShaderResourceView> chain_view;
  if (FAILED(device->CreateTexture2D(&chain, nullptr, &chain_texture)) ||
      FAILED(device->CreateShaderResourceView(chain_texture.Get(), nullptr, &chain_view))) {
    fprintf(stderr, "mip chain creation failed\n");
    return 1;
  }
  const UINT mw = (std::max)(1u, width >> o.mip), mh = (std::max)(1u, height >> o.mip);
  D3D11_TEXTURE2D_DESC staging{};
  staging.Width = mw;
  staging.Height = mh;
  staging.MipLevels = 1;
  staging.ArraySize = 1;
  staging.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  staging.SampleDesc.Count = 1;
  staging.Usage = D3D11_USAGE_STAGING;
  staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging_texture;
  if (FAILED(device->CreateTexture2D(&staging, nullptr, &staging_texture))) return 1;

  LARGE_INTEGER frequency, start, now;
  QueryPerformanceFrequency(&frequency);
  if (!o.start_file.empty()) {
    fprintf(stdout, "waiting for %s\n", o.start_file.c_str());
    fflush(stdout);
    while (!Exists(o.start_file)) Sleep(5);
  }
  QueryPerformanceCounter(&start);
  const int64_t end = start.QuadPart + int64_t(o.seconds * double(frequency.QuadPart));
  struct Header { int64_t present; int64_t acquire; uint32_t accumulated; };
  std::vector<Header> headers;
  std::vector<uint8_t> pixels;
  headers.reserve(4096);
  const size_t frame_bytes = size_t(mw) * mh * (o.color ? 3 : 1);
  pixels.reserve(frame_bytes * 2048);
  uint64_t timeouts = 0, lost = 0;
  for (;;) {
    QueryPerformanceCounter(&now);
    if (now.QuadPart >= end || Exists(o.stop_file)) break;
    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    hr = duplication->AcquireNextFrame(50, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
      ++timeouts;
      continue;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST) {
      ++lost;
      duplication.Reset();
      if (FAILED(output1->DuplicateOutput(device.Get(), &duplication))) break;
      continue;
    }
    if (FAILED(hr)) break;
    QueryPerformanceCounter(&now);
    // Only frames with new desktop content (a present) are recorded.
    if (info.LastPresentTime.QuadPart != 0 && resource) {
      ComPtr<ID3D11Texture2D> frame;
      resource.As(&frame);
      context->CopySubresourceRegion(chain_texture.Get(), 0, 0, 0, 0, frame.Get(), 0, nullptr);
      context->GenerateMips(chain_view.Get());
      context->CopySubresourceRegion(staging_texture.Get(), 0, 0, 0, 0, chain_texture.Get(),
                                     o.mip, nullptr);
      D3D11_MAPPED_SUBRESOURCE mapped{};
      if (SUCCEEDED(context->Map(staging_texture.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        const size_t base = pixels.size();
        pixels.resize(base + frame_bytes);
        for (UINT y = 0; y < mh; ++y) {
          const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
          if (o.color) {
            uint8_t* out = pixels.data() + base + size_t(y) * mw * 3;
            for (UINT x = 0; x < mw; ++x) {
              const uint8_t* p = row + x * 4;  // B G R A
              out[x * 3 + 0] = p[2];
              out[x * 3 + 1] = p[1];
              out[x * 3 + 2] = p[0];
            }
            continue;
          }
          uint8_t* out = pixels.data() + base + size_t(y) * mw;
          for (UINT x = 0; x < mw; ++x) {
            const uint8_t* p = row + x * 4;  // B G R A
            out[x] = uint8_t((29u * p[0] + 150u * p[1] + 77u * p[2]) >> 8);
          }
        }
        context->Unmap(staging_texture.Get(), 0);
        headers.push_back({info.LastPresentTime.QuadPart, now.QuadPart, info.AccumulatedFrames});
      }
    }
    duplication->ReleaseFrame();
  }
  FILE* f = fopen(o.out.c_str(), "wb");
  if (!f) return 1;
  const uint32_t count = uint32_t(headers.size());
  fwrite(o.color ? "SFCC" : "SFC1", 1, 4, f);
  fwrite(&mw, 4, 1, f);
  fwrite(&mh, 4, 1, f);
  fwrite(&count, 4, 1, f);
  fwrite(&frequency.QuadPart, 8, 1, f);
  for (uint32_t i = 0; i < count; ++i) {
    fwrite(&headers[i].present, 8, 1, f);
    fwrite(&headers[i].acquire, 8, 1, f);
    fwrite(&headers[i].accumulated, 4, 1, f);
    fwrite(pixels.data() + size_t(i) * frame_bytes, 1, frame_bytes, f);
  }
  fclose(f);
  fprintf(stdout, "frames=%u width=%u height=%u timeouts=%llu lost=%llu out=%s\n", count, mw, mh,
          (unsigned long long)timeouts, (unsigned long long)lost, o.out.c_str());
  return 0;
}
