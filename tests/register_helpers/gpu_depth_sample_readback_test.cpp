// Independent D3D12 WARP shader validation, never submitted to the title.
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <cstring>
#include <memory>
#include "../../external/ReXGlue/src/graphics/shaders/bytecode/d3d12_5_1/diagnostic_depth_samples_single.h"
#include "../../external/ReXGlue/src/graphics/shaders/bytecode/d3d12_5_1/diagnostic_depth_samples_msaa.h"
using Microsoft::WRL::ComPtr;
static void check(bool v, const char* s) { if (!v) throw std::runtime_error(s); }
static void hr(HRESULT v, const char* s) { check(SUCCEEDED(v), s); }
static void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r,
                    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
  list->ResourceBarrier(1, &b);
}
static void run(ID3D12Device* device, UINT samples) {
  // Odd dimensions exercise final partial thread groups; y=256 crosses the
  // observed boundary, but test values are artificial, not guest evidence.
  constexpr UINT width=17, height=385;
  D3D12_RESOURCE_DESC d{};
  d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=width; d.Height=height;
  d.DepthOrArraySize=d.MipLevels=1; d.SampleDesc.Count=samples;
  d.Format=DXGI_FORMAT_R32G8X24_TYPELESS;
  d.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask=heap.VisibleNodeMask=1;
  ComPtr<ID3D12Resource> depth, output, readback;
  hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,
      D3D12_RESOURCE_STATE_DEPTH_WRITE,nullptr,IID_PPV_ARGS(&depth)),"depth");
  const UINT64 bytes=UINT64(width)*height*samples*8;
  D3D12_RESOURCE_DESC b{}; b.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
  b.Width=bytes; b.Height=1; b.DepthOrArraySize=b.MipLevels=1;
  b.SampleDesc.Count=1; b.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  b.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&b,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&output)),"output");
  heap.Type=D3D12_HEAP_TYPE_READBACK; b.Flags=D3D12_RESOURCE_FLAG_NONE;
  hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&b,
      D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"readback");
  D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.NumDescriptors=1; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  ComPtr<ID3D12DescriptorHeap> dsvs,srvs;
  hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&dsvs)),"DSV heap");
  D3D12_DEPTH_STENCIL_VIEW_DESC dv{}; dv.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  dv.ViewDimension=samples>1?D3D12_DSV_DIMENSION_TEXTURE2DMS:D3D12_DSV_DIMENSION_TEXTURE2D;
  device->CreateDepthStencilView(depth.Get(),&dv,dsvs->GetCPUDescriptorHandleForHeapStart());
  hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors=2;
  hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&srvs)),"SRV heap");
  auto handle=srvs->GetCPUDescriptorHandleForHeapStart();
  for(UINT plane=0;plane<2;++plane) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format=plane?DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    sv.ViewDimension=samples>1?D3D12_SRV_DIMENSION_TEXTURE2DMS:D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if(samples==1) { sv.Texture2D.MipLevels=1; sv.Texture2D.PlaneSlice=plane; }
    device->CreateShaderResourceView(depth.Get(),&sv,handle);
    handle.ptr+=device->GetDescriptorHandleIncrementSize(hd.Type);
  }
  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  D3D12_ROOT_PARAMETER rp[4]{};
  rp[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants.Num32BitValues=3;
  for(UINT i=0;i<2;++i) {
    ranges[i].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[i].NumDescriptors=1;
    ranges[i].BaseShaderRegister=i;
    rp[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[i+1].DescriptorTable={1,&ranges[i]};
  }
  rp[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
  D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters=4; rd.pParameters=rp;
  ComPtr<ID3DBlob> serialized,code,error;
  hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error),"root bytes");
  ComPtr<ID3D12RootSignature> root;
  hr(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),
      IID_PPV_ARGS(&root)),"root");
  const std::string path=std::string(DARKNESS_SOURCE_ROOT)+
      "/external/ReXGlue/src/graphics/shaders/diagnostic_depth_samples.hlsl";
  D3D_SHADER_MACRO macros[]={{"SINGLE_SAMPLE",samples==1?"1":"0"},{nullptr,nullptr}};
  const auto compiled=D3DCompileFromFile(std::wstring(path.begin(),path.end()).c_str(),macros,
      nullptr,"main","cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
  if(FAILED(compiled)&&error) std::cerr<<static_cast<char*>(error->GetBufferPointer());
  hr(compiled,"compile diagnostic shader");
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature=root.Get();
  // Exercise the exact embedded bytecode used by the runtime, not only a
  // freshly compiled test variant. HLSL compilation above is a source check.
  pd.CS=samples==1
      ? D3D12_SHADER_BYTECODE{diagnostic_depth_samples_single,sizeof(diagnostic_depth_samples_single)}
      : D3D12_SHADER_BYTECODE{diagnostic_depth_samples_msaa,sizeof(diagnostic_depth_samples_msaa)};
  ComPtr<ID3D12PipelineState> pipeline;
  hr(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)),"pipeline");
  // Distinct per-sample depth values reject accidental averaging or reading
  // sample zero repeatedly. This synthetic draw belongs only to this test.
  ComPtr<ID3DBlob> vs,ps;
  const char* vs_text="float4 main(uint i:SV_VertexID):SV_Position { return float4(i==1?3:-1,i==2?3:-1,0,1); }";
  const char* ps_text="float main(uint s:SV_SampleIndex):SV_Depth { return (s+1)*0.125; }";
  hr(D3DCompile(vs_text,std::strlen(vs_text),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&vs,&error),"test VS");
  hr(D3DCompile(ps_text,std::strlen(ps_text),nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&ps,&error),"test PS");
  D3D12_GRAPHICS_PIPELINE_STATE_DESC gp{}; gp.pRootSignature=root.Get();
  gp.VS={vs->GetBufferPointer(),vs->GetBufferSize()}; gp.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
  gp.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; gp.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
  gp.RasterizerState.DepthClipEnable=TRUE; gp.SampleMask=UINT_MAX; gp.SampleDesc.Count=samples;
  gp.DepthStencilState.DepthEnable=TRUE; gp.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;
  gp.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
  gp.DSVFormat=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  gp.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  ComPtr<ID3D12PipelineState> draw_pipeline;
  hr(device->CreateGraphicsPipelineState(&gp,IID_PPV_ARGS(&draw_pipeline)),"sample draw pipeline");
  ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC qd{};
  hr(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)),"queue");
  ComPtr<ID3D12CommandAllocator> allocator;
  hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");
  ComPtr<ID3D12GraphicsCommandList> list;
  hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,
      IID_PPV_ARGS(&list)),"list");
  const auto dh=dsvs->GetCPUDescriptorHandleForHeapStart();
  const auto clear=D3D12_CLEAR_FLAG_DEPTH|D3D12_CLEAR_FLAG_STENCIL;
  list->ClearDepthStencilView(dh,clear,.25f,127,0,nullptr);
  list->SetGraphicsRootSignature(root.Get()); list->SetPipelineState(draw_pipeline.Get());
  list->OMSetRenderTargets(0,nullptr,FALSE,&dh);
  D3D12_VIEWPORT vp{0,0,float(width),float(height),0,1};
  D3D12_RECT scissor{0,0,width,height};
  list->RSSetViewports(1,&vp); list->RSSetScissorRects(1,&scissor);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->DrawInstanced(3,1,0,0);
  D3D12_RECT stripe{0,256,width,height};
  list->ClearDepthStencilView(dh,clear,.75f,33,1,&stripe);
  barrier(list.Get(),depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  ID3D12DescriptorHeap* heaps[]{srvs.Get()}; list->SetDescriptorHeaps(1,heaps);
  list->SetComputeRootSignature(root.Get()); list->SetPipelineState(pipeline.Get());
  UINT constants[]{width,height,samples}; list->SetComputeRoot32BitConstants(0,3,constants,0);
  list->SetComputeRootDescriptorTable(1,srvs->GetGPUDescriptorHandleForHeapStart());
  auto stencil_handle=srvs->GetGPUDescriptorHandleForHeapStart();
  stencil_handle.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  list->SetComputeRootDescriptorTable(2,stencil_handle);
  list->SetComputeRootUnorderedAccessView(3,output->GetGPUVirtualAddress());
  list->Dispatch((width+7)/8,(height+7)/8,samples);
  barrier(list.Get(),depth.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_DEPTH_WRITE);
  barrier(list.Get(),output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(),0,output.Get(),0,bytes);
  hr(list->Close(),"close"); ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1,lists);
  ComPtr<ID3D12Fence> fence; hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
  hr(queue->Signal(fence.Get(),1),"signal"); HANDLE event=CreateEvent(nullptr,FALSE,FALSE,nullptr);
  check(event!=nullptr,"event"); hr(fence->SetEventOnCompletion(1,event),"completion");
  auto waited=WaitForSingleObject(event,30000); CloseHandle(event);
  check(waited==WAIT_OBJECT_0&&fence->GetCompletedValue()>=1,"fence wait");
  void* data=nullptr; D3D12_RANGE r{0,SIZE_T(bytes)};
  hr(readback->Map(0,&r,&data),"map"); auto values=static_cast<const UINT*>(data);
  for(UINT y=0;y<height;++y) for(UINT x=0;x<width;++x) for(UINT s=0;s<samples;++s) {
    const auto i=((y*width+x)*samples+s)*2;
    float expected=y<256?float(s+1)*.125f:.75f;
    UINT expected_bits; std::memcpy(&expected_bits,&expected,sizeof(expected_bits));
    check(values[i]==expected_bits,"depth sample bits/coverage");
    check(values[i+1]==(y<256?127u:33u),"stencil plane/coverage");
  }
  D3D12_RANGE no_write{}; readback->Unmap(0,&no_write);
  hr(device->GetDeviceRemovedReason(),"device removed");
  std::cout<<"PASS depth+stencil extraction samples="<<samples<<" all rows including y256..384\n";
}
static void run_snapshot(ID3D12Device* device) {
  constexpr UINT width=17, height=385;
  D3D12_RESOURCE_DESC d{};
  d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=width; d.Height=height;
  d.DepthOrArraySize=d.MipLevels=1; d.SampleDesc.Count=1; d.Format=DXGI_FORMAT_R32_FLOAT;
  D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask=heap.VisibleNodeMask=1;
  ComPtr<ID3D12Resource> source,snapshot,upload,readback;
  for(auto* resource:{std::addressof(source),std::addressof(snapshot)})
    hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,
        D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(resource->GetAddressOf())),"snapshot texture");
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
  UINT rows{}; UINT64 row_bytes{},total{};
  device->GetCopyableFootprints(&d,0,1,0,&fp,&rows,&row_bytes,&total);
  const UINT64 stride=(total+511)&~UINT64(511);
  D3D12_RESOURCE_DESC b{}; b.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
  b.Width=stride*2; b.Height=1; b.DepthOrArraySize=b.MipLevels=1;
  b.SampleDesc.Count=1; b.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  heap.Type=D3D12_HEAP_TYPE_UPLOAD;
  hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&b,
      D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)),"snapshot upload");
  heap.Type=D3D12_HEAP_TYPE_READBACK;
  hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&b,
      D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"snapshot readback");
  void* mapped=nullptr; D3D12_RANGE no_read{};
  hr(upload->Map(0,&no_read,&mapped),"snapshot upload map");
  for(UINT y=0;y<height;++y) for(UINT x=0;x<width;++x) {
    const float original=float((y*width+x)%1024)/1024.0f, replacement=.875f;
    auto* row=static_cast<unsigned char*>(mapped)+y*fp.Footprint.RowPitch+x*4;
    std::memcpy(row,&original,4); std::memcpy(row+stride,&replacement,4);
  }
  upload->Unmap(0,nullptr);
  ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC qd{};
  hr(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)),"snapshot queue");
  ComPtr<ID3D12CommandAllocator> allocator;
  hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"snapshot allocator");
  ComPtr<ID3D12GraphicsCommandList> list;
  hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,
      IID_PPV_ARGS(&list)),"snapshot list");
  D3D12_TEXTURE_COPY_LOCATION src{},snap{},up{},out{};
  src.pResource=source.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  snap.pResource=snapshot.Get(); snap.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  up.pResource=upload.Get(); up.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; up.PlacedFootprint=fp;
  out.pResource=readback.Get(); out.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; out.PlacedFootprint=fp;
  list->CopyTextureRegion(&src,0,0,0,&up,nullptr);
  barrier(list.Get(),source.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyTextureRegion(&snap,0,0,0,&src,nullptr);
  barrier(list.Get(),snapshot.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
  barrier(list.Get(),source.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
  up.PlacedFootprint.Offset=stride;
  list->CopyTextureRegion(&src,0,0,0,&up,nullptr);
  barrier(list.Get(),source.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyTextureRegion(&out,0,0,0,&snap,nullptr);
  out.PlacedFootprint.Offset=stride;
  list->CopyTextureRegion(&out,0,0,0,&src,nullptr);
  hr(list->Close(),"snapshot close"); ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1,lists);
  ComPtr<ID3D12Fence> fence; hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"snapshot fence");
  hr(queue->Signal(fence.Get(),1),"snapshot signal");
  HANDLE event=CreateEvent(nullptr,FALSE,FALSE,nullptr); check(event!=nullptr,"snapshot event");
  hr(fence->SetEventOnCompletion(1,event),"snapshot completion");
  auto waited=WaitForSingleObject(event,30000); CloseHandle(event);
  check(waited==WAIT_OBJECT_0 && fence->GetCompletedValue()>=1,"snapshot fence wait");
  D3D12_RANGE range{0,SIZE_T(stride*2)};
  hr(readback->Map(0,&range,&mapped),"snapshot map");
  for(UINT y=0;y<height;++y) for(UINT x=0;x<width;++x) {
    const auto* row=static_cast<const unsigned char*>(mapped)+y*fp.Footprint.RowPitch+x*4;
    float original{},replacement{}; std::memcpy(&original,row,4); std::memcpy(&replacement,row+stride,4);
    check(original==float((y*width+x)%1024)/1024.0f,"snapshot mutated with source");
    check(replacement==.875f,"source replacement did not execute");
  }
  D3D12_RANGE no_write{}; readback->Unmap(0,&no_write);
  hr(device->GetDeviceRemovedReason(),"snapshot device removed");
  std::cout<<"PASS R32 GPU snapshot survives source overwrite, 6545 values, padded rows, fence completed\n";
}
int main() {
  try {
    ComPtr<IDXGIFactory4> factory; hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");
    ComPtr<IDXGIAdapter> warp; hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)),"WARP");
    ComPtr<ID3D12Device> device; hr(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"device");
    run_snapshot(device.Get());
    for(UINT s:{1u,2u,4u}) {
      D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{};
      q.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT; q.SampleCount=s;
      hr(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,&q,sizeof(q)),"support");
      if(!q.NumQualityLevels) { check(s==2,"required MSAA unsupported"); std::cout<<"2x unavailable on WARP\n"; continue; }
      run(device.Get(),s);
    }
    return 0;
  } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
