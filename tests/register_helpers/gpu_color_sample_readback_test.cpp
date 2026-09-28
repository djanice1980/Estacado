// Real D3D12 WARP validation of the production sample extractor. Synthetic
// shaders here are never submitted to the title or counted as game evidence.
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <rex/graphics/d3d12/diagnostic_color_samples_readback.h>
#include <rex/graphics/embedded_scene_alias_capture_policy.h>
#include "../../external/ReXGlue/src/graphics/shaders/bytecode/d3d12_5_1/diagnostic_color_samples_single.h"
#include "../../external/ReXGlue/src/graphics/shaders/bytecode/d3d12_5_1/diagnostic_color_samples_msaa.h"

using Microsoft::WRL::ComPtr;
using Copy = rex::graphics::d3d12::DiagnosticColorSamplesReadback;
static void check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
static void hr(HRESULT value, const char* reason) { check(SUCCEEDED(value), reason); }
struct Commands {
  ID3D12GraphicsCommandList* list;
  void D3DResourceBarrier(UINT n, const D3D12_RESOURCE_BARRIER* b) { list->ResourceBarrier(n,b); }
  void D3DSetComputeRootSignature(ID3D12RootSignature* r) { list->SetComputeRootSignature(r); }
  void D3DSetComputeRoot32BitConstants(UINT a, UINT b, const void* c, UINT d) { list->SetComputeRoot32BitConstants(a,b,c,d); }
  void D3DSetComputeRootDescriptorTable(UINT a, D3D12_GPU_DESCRIPTOR_HANDLE b) { list->SetComputeRootDescriptorTable(a,b); }
  void D3DSetComputeRootUnorderedAccessView(UINT a, D3D12_GPU_VIRTUAL_ADDRESS b) { list->SetComputeRootUnorderedAccessView(a,b); }
  void D3DDispatch(UINT x, UINT y, UINT z) { list->Dispatch(x,y,z); }
  void D3DCopyBufferRegion(ID3D12Resource* a, UINT64 b, ID3D12Resource* c, UINT64 d, UINT64 e) { list->CopyBufferRegion(a,b,c,d,e); }
};
static ComPtr<ID3DBlob> shader(const char* source, const char* type) {
  ComPtr<ID3DBlob> code, error;
  hr(D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", type,
      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error), "test shader compile");
  return code;
}
static std::vector<uint16_t> read(const Copy& copy) {
  void* data = nullptr; D3D12_RANGE range{0, SIZE_T(copy.bytes)};
  hr(copy.readback->Map(0, &range, &data), "completed readback map");
  std::vector<uint16_t> result(size_t(copy.bytes / 2));
  std::memcpy(result.data(), data, size_t(copy.bytes));
  D3D12_RANGE no_write{}; copy.readback->Unmap(0, &no_write);
  return result;
}
static float half(uint16_t bits) {
  const int exponent = (bits >> 10) & 31, mantissa = bits & 1023;
  check(exponent != 31, "unexpected nonfinite test value");
  return (bits & 32768 ? -1.0f : 1.0f) *
      (exponent ? std::ldexp(float(1024 + mantissa), exponent - 25) : std::ldexp(float(mantissa), -24));
}
static void run(ID3D12Device* device, UINT samples) {
  constexpr UINT width = 23, height = 17, left = 3, top = 4, crop_width = 17, crop_height = 9;
  const UINT64 bytes = UINT64(crop_width) * crop_height * samples * 8;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = width; desc.Height = height;
  desc.DepthOrArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = samples;
  desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  ComPtr<ID3D12Resource> target;
  hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
      D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)), "color target");
  const D3D12_SHADER_BYTECODE single{diagnostic_color_samples_single, sizeof(diagnostic_color_samples_single)};
  const D3D12_SHADER_BYTECODE msaa{diagnostic_color_samples_msaa, sizeof(diagnostic_color_samples_msaa)};
  Copy before, after, same, overwritten;
  for (auto* copy : {&before, &after, &same, &overwritten})
    check(copy->Create(device, target.Get(), left, top, crop_width, crop_height, bytes, single, msaa), "create production extractor");
  Copy rejected;
  check(!rejected.Create(device, target.Get(), left, top, crop_width, crop_height, bytes-1, single, msaa), "byte bound");
  check(!rejected.Create(device, target.Get(), UINT32_MAX, top, crop_width, crop_height, bytes, single, msaa), "overflow crop");
  check(!rejected.Create(device, target.Get(), left, height, crop_width, crop_height, bytes, single, msaa), "outside crop");
  check(!rejected.Create(device, target.Get(), left, top, 0, crop_height, bytes, single, msaa), "empty crop");

  const std::string path = std::string(DARKNESS_SOURCE_ROOT) + "/external/ReXGlue/src/graphics/shaders/diagnostic_color_samples.hlsl";
  D3D_SHADER_MACRO macros[] = {{"SINGLE_SAMPLE", samples == 1 ? "1" : "0"}, {nullptr,nullptr}};
  ComPtr<ID3DBlob> compiled, error;
  hr(D3DCompileFromFile(std::wstring(path.begin(), path.end()).c_str(), macros, nullptr,
      "main", "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &compiled, &error), "production HLSL compile");
  const auto embedded = samples == 1 ? single : msaa;
  check(compiled->GetBufferSize() == embedded.BytecodeLength &&
      !std::memcmp(compiled->GetBufferPointer(), embedded.pShaderBytecode, embedded.BytecodeLength), "embedded shader differs from source");

  D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> rtvs, srvs;
  hr(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvs)), "RTV heap");
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  hr(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvs)), "SRV heap");
  before.WriteSRV(device, srvs->GetCPUDescriptorHandleForHeapStart());
  const auto srv = srvs->GetGPUDescriptorHandleForHeapStart();
  D3D12_ROOT_SIGNATURE_DESC rd{}; ComPtr<ID3DBlob> serialized;
  hr(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr), "graphics root bytes");
  ComPtr<ID3D12RootSignature> root;
  hr(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root)), "graphics root");
  auto vs = shader("float4 main(uint i:SV_VertexID):SV_Position{return float4(i==1?3:-1,i==2?3:-1,0,1);}", "vs_5_0");
  auto ps = shader("float4 main(float4 p:SV_Position,uint s:SV_SampleIndex):SV_Target0 {"
      "return float4((s+1)*.125,(floor(p.x)+1)*.03125,(floor(p.y)+1)*.03125,-.5);}", "ps_5_0");
  D3D12_GRAPHICS_PIPELINE_STATE_DESC gp{}; gp.pRootSignature = root.Get();
  gp.VS = {vs->GetBufferPointer(),vs->GetBufferSize()}; gp.PS = {ps->GetBufferPointer(),ps->GetBufferSize()};
  gp.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; gp.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  gp.RasterizerState.DepthClipEnable = TRUE; gp.SampleMask = UINT_MAX; gp.SampleDesc.Count = samples;
  gp.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  gp.NumRenderTargets = 1; gp.RTVFormats[0] = desc.Format;
  gp.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  ComPtr<ID3D12PipelineState> draw;
  hr(device->CreateGraphicsPipelineState(&gp, IID_PPV_ARGS(&draw)), "sample-index draw pipeline");
  ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC qd{};
  hr(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
  ComPtr<ID3D12CommandAllocator> allocator;
  hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
  ComPtr<ID3D12GraphicsCommandList> list;
  hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
  Commands commands{list.Get()};
  list->SetGraphicsRootSignature(root.Get()); list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  D3D12_VIEWPORT vp{0,0,float(width),float(height),0,1}; D3D12_RECT scissor{0,0,width,height};
  list->RSSetViewports(1,&vp); list->RSSetScissorRects(1,&scissor);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D12DescriptorHeap* heaps[]{srvs.Get()}; list->SetDescriptorHeaps(1,heaps);
  const auto capture = [&](const Copy& copy) {
    list->SetPipelineState(copy.pipeline.Get());
    copy.Enqueue(commands, D3D12_RESOURCE_STATE_RENDER_TARGET, srv);
  };
  float clear[4]{}; list->ClearRenderTargetView(rtv,clear,0,nullptr);
  capture(before);
  // The runtime invalidates its guest pipeline before this observer. Rebind
  // only that pipeline here; all other graphics state must survive extraction.
  list->SetPipelineState(draw.Get()); list->DrawInstanced(3,1,0,0);
  capture(after); capture(same);
  for (auto& value : clear) value = 1;
  list->ClearRenderTargetView(rtv,clear,0,nullptr); capture(overwritten);
  hr(list->Close(), "close"); ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1,lists);
  ComPtr<ID3D12Fence> fence; hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)), "fence");
  hr(queue->Signal(fence.Get(),1), "signal"); HANDLE event = CreateEvent(nullptr,FALSE,FALSE,nullptr);
  check(event != nullptr, "event"); hr(fence->SetEventOnCompletion(1,event), "fence event");
  const auto waited = WaitForSingleObject(event,30000); CloseHandle(event);
  check(waited == WAIT_OBJECT_0 && fence->GetCompletedValue() >= 1, "completed GPU work");
  const auto a = read(before), b = read(after), c = read(same), d = read(overwritten);
  check(b == c, "extractor changed source samples");
  for (UINT y=0; y<crop_height; ++y) for (UINT x=0; x<crop_width; ++x) for (UINT s=0; s<samples; ++s) {
    const UINT i = ((y*crop_width+x)*samples+s)*4;
    const float expected[] = {float(s+1)*.125f, float(x+left+1)*.03125f, float(y+top+1)*.03125f, -.5f};
    for (UINT k=0; k<4; ++k) {
      check(a[i+k] == 0, "before image ordering");
      check(half(b[i+k]) == expected[k], "sample identity, crop, channel order or draw-state preservation");
      check(d[i+k] == 0x3C00, "later overwrite/copy ordering");
    }
  }
  hr(device->GetDeviceRemovedReason(), "device removed");
  std::cout << "PASS production individual color samples=" << samples << " cropped odd extent, draw/copy order, source preservation, fence\n";
}
static void contracts() {
  using Policy = rex::graphics::embedded_scene_alias_capture_policy::Budget;
  Policy b;
  check(!b.Reserve(0,768,1536) && !b.Reserve(1,769,1536) && !b.Reserve(1,768,1552), "reject invalid epoch/range");
  for (UINT i=1; i<=8; ++i) check(b.Reserve(1,768,1536) == i, "reserve bounded complete pairs");
  check(b.reserved_bytes == UINT64_C(125829120) && !b.Reserve(1,768,1440) && !b.Reserve(2,768,1440), "pair/frame bound");
  check(Policy::CopyBytes(768,1440) == 6881280 && !Policy::CopyBytes(1536,768), "second section range and wrap rejection");
  std::ifstream file(std::string(DARKNESS_SOURCE_ROOT)+"/external/ReXGlue/src/graphics/d3d12/render_target_cache.cpp");
  const std::string source((std::istreambuf_iterator<char>(file)),{});
  check(source.find("REXCVAR_DEFINE_BOOL(embedded_camera_scene_alias_capture, false") != std::string::npos &&
      source.find("if (!scene_update_capture_ || !REXCVAR_GET(embedded_camera_scene_alias_capture)) return;") != std::string::npos, "selected-frame default-off gate");
  const auto update = source.find("bool D3D12RenderTargetCache::Update(");
  const auto before = source.find("CaptureSceneAliasTransfers(depth_and_color_render_targets, last_update_transfers(), false)", update);
  const auto transfer = source.find("PerformTransfersAndResolveClears(", before);
  const auto after = source.find("CaptureSceneAliasTransfers(depth_and_color_render_targets, last_update_transfers(), true)", transfer);
  const auto bind = source.find("SetCommandListRenderTargets(depth_and_color_render_targets)", after);
  check(before != std::string::npos && before < transfer && transfer < after && after < bind, "bracket actual transfers before guest binding");
  const auto queue = source.find("bool D3D12RenderTargetCache::QueueSceneAliasReadback(");
  const auto complete = source.find("void D3D12RenderTargetCache::CompleteSceneAliasReadbacks()", queue);
  check(queue != std::string::npos && complete != std::string::npos, "production queue and completion helpers");
  const auto body = source.substr(queue,complete-queue);
  check(body.find("SetExternalPipeline(pending.copy.pipeline.Get())") != std::string::npos &&
      body.find("Await") == std::string::npos && body.find("->Map(") == std::string::npos, "guest pipeline invalidation and deferred mapping");
  check(source.find("pending_scene_alias_readbacks_.front().submission <= completed",complete) < source.find("readback->Map(",complete), "completion precedes map");
}
int main() {
  try {
    contracts();
    ComPtr<IDXGIFactory4> factory; hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    ComPtr<IDXGIAdapter> warp; hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP");
    ComPtr<ID3D12Device> device; hr(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)), "device");
    for (UINT samples : {1u,2u,4u}) {
      D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{};
      quality.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; quality.SampleCount = samples;
      hr(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,&quality,sizeof(quality)), "sample support");
      if (!quality.NumQualityLevels) { check(samples==2, "required sample count"); std::cout << "2x unavailable on WARP\n"; continue; }
      run(device.Get(),samples);
    }
    return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
