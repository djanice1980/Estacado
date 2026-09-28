// Headless real D3D12 test. Artificial test shaders are never submitted to the
// game and are not game-rendering evidence. Exercise the production copy helper
// around Draw without rebinding ANY graphics state between copies and draws.
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <rex/graphics/d3d12/diagnostic_color_readback.h>

using Microsoft::WRL::ComPtr;
using rex::graphics::d3d12::DiagnosticColorReadback;
static void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
static void hr(HRESULT value, const char* message) { check(SUCCEEDED(value), message); }
struct Commands {
  ID3D12GraphicsCommandList* list;
  void D3DResourceBarrier(UINT n, const D3D12_RESOURCE_BARRIER* b) { list->ResourceBarrier(n, b); }
  void D3DResolveSubresource(ID3D12Resource* a, UINT b, ID3D12Resource* c, UINT d, DXGI_FORMAT f) {
    list->ResolveSubresource(a, b, c, d, f);
  }
  void D3DCopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* a, UINT x, UINT y, UINT z,
                            const D3D12_TEXTURE_COPY_LOCATION* b, const D3D12_BOX* box) {
    list->CopyTextureRegion(a, x, y, z, b, box);
  }
};
static ComPtr<ID3DBlob> shader(const char* source, const char* target) {
  ComPtr<ID3DBlob> code, error;
  const auto result = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr,
                                "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error);
  if (FAILED(result) && error) std::cerr << static_cast<char*>(error->GetBufferPointer());
  hr(result, "shader compilation");
  return code;
}
static std::vector<uint16_t> read(const DiagnosticColorReadback& copy) {
  void* mapping = nullptr;
  D3D12_RANGE range{0, SIZE_T(copy.buffer_bytes)};
  hr(copy.buffer->Map(0, &range, &mapping), "completed map");
  std::vector<uint16_t> output(size_t(copy.row_bytes * copy.rows / 2));
  for (UINT row = 0; row < copy.rows; ++row)
    std::memcpy(output.data() + size_t(row * copy.row_bytes / 2),
                static_cast<char*>(mapping) + copy.footprint.Offset +
                    size_t(row) * copy.footprint.Footprint.RowPitch, size_t(copy.row_bytes));
  D3D12_RANGE no_write{};
  copy.buffer->Unmap(0, &no_write);
  return output;
}
static void run(ID3D12Device* device, UINT samples, UINT scale) {
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC qdesc{};
  hr(device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue)), "queue");
  ComPtr<ID3D12CommandAllocator> allocator;
  hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
  ComPtr<ID3D12GraphicsCommandList> list;
  hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                               IID_PPV_ARGS(&list)), "list");
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = 17 * scale;
  desc.Height = 5 * scale;
  desc.DepthOrArraySize = desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  desc.SampleDesc.Count = samples;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  ComPtr<ID3D12Resource> target;
  hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
       D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)), "target");
  D3D12_DESCRIPTOR_HEAP_DESC hdesc{};
  hdesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  hdesc.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> rtvs;
  hr(device->CreateDescriptorHeap(&hdesc, IID_PPV_ARGS(&rtvs)), "RTV heap");
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_ROOT_SIGNATURE_DESC rootdesc{};
  ComPtr<ID3DBlob> serialized;
  hr(D3D12SerializeRootSignature(&rootdesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr), "root bytes");
  ComPtr<ID3D12RootSignature> root;
  hr(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                  IID_PPV_ARGS(&root)), "root");
  auto vs = shader("float4 main(uint i:SV_VertexID):SV_Position {"
                   "return float4(i==1?3:-1,i==2?3:-1,0,1);}", "vs_5_0");
  auto ps = shader("float4 main():SV_Target0{return float4(.25,.5,.75,.5);}", "ps_5_0");
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = root.Get();
  pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pso.SampleMask = UINT_MAX;
  pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pso.RasterizerState.DepthClipEnable = TRUE;
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = desc.Format;
  pso.SampleDesc = desc.SampleDesc;
  ComPtr<ID3D12PipelineState> pipeline;
  hr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline)), "pipeline");
  list->SetPipelineState(pipeline.Get());
  list->SetGraphicsRootSignature(root.Get());
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  D3D12_VIEWPORT vp{0, 0, float(desc.Width), float(desc.Height), 0, 1};
  D3D12_RECT scissor{0, 0, LONG(desc.Width), LONG(desc.Height)};
  list->RSSetViewports(1, &vp);
  list->RSSetScissorRects(1, &scissor);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  float clear[4]{};
  list->ClearRenderTargetView(rtv, clear, 0, nullptr);
  DiagnosticColorReadback before, after, after_second;
  for (auto* copy : {&before, &after, &after_second})
    check(copy->Create(device, target.Get(), 1024 * 1024), "prepare readback");
  DiagnosticColorReadback rejected;
  check(!rejected.Create(device, target.Get(), 1), "byte limit failed");
  Commands commands{list.Get()};
  before.Enqueue(commands, D3D12_RESOURCE_STATE_RENDER_TARGET);
  list->DrawInstanced(3, 1, 0, 0);
  after.Enqueue(commands, D3D12_RESOURCE_STATE_RENDER_TARGET);
  // NO rebinding. Proves copy/resolve did not reset graphics state or attachments.
  list->DrawInstanced(3, 1, 0, 0);
  after_second.Enqueue(commands, D3D12_RESOURCE_STATE_RENDER_TARGET);
  hr(list->Close(), "close");
  ID3D12CommandList* lists[]{list.Get()};
  queue->ExecuteCommandLists(1, lists);
  ComPtr<ID3D12Fence> fence;
  hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
  hr(queue->Signal(fence.Get(), 1), "signal");
  HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  check(event != nullptr, "event");
  hr(fence->SetEventOnCompletion(1, event), "completion event");
  const auto waited = WaitForSingleObject(event, 30000);
  CloseHandle(event);
  check(waited == WAIT_OBJECT_0 && fence->GetCompletedValue() >= 1, "GPU completion");
  auto a = read(before), b = read(after), c = read(after_second);
  check(b == c, "graphics bindings changed across queued readback");
  const std::array<uint16_t, 4> expected{0x3400, 0x3800, 0x3A00, 0x3800};
  for (size_t i = 0; i < a.size(); ++i) {
    check(a[i] == 0, "before-image not before draw");
    check(b[i] == expected[i % 4], "after-image missing actual draw or wrong sample resolve");
  }
  hr(device->GetDeviceRemovedReason(), "device removed");
  std::cout << "PASS actual Draw/copy/fence, samples=" << samples << " scale=" << scale << '\n';
}
static void source_contract() {
  std::ifstream file(std::string(DARKNESS_SOURCE_ROOT) +
      "/external/ReXGlue/src/graphics/d3d12/command_processor.cpp");
  std::string source((std::istreambuf_iterator<char>(file)), {});
  auto start = source.find("const auto capture_target_writer =");
  auto end = source.find("if (target_writer_selection.before &&", start);
  check(start != std::string::npos && end != std::string::npos, "capture hook missing");
  const auto hook = source.substr(start, end - start);
  check(hook.find("QueueEmbeddedColorTarget(") != std::string::npos &&
        hook.find("->CaptureEmbeddedColorTarget(") == std::string::npos &&
        hook.find("AwaitAllQueueOperationsCompletion(") == std::string::npos,
        "mid-draw observer reintroduced a synchronous submission boundary");
}
int main() {
  try {
    source_contract();
    ComPtr<IDXGIFactory4> factory;
    hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
    ComPtr<IDXGIAdapter> warp;
    hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP adapter");
    ComPtr<ID3D12Device> device;
    hr(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12 WARP");
    for (UINT samples : {1u, 2u, 4u}) {
      D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{};
      quality.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
      quality.SampleCount = samples;
      hr(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                                      &quality, sizeof(quality)), "sample support");
      if (!quality.NumQualityLevels) {
        check(samples == 2, "required sample count unsupported");
        std::cout << "2x MSAA unsupported on WARP; actual 1x/4x still required\n";
        continue;
      }
      for (UINT scale : {1u, 2u}) run(device.Get(), samples, scale);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
