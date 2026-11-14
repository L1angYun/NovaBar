#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <limits>
#include "../GpuClientCanvasOptics.h"
#include "../ClientCanvasOptics.h"
using Microsoft::WRL::ComPtr;
using namespace novabar;
int checks=0,failures=0;
void Check(bool ok,char const* name){++checks;if(!ok)++failures;printf("%s: %s\n",ok?"PASS":"FAIL",name);}
void Hr(HRESULT hr){if(FAILED(hr))throw hr;}
struct Input
{
    ComPtr<ID3D11Texture2D> Texture;
    ComPtr<ID3D11ShaderResourceView> View;
};
Input MakeInput(ID3D11Device* device,int width,int height,std::vector<uint8_t> const* pixels=nullptr,
    DXGI_FORMAT format=DXGI_FORMAT_B8G8R8A8_UNORM,UINT mips=1,UINT array=1,UINT samples=1,
    D3D11_SHADER_RESOURCE_VIEW_DESC const* viewDesc=nullptr)
{
    D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=mips;d.ArraySize=array;d.Format=format;
    d.SampleDesc.Count=samples;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{pixels?pixels->data():nullptr,UINT(width*4),0};Input input;
    Hr(device->CreateTexture2D(&d,pixels?&initial:nullptr,&input.Texture));
    Hr(device->CreateShaderResourceView(input.Texture.Get(),viewDesc,&input.View));return input;
}
PixelFrame Base(int width,int height,PixelRect bounds,bool alphaExtremes=false)
{
    PixelFrame base;base.Width=width;base.Height=height;base.Bounds=bounds;base.Pixels.resize(size_t(width)*height*4);
    constexpr uint8_t alpha[]={0,1,2,254,255};
    for(size_t p=0;p<base.Pixels.size()/4;++p){unsigned hash=unsigned(p)*374761393u+668265263u;hash^=hash>>13;
        const uint8_t a=alphaExtremes?alpha[p%5]:uint8_t(30+hash%226);
        base.Pixels[p*4]=uint8_t(hash%unsigned(a+1));base.Pixels[p*4+1]=uint8_t((hash>>8)%unsigned(a+1));
        base.Pixels[p*4+2]=uint8_t((hash>>16)%unsigned(a+1));base.Pixels[p*4+3]=a;}
    return base;
}
std::vector<uint8_t> Readback(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* texture)
{
    D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;Hr(device->CreateTexture2D(&d,nullptr,&staging));context->CopyResource(staging.Get(),texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};Hr(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
    std::vector<uint8_t> pixels(size_t(d.Width)*d.Height*4);for(UINT y=0;y<d.Height;++y)memcpy(pixels.data()+size_t(y)*d.Width*4,static_cast<uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch,size_t(d.Width)*4);
    context->Unmap(staging.Get(),0);return pixels;
}
struct Output
{
    ComPtr<ID3D11Texture2D> Texture;
    ComPtr<ID3D11RenderTargetView> View;
};
Output MakeOutput(ID3D11Device* device,int width,int height,
    DXGI_FORMAT format=DXGI_FORMAT_B8G8R8A8_UNORM,UINT mips=1,UINT array=1,UINT samples=1,
    D3D11_RENDER_TARGET_VIEW_DESC const* viewDesc=nullptr,UINT extraBindings=0,UINT miscFlags=0)
{
    D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=mips;d.ArraySize=array;
    d.Format=format;d.SampleDesc.Count=samples;d.Usage=D3D11_USAGE_DEFAULT;
    d.BindFlags=D3D11_BIND_RENDER_TARGET|extraBindings;d.MiscFlags=miscFlags;
    Output output;Hr(device->CreateTexture2D(&d,nullptr,&output.Texture));
    Hr(device->CreateRenderTargetView(output.Texture.Get(),viewDesc,&output.View));
    const float sentinel[]={.125f,.25f,.5f,1};
    ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);
    context->ClearRenderTargetView(output.View.Get(),sentinel);return output;
}
// Queries prove rejection occurs before any draw; the test owns all readback.
UINT64 DrawVertices(ID3D11Device* device,ID3D11DeviceContext* context,
    auto&& action)
{
    D3D11_QUERY_DESC desc{D3D11_QUERY_PIPELINE_STATISTICS,0};ComPtr<ID3D11Query> query;
    Hr(device->CreateQuery(&desc,&query));context->Begin(query.Get());action();context->End(query.Get());
    D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};HRESULT result=S_FALSE;
    const auto deadline=GetTickCount64()+10000;
    while((result=context->GetData(query.Get(),&stats,sizeof(stats),0))==S_FALSE&&GetTickCount64()<deadline)Sleep(1);
    Hr(result);if(result!=S_OK)throw HRESULT_FROM_WIN32(ERROR_TIMEOUT);return stats.IAVertices;
}
int main()
{
    setvbuf(stdout,nullptr,_IONBF,0);
    try
    {
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level{};
        Hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context));
        ComPtr<IDXGIDevice> dxgi;Hr(device.As(&dxgi));ComPtr<IDXGIAdapter> adapter;Hr(dxgi->GetAdapter(&adapter));DXGI_ADAPTER_DESC ad{};Hr(adapter->GetDesc(&ad));printf("Adapter: %ls feature=%x\n",ad.Description,unsigned(level));
        const auto rejectsContext=[&](char const* name,ID3D11Device* owner,ID3D11DeviceContext* supplied)
        {
            HRESULT error=S_OK;
            try{GpuClientCanvasOptics invalid(owner,supplied);}catch(HRESULT hr){error=hr;}
            Check(error==E_INVALIDARG,name);
        };
        rejectsContext("constructor-rejects-null-device",nullptr,context.Get());
        rejectsContext("constructor-rejects-null-context",device.Get(),nullptr);
        ComPtr<ID3D11DeviceContext> deferred;Hr(device->CreateDeferredContext(0,&deferred));
        rejectsContext("constructor-rejects-deferred-context",device.Get(),deferred.Get());
        GpuClientCanvasOptics gpu(device.Get(),context.Get());
        struct Case{char const* Name;int Width,Height;PixelRect Canvas,Target,Viewport;double Radius,Lens,Dispersion,Specular;};
        const Case cases[]={
          {"passthrough",160,128,{-120,-48,120,144},{-96,-24,96,120},{-240,-96,240,192},12,0,0,0},
          {"rounded",160,128,{-120,-48,120,144},{-96,-24,96,120},{-240,-96,240,192},12,.55,.18,.6},
          {"wide-corners-max",160,128,{-120,-48,120,144},{-96,-24,96,120},{-240,-96,240,192},48,1,1,1},
          {"sharp-corners",160,128,{-120,-48,120,144},{-96,-24,96,120},{-240,-96,240,192},0,.8,.4,.1},
          {"specular-only",160,128,{-120,-48,120,144},{-96,-24,96,120},{-240,-96,240,192},12,0,.4,.9},
          {"lens-only",160,128,{-120,-48,120,144},{-96,-24,96,120},{-240,-96,240,192},12,1,0,0},
          {"radius-clamp",160,128,{-120,-48,120,144},{-96,-24,96,120},{-240,-96,240,192},1000,.9,.8,.4},
          {"viewport-edges",160,128,{-120,-48,120,144},{-120,-48,120,144},{-120,-48,120,144},12,.55,.18,.6},
          {"negative-monitor",160,128,{-1920,-1080,-1680,-888},{-1896,-1056,-1704,-912},{-3840,-2160,0,0},12,.55,.18,.6},
          {"moved-pose",160,128,{-120,-48,120,144},{-88,-16,104,128},{-240,-96,240,192},12,.55,.18,.6},
          {"fractional-sampling",137,119,{-120,-48,120,144},{-97,-23,95,121},{-240,-96,240,192},13,.61,.37,.7},
          {"small-pane",32,24,{-12,-9,36,27},{0,0,16,10},{-240,-96,240,192},1000,.55,.18,.6},
          {"true-desktop-clamp",100,100,{100,100,200,200},{198,110,298,190},{100,100,200,200},12,1,1,0},
          {"fractional-viewport-cut",137,119,{-120,-48,120,144},{60,-23,252,121},{-82,-9,66,103},13,.61,.37,.7},
          {"alpha-extremes",160,128,{-120,-48,120,144},{-96,-24,96,120},{-240,-96,240,192},12,1,1,1},
          {"tiny-sharp-expanded",32,24,{-12,-9,36,27},{0,0,4,3},{-240,-96,240,192},0,1,1,.5},
          {"target-outside-viewport",160,128,{-120,-48,120,144},{600,700,800,900},{-240,-96,240,192},12,1,1,1},
          {"partial-specular-no-refraction",100,100,{100,100,200,200},{198,110,298,190},{0,0,3840,2160},12,0,1,.9},
          {"finite-overrange-clamp",160,128,{-120,-48,120,144},{-96,-24,96,120},{-240,-96,240,192},12,10,10,10},
          {"single-texel",1,1,{0,0,1,1},{0,0,1,1},{0,0,1,1},1000,1,1,1},
          {"single-column",1,17,{0,0,10,34},{0,0,10,34},{0,0,10,34},0,1,1,1},
          {"single-row",17,1,{0,0,34,10},{0,0,34,10},{0,0,34,10},0,1,1,1},
          {"tight-sharp-inward",100,80,{100,100,200,180},{100,100,200,180},{0,0,3840,2160},0,1,1,1},
          {"tight-rounded-inward",100,80,{100,100,200,180},{100,100,200,180},{0,0,3840,2160},40,1,1,1},
          {"tight-12px-sharp-inward",12,12,{100,100,112,112},{100,100,112,112},{0,0,3840,2160},0,1,1,1},
          {"critical-rounded-boundary",100,100,{0,0,100,100},{0,0,100,100},{0,0,100,100},.5*(2+std::sqrt(2.0)),0,0,1},
          {"critical-rounded-minus-nextafter",100,100,{0,0,100,100},{0,0,100,100},{0,0,100,100},std::nextafter(.5*(2+std::sqrt(2.0)),0.0),0,0,1},
          {"critical-rounded-plus-nextafter",100,100,{0,0,100,100},{0,0,100,100},{0,0,100,100},std::nextafter(.5*(2+std::sqrt(2.0)),INFINITY),0,0,1},
          {"critical-rounded-minus-1e-8",100,100,{0,0,100,100},{0,0,100,100},{0,0,100,100},.5*(2+std::sqrt(2.0))-1e-8,0,0,1},
          {"critical-rounded-plus-1e-8",100,100,{0,0,100,100},{0,0,100,100},{0,0,100,100},.5*(2+std::sqrt(2.0))+1e-8,0,0,1},
          {"sharp-medial-axis-roundoff",99,101,{0,0,100,100},{48,48,52,52},{0,0,100,100},0,1,1,1},
          {"row-metadata-maximum-height",2,16384,{0,0,4,32768},{0,0,4,32768},{0,0,4,32768},2,1,1,1},
          {"row-metadata-maximum-width",16384,2,{0,0,32768,4},{0,0,32768,4},{0,0,32768,4},2,1,1,1},
          {"critical-boundary-large-positive-origin",100,100,{2147483547,2147483547,2147483647,2147483647},{2147483547,2147483547,2147483647,2147483647},{2147483547,2147483547,2147483647,2147483647},.5*(2+std::sqrt(2.0)),0,0,1},
          {"native-radius12-raster-density4",1024,576,{0,0,256,144},{8,8,248,136},{0,0,256,144},12,.55,.18,.6},
          {"native-radius12-raster-density-eighth",480,270,{0,0,3840,2160},{3200,120,3800,2040},{0,0,3840,2160},12,.55,.18,.6},
          {"native-notification-radius12",1024,576,{0,0,3840,2160},{3200,120,3800,2040},{0,0,3840,2160},12,.55,.18,.6},
          {"critical-boundary-large-negative-origin",100,100,{-2147483647,-2147483647,-2147483547,-2147483547},{-2147483647,-2147483647,-2147483547,-2147483547},{-2147483647,-2147483647,-2147483547,-2147483547},.5*(2+std::sqrt(2.0)),0,0,1}
        };
        for(auto const& item:cases)
        {
            auto base=Base(item.Width,item.Height,item.Canvas,strcmp(item.Name,"alpha-extremes")==0);
            std::vector<uint8_t> expected;Check(RenderClientCanvasOptics(base,item.Target,item.Radius,item.Lens,item.Dispersion,item.Specular,item.Viewport,expected),"CPU oracle accepts valid scene");
            D3D11_TEXTURE2D_DESC d{};d.Width=base.Width;d.Height=base.Height;d.MipLevels=d.ArraySize=1;d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA initial{base.Pixels.data(),UINT(base.Width*4),0};ComPtr<ID3D11Texture2D> input;Hr(device->CreateTexture2D(&d,&initial,&input));ComPtr<ID3D11ShaderResourceView> view;Hr(device->CreateShaderResourceView(input.Get(),nullptr,&view));
            auto output=MakeOutput(device.Get(),base.Width,base.Height);const bool rendered=gpu.Render(view.Get(),base,item.Target,item.Radius,item.Lens,item.Dispersion,item.Specular,item.Viewport,output.View.Get());Check(rendered&&gpu.LastError()==S_OK,"GPU draws optics into the caller-owned target");if(!rendered)continue;
            auto actual=Readback(device.Get(),context.Get(),output.Texture.Get());size_t wrong=0,changed=0;int maxError=0;for(size_t i=0;i<expected.size();++i){const int e=abs(int(expected[i])-int(actual[i]));maxError=std::max(maxError,e);if(e>1)++wrong;if(expected[i]!=base.Pixels[i])++changed;}
            printf("CASE %s: bytes=%zu changed=%zu wrongOver1LSB=%zu max=%d\n",item.Name,expected.size(),changed,wrong,maxError);Check(wrong==0,"GPU matches CPU refraction/dispersion/rim within1LSB");
            size_t alphaErrors=0;for(size_t p=0;p<actual.size()/4;++p)for(size_t c=0;c<3;++c)if(actual[p*4+c]>actual[p*4+3])++alphaErrors;Check(alphaErrors==0,"GPU output stays premultiplied at every pixel");
        }
        auto base=Base(100,100,{100,100,200,200});auto input=MakeInput(device.Get(),100,100,&base.Pixels);
        const auto reject=[&](char const* name,ID3D11ShaderResourceView* source,PixelFrame const& geometry,
            PixelRect target=PixelRect{110,110,190,190},double radius=12,double lens=1,double dispersion=1,double specular=0,
            PixelRect viewport=PixelRect{0,0,3840,2160})
        {
            auto output=MakeOutput(device.Get(),100,100);const auto before=Readback(device.Get(),context.Get(),output.Texture.Get());
            bool rendered=false,threw=false;
            const auto vertices=DrawVertices(device.Get(),context.Get(),[&]{
                try{rendered=gpu.Render(source,geometry,target,radius,lens,dispersion,specular,viewport,output.View.Get());}catch(...){threw=true;}
            });
            printf("REJECT %s: rendered=%d error=%08x vertices=%llu threw=%d\n",name,rendered,unsigned(gpu.LastError()),vertices,threw);
            Check(!threw&&!rendered&&FAILED(gpu.LastError())&&vertices==0
                &&Readback(device.Get(),context.Get(),output.Texture.Get())==before,name);
        };
        std::vector<uint8_t> oracle;
        Check(!RenderClientCanvasOptics(base,{198,110,298,190},12,1,1,0,{0,0,3840,2160},oracle),"CPU rejects missing in-desktop source");
        reject("missing-source",input.View.Get(),base,{198,110,298,190});
        auto dispersionBase=Base(108,100,{100,100,208,200});auto dispersionInput=MakeInput(device.Get(),108,100,&dispersionBase.Pixels);
        Check(RenderClientCanvasOptics(dispersionBase,{198,110,298,190},0,.7,0,0,{0,0,3840,2160},oracle)
            &&!RenderClientCanvasOptics(dispersionBase,{198,110,298,190},0,.7,1,0,{0,0,3840,2160},oracle),
            "CPU rejection isolates a dispersion sample escaping the canvas");
        reject("dispersion-source-escape",dispersionInput.View.Get(),dispersionBase,{198,110,298,190},0,.7,1);
        reject("partial-material-conservative-fallback",dispersionInput.View.Get(),dispersionBase,{198,110,298,190},0,.7,0);
        auto tiny=Base(4,3,{0,0,4,3});auto tinyInput=MakeInput(device.Get(),4,3,&tiny.Pixels);
        reject("tiny-unprovable-source",tinyInput.View.Get(),tiny,{0,0,4,3},0,1,1,0,{0,0,100,100});
        const auto nan=std::numeric_limits<double>::quiet_NaN(),inf=std::numeric_limits<double>::infinity();
        reject("null-view",nullptr,base);reject("negative-radius",input.View.Get(),base,{110,110,190,190},-1);
        reject("NaN-radius",input.View.Get(),base,{110,110,190,190},nan);
        reject("infinite-radius",input.View.Get(),base,{110,110,190,190},inf);
        reject("NaN-lens",input.View.Get(),base,{110,110,190,190},12,nan);
        reject("infinite-lens",input.View.Get(),base,{110,110,190,190},12,inf);
        reject("NaN-dispersion",input.View.Get(),base,{110,110,190,190},12,1,nan);
        reject("infinite-dispersion",input.View.Get(),base,{110,110,190,190},12,1,inf);
        reject("NaN-specular",input.View.Get(),base,{110,110,190,190},12,1,1,nan);
        reject("infinite-specular",input.View.Get(),base,{110,110,190,190},12,1,1,inf);
        reject("inverted-target",input.View.Get(),base,{190,110,110,190});
        reject("inverted-viewport",input.View.Get(),base,{110,110,190,190},12,1,1,0,{200,0,100,100});
        auto bad=base;bad.Width=0;reject("zero-width",input.View.Get(),bad);
        bad=base;bad.Width=16385;reject("width-cap",input.View.Get(),bad);
        bad=base;bad.Height=16385;reject("height-cap",input.View.Get(),bad);
        bad=base;bad.Width=4096;bad.Height=2049;reject("pixel-count-cap",input.View.Get(),bad);
        bad=base;bad.Bounds.Right=bad.Bounds.Left+32769;reject("canvas-width-span-cap",input.View.Get(),bad,{110,110,190,190},12,0);
        bad=base;bad.Bounds.Bottom=bad.Bounds.Top+32769;reject("canvas-height-span-cap",input.View.Get(),bad,{110,110,190,190},12,0);
        reject("target-width-span-cap",input.View.Get(),base,{100,100,32869,200},12,0);
        reject("target-height-span-cap",input.View.Get(),base,{100,100,200,32869},12,0);
        auto largeInput=MakeInput(device.Get(),4096,2049);PixelFrame largeGeometry;largeGeometry.Width=4096;largeGeometry.Height=2049;largeGeometry.Bounds={0,0,4096,2049};
        reject("pixel-count-cap-with-matching-SRV",largeInput.View.Get(),largeGeometry,{100,100,200,200},12,0);
        auto smaller=MakeInput(device.Get(),64,64);reject("SRV-extent-mismatch",smaller.View.Get(),base);
        auto rgba=MakeInput(device.Get(),100,100,nullptr,DXGI_FORMAT_R8G8B8A8_UNORM);reject("unsupported-RGBA-format",rgba.View.Get(),base);
        auto srgb=MakeInput(device.Get(),100,100,nullptr,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);reject("unsupported-sRGB-format",srgb.View.Get(),base);
        auto floating=MakeInput(device.Get(),100,100,nullptr,DXGI_FORMAT_R16G16B16A16_FLOAT);reject("unsupported-float-format",floating.View.Get(),base);
        D3D11_SHADER_RESOURCE_VIEW_DESC mipView{};mipView.Format=DXGI_FORMAT_B8G8R8A8_UNORM;mipView.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;mipView.Texture2D.MostDetailedMip=1;mipView.Texture2D.MipLevels=1;
        auto mip=MakeInput(device.Get(),200,200,nullptr,DXGI_FORMAT_B8G8R8A8_UNORM,2,1,1,&mipView);reject("unsupported-mip-view",mip.View.Get(),base);
        mipView.Texture2D.MostDetailedMip=0;
        auto fullMip=MakeInput(device.Get(),100,100,nullptr,DXGI_FORMAT_B8G8R8A8_UNORM,2,1,1,&mipView);reject("unsupported-multi-mip-resource-with-base-only-view",fullMip.View.Get(),base);
        D3D11_SHADER_RESOURCE_VIEW_DESC arraySrv{};arraySrv.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        arraySrv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY;arraySrv.Texture2DArray.MipLevels=1;arraySrv.Texture2DArray.ArraySize=1;
        auto arrayView=MakeInput(device.Get(),100,100,nullptr,DXGI_FORMAT_B8G8R8A8_UNORM,1,1,1,&arraySrv);reject("unsupported-singleton-array-view",arrayView.View.Get(),base);
        auto array=MakeInput(device.Get(),100,100,nullptr,DXGI_FORMAT_B8G8R8A8_UNORM,1,2);reject("unsupported-array-dimension",array.View.Get(),base);
        D3D11_TEXTURE1D_DESC oneD{};oneD.Width=100;oneD.MipLevels=oneD.ArraySize=1;oneD.Format=DXGI_FORMAT_R8G8B8A8_UNORM;oneD.Usage=D3D11_USAGE_DEFAULT;oneD.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture1D> oneTexture;Hr(device->CreateTexture1D(&oneD,nullptr,&oneTexture));ComPtr<ID3D11ShaderResourceView> oneView;Hr(device->CreateShaderResourceView(oneTexture.Get(),nullptr,&oneView));reject("unsupported-1D-dimension",oneView.Get(),base);
        UINT quality=0;Hr(device->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM,2,&quality));
        if(quality){auto msaa=MakeInput(device.Get(),100,100,nullptr,DXGI_FORMAT_B8G8R8A8_UNORM,1,1,2);reject("unsupported-multisampling",msaa.View.Get(),base);}else printf("SKIP: hardware does not expose2x BGRA sampling fixture\n");
        ComPtr<ID3D11Device> foreignDevice;ComPtr<ID3D11DeviceContext> foreignContext;
        Hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&foreignDevice,&level,&foreignContext));
        rejectsContext("constructor-rejects-foreign-context",device.Get(),foreignContext.Get());
        auto foreign=MakeInput(foreignDevice.Get(),100,100,&base.Pixels);reject("foreign-device-view",foreign.View.Get(),base);
        auto geometryOnly=base;geometryOnly.Pixels.clear();
        auto first=MakeOutput(device.Get(),100,100),second=MakeOutput(device.Get(),100,100);
        Check(gpu.Render(input.View.Get(),geometryOnly,{110,110,190,190},12,.55,.18,.6,{0,0,3840,2160},first.View.Get())
            &&gpu.LastError()==S_OK,"GPU geometry admission requires no CPU pixel vector");
        const auto retained=Readback(device.Get(),context.Get(),first.Texture.Get());
        Check(gpu.Render(input.View.Get(),geometryOnly,{112,112,192,192},12,1,1,1,{0,0,3840,2160},second.View.Get())
            &&gpu.LastError()==S_OK,"successive renders use distinct caller-owned output textures");
        Check(Readback(device.Get(),context.Get(),first.Texture.Get())==retained,"a later render does not overwrite a retained output texture");
        Check(Readback(device.Get(),context.Get(),input.Texture.Get())==base.Pixels,"rendering never mutates the source texture");
        const auto secondPixels=Readback(device.Get(),context.Get(),second.Texture.Get());
        Check(!gpu.Render(nullptr,geometryOnly,{110,110,190,190},12,1,1,1,{0,0,3840,2160},second.View.Get())
            &&gpu.LastError()==E_INVALIDARG&&Readback(device.Get(),context.Get(),second.Texture.Get())==secondPixels
            &&Readback(device.Get(),context.Get(),first.Texture.Get())==retained,
            "failure preserves caller-owned and retained targets and reports invalid input");
        const auto rejectTarget=[&](char const* name,ID3D11RenderTargetView* destination)
        {
            bool rendered=true;
            const auto vertices=DrawVertices(device.Get(),context.Get(),[&]{
                rendered=gpu.Render(input.View.Get(),geometryOnly,{110,110,190,190},12,1,1,1,{0,0,3840,2160},destination);
            });
            Check(!rendered&&gpu.LastError()==E_INVALIDARG&&vertices==0,name);
        };
        rejectTarget("null-destination",nullptr);
        D3D11_TEXTURE1D_DESC target1dDesc{};target1dDesc.Width=100;target1dDesc.MipLevels=target1dDesc.ArraySize=1;
        target1dDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;target1dDesc.Usage=D3D11_USAGE_DEFAULT;target1dDesc.BindFlags=D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture1D> target1d;Hr(device->CreateTexture1D(&target1dDesc,nullptr,&target1d));
        ComPtr<ID3D11RenderTargetView> target1dView;Hr(device->CreateRenderTargetView(target1d.Get(),nullptr,&target1dView));
        rejectTarget("destination-1D-rejected",target1dView.Get());
        auto smallTarget=MakeOutput(device.Get(),99,100);rejectTarget("destination-extent-mismatch",smallTarget.View.Get());
        auto rgbaTarget=MakeOutput(device.Get(),100,100,DXGI_FORMAT_R8G8B8A8_UNORM);rejectTarget("destination-RGBA-rejected",rgbaTarget.View.Get());
        auto srgbTarget=MakeOutput(device.Get(),100,100,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);rejectTarget("destination-sRGB-rejected",srgbTarget.View.Get());
        auto floatTarget=MakeOutput(device.Get(),100,100,DXGI_FORMAT_R16G16B16A16_FLOAT);rejectTarget("destination-float-rejected",floatTarget.View.Get());
        auto mipTarget=MakeOutput(device.Get(),100,100,DXGI_FORMAT_B8G8R8A8_UNORM,2);rejectTarget("destination-multi-mip-rejected",mipTarget.View.Get());
        D3D11_RENDER_TARGET_VIEW_DESC mipRtv{};mipRtv.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        mipRtv.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;mipRtv.Texture2D.MipSlice=1;
        auto mipSliceTarget=MakeOutput(device.Get(),200,200,DXGI_FORMAT_B8G8R8A8_UNORM,2,1,1,&mipRtv);rejectTarget("destination-mip-slice-rejected",mipSliceTarget.View.Get());
        auto arrayTarget=MakeOutput(device.Get(),100,100,DXGI_FORMAT_B8G8R8A8_UNORM,1,2);rejectTarget("destination-array-rejected",arrayTarget.View.Get());
        D3D11_RENDER_TARGET_VIEW_DESC arrayRtv{};arrayRtv.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        arrayRtv.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2DARRAY;arrayRtv.Texture2DArray.ArraySize=1;
        auto arrayViewTarget=MakeOutput(device.Get(),100,100,DXGI_FORMAT_B8G8R8A8_UNORM,1,1,1,&arrayRtv);rejectTarget("destination-array-view-rejected",arrayViewTarget.View.Get());
        if(quality){auto msTarget=MakeOutput(device.Get(),100,100,DXGI_FORMAT_B8G8R8A8_UNORM,1,1,2);rejectTarget("destination-MSAA-rejected",msTarget.View.Get());}
        auto foreignTarget=MakeOutput(foreignDevice.Get(),100,100);rejectTarget("destination-foreign-device-rejected",foreignTarget.View.Get());
        auto alias=MakeOutput(device.Get(),100,100,DXGI_FORMAT_B8G8R8A8_UNORM,1,1,1,nullptr,D3D11_BIND_SHADER_RESOURCE);
        ComPtr<ID3D11ShaderResourceView> aliasView;Hr(device->CreateShaderResourceView(alias.Texture.Get(),nullptr,&aliasView));
        const auto aliasBefore=Readback(device.Get(),context.Get(),alias.Texture.Get());bool aliasResult=true;
        const auto aliasVertices=DrawVertices(device.Get(),context.Get(),[&]{aliasResult=gpu.Render(aliasView.Get(),geometryOnly,{110,110,190,190},12,1,1,1,{0,0,3840,2160},alias.View.Get());});
        Check(!aliasResult&&gpu.LastError()==E_INVALIDARG&&aliasVertices==0
            &&Readback(device.Get(),context.Get(),alias.Texture.Get())==aliasBefore,"source-target alias rejects before draw and preserves pixels");
        // Reopening one shared allocation produces distinct COM identities. Such
        // sources must be copied into the client's private base before optics.
        D3D11_TEXTURE2D_DESC sharedDesc{};sharedDesc.Width=sharedDesc.Height=100;
        sharedDesc.MipLevels=sharedDesc.ArraySize=1;sharedDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        sharedDesc.SampleDesc.Count=1;sharedDesc.Usage=D3D11_USAGE_DEFAULT;
        sharedDesc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        sharedDesc.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        D3D11_SUBRESOURCE_DATA sharedInitial{base.Pixels.data(),400,0};ComPtr<ID3D11Texture2D> sharedSource;
        Hr(device->CreateTexture2D(&sharedDesc,&sharedInitial,&sharedSource));
        ComPtr<IDXGIResource1> sharedResource;Hr(sharedSource.As(&sharedResource));HANDLE sharedHandle{};
        Hr(sharedResource->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&sharedHandle));
        ComPtr<ID3D11Device1> device1;Hr(device.As(&device1));ComPtr<ID3D11Texture2D> reopened;
        const HRESULT openResult=device1->OpenSharedResource1(sharedHandle,IID_PPV_ARGS(&reopened));CloseHandle(sharedHandle);Hr(openResult);
        ComPtr<IUnknown> originalIdentity,reopenedIdentity;Hr(sharedSource.As(&originalIdentity));Hr(reopened.As(&reopenedIdentity));
        Check(originalIdentity.Get()!=reopenedIdentity.Get(),"same-device shared-allocation reopen has distinct COM identity");
        ComPtr<ID3D11ShaderResourceView> sharedView;Hr(device->CreateShaderResourceView(sharedSource.Get(),nullptr,&sharedView));
        ComPtr<ID3D11RenderTargetView> reopenedTarget;Hr(device->CreateRenderTargetView(reopened.Get(),nullptr,&reopenedTarget));
        ComPtr<IDXGIKeyedMutex> keyed;Hr(sharedSource.As(&keyed));Hr(keyed->AcquireSync(0,1000));
        const auto sharedBefore=Readback(device.Get(),context.Get(),sharedSource.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        ID3D11ShaderResourceView* inheritedSource=input.View.Get();context->PSSetShaderResources(3,1,&inheritedSource);
        bool sharedRendered=true;const auto sharedVertices=DrawVertices(device.Get(),context.Get(),[&]{
            sharedRendered=gpu.Render(sharedView.Get(),geometryOnly,{110,110,190,190},12,1,1,1,{0,0,3840,2160},reopenedTarget.Get());});
        const HRESULT sharedError=gpu.LastError();D3D11_PRIMITIVE_TOPOLOGY retainedTopology{};
        context->IAGetPrimitiveTopology(&retainedTopology);ComPtr<ID3D11ShaderResourceView> retainedBinding;context->PSGetShaderResources(3,1,&retainedBinding);
        Check(!sharedRendered&&sharedError==E_INVALIDARG&&sharedVertices==0,"distinct-wrapper shared-allocation alias rejects before draw");
        Check(retainedTopology==D3D11_PRIMITIVE_TOPOLOGY_LINELIST&&retainedBinding.Get()==input.View.Get(),"shared-source rejection preserves inherited pipeline bindings");
        Check(Readback(device.Get(),context.Get(),sharedSource.Get())==sharedBefore,"rejected shared-allocation alias preserves source and destination pixels");
        Hr(keyed->ReleaseSync(0));
        auto sharedOutput=MakeOutput(device.Get(),100,100,DXGI_FORMAT_B8G8R8A8_UNORM,1,1,1,nullptr,0,
            D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE);
        Check(gpu.Render(input.View.Get(),geometryOnly,{110,110,190,190},12,.55,.18,.6,{0,0,3840,2160},sharedOutput.View.Get())
            &&gpu.LastError()==S_OK,"private source remains compatible with Composition destination flags0x802");
        Check(!gpu.Render(input.View.Get(),base,{198,110,298,190},12,1,1,0,{0,0,3840,2160},first.View.Get())
            &&gpu.LastError()==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),"unprovable source coverage reports unsupported instead of fabricated pixels");
        Check(gpu.Render(input.View.Get(),geometryOnly,{110,110,190,190},12,.55,.18,.6,{0,0,3840,2160},second.View.Get())
            &&gpu.LastError()==S_OK,"valid render recovers and clears the prior failure code");
        // A foreign user's false predicate must not silently suppress our draw.
        D3D11_QUERY_DESC predicateDesc{D3D11_QUERY_OCCLUSION_PREDICATE,0};ComPtr<ID3D11Predicate> predicate;
        Hr(device->CreatePredicate(&predicateDesc,&predicate));context->Begin(predicate.Get());context->End(predicate.Get());
        BOOL predicateValue=TRUE;HRESULT predicateResult=S_FALSE;const auto deadline=GetTickCount64()+10000;
        while((predicateResult=context->GetData(predicate.Get(),&predicateValue,sizeof(predicateValue),0))==S_FALSE&&GetTickCount64()<deadline)Sleep(1);
        Hr(predicateResult);if(predicateResult!=S_OK)throw HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        Check(!predicateValue,"empty occlusion predicate fixture resolves false");
        auto predicatedOutput=MakeOutput(device.Get(),100,100);
        context->SetPredication(predicate.Get(),FALSE);
        Check(!gpu.Render(nullptr,geometryOnly,{110,110,190,190},12,1,1,1,{0,0,3840,2160},second.View.Get()),"invalid render rejects with inherited predication");
        ComPtr<ID3D11Predicate> retainedPredicate;BOOL retainedValue=TRUE;context->GetPredication(&retainedPredicate,&retainedValue);
        Check(retainedPredicate.Get()==predicate.Get()&&!retainedValue,"admission failure preserves caller context state");
        std::vector<uint8_t> predicateExpected;
        Hr(RenderClientCanvasOptics(base,{110,110,190,190},12,.55,.18,.6,{0,0,3840,2160},predicateExpected)?S_OK:E_FAIL);
        Check(gpu.Render(input.View.Get(),geometryOnly,{110,110,190,190},12,.55,.18,.6,{0,0,3840,2160},predicatedOutput.View.Get())
            &&gpu.LastError()==S_OK,"accepted render clears inherited predication");
        ComPtr<ID3D11Predicate> afterRenderPredicate;context->GetPredication(&afterRenderPredicate,nullptr);
        Check(!afterRenderPredicate,"successful render unbinds caller predication");
        context->SetPredication(nullptr,FALSE); // Test readback must run even under the mutation.
        auto predicatePixels=Readback(device.Get(),context.Get(),predicatedOutput.Texture.Get());bool predicateMatch=true;
        for(size_t i=0;i<predicateExpected.size();++i)if(abs(int(predicateExpected[i])-int(predicatePixels[i]))>1)predicateMatch=false;
        Check(predicateMatch,"external immediate-context predication cannot suppress optics pixels");
        ComPtr<ID3D11ShaderResourceView> boundSource;context->PSGetShaderResources(0,1,&boundSource);
        ComPtr<ID3D11ShaderResourceView> boundRows;context->PSGetShaderResources(1,1,&boundRows);
        ComPtr<ID3D11RenderTargetView> boundTarget;context->OMGetRenderTargets(1,&boundTarget,nullptr);
        Check(!boundSource&&!boundRows&&!boundTarget,"render releases source, geometry and target bindings after draw");
        // Positive optical bands below the declared float-resolution budget are
        // unsupported. They must not silently produce a different included rim.
        auto thinBase=Base(45,100,{0,0,100,100});auto thinInput=MakeInput(device.Get(),45,100,&thinBase.Pixels);
        auto thinOutput=MakeOutput(device.Get(),45,100);const auto thinBefore=Readback(device.Get(),context.Get(),thinOutput.Texture.Get());
        for(double thinRadius:{std::ldexp(1.0,-30),std::nextafter(std::ldexp(1.0,-30),0.0),
            std::nextafter(std::ldexp(1.0,-30),INFINITY),1e-4,std::nextafter(.048828125,0.0)})
        {
            std::vector<uint8_t> thinExpected;
            Check(RenderClientCanvasOptics(thinBase,{50,0,51,100},thinRadius,0,0,1,{0,0,100,100},thinExpected),"CPU oracle accepts thin rounded scene");
            bool thinRendered=true;const auto thinVertices=DrawVertices(device.Get(),context.Get(),[&]{
                thinRendered=gpu.Render(thinInput.View.Get(),thinBase,{50,0,51,100},thinRadius,0,0,1,{0,0,100,100},thinOutput.View.Get());});
            Check(!thinRendered&&gpu.LastError()==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)&&thinVertices==0
                &&Readback(device.Get(),context.Get(),thinOutput.Texture.Get())==thinBefore,
                "unresolved optical band rejects before draw and preserves destination");
        }
        Check(gpu.Render(thinInput.View.Get(),thinBase,{50,0,51,100},std::ldexp(1.0,-30),0,0,0,{0,0,100,100},thinOutput.View.Get())
            &&gpu.LastError()==S_OK&&Readback(device.Get(),context.Get(),thinOutput.Texture.Get())==thinBase.Pixels,
            "thin-radius passthrough remains supported when optics are disabled");
        // This fixture's physical scale is100: its declared 4096-ULP
        // boundary is100*2^-11 = .048828125 physical pixels.
        for(double resolvedRadius:{.048828125,std::nextafter(.048828125,INFINITY)})
        {
            std::vector<uint8_t> expected;
            Check(RenderClientCanvasOptics(thinBase,{50,0,51,100},resolvedRadius,0,0,1,{0,0,100,100},expected),"CPU accepts the precision admission boundary");
            Check(gpu.Render(thinInput.View.Get(),thinBase,{50,0,51,100},resolvedRadius,0,0,1,{0,0,100,100},thinOutput.View.Get())
                &&gpu.LastError()==S_OK,"precision threshold and nearest larger radius remain admitted");
            const auto actual=Readback(device.Get(),context.Get(),thinOutput.Texture.Get());bool matches=true;
            for(size_t i=0;i<expected.size();++i)if(abs(int(actual[i])-int(expected[i]))>1)matches=false;
            Check(matches,"closest admitted precision-boundary output matches CPU within1LSB");
        }
        Check(SUCCEEDED(device->GetDeviceRemovedReason()),"GPU device remains healthy");
    }catch(HRESULT hr){printf("GPU failure=%08x\n",unsigned(hr));++failures;}catch(...){printf("Unexpected failure\n");++failures;}
    printf("GPU client optics: %d checks, %d failures; test-only readback, no UI/window/capture.\n",checks,failures);return failures?1:0;
}
