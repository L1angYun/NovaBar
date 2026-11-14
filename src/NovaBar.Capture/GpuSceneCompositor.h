#pragma once
#include "GpuPanelPreprocessor.h"
#include <mutex>
#include <unordered_map>

namespace novabar
{
    // All methods run on the broker MTA using the injected immediate context.
    // Only four coverage/status bytes leave the GPU; scene pixels remain there.
    class GpuSceneCompositor
    {
        template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
        static void Check(HRESULT value) { if (FAILED(value)) throw value; }
        static bool Empty(PixelRect r) { return r.Right <= r.Left || r.Bottom <= r.Top; }
        static PixelRect Intersect(PixelRect a, PixelRect b)
        { return {std::max(a.Left,b.Left),std::max(a.Top,b.Top),std::min(a.Right,b.Right),std::min(a.Bottom,b.Bottom)}; }
        static bool Same(PixelRect a, PixelRect b)
        { return a.Left==b.Left && a.Top==b.Top && a.Right==b.Right && a.Bottom==b.Bottom; }
        struct Parameters
        { int SceneBounds[4]{}, SourceRegion[4]{}, Crop[4]{}, Clip[4]{}; unsigned Layer[4]{}; };
        static_assert(sizeof(Parameters)==80);
        inline static constexpr char Shader[]=R"hlsl(
cbuffer Parameters : register(b0) {
    int4 SceneBounds; int4 SourceRegion; int4 Crop; int4 Clip; uint4 Layer;
};
Texture2D<float4> Source : register(t0);
Texture2D<uint> Previous : register(t1);
Texture2D<uint> Completed : register(t2);
uint4 Unpack(uint p) { return uint4(p & 255u,(p >> 8) & 255u,(p >> 16) & 255u,p >> 24); }
uint Pack(uint4 p) { return p.r | (p.g << 8) | (p.b << 16) | (p.a << 24); }
#ifdef COMPUTE_STAGE
RWTexture2D<uint> Scene : register(u0);
RWStructuredBuffer<uint> Status : register(u1);
[numthreads(16,16,1)]
void Clear(uint3 at : SV_DispatchThreadID) {
    if (all(at.xy < uint2(SceneBounds.zw-SceneBounds.xy))) Scene[at.xy]=0;
}
[numthreads(16,16,1)]
void Composite(uint3 at : SV_DispatchThreadID) {
    int2 world=Clip.xy+int2(at.xy);
    if (any(world>=Clip.zw)) return;
    int2 output=world-SceneBounds.xy;
    uint4 dst=Unpack(Scene[output]);
    if (dst.a==255u) return;
    if (Layer.w==0u) { InterlockedMin(Status[0],Layer.x); return; }
    int2 size=SourceRegion.zw-SourceRegion.xy;
    uint2 offset=uint2(world-SourceRegion.xy);
    if (any(Crop.zw!=size)) offset=offset*uint2(Crop.zw)/uint2(size);
    uint4 pixel=uint4(round(Source.Load(int3(Crop.xy+int2(offset),0))*255.0));
    uint3 key=uint3(Layer.z&255u,(Layer.z>>8)&255u,(Layer.z>>16)&255u);
    if (Layer.z<=0xffffffu && pixel.a==255u && all(pixel.rgb==key)) return;
    uint alpha=(pixel.a*Layer.y+127u)/255u;
    if (alpha==0u) return;
    uint remaining=255u-dst.a;
    uint3 premul=(min(pixel.rgb,pixel.aaa)*Layer.y+127u)/255u;
    uint4 next;
    next.rgb=min(255u,dst.rgb+(premul*remaining+127u)/255u);
    next.a=dst.a+(alpha*remaining+127u)/255u;
    Scene[output]=Pack(next);
}
groupshared uint GroupFlags;
[numthreads(16,16,1)]
void Finish(uint3 at:SV_DispatchThreadID,uint lane:SV_GroupIndex) {
    if (lane==0u) GroupFlags=0u;
    GroupMemoryBarrierWithGroupSync();
    if (all(at.xy<uint2(SceneBounds.zw-SceneBounds.xy))) {
        uint value=Scene[at.xy];
        uint flags=((value>>24)!=255u ? (1u<<16):0u)
            | (value!=Previous.Load(int3(at.xy,0)) ? (1u<<17):0u);
        if(flags) InterlockedOr(GroupFlags,flags);
    }
    GroupMemoryBarrierWithGroupSync();
    if(lane==0u && GroupFlags) InterlockedOr(Status[0],GroupFlags);
}
#endif
float4 Vertex(uint id:SV_VertexID):SV_Position {
    float2 p=float2((id<<1)&2,id&2);
    return float4(p*float2(2,-2)+float2(-1,1),0,1);
}
float4 Convert(float4 at:SV_Position):SV_Target {
    return float4(Unpack(Completed.Load(int3(int2(at.xy),0))))/255.0;
}
)hlsl";
        struct Pipeline
        {
            ComPtr<ID3D11ComputeShader> Clear, Composite, Finish;
            ComPtr<ID3D11VertexShader> Vertex;
            ComPtr<ID3D11PixelShader> Convert;
            ComPtr<ID3D11Buffer> Constants;
        };
        static std::shared_ptr<Pipeline> SharedPipeline(std::shared_ptr<GpuPanelPreprocessor::SharedDevice> const& device)
        {
            static std::mutex mutex;
            static std::unordered_map<ID3D11Device*,std::weak_ptr<Pipeline>> pipelines;
            std::lock_guard<std::mutex> lock(mutex);
            auto& cached=pipelines[device->Device.Get()]; if(auto existing=cached.lock()) return existing;
            auto pipeline=std::make_shared<Pipeline>();
            auto compile=[&](char const* entry,char const* profile,bool compute) {
                ComPtr<ID3DBlob> code,errors;
                const D3D_SHADER_MACRO macros[]={{"COMPUTE_STAGE","1"},{nullptr,nullptr}};
                Check(D3DCompile(Shader,sizeof(Shader)-1,"NovaBar.GpuScene",compute?macros:nullptr,nullptr,entry,profile,
                    D3DCOMPILE_OPTIMIZATION_LEVEL3|D3DCOMPILE_IEEE_STRICTNESS,0,code.GetAddressOf(),errors.GetAddressOf()));
                return code;
            };
            auto createCompute=[&](char const* entry,ComPtr<ID3D11ComputeShader>& shader) {
                auto code=compile(entry,"cs_5_0",true); Check(device->Device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,shader.GetAddressOf()));
            };
            createCompute("Clear",pipeline->Clear);createCompute("Composite",pipeline->Composite);createCompute("Finish",pipeline->Finish);
            auto code=compile("Vertex","vs_5_0",false);Check(device->Device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,pipeline->Vertex.GetAddressOf()));
            code=compile("Convert","ps_5_0",false);Check(device->Device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,pipeline->Convert.GetAddressOf()));
            D3D11_BUFFER_DESC constants{};constants.ByteWidth=sizeof(Parameters);constants.Usage=D3D11_USAGE_DEFAULT;constants.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            Check(device->Device->CreateBuffer(&constants,nullptr,pipeline->Constants.GetAddressOf()));
            cached=pipeline;return pipeline;
        }
    public:
        struct Layer
        {
            ID3D11ShaderResourceView* View=nullptr;
            PixelRect Region{},CropTexels{};
            std::vector<PixelRect> const* Clips=nullptr;
            unsigned Opacity=255;
            uint32_t ColorKey=0x1000000,Index=0;
        };
        struct Result
        {
            bool Ready=false,Changed=false;
            uint32_t FirstNeededIndex=0x7fff;
            HRESULT Error=E_PENDING;
        };
        explicit GpuSceneCompositor(std::shared_ptr<GpuPanelPreprocessor::SharedDevice> const& shared):m_shared(shared) {}
        GpuSceneCompositor(GpuSceneCompositor const&)=delete;
        GpuSceneCompositor& operator=(GpuSceneCompositor const&)=delete;
        bool Begin(PixelRect bounds) noexcept
        {
            m_begun=false; m_error=S_OK;
            try
            {
                const int64_t width=int64_t(bounds.Right)-bounds.Left,height=int64_t(bounds.Bottom)-bounds.Top;
                if(width<=0 || height<=0 || width>16384 || height>16384 || width*height>16*1024*1024) throw HRESULT(E_INVALIDARG);
                if(!m_shared || !m_shared->Device || !m_shared->Context) throw HRESULT(E_NOINTERFACE);
                if(!m_pipeline) m_pipeline=SharedPipeline(m_shared);
                Ensure(int(width),int(height)); m_geometry.Bounds=bounds;m_geometry.Width=int(width);m_geometry.Height=int(height);
                m_parameters={};Rect(m_parameters.SceneBounds,bounds);
                ResetBindings();auto* context=m_shared->Context.Get();
                const UINT empty[4]={0x7fff,0,0,0};context->ClearUnorderedAccessViewUint(m_statusView.Get(),empty);
                Dispatch(m_pipeline->Clear.Get(),int(width),int(height));m_begun=true;return true;
            }
            catch(HRESULT error){m_error=error;}
            catch(std::bad_alloc const&){m_error=E_OUTOFMEMORY;}
            catch(...){m_error=E_FAIL;}
            m_lastReady=false;
            return false;
        }
        bool Composite(Layer const& layer) noexcept
        {
            if(!m_begun || FAILED(m_error)) return false;
            try
            {
                if(layer.Index>=0x7fff) throw HRESULT(E_INVALIDARG);
                if(!layer.Opacity || Empty(Intersect(layer.Region,m_geometry.Bounds))) return true;
                const auto region=Intersect(layer.Region,m_geometry.Bounds);
                if(layer.View) Validate(layer);
                Rect(m_parameters.SourceRegion,layer.Region);
                m_parameters.Crop[0]=layer.CropTexels.Left;m_parameters.Crop[1]=layer.CropTexels.Top;
                m_parameters.Crop[2]=layer.CropTexels.Right-layer.CropTexels.Left;
                m_parameters.Crop[3]=layer.CropTexels.Bottom-layer.CropTexels.Top;
                m_parameters.Layer[0]=layer.Index;m_parameters.Layer[1]=std::min(layer.Opacity,255u);
                m_parameters.Layer[2]=layer.ColorKey;m_parameters.Layer[3]=layer.View?1u:0u;
                auto submit=[&](PixelRect clip) {
                    clip=Intersect(region,clip);if(Empty(clip))return;
                    Rect(m_parameters.Clip,clip);Dispatch(m_pipeline->Composite.Get(),clip.Right-clip.Left,clip.Bottom-clip.Top,layer.View);
                };
                if(!layer.Clips || layer.Clips->empty()) submit(region);else for(auto const& clip:*layer.Clips)submit(clip);
                return true;
            }
            catch(HRESULT error){m_error=error;}
            catch(...){m_error=E_FAIL;}
            return false;
        }
        Result Finish(bool forceChanged=false) noexcept
        {
            Result result;
            if(!m_begun || FAILED(m_error)){m_lastReady=false;result.Error=FAILED(m_error)?m_error:E_UNEXPECTED;return result;}
            m_begun=false;
            try
            {
                auto* context=m_shared->Context.Get();
                Dispatch(m_pipeline->Finish.Get(),m_geometry.Width,m_geometry.Height,nullptr,m_previousView.Get());
                ResetBindings();context->CopyResource(m_statusReadback.Get(),m_status.Get());
                D3D11_MAPPED_SUBRESOURCE mapped{};Check(context->Map(m_statusReadback.Get(),0,D3D11_MAP_READ,0,&mapped));
                const uint32_t status=*static_cast<uint32_t const*>(mapped.pData);context->Unmap(m_statusReadback.Get(),0);
                result.FirstNeededIndex=status&0x7fff;
                result.Ready=result.FirstNeededIndex==0x7fff && !(status&(1u<<16));
                result.Error=result.Ready?S_OK:E_PENDING;
                if(!result.Ready){m_lastReady=false;return result;}
                result.Changed=forceChanged || !m_lastReady || !m_havePrevious || !Same(m_lastBounds,m_geometry.Bounds) || (status&(1u<<17));
                // Skip conversion when exact scene bytes and physical placement are unchanged.
                if(result.Changed)Convert();
                std::swap(m_current,m_previous);std::swap(m_currentView,m_previousView);std::swap(m_currentUav,m_previousUav);
                m_lastBounds=m_geometry.Bounds;m_havePrevious=true;m_lastReady=true;m_error=S_OK;return result;
            }
            catch(HRESULT error){m_error=error;}
            catch(std::bad_alloc const&){m_error=E_OUTOFMEMORY;}
            catch(...){m_error=E_FAIL;}
            m_lastReady=false;result.Ready=false;result.Changed=false;result.Error=m_error;return result;
        }
        ID3D11ShaderResourceView* View() const { return m_convertedView.Get(); }
        PixelFrame const& Geometry() const { return m_geometry; }
        HRESULT LastError() const { return m_error; }
    private:
        std::shared_ptr<GpuPanelPreprocessor::SharedDevice> m_shared;
        std::shared_ptr<Pipeline> m_pipeline;
        PixelFrame m_geometry;
        PixelRect m_lastBounds{};
        Parameters m_parameters{};
        HRESULT m_error=S_OK;
        bool m_begun=false,m_havePrevious=false,m_lastReady=false;
        int m_width=0,m_height=0;
        ComPtr<ID3D11Texture2D> m_current,m_previous,m_converted;
        ComPtr<ID3D11ShaderResourceView> m_currentView,m_previousView,m_convertedView;
        ComPtr<ID3D11UnorderedAccessView> m_currentUav,m_previousUav;
        ComPtr<ID3D11RenderTargetView> m_convertedTarget;
        ComPtr<ID3D11Buffer> m_status,m_statusReadback;
        ComPtr<ID3D11UnorderedAccessView> m_statusView;
        static void Rect(int* values,PixelRect rect)
        {values[0]=rect.Left;values[1]=rect.Top;values[2]=rect.Right;values[3]=rect.Bottom;}
        void ResetBindings()
        {
            auto* context=m_shared->Context.Get();ID3D11ShaderResourceView* empty[3]={};ID3D11UnorderedAccessView* noUav[2]={};
            context->CSSetShaderResources(0,3,empty);context->PSSetShaderResources(0,3,empty);context->CSSetUnorderedAccessViews(0,2,noUav,nullptr);
            context->OMSetRenderTargets(0,nullptr,nullptr);context->CSSetShader(nullptr,nullptr,0);
            context->OMSetBlendState(nullptr,nullptr,0xffffffff);context->OMSetDepthStencilState(nullptr,0);
        }
        void Ensure(int width,int height)
        {
            if(m_width==width && m_height==height)return;
            // Allocation can fail after releasing only part of the old set.
            // No later request, including the previous size, may reuse it.
            m_width=0;m_height=0;m_havePrevious=false;m_lastReady=false;
            ResetBindings();m_current.Reset();m_previous.Reset();m_converted.Reset();m_currentView.Reset();m_previousView.Reset();
            m_currentUav.Reset();m_previousUav.Reset();m_convertedView.Reset();m_convertedTarget.Reset();
            auto texture=[&](DXGI_FORMAT format,UINT flags,ComPtr<ID3D11Texture2D>& value,ComPtr<ID3D11ShaderResourceView>& view) {
                D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=1;d.Format=format;d.SampleDesc.Count=1;d.BindFlags=flags;
                Check(m_shared->Device->CreateTexture2D(&d,nullptr,value.GetAddressOf()));Check(m_shared->Device->CreateShaderResourceView(value.Get(),nullptr,view.GetAddressOf()));
            };
            texture(DXGI_FORMAT_R32_UINT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,m_current,m_currentView);
            texture(DXGI_FORMAT_R32_UINT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,m_previous,m_previousView);
            Check(m_shared->Device->CreateUnorderedAccessView(m_current.Get(),nullptr,m_currentUav.GetAddressOf()));
            Check(m_shared->Device->CreateUnorderedAccessView(m_previous.Get(),nullptr,m_previousUav.GetAddressOf()));
            const UINT zero[4]={};m_shared->Context->ClearUnorderedAccessViewUint(m_previousUav.Get(),zero);
            texture(DXGI_FORMAT_B8G8R8A8_UNORM,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET,m_converted,m_convertedView);
            Check(m_shared->Device->CreateRenderTargetView(m_converted.Get(),nullptr,m_convertedTarget.GetAddressOf()));
            if(!m_status || !m_statusView || !m_statusReadback)
            {
                m_status.Reset();m_statusView.Reset();m_statusReadback.Reset();
                D3D11_BUFFER_DESC d{};d.ByteWidth=4;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.StructureByteStride=4;
                Check(m_shared->Device->CreateBuffer(&d,nullptr,m_status.GetAddressOf()));
                D3D11_UNORDERED_ACCESS_VIEW_DESC u{};u.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;u.Buffer.NumElements=1;
                Check(m_shared->Device->CreateUnorderedAccessView(m_status.Get(),&u,m_statusView.GetAddressOf()));
                d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;d.StructureByteStride=0;
                Check(m_shared->Device->CreateBuffer(&d,nullptr,m_statusReadback.GetAddressOf()));
            }
            m_width=width;m_height=height;m_havePrevious=false;m_lastReady=false;
        }
        void Validate(Layer const& layer)
        {
            ComPtr<ID3D11Device> owner;layer.View->GetDevice(owner.GetAddressOf());if(owner.Get()!=m_shared->Device.Get())throw HRESULT(E_INVALIDARG);
            D3D11_SHADER_RESOURCE_VIEW_DESC view{};layer.View->GetDesc(&view);
            if(view.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || view.Texture2D.MostDetailedMip!=0
                || (view.Format!=DXGI_FORMAT_B8G8R8A8_UNORM && view.Format!=DXGI_FORMAT_R8G8B8A8_UNORM))throw HRESULT(E_INVALIDARG);
            ComPtr<ID3D11Resource> resource;layer.View->GetResource(resource.GetAddressOf());ComPtr<ID3D11Texture2D> texture;Check(resource.As(&texture));
            D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);
            const int64_t width=int64_t(layer.Region.Right)-layer.Region.Left,height=int64_t(layer.Region.Bottom)-layer.Region.Top;
            if(width<=0 || height<=0 || width>16384 || height>16384 || Empty(layer.CropTexels)
                || layer.CropTexels.Left<0 || layer.CropTexels.Top<0 || uint64_t(layer.CropTexels.Right)>d.Width
                || uint64_t(layer.CropTexels.Bottom)>d.Height || d.SampleDesc.Count!=1)throw HRESULT(E_INVALIDARG);
        }
        void Dispatch(ID3D11ComputeShader* shader,int width,int height,ID3D11ShaderResourceView* source=nullptr,ID3D11ShaderResourceView* previous=nullptr)
        {
            auto* context=m_shared->Context.Get();context->UpdateSubresource(m_pipeline->Constants.Get(),0,nullptr,&m_parameters,0,0);
            auto* constants=m_pipeline->Constants.Get();context->CSSetConstantBuffers(0,1,&constants);
            ID3D11ShaderResourceView* views[2]={source,previous};context->CSSetShaderResources(0,2,views);
            ID3D11UnorderedAccessView* uavs[2]={m_currentUav.Get(),m_statusView.Get()};context->CSSetUnorderedAccessViews(0,2,uavs,nullptr);
            context->CSSetShader(shader,nullptr,0);context->Dispatch((UINT(width)+15)/16,(UINT(height)+15)/16,1);
            ID3D11ShaderResourceView* empty[2]={};ID3D11UnorderedAccessView* noUav[2]={};context->CSSetShaderResources(0,2,empty);context->CSSetUnorderedAccessViews(0,2,noUav,nullptr);
        }
        void Convert()
        {
            auto* context=m_shared->Context.Get();ResetBindings();context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(m_pipeline->Vertex.Get(),nullptr,0);context->PSSetShader(m_pipeline->Convert.Get(),nullptr,0);
            context->RSSetState(m_shared->Rasterizer.Get());const D3D11_VIEWPORT viewport{0,0,float(m_geometry.Width),float(m_geometry.Height),0,1};context->RSSetViewports(1,&viewport);
            auto* target=m_convertedTarget.Get();context->OMSetRenderTargets(1,&target,nullptr);auto* view=m_currentView.Get();context->PSSetShaderResources(2,1,&view);
            context->Draw(3,0);ResetBindings();
        }
    };
}
