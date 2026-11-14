#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <cstdio>
#include <cstring>
#include "../GpuPanelPreprocessor.h"
using namespace novabar;
using Microsoft::WRL::ComPtr;
namespace {
int checks=0,failures=0;
void Check(bool ok,char const* name){++checks;if(!ok)++failures;printf("%s: %s\n",ok?"PASS":"FAIL",name);}
void Hr(HRESULT hr){if(FAILED(hr))throw hr;}
struct Texture
{
 ComPtr<ID3D11Texture2D> Pixels;ComPtr<ID3D11ShaderResourceView> View;ComPtr<ID3D11RenderTargetView> Target;
};
Texture Make(ID3D11Device* device,int width,int height,void const* bytes=nullptr,DXGI_FORMAT format=DXGI_FORMAT_B8G8R8A8_UNORM)
{
 D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=1;d.Format=format;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
 D3D11_SUBRESOURCE_DATA initial{bytes,UINT(width*4),0};Texture t;Hr(device->CreateTexture2D(&d,bytes?&initial:nullptr,&t.Pixels));Hr(device->CreateShaderResourceView(t.Pixels.Get(),nullptr,&t.View));Hr(device->CreateRenderTargetView(t.Pixels.Get(),nullptr,&t.Target));return t;
}
std::vector<uint8_t> Read(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* texture)
{
 D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
 ComPtr<ID3D11Texture2D> staging;Hr(device->CreateTexture2D(&d,nullptr,&staging));context->CopyResource(staging.Get(),texture);D3D11_MAPPED_SUBRESOURCE m{};Hr(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&m));
 std::vector<uint8_t> result(size_t(d.Width)*d.Height*4);for(UINT y=0;y<d.Height;++y)memcpy(result.data()+size_t(y)*d.Width*4,static_cast<uint8_t const*>(m.pData)+size_t(y)*m.RowPitch,size_t(d.Width)*4);context->Unmap(staging.Get(),0);return result;
}
}
int main()
{
 setvbuf(stdout,nullptr,_IONBF,0);
 try
 {
  auto shared=GpuPanelPreprocessor::CreateSharedDevice();Check(bool(shared),"hardware preprocessor device initializes");if(!shared)return 1;
  auto* device=shared->Device.Get();auto* context=shared->Context.Get();GpuPanelPreprocessor gpu(shared);
  PixelFrame geometry;geometry.Width=128;geometry.Height=96;geometry.Bounds={-64,-48,64,48};
  std::vector<uint8_t> source(size_t(geometry.Width)*geometry.Height*4);
  for(size_t p=0;p<source.size()/4;++p){uint32_t h=uint32_t(p)*374761393u+668265263u;const uint8_t a=uint8_t(30+h%226);source[p*4]=uint8_t(h%unsigned(a+1));source[p*4+1]=uint8_t((h>>8)%unsigned(a+1));source[p*4+2]=uint8_t((h>>16)%unsigned(a+1));source[p*4+3]=a;}
  auto input=Make(device,128,96,source.data());auto first=Make(device,64,48);auto second=Make(device,64,48);
  PanelGlassOptions options;options.Canvas=PixelRect{-32,-24,32,24};options.Viewport=geometry.Bounds;options.ClipRounded=false;options.Radius=options.Specular=options.Material.Lens=options.Material.Dispersion=0;
  options.Material.Blur=14;options.Material.Tint=.55;options.Material.Noise=.12;options.Material.Color=0x26000000;
  const PixelRect target{-28,-20,28,20};std::vector<uint8_t> expected;
  Check(gpu.RenderTexture(input.View.Get(),geometry,target,64,48,options,expected),"existing GPU readback establishes genuine base oracle");
  Check(gpu.RenderTextureToTarget(input.View.Get(),geometry,target,64,48,options,first.Target.Get()),"GPU-only output accepts caller-owned target and no CPU source pixels");
  auto rendered=Read(device,context,first.Pixels.Get());Check(rendered==expected,"GPU-only output exactly matches existing filtered BGRA base");
  const auto retained=rendered;const float solid[4]{.15f,.65f,.25f,1};context->ClearRenderTargetView(input.Target.Get(),solid);
  Check(gpu.RenderTextureToTarget(input.View.Get(),geometry,target,64,48,options,second.Target.Get()),"next source renders into a separate caller-owned target");
  Check(Read(device,context,first.Pixels.Get())==retained,"later render preserves a retained output generation");
  Check(Read(device,context,second.Pixels.Get())!=retained,"new output contains changed source content");
  auto verifyReject=[&](ID3D11RenderTargetView* output,PixelFrame const& base,char const* name){
   const auto before=Read(device,context,second.Pixels.Get());const bool ok=gpu.RenderTextureToTarget(input.View.Get(),base,target,64,48,options,output);
   Check(!ok&&FAILED(gpu.LastError()),name);Check(Read(device,context,second.Pixels.Get())==before,"rejection leaves previous valid output unchanged");
  };
  auto undersized=Make(device,32,24);verifyReject(undersized.Target.Get(),geometry,"target extent mismatch is rejected");
  auto rgba=Make(device,64,48,nullptr,DXGI_FORMAT_R8G8B8A8_UNORM);verifyReject(rgba.Target.Get(),geometry,"target format mismatch is rejected");
  verifyReject(nullptr,geometry,"null output view is rejected");
  PixelFrame wrong=geometry;wrong.Width=64;verifyReject(second.Target.Get(),wrong,"source geometry mismatch is rejected");
  auto alias=Make(device,128,96,source.data());PanelGlassOptions aliasOptions=options;aliasOptions.Canvas=geometry.Bounds;
  const auto aliasBefore=Read(device,context,alias.Pixels.Get());Check(!gpu.RenderTextureToTarget(alias.View.Get(),geometry,target,128,96,aliasOptions,alias.Target.Get()),"source/output alias is rejected before drawing");Check(Read(device,context,alias.Pixels.Get())==aliasBefore,"alias rejection never mutates source pixels");
  auto other=GpuPanelPreprocessor::CreateSharedDevice();Check(bool(other),"second hardware device initializes");if(other){auto foreign=Make(other->Device.Get(),64,48);verifyReject(foreign.Target.Get(),geometry,"foreign-device target is rejected");}
  D3D11_TEXTURE2D_DESC sharedDesc{};input.Pixels->GetDesc(&sharedDesc);sharedDesc.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
  Texture exported;Hr(device->CreateTexture2D(&sharedDesc,nullptr,&exported.Pixels));Hr(device->CreateShaderResourceView(exported.Pixels.Get(),nullptr,&exported.View));
  const bool rejectedShared=!gpu.RenderTextureToTarget(exported.View.Get(),geometry,target,64,48,options,second.Target.Get());
  Check(rejectedShared,"GPU-only scene input must be private rather than shared under another object identity");
  if(rejectedShared){
   ComPtr<IDXGIResource1> share;Hr(exported.Pixels.As(&share));HANDLE nt=nullptr;Hr(share->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&nt));
   ComPtr<ID3D11Device1> device1;Hr(shared->Device.As(&device1));ComPtr<ID3D11Texture2D> reopened;const auto opened=device1->OpenSharedResource1(nt,IID_PPV_ARGS(&reopened));CloseHandle(nt);Hr(opened);
   ComPtr<ID3D11RenderTargetView> aliasTarget;Hr(device->CreateRenderTargetView(reopened.Get(),nullptr,&aliasTarget));
   ComPtr<IUnknown> firstObject,secondObject;Hr(exported.Pixels.As(&firstObject));Hr(reopened.As(&secondObject));Check(firstObject.Get()!=secondObject.Get(),"shared allocation can reopen with distinct COM identity");
   Check(!gpu.RenderTextureToTarget(exported.View.Get(),geometry,target,128,96,aliasOptions,aliasTarget.Get()),"distinct-object shared alias rejects before feedback draw");
  }
  PanelGlassOptions clipped=options;clipped.Viewport=PixelRect{100,100,200,200};
  const float sentinel[4]{.25f,.5f,.75f,1};context->ClearRenderTargetView(second.Target.Get(),sentinel);
  D3D11_QUERY_DESC predicateDesc{};predicateDesc.Query=D3D11_QUERY_OCCLUSION_PREDICATE;ComPtr<ID3D11Predicate> predicate;
  Hr(device->CreatePredicate(&predicateDesc,&predicate));context->Begin(predicate.Get());context->End(predicate.Get());
  BOOL predicateValue=TRUE;HRESULT predicateResult=S_FALSE;
  for(int i=0;i<1000&&predicateResult==S_FALSE;++i){predicateResult=context->GetData(predicate.Get(),&predicateValue,sizeof(predicateValue),0);if(predicateResult==S_FALSE)Sleep(1);}
  Hr(predicateResult);Check(predicateResult==S_OK&&!predicateValue,"empty viewport regression uses a completed false occlusion predicate");
  context->SetPredication(predicate.Get(),FALSE);
  Check(gpu.RenderTextureToTarget(input.View.Get(),geometry,target,64,48,clipped,second.Target.Get()),"empty visible viewport produces a defined transparent target under false predication");
  context->SetPredication(nullptr,FALSE); // Readback must run unconditionally, independently of the tested clear.
  const auto empty=Read(device,context,second.Pixels.Get());Check(std::all_of(empty.begin(),empty.end(),[](uint8_t b){return b==0;}),"empty viewport clears all caller target pixels under false predication");
  Check(gpu.RenderTexture(input.View.Get(),geometry,target,64,48,options,expected)&&!expected.empty(),"CPU readback API still works after GPU-only calls");
  Check(gpu.RenderTextureToTarget(input.View.Get(),geometry,target,64,48,options,second.Target.Get())&&Read(device,context,second.Pixels.Get())==expected,"GPU-only API remains correct after CPU/GPU backend switches");
  Check(SUCCEEDED(device->GetDeviceRemovedReason()),"GPU device remains healthy");
 }
 catch(HRESULT hr){printf("HRESULT=%08x\n",unsigned(hr));++failures;}
 catch(...){printf("Unexpected GPU test failure\n");++failures;}
 printf("GPU texture output: %d checks, %d failures; readback exists only in this oracle fixture.\n",checks,failures);return failures?1:0;
}
