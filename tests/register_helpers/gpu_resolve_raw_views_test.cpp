// Headless descriptor-contract regression using the shipped depth resolve DXBC.
// No title commands, memory or input. --legacy reports the invalid old binding
// only as a diagnostic; its device-dependent result is not a semantic oracle.
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <d3d12shader.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <string>
#include "../../external/ReXGlue/src/graphics/shaders/bytecode/d3d12_5_1/resolve_fast_32bpp_1x2xmsaa_cs.h"
#include "../../external/ReXGlue/src/graphics/shaders/bytecode/d3d12_5_1/resolve_fast_32bpp_1x2xmsaa_scaled_cs.h"
#include "../../external/ReXGlue/src/graphics/shaders/bytecode/d3d12_5_1/resolve_fast_32bpp_4xmsaa_cs.h"
#include "../../external/ReXGlue/src/graphics/shaders/bytecode/d3d12_5_1/resolve_fast_32bpp_4xmsaa_scaled_cs.h"
#include "../../external/ReXGlue/src/graphics/shaders/bytecode/d3d12_5_1/resolve_fast_64bpp_1x2xmsaa_cs.h"
#include "../../external/ReXGlue/src/graphics/shaders/bytecode/d3d12_5_1/resolve_fast_64bpp_1x2xmsaa_scaled_cs.h"
#include "../../external/ReXGlue/src/graphics/shaders/bytecode/d3d12_5_1/resolve_fast_64bpp_4xmsaa_cs.h"
#include "../../external/ReXGlue/src/graphics/shaders/bytecode/d3d12_5_1/resolve_fast_64bpp_4xmsaa_scaled_cs.h"
#include "../../external/ReXGlue/src/graphics/shaders/bytecode/d3d12_5_1/resolve_full_8bpp_cs.h"
#include "../../external/ReXGlue/src/graphics/shaders/bytecode/d3d12_5_1/resolve_full_8bpp_scaled_cs.h"
#include "../../external/ReXGlue/src/graphics/shaders/bytecode/d3d12_5_1/resolve_full_16bpp_cs.h"
#include "../../external/ReXGlue/src/graphics/shaders/bytecode/d3d12_5_1/resolve_full_16bpp_scaled_cs.h"
#include "../../external/ReXGlue/src/graphics/shaders/bytecode/d3d12_5_1/resolve_full_32bpp_cs.h"
#include "../../external/ReXGlue/src/graphics/shaders/bytecode/d3d12_5_1/resolve_full_32bpp_scaled_cs.h"
#include "../../external/ReXGlue/src/graphics/shaders/bytecode/d3d12_5_1/resolve_full_64bpp_cs.h"
#include "../../external/ReXGlue/src/graphics/shaders/bytecode/d3d12_5_1/resolve_full_64bpp_scaled_cs.h"
#include "../../external/ReXGlue/src/graphics/shaders/bytecode/d3d12_5_1/resolve_full_128bpp_cs.h"
#include "../../external/ReXGlue/src/graphics/shaders/bytecode/d3d12_5_1/resolve_full_128bpp_scaled_cs.h"
using Microsoft::WRL::ComPtr;
static void check(bool b,const char* s) { if(!b) throw std::runtime_error(s); }
static void hr(HRESULT h,const char* s) { check(SUCCEEDED(h),s); }
static void verifyShaderViews(const void* code,SIZE_T size) {
  // Packaged shaders omit RDEF resource reflection. Inspect declarations from
  // the actual executable bytecode, not the human-readable header comment.
  ComPtr<ID3DBlob> assembly;hr(D3DDisassemble(code,size,0,nullptr,&assembly),"disassemble production DXBC");
  const std::string text(static_cast<const char*>(assembly->GetBufferPointer()),assembly->GetBufferSize());
  check(text.find("dcl_resource_raw T0[0:0], space=0")!=std::string::npos,"resolve source raw ABI");
  check(text.find("dcl_uav_raw U0[0:0], space=0")!=std::string::npos,"resolve destination raw ABI");
}
static ComPtr<ID3D12Resource> buffer(ID3D12Device* d,UINT size,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES heap{};heap.Type=type;heap.CreationNodeMask=heap.VisibleNodeMask=1;
  D3D12_RESOURCE_DESC r{};r.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;r.Width=size;
  r.Height=1;r.DepthOrArraySize=r.MipLevels=1;r.SampleDesc.Count=1;r.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if(type==D3D12_HEAP_TYPE_DEFAULT) r.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ComPtr<ID3D12Resource> out;hr(d->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&r,state,nullptr,IID_PPV_ARGS(&out)),"buffer");return out;
}
static UINT tiled(UINT x,UINT y,UINT pitch) {
  const UINT macro=((x>>5)+(y>>5)*(pitch>>5))<<9;
  const UINT micro=((x&7)+((y&14)<<2))<<2;
  const UINT q=macro+((micro&~15)<<1)+(micro&15)+((y&1)<<4);
  return ((q&~511)<<3)+((y&16)<<7)+((q&448)<<2)+(((((y&8)>>2)+(x>>3))&3)<<6)+(q&63);
}
static void run(ID3D12Device* device,bool rawSource,bool rawDest,UINT base,UINT msaa,UINT width,UINT height) {
  constexpr UINT sourceBytes=10*1024*1024,outputBytes=4*1024*1024,pattern=0x1234567f;
  auto source=buffer(device,sourceBytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
  auto output=buffer(device,outputBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto readback=buffer(device,outputBytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
  void* map=nullptr;D3D12_RANGE noRead{};hr(source->Map(0,&noRead,&map),"map source");
  std::fill_n(static_cast<UINT*>(map),sourceBytes/4,pattern);source->Unmap(0,nullptr);
  D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.NumDescriptors=2;hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> heap;hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"heap");
  const UINT step=device->GetDescriptorHandleIncrementSize(hd.Type);
  auto cpu=heap->GetCPUDescriptorHandleForHeapStart();
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
  srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Format=rawSource?DXGI_FORMAT_R32_TYPELESS:DXGI_FORMAT_R32G32B32A32_UINT;
  srv.Buffer.NumElements=sourceBytes/(rawSource?4:16);srv.Buffer.Flags=rawSource?D3D12_BUFFER_SRV_FLAG_RAW:D3D12_BUFFER_SRV_FLAG_NONE;
  device->CreateShaderResourceView(source.Get(),&srv,cpu);cpu.ptr+=step;
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
  uav.Format=rawDest?DXGI_FORMAT_R32_TYPELESS:DXGI_FORMAT_R32G32B32A32_UINT;
  uav.Buffer.NumElements=outputBytes/(rawDest?4:16);uav.Buffer.Flags=rawDest?D3D12_BUFFER_UAV_FLAG_RAW:D3D12_BUFFER_UAV_FLAG_NONE;
  device->CreateUnorderedAccessView(output.Get(),nullptr,&uav,cpu);
  D3D12_DESCRIPTOR_RANGE ranges[2]{};ranges[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV;ranges[0].NumDescriptors=ranges[1].NumDescriptors=1;
  D3D12_ROOT_PARAMETER params[3]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[0].Constants.Num32BitValues=5;
  for(UINT i=0;i<2;++i){params[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[i+1].DescriptorTable={1,&ranges[i]};}
  D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=3;rd.pParameters=params;ComPtr<ID3DBlob> bytes;
  hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&bytes,nullptr),"serialize");
  ComPtr<ID3D12RootSignature> root;hr(device->CreateRootSignature(0,bytes->GetBufferPointer(),bytes->GetBufferSize(),IID_PPV_ARGS(&root)),"root");
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={resolve_fast_32bpp_1x2xmsaa_cs,sizeof(resolve_fast_32bpp_1x2xmsaa_cs)};
  ComPtr<ID3D12PipelineState> pipeline;hr(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)),"pipeline");
  ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC qd{};hr(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)),"queue");
  ComPtr<ID3D12CommandAllocator> alloc;hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc)),"allocator");
  ComPtr<ID3D12GraphicsCommandList> list;hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),pipeline.Get(),IID_PPV_ARGS(&list)),"list");
  ID3D12DescriptorHeap* heaps[]={heap.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(root.Get());
  const UINT pitch=width/80;
  const UINT constants[]={pitch|(msaa<<10)|(1u<<12)|(base<<13)|(1u<<24),
    (width/8)<<5,2u|(23u<<7),(width/32)|(((height+31)/32)<<10),0};
  list->SetComputeRoot32BitConstants(0,5,constants,0);
  auto gpu=heap->GetGPUDescriptorHandleForHeapStart();list->SetComputeRootDescriptorTable(1,gpu);gpu.ptr+=step;list->SetComputeRootDescriptorTable(2,gpu);
  list->Dispatch((width+63)/64,(height+7)/8,1);
  D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={output.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};
  list->ResourceBarrier(1,&b);list->CopyBufferRegion(readback.Get(),0,output.Get(),0,outputBytes);hr(list->Close(),"close");
  ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);
  ComPtr<ID3D12Fence> fence;hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");hr(queue->Signal(fence.Get(),1),"signal");
  HANDLE event=CreateEvent(nullptr,FALSE,FALSE,nullptr);check(event!=nullptr,"event");hr(fence->SetEventOnCompletion(1,event),"wait event");
  const auto wait=WaitForSingleObject(event,30000);CloseHandle(event);check(wait==WAIT_OBJECT_0&&fence->GetCompletedValue()>=1,"timeout");
  D3D12_RANGE range{0,outputBytes};hr(readback->Map(0,&range,&map),"readback");
  UINT mismatches=0,zeros=0,firstBad=height;auto values=static_cast<const UINT*>(map);
  for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){UINT word=values[tiled(x,y,width)/4];if(word!=0x7f563412){++mismatches;firstBad=(std::min)(firstBad,y);}zeros+=word==0;}
  readback->Unmap(0,&noRead);hr(device->GetDeviceRemovedReason(),"removed");
  std::cout<<"raw_source="<<rawSource<<" raw_dest="<<rawDest<<" base="<<base<<" msaa="<<msaa<<" mismatch="<<mismatches<<" zeros="<<zeros<<" first_bad_row="<<firstBad<<'\n';
  if(rawSource&&rawDest)check(mismatches==0,"raw resolve output differs");
}
int main(int argc,char** argv){try{
  #define VERIFY_PAIR(name) verifyShaderViews(name##_cs,sizeof(name##_cs)); verifyShaderViews(name##_scaled_cs,sizeof(name##_scaled_cs));
  VERIFY_PAIR(resolve_fast_32bpp_1x2xmsaa)
  VERIFY_PAIR(resolve_fast_32bpp_4xmsaa)
  VERIFY_PAIR(resolve_fast_64bpp_1x2xmsaa)
  VERIFY_PAIR(resolve_fast_64bpp_4xmsaa)
  VERIFY_PAIR(resolve_full_8bpp)
  VERIFY_PAIR(resolve_full_16bpp)
  VERIFY_PAIR(resolve_full_32bpp)
  VERIFY_PAIR(resolve_full_64bpp)
  VERIFY_PAIR(resolve_full_128bpp)
  #undef VERIFY_PAIR
  std::cout<<"18 embedded resolve-copy shader raw SRV/UAV contracts PASS\n";
  const bool legacy=argc>1&&std::strcmp(argv[1],"--legacy")==0;
  ComPtr<ID3D12Device> device;hr(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"device");
  for(bool rawSource:{true,false})for(bool rawDest:{true,false}){
    if(!legacy&&(!rawSource||!rawDest))continue;
    run(device.Get(),rawSource,rawDest,0,1,1280,384);
    run(device.Get(),rawSource,rawDest,1536,0,800,800);
  }
  return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
