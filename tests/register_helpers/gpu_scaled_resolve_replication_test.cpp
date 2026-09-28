// Headless GPU regression. Synthetic byte patterns only, never submitted to a
// title. Executes the SAME compiled copy shaders shipped in rexgpu-xenos.dll.
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "../../external/rexglue/src/graphics/shaders/bytecode/d3d12_5_1/scaled_resolve_initialize_cs.h"
#include "../../external/rexglue/src/graphics/shaders/bytecode/d3d12_5_1/resolve_downscale_cs.h"
using Microsoft::WRL::ComPtr;
static void check(bool b, const char* msg) { if (!b) throw std::runtime_error(msg); }
static void hr(HRESULT h, const char* msg) { check(SUCCEEDED(h), msg); }

// Independent full Xenos 2D tiling + pinned Xenia rectangular HOST STORAGE
// addressing. Expected replication is derived from host(x,y)/scale, not from
// the implementation's group-local initialization or downscale helpers.
static uint32_t tiled(uint32_t x,uint32_t y,uint32_t b) {
  uint32_t outer=(((y>>5)*4)+(x>>5))<<6;
  uint32_t inner=(((y>>1)&7)<<3)|(x&7);
  uint32_t q=(outer|inner)<<b,bank=(y>>4)&1,pipe=((x>>3)&3)^(((y>>3)&1)<<1);
  return ((y&1)<<4)|(pipe<<6)|(bank<<11)|(q&15)|(((q>>4)&1)<<5)|(((q>>5)&7)<<8)|(q>>8<<12);
}
static uint32_t scaled(uint32_t x,uint32_t y,uint32_t b,uint32_t sx,uint32_t sy) {
  uint32_t gx=b>=3?5-b:4,gy=3-(b<2?b:2),size=1u<<(gx+gy+b);
  uint32_t groupx=(x>>gx)/sx,groupy=(y>>gy)/sy;
  uint32_t localgroupx=(x>>gx)-groupx*sx,localgroupy=(y>>gy)-groupy*sy;
  return tiled(groupx<<gx,groupy<<gy,b)*sx*sy+(localgroupx*sy+localgroupy)*size+
      (((y&((1u<<gy)-1))<<gx)+(x&((1u<<gx)-1)))*(1u<<b);
}
struct Gpu {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3D12PipelineState> initialize,downscale;
  Gpu() {
    hr(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"D3D12 device");
    D3D12_COMMAND_QUEUE_DESC q{};
    hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"queue");
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues=5;
    params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=3;desc.pParameters=params;
    ComPtr<ID3DBlob> data;
    hr(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&data,nullptr),"root bytes");
    hr(device->CreateRootSignature(0,data->GetBufferPointer(),data->GetBufferSize(),IID_PPV_ARGS(&root)),"root");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};pso.pRootSignature=root.Get();
    pso.CS={scaled_resolve_initialize_cs,sizeof(scaled_resolve_initialize_cs)};
    hr(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&initialize)),"production initialization PSO");
    pso.CS={resolve_downscale_cs,sizeof(resolve_downscale_cs)};
    hr(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&downscale)),"production downscale PSO");
  }
  ComPtr<ID3D12Resource> buffer(size_t bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap{};heap.Type=type;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;
    d.Height=1;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if(type==D3D12_HEAP_TYPE_DEFAULT)d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)),"buffer");
    return r;
  }
  std::vector<uint8_t> run(ID3D12PipelineState* shader,const std::vector<uint8_t>& input,
      const std::array<uint32_t,5>& constants,uint32_t length,uint32_t groups) {
    auto upload=buffer(input.size(),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    auto output=buffer(length,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto readback=buffer(length,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    void* mapping=nullptr;D3D12_RANGE no_read{};
    hr(upload->Map(0,&no_read,&mapping),"upload map");std::memcpy(mapping,input.data(),input.size());upload->Unmap(0,nullptr);
    ComPtr<ID3D12CommandAllocator> alloc;
    hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc)),"allocator");
    ComPtr<ID3D12GraphicsCommandList> list;
    hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),shader,IID_PPV_ARGS(&list)),"list");
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRoot32BitConstants(0,5,constants.data(),0);
    list->SetComputeRootShaderResourceView(1,upload->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(2,output->GetGPUVirtualAddress());
    list->Dispatch(groups,1,1);
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource=output.Get();barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1,&barrier);list->CopyBufferRegion(readback.Get(),0,output.Get(),0,length);
    hr(list->Close(),"close");ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);
    ComPtr<ID3D12Fence> fence;hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
    hr(queue->Signal(fence.Get(),1),"signal");
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(event!=nullptr,"event");
    hr(fence->SetEventOnCompletion(1,event),"completion event");
    DWORD waited=WaitForSingleObject(event,10000);CloseHandle(event);check(waited==WAIT_OBJECT_0,"GPU timeout");
    D3D12_RANGE range{0,length};hr(readback->Map(0,&range,&mapping),"readback map");
    std::vector<uint8_t> bytes(length);std::memcpy(bytes.data(),mapping,length);readback->Unmap(0,&no_read);
    return bytes;
  }
};
static uint8_t pattern(uint32_t i) { return uint8_t((i*73u)^(i>>3)^(i>>11)^0xA7u); }
static void run_case(Gpu& gpu,uint32_t b,uint32_t sx,uint32_t sy) {
  uint32_t bytes=1u<<b,native_length=128*128*bytes,host_length=native_length*sx*sy;
  constexpr uint32_t prefix=4096;
  std::vector<uint8_t> source(prefix+native_length);
  for(uint32_t i=0;i<source.size();++i)source[i]=pattern(i);
  auto actual=gpu.run(gpu.initialize.Get(),source,{prefix,native_length,sx,sy,b},host_length,(host_length+1023)/1024);
  for(uint32_t y=0;y<128*sy;++y)for(uint32_t x=0;x<128*sx;++x)for(uint32_t byte=0;byte<bytes;++byte) {
    uint32_t dst=scaled(x,y,b,sx,sy)+byte,src=tiled(x/sx,y/sy,b)+byte;
    check(dst<actual.size() && prefix+src<source.size(),"fixture address range");
    if(actual[dst]!=source[prefix+src]) {std::cerr<<"initialize "<<b<<" "<<sx<<"x"<<sy<<" @"<<x<<","<<y<<" byte"<<byte<<"\n";throw std::runtime_error("GPU texel replication");}
  }
  if(b>3)return; // Production CPU-visible downscale supports up to64bpp.
  // Independent host data, not merely round-tripping the initializer.
  for(uint32_t i=0;i<actual.size();++i)actual[i]=pattern(i+257);
  for(uint32_t center=0;center<2;++center) {
    auto native=gpu.run(gpu.downscale.Get(),actual,{sx,sy,b,native_length/(1024*bytes),center},native_length,native_length/(1024*bytes));
    uint32_t ox=center?sx/2:0,oy=center?sy/2:0;
    for(uint32_t y=0;y<128;++y)for(uint32_t x=0;x<128;++x)for(uint32_t byte=0;byte<bytes;++byte) {
      uint32_t dst=tiled(x,y,b)+byte,src=scaled(x*sx+ox,y*sy+oy,b,sx,sy)+byte;
      if(native[dst]!=actual[src]){std::cerr<<"downscale "<<b<<" "<<sx<<"x"<<sy<<" center"<<center<<" @"<<x<<","<<y<<"\n";throw std::runtime_error("GPU selected subpixel");}
    }
  }
}
int main() {
  try {
    Gpu gpu;
    for(uint32_t b=0;b<=4;++b)for(auto scale:{std::array<uint32_t,2>{1,1},{2,2},{3,3},{2,3}})
      run_case(gpu,b,scale[0],scale[1]);
    std::cout<<"Production GPU replication20 and independent downscale32 cases PASS\n";
    return 0;
  } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
