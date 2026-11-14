#include "../GpuPanelPreprocessor.h"
#include <chrono>
#include <cstdio>
#include <limits>

using namespace novabar;
template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

template<class Processor>
bool RenderStrip(Processor& processor, ID3D11ShaderResourceView* source, PixelFrame const& geometry,
    PixelRect target, int width, int height, GlassOptions const& options, std::vector<uint8_t>& out)
{
    if constexpr (requires { processor.RenderTaskbarTexture(source, geometry, target, width, height, options, out); })
        return processor.RenderTaskbarTexture(source,geometry,target,width,height,options,out);
    else { out.clear(); return false; }
}
namespace
{
    PixelFrame Fixture(PixelRect bounds, bool transparent, uint32_t background)
    {
        PixelFrame frame; frame.Bounds=bounds; frame.Width=bounds.Right-bounds.Left;
        frame.Height=bounds.Bottom-bounds.Top; frame.Background=background;
        frame.Pixels.resize(size_t(frame.Width)*frame.Height*4);
        for(int y=0;y<frame.Height;++y)for(int x=0;x<frame.Width;++x)
        {
            const auto at=(size_t(y)*frame.Width+x)*4;
            frame.Pixels[at]=uint8_t(x*31+y*7);
            frame.Pixels[at+1]=uint8_t(x*13+y*29);
            frame.Pixels[at+2]=uint8_t(x*7+y*19);
            frame.Pixels[at+3]=transparent?uint8_t(x*17+y*23):255;
        }
        return frame;
    }
    ComPtr<ID3D11ShaderResourceView> Upload(ID3D11Device* device, PixelFrame const& frame)
    {
        D3D11_TEXTURE2D_DESC d{};d.Width=frame.Width;d.Height=frame.Height;
        d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        d.Usage=D3D11_USAGE_IMMUTABLE;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial{frame.Pixels.data(),UINT(frame.Width*4),0};
        ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11ShaderResourceView> view;
        if(SUCCEEDED(device->CreateTexture2D(&d,&initial,texture.GetAddressOf())))
            device->CreateShaderResourceView(texture.Get(),nullptr,view.GetAddressOf());
        return view;
    }
    bool Near(std::vector<uint8_t> const& a,std::vector<uint8_t> const& b)
    {
        if(a.size()!=b.size())return false;
        for(size_t i=0;i<a.size();++i)if(std::abs(int(a[i])-int(b[i]))>1)return false;
        return true;
    }
    struct Case
    {
        char const* Name; PixelRect Bounds,Target; bool Transparent=false;
        uint32_t Background=0xff000000; GlassOptions Options{};
    };
    Case const Cases[]{
        {"default opaque strip",{0,-64,128,64},{0,0,128,24}},
        {"default alpha strip",{0,-64,128,64},{0,0,128,24},true},
        {"zero optics and blur",{0,-64,128,64},{0,0,128,24},false,0xff182a3c,{0,0,0,0,0,0}},
        {"maximum lens",{0,-64,128,64},{0,0,128,24},false,0xff000000,{1,.18,.12,.3,0,0}},
        {"maximum dispersion",{0,-64,128,64},{0,0,128,24},true,0xff284b6e,{1,1,.12,.3,0,0}},
        {"maximum noise",{0,-64,128,64},{0,0,128,24},false,0xff000000,{.55,.18,1,.3,0,0}},
        {"opaque color tint",{0,-64,128,64},{0,0,128,24},true,0xff4080c0,{.55,.18,0,1,0,0xff963a72}},
        {"zero tint alpha",{0,-64,128,64},{0,0,128,24},false,0xff000000,{.55,.18,.12,1,0,0x003a7296}},
        {"maximum blur opaque",{0,-64,128,64},{0,0,128,24},false,0xff000000,{.55,.18,.12,.3,256,0x263a7296}},
        {"maximum blur alpha",{0,-64,128,64},{0,0,128,24},true,0xff5a2c81,{1,1,1,.7,256,0x8c2a94e6}},
        {"negative monitor origin",{-128,-160,0,-32},{-128,-96,0,-72},true,0xff315579},
        {"large negative origin",{-3840,-2160,-3712,-2032},{-3840,-2096,-3712,-2072},false},
        {"partial left outside",{0,-64,128,64},{-16,0,112,24},true,0xff8291a3},
        {"partial right outside",{0,-64,128,64},{16,0,144,24},false,0xff456789},
        {"top outside lens samples",{0,0,128,96},{0,0,128,24},true,0xff9c734a},
        {"bottom outside samples",{0,-64,128,64},{0,60,128,84},true,0xff36597c,{0,0,.5,.5,24,0xa05b8dc1}},
        {"entire target outside",{0,-64,128,64},{128,0,256,24},true,0xff739abf,{1,1,1,1,12,0x707824d5}},
        {"fractional blur radius",{0,-64,128,64},{0,0,128,24},true,0xff6f8b2d,{.73,.91,.37,.83,255.9,0xb974b91e}},
        {"one-pixel output height",{-7,-71,121,57},{-7,-7,121,-6},true,0xff123456,{1,1,1,1,256,0x36bf7524}},
        {"nonfinite material sanitization",{0,-64,128,64},{0,0,128,24},true,0xffc28b53,
            {std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity(),
             std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity(),
             std::numeric_limits<double>::quiet_NaN(),0x8c2f71b3}},
    };
}
int main()
{
    int checks=0,failed=0;
    auto check=[&](bool ok,char const* label){++checks;if(!ok){++failed;std::printf("FAIL: %s\n",label);}};
    auto shared=GpuPanelPreprocessor::CreateSharedDevice();check(shared!=nullptr,"hardware GPU initialization");
    if(!shared)return 1;GpuPanelPreprocessor processor(shared);
    std::vector<uint8_t> actual,expected;
    for(auto const& item:Cases)
    {
        auto frame=Fixture(item.Bounds,item.Transparent,item.Background);
        auto view=Upload(shared->Device.Get(),frame);check(view!=nullptr,"strip fixture texture upload");
        auto geometry=frame;geometry.Pixels.clear();
        const int width=item.Target.Right-item.Target.Left,height=item.Target.Bottom-item.Target.Top;
        check(RenderGlass(frame,item.Target,width,height,item.Options,expected),"CPU strip reference is valid");
        const bool rendered=RenderStrip(processor,view.Get(),geometry,item.Target,width,height,item.Options,actual);
        check(rendered,item.Name);
        if(rendered)
        {
            const bool matches=Near(actual,expected);
            if(!matches)
            {
                for(size_t i=0;i<std::min(actual.size(),expected.size());++i)
                    if(std::abs(int(actual[i])-int(expected[i]))>1)
                    {std::printf("MISMATCH: %s at byte %zu (GPU=%u CPU=%u)\n",item.Name,i,unsigned(actual[i]),unsigned(expected[i]));break;}
            }
            check(matches,"GPU taskbar strip agrees with CPU within one byte");
        }
    }
    auto frame=Fixture({0,-64,128,64},true,0xff3a7296);auto view=Upload(shared->Device.Get(),frame);
    auto geometry=frame;geometry.Pixels.clear();const PixelRect target{0,0,128,24};GlassOptions options;
    auto rejects=[&](ID3D11ShaderResourceView* source,PixelFrame const& shape,PixelRect region,int width,int height,char const* label){
        actual={1,2,3};check(!RenderStrip(processor,source,shape,region,width,height,options,actual)&&actual.empty(),label);
    };
    rejects(nullptr,geometry,target,128,24,"null strip SRV fails without stale output");
    auto invalid=geometry;invalid.Tile=true;rejects(view.Get(),invalid,target,128,24,"tiled strip source is unsupported");
    invalid=geometry;invalid.Bounds.Right+=1;rejects(view.Get(),invalid,target,128,24,"nonphysical source scale is rejected");
    rejects(view.Get(),geometry,target,64,24,"scaled output width is rejected");
    rejects(view.Get(),geometry,target,128,12,"scaled output height is rejected");
    rejects(view.Get(),geometry,{0,0,0,24},0,24,"empty taskbar output is rejected");
    auto foreign=GpuPanelPreprocessor::CreateSharedDevice();check(foreign!=nullptr,"foreign strip device fixture initializes");
    if(foreign){auto other=Upload(foreign->Device.Get(),frame);
        rejects(other.Get(),geometry,target,128,24,"foreign-device strip texture is rejected");}
    check(RenderStrip(processor,view.Get(),geometry,target,128,24,options,actual)&&processor.LastError()==S_OK,
        "valid strip recovers after rejection and clears LastError");

    auto bench=Fixture({0,-128,3840,72},false,0xff000000);auto benchView=Upload(shared->Device.Get(),bench);
    auto benchGeometry=bench;benchGeometry.Pixels.clear();const PixelRect benchTarget{0,0,3840,72};
    GlassOptions benchOptions{.55,.18,.12,.3,0,0x263a7296};std::vector<double> elapsed;
    for(int pass=0;pass<33;++pass)
    {
        const auto start=std::chrono::steady_clock::now();
        const bool ok=RenderStrip(processor,benchView.Get(),benchGeometry,benchTarget,3840,72,benchOptions,actual);
        if(!ok)break;
        if(pass>=3)elapsed.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
    }
    check(elapsed.size()==30,"3840x72 strip GPU path completes thirty reused-resource rounds");
    if(elapsed.size()==30){std::sort(elapsed.begin(),elapsed.end());
        std::printf("GPU taskbar strip 3840x72 including readback: p50=%.3fms p95=%.3fms (30 rounds, target <1ms)\n",elapsed[15],elapsed[28]);}
    std::printf("GPU strip tests: %d goldens, %d checks, %d failures\n",int(std::size(Cases)),checks,failed);
    return failed?1:0;
}
