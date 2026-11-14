#include "../GpuSceneCompositor.h"
#include "../SceneBackdropSource.h"
#include <cstdio>
using namespace novabar;
using Microsoft::WRL::ComPtr;
namespace {
PixelFrame Solid(PixelRect r,uint32_t bgra) {
 PixelFrame f; f.Bounds=r;f.Width=r.Right-r.Left;f.Height=r.Bottom-r.Top;f.Pixels.resize(size_t(f.Width)*f.Height*4);
 for(size_t p=0;p<f.Pixels.size();p+=4)memcpy(f.Pixels.data()+p,&bgra,4);return f;
}
struct Input { PixelFrame Frame; ComPtr<ID3D11Texture2D> Texture; ComPtr<ID3D11ShaderResourceView> View; std::vector<PixelRect> Clips; unsigned Opacity=255; uint32_t Key=0x1000000,Index=0; bool Missing=false; PixelRect Crop{}; };
Input Upload(std::shared_ptr<GpuPanelPreprocessor::SharedDevice> const& shared,PixelFrame frame) {
 Input v;v.Frame=std::move(frame);D3D11_TEXTURE2D_DESC d{};d.Width=v.Frame.Width;d.Height=v.Frame.Height;d.MipLevels=d.ArraySize=1;d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
 D3D11_SUBRESOURCE_DATA data{v.Frame.Pixels.data(),UINT(v.Frame.Width*4),0};
 if(FAILED(shared->Device->CreateTexture2D(&d,&data,v.Texture.GetAddressOf()))||FAILED(shared->Device->CreateShaderResourceView(v.Texture.Get(),nullptr,v.View.GetAddressOf())))throw E_FAIL;
 v.Crop={0,0,v.Frame.Width,v.Frame.Height};return v;
}
std::vector<uint8_t> Read(std::shared_ptr<GpuPanelPreprocessor::SharedDevice> const& shared,GpuSceneCompositor const& gpu) {
 if(!gpu.View())return {};ComPtr<ID3D11Resource> r;gpu.View()->GetResource(r.GetAddressOf());ComPtr<ID3D11Texture2D> tex;if(FAILED(r.As(&tex)))return {};
 D3D11_TEXTURE2D_DESC d{};tex->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;ComPtr<ID3D11Texture2D> staging;
 if(FAILED(shared->Device->CreateTexture2D(&d,nullptr,staging.GetAddressOf())))return {};shared->Context->CopyResource(staging.Get(),tex.Get());D3D11_MAPPED_SUBRESOURCE map{};
 if(FAILED(shared->Context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map)))return {};std::vector<uint8_t> bytes(size_t(d.Width)*d.Height*4);
 for(UINT y=0;y<d.Height;++y)memcpy(bytes.data()+size_t(y)*d.Width*4,static_cast<uint8_t*>(map.pData)+size_t(y)*map.RowPitch,d.Width*4);shared->Context->Unmap(staging.Get(),0);return bytes;
}
GpuSceneCompositor::Layer Layer(Input const& i) {return {i.Missing?nullptr:i.View.Get(),i.Frame.Bounds,i.Crop,&i.Clips,i.Opacity,i.Key,i.Index};}
}
int main(){
 int checks=0,failed=0;auto check=[&](bool ok,char const* s){++checks;if(!ok){++failed;printf("FAIL: %s\n",s);}};
 auto shared=GpuPanelPreprocessor::CreateSharedDevice();check(bool(shared),"hardware shared device initializes");if(!shared)return 1;
 const PixelRect bounds{-3,-2,5,4};GpuSceneCompositor gpu(shared);
 auto run=[&](char const* name,std::vector<Input> const& inputs,bool ready,uint32_t missing=0x7fff){
  check(gpu.Begin(bounds),"begin valid bounds");scene_detail::SceneAccumulator cpu(bounds);
  for(auto const& i:inputs){check(gpu.Composite(Layer(i)),"submit validated layer");if(!i.Missing)cpu.Composite(i.Frame,i.Clips,i.Opacity,i.Key);}
  auto r=gpu.Finish();check(r.Ready==ready&&r.FirstNeededIndex==missing,name);
  if(ready)check(Read(shared,gpu)==cpu.Frame().Pixels,"GPU scene bytes equal CPU compositor exactly");return r;
 };
 auto back=Upload(shared,Solid(bounds,0xff302010));
 run("opaque source closes scene",{back},true);
 auto same=run("same scene remains ready",{back},true);check(!same.Changed,"identical scene reports unchanged");
 auto front=Upload(shared,Solid(bounds,0x80504030));run("per-pixel source alpha",{front,back},true);
 front.Opacity=127;run("global opacity rounds twice",{front,back},true);
 front.Opacity=1;run("one-byte global opacity",{front,back},true);
 front.Opacity=0;run("zero opacity contributes nothing",{front,back},true);
 front=Upload(shared,Solid(bounds,0x4020e080));run("nonpremultiplied channels clamp to alpha",{front,back},true);
 front=Upload(shared,Solid(bounds,0xffe04020));front.Key=RGB(224,64,32);run("opaque COLORREF key opens transparent holes",{front,back},true);
 front=Upload(shared,Solid(bounds,0x80e04020));front.Key=RGB(224,64,32);run("key does not discard translucent pixel",{front,back},true);
 front=Upload(shared,Solid(bounds,0xffaa5511));front.Clips={{-3,-2,0,1},{2,1,5,4}};run("disjoint HRGN clips",{front,back},true);
 front.Clips={{100,100,110,110}};run("outside HRGN contributes nothing",{front,back},true);
 Input unknown;unknown.Missing=true;unknown.Frame.Bounds=bounds;unknown.Index=7;
 run("opaque predecessor never requires unknown",{back,unknown},true);
 run("needed unknown blocks ready despite opaque lower",{unknown,back},false,7);
 unknown.Clips={{100,100,110,110}};run("outside unknown HRGN never required",{unknown,back},true);
 unknown.Clips.clear();unknown.Opacity=0;run("zero-opacity unknown never required",{unknown,back},true);unknown.Opacity=255;
 run("translucent predecessor requires unknown",{front=Upload(shared,Solid(bounds,0x80804020)),unknown,back},false,7);
 Input other=unknown;other.Index=8;run("first needed unknown preserves ordered index",{unknown,other,back},false,7);
 auto reversed=run("source order changes result",{back,front},true);check(reversed.Changed,"source order change reports changed");
 run("uncovered real pixels remain pending",{front},false);
 auto scaled=Solid(bounds,0xff010101);scaled.Width=3;scaled.Height=2;scaled.Pixels.resize(24);for(size_t p=0;p<scaled.Pixels.size();p+=4){scaled.Pixels[p]=uint8_t(p);scaled.Pixels[p+1]=17;scaled.Pixels[p+2]=201;scaled.Pixels[p+3]=255;}
 run("negative-world non-unit source scaling",{Upload(shared,scaled)},true);
 // A crop uses the cropped physical region width, not whole-window scaling.
 PixelRect region{-2,-1,4,3};auto large=Solid({0,0,7,5},0xff403020);for(size_t p=0;p<large.Pixels.size();p+=4)large.Pixels[p]=uint8_t(p/4);
 auto crop=Upload(shared,large);crop.Frame.Bounds=region;crop.Crop={1,1,5,4};PixelFrame ref;ref.Bounds=region;ref.Width=4;ref.Height=3;ref.Pixels.resize(48);
 for(int y=0;y<3;++y)memcpy(ref.Pixels.data()+size_t(y)*16,large.Pixels.data()+(size_t(y+1)*7+1)*4,16);
 check(gpu.Begin(bounds),"begin cropped test");check(gpu.Composite(Layer(crop))&&gpu.Composite(Layer(back)),"crop and background submit");auto cr=gpu.Finish();scene_detail::SceneAccumulator cpu(bounds);cpu.Composite(ref);cpu.Composite(back.Frame);check(cr.Ready&&Read(shared,gpu)==cpu.Frame().Pixels,"crop-then-scale byte parity");
 // Resource ownership: submitting does not require caller to retain the SRV after submission.
 check(gpu.Begin(bounds),"begin pinned-view test");{auto transient=Upload(shared,Solid(bounds,0xff778899));check(gpu.Composite(Layer(transient)),"submit temporary owning view");}check(gpu.Finish().Ready,"submitted GPU commands retain temporary source view");
 check(gpu.Begin(bounds),"begin invalid-device test");auto foreignShared=GpuPanelPreprocessor::CreateSharedDevice();auto foreign=Upload(foreignShared,Solid(bounds,0xffabcdef));check(!gpu.Composite(Layer(foreign))&&!gpu.Finish().Ready,"foreign-device source fails closed");
 auto opaque=Upload(shared,Solid(bounds,0xff204060));
 run("recovery after failed layer",{opaque},true);
 check(gpu.Begin(bounds)&&gpu.Composite(Layer(opaque))&&gpu.Finish(true).Changed,"caller-forced layout changes preserve redraw");
 run("pending transition establishes prior ready scene",{opaque},true);
 run("required unknown invalidates prior ready",{unknown,opaque},false,7);
 check(run("readiness recovery redraws identical bytes",{opaque},true).Changed,"ready transition redraws matching prior pixels");
 check(gpu.Geometry().Pixels.empty(),"production GPU geometry contains no CPU scene allocation");
 auto invalid=opaque;invalid.Crop={0,0,100,100};check(gpu.Begin(bounds)&&!gpu.Composite(Layer(invalid))&&!gpu.Finish().Ready,"out-of-range crop fails closed");
 invalid=opaque;invalid.Crop={3,2,1,0};check(gpu.Begin(bounds)&&!gpu.Composite(Layer(invalid))&&!gpu.Finish().Ready,"reversed crop fails closed");
 // Byte-valued varying fixtures catch integer rounding and premultiplied clamps.
 auto varying=Solid(bounds,0);for(size_t p=0;p<varying.Pixels.size();p+=4){varying.Pixels[p]=uint8_t(p*17+7);varying.Pixels[p+1]=uint8_t(p*29+99);varying.Pixels[p+2]=uint8_t(p*13+201);varying.Pixels[p+3]=uint8_t(p*31+1);}
 auto rounder=Upload(shared,varying);for(unsigned opacity:{1u,2u,63u,127u,128u,129u,191u,254u,255u}){rounder.Opacity=opacity;run("varying alpha/channel rounding golden",{rounder,back},true);}
 // Independent hand-derived source-over: B=(16 + round(32*127/255))=32,
 // G=32+round(16*127/255)=40, R=64+round(8*127/255)=68.
 auto literalFront=Upload(shared,Solid(bounds,0x80402010));auto literalBack=Upload(shared,Solid(bounds,0xff081020));
 check(gpu.Begin(bounds)&&gpu.Composite(Layer(literalFront))&&gpu.Composite(Layer(literalBack))&&gpu.Finish().Ready,"literal source-over submits");
 check(Read(shared,gpu)==Solid(bounds,0xff442820).Pixels,"hand-derived integer source-over bytes");
 check(!gpu.Begin({0,0,0,8}),"empty bounds rejected");check(!gpu.Begin({0,0,16385,1}),"oversized bounds rejected");
 GpuSceneCompositor absent(nullptr);check(!absent.Begin(bounds)&&!absent.Finish().Ready,"absent device fails closed");
 printf("GPU scene tests: %d checks, %d failures\n",checks,failed);return failed?1:0;
}
