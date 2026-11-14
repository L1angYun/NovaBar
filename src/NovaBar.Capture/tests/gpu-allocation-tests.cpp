#include "../GpuSceneCompositor.h"
#include "../../NovaBar.XamlBridge/WgcCapture.h"
#include "FailingD3D11Device.h"
#include <cstdio>
using namespace novabar;
using Microsoft::WRL::ComPtr;
int main() {
 setvbuf(stdout,nullptr,_IONBF,0);
 int checks=0,failed=0;auto check=[&](bool ok,char const* name){++checks;if(!ok)++failed;printf("%s: %s\n",ok?"PASS":"FAIL",name);};
 auto shared=GpuPanelPreprocessor::CreateSharedDevice();check(bool(shared),"real hardware device initializes");if(!shared)return 1;
 ComPtr<ID3D11Device> real=shared->Device;ComPtr<FailingD3D11Device> faults;faults.Attach(new FailingD3D11Device(real.Get()));
 const PixelRect oldBounds{0,0,8,6},newBounds{0,0,11,9};
 D3D11_TEXTURE2D_DESC source{};source.Width=8;source.Height=6;source.MipLevels=source.ArraySize=1;source.Format=DXGI_FORMAT_B8G8R8A8_UNORM;source.SampleDesc.Count=1;source.BindFlags=D3D11_BIND_SHADER_RESOURCE;
 uint32_t bytes[48];for(auto& b:bytes)b=0xff5a3821;D3D11_SUBRESOURCE_DATA data{bytes,32,0};ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11ShaderResourceView> view;
 check(SUCCEEDED(real->CreateTexture2D(&source,&data,texture.GetAddressOf()))&&SUCCEEDED(real->CreateShaderResourceView(texture.Get(),nullptr,view.GetAddressOf())),"opaque source fixture initializes");
 GpuSceneCompositor compositor(shared);GpuSceneCompositor::Layer layer;layer.View=view.Get();layer.Region=oldBounds;layer.CropTexels=oldBounds;
 check(compositor.Begin(oldBounds)&&compositor.Composite(layer)&&compositor.Finish().Ready,"original scene establishes reusable textures");
 faults->FailTextureAt=1;faults->TextureCalls=0;shared->Device=faults;
 check(!compositor.Begin(newBounds)&&compositor.LastError()==E_OUTOFMEMORY,"resize fails at injected first texture allocation");
 shared->Device=real;
 check(compositor.Begin(oldBounds)&&compositor.Composite(layer)&&compositor.Finish().Ready,"original size recovers after failed resize without null resources");
 check(compositor.Begin(newBounds),"new size can be allocated after recovery");
 check(compositor.Begin(oldBounds)&&compositor.Composite(layer)&&compositor.Finish().Ready,"original size remains usable after successful resize");
 for(unsigned failAt:{2u,3u}){
  faults->FailTextureAt=failAt;faults->TextureCalls=0;shared->Device=faults;
  check(!compositor.Begin(newBounds)&&compositor.LastError()==E_OUTOFMEMORY,"partially allocated resize reports injected failure");
  shared->Device=real;
  check(compositor.Begin(oldBounds)&&compositor.Composite(layer)&&compositor.Finish().Ready,"old dimensions recover after partial resize allocation");
 }
 // Exercise the production retained-source allocator without WGC/window fixtures.
 winrt::com_ptr<ID3D11Texture2D> retained;winrt::com_ptr<ID3D11ShaderResourceView> retainedView;std::shared_ptr<void> lease;
 faults->FailTextureAt=0;faults->FailViewAt=1;faults->ViewCalls=0;bool injected=false;
 try {capture_detail::EnsureRetainedGpuTexture(faults.Get(),source,retained,retainedView,lease);}catch(...){injected=winrt::to_hresult()==E_OUTOFMEMORY;}
 check(injected,"retained source injects SRV failure after texture creation");
 check(!retained&&!retainedView&&!lease,"failed initial retained generation leaves no partial resources");
 faults->FailViewAt=0;
 capture_detail::EnsureRetainedGpuTexture(real.Get(),source,retained,retainedView,lease);
 check(retained&&retainedView&&lease,"same source dimensions recover a usable SRV after allocation failure");
 // A missing view must also repair a retained texture supplied by an earlier incomplete state.
 retainedView=nullptr;capture_detail::EnsureRetainedGpuTexture(real.Get(),source,retained,retainedView,lease);
 check(retained&&retainedView&&lease,"missing retained SRV explicitly triggers recreation");
 // A pinned prior generation stays valid when its replacement allocation fails.
 auto oldTexture=retained;auto oldView=retainedView;auto oldLease=lease;faults->FailViewAt=1;faults->ViewCalls=0;injected=false;
 try {capture_detail::EnsureRetainedGpuTexture(faults.Get(),source,retained,retainedView,lease);}catch(...){injected=winrt::to_hresult()==E_OUTOFMEMORY;}
 check(injected&&oldTexture&&oldView&&oldLease,"pinned generation survives replacement SRV failure");
 check(retained.get()==oldTexture.get()&&retainedView.get()==oldView.get()&&lease==oldLease,"failed replacement preserves the complete previous generation");
 faults->FailViewAt=0;capture_detail::EnsureRetainedGpuTexture(real.Get(),source,retained,retainedView,lease);
 check(retained&&retainedView&&lease&&retained.get()!=oldTexture.get()&&lease!=oldLease,"retry commits complete replacement with a fresh generation lease");
 printf("GPU allocation recovery tests: %d checks, %d failures\n",checks,failed);return failed?1:0;
}
