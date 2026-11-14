#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <new>
#include <limits>
#include "PanelGlassPixels.h"

namespace novabar
{
// One externally synchronized immediate context. Render replaces pipeline state;
// the caller owns target allocation, GPU completion and compositor release fences.
// No CPU pixels are consumed. Only the supplied destination is written; the
// caller must not reuse an output while its consumers still retain it.
class GpuClientCanvasOptics
{
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11Device> m_device;Ptr<ID3D11DeviceContext> m_context;
    Ptr<ID3D11VertexShader> m_vertex;Ptr<ID3D11PixelShader> m_pixel;
    Ptr<ID3D11Buffer> m_constants;Ptr<ID3D11RasterizerState> m_raster;
    // Geometry metadata only: exact outer inclusion and sharp-pane normal splits.
    // Both CPU storage and its typed GPU buffer are allocated once, never in Render.
    static constexpr UINT MaximumHeight = 16384;
    struct RowGeometry { UINT First, Last, LeftNormalEnd, RightNormalBegin; };
    static_assert(sizeof(RowGeometry) == 16);
    std::unique_ptr<RowGeometry[]> m_rows;
    Ptr<ID3D11Buffer> m_rowBuffer;
    Ptr<ID3D11ShaderResourceView> m_rowView;
    HRESULT m_lastError = S_OK;
    struct alignas(16) Parameters
    {
        int Size[4]{};
        float Physical[4]{},Target[4]{},Shape[4]{},Material[4]{},Viewport[4]{},Limits[4]{};
        int Clip[4]{};
    };
    static_assert(sizeof(Parameters)==128);
    static void Check(HRESULT hr){if(FAILED(hr))throw hr;}
    static bool TextureMatches(ID3D11Resource* resource, PixelFrame const& base, bool privateSource) noexcept
    {
        Ptr<ID3D11Texture2D> texture;
        if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) return false;
        D3D11_TEXTURE2D_DESC d{};
        texture->GetDesc(&d);
        return d.Width == UINT(base.Width) && d.Height == UINT(base.Height)
            && d.MipLevels == 1 && d.ArraySize == 1
            && d.Format == DXGI_FORMAT_B8G8R8A8_UNORM
            && d.SampleDesc.Count == 1 && d.SampleDesc.Quality == 0
            && (!privateSource || d.MiscFlags == 0);
    }
    bool SourceMatches(ID3D11ShaderResourceView* source, PixelFrame const& base,
        Ptr<ID3D11Resource>& resource) const noexcept
    {
        // Optics consumes the client's private GPU base (MiscFlags == 0), never
        // a shared input directly. Reopened shared allocations can have distinct
        // COM identities, so identity comparison alone cannot exclude feedback.
        // Geometry deliberately does not require a CPU Pixels vector.
        Ptr<ID3D11Device> owner;source->GetDevice(&owner);if(owner.Get()!=m_device.Get())return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};source->GetDesc(&view);
        if(view.Format!=DXGI_FORMAT_B8G8R8A8_UNORM||view.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D
            ||view.Texture2D.MostDetailedMip!=0||view.Texture2D.MipLevels!=1)return false;
        source->GetResource(&resource);
        return resource && TextureMatches(resource.Get(), base, true);
    }
    bool DestinationMatches(ID3D11RenderTargetView* destination, PixelFrame const& base,
        Ptr<ID3D11Resource>& resource) const noexcept
    {
        Ptr<ID3D11Device> owner;
        destination->GetDevice(&owner);
        if (owner.Get() != m_device.Get()) return false;
        D3D11_RENDER_TARGET_VIEW_DESC view{};
        destination->GetDesc(&view);
        if (view.Format != DXGI_FORMAT_B8G8R8A8_UNORM
            || view.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D
            || view.Texture2D.MipSlice != 0) return false;
        destination->GetResource(&resource);
        return resource && TextureMatches(resource.Get(), base, false);
    }
    static bool SourceCoverage(novabar::PixelRect canvas,novabar::PixelRect target,novabar::PixelRect viewport,
        novabar::panel_detail::EdgeGeometry const& shape,double lens,double dispersion)
    {
        if(!lens)return true;
        const double left=std::max(target.Left,viewport.Left),right=std::min(target.Right,viewport.Right);
        const double top=std::max(target.Top,viewport.Top),bottom=std::min(target.Bottom,viewport.Bottom);
        if(right<=left||bottom<=top)return true;
        if(left<canvas.Left||right>canvas.Right||top<canvas.Top||bottom>canvas.Bottom)return false;
        const double minSpan=double(std::min(int64_t(target.Right)-target.Left,int64_t(target.Bottom)-target.Top));
        // All channel shifts are inward. With e in[0,1], shift/(band*e)
        // <= .59*e*(3-2e) <= .66375: they cannot cross the inner band limit.
        // band<=minSpan therefore stays within the material rectangle; curved
        // normals additionally have band<=radius. Viewport projection then
        // remains in target intersect viewport, already proved present above.
        if(shape.Band<=minSpan)return true;
        // Very small sharp panes can overshoot their opposite material edge.
        // Admit only when even this conservative sampling envelope is present.
        const double reach=shape.MaxShift*lens*(1+dispersion)+1e-3;
        return std::clamp(left-reach,double(viewport.Left),double(viewport.Right))>=canvas.Left
            &&std::clamp(right+reach,double(viewport.Left),double(viewport.Right))<=canvas.Right
            &&std::clamp(top-reach,double(viewport.Top),double(viewport.Bottom))>=canvas.Top
            &&std::clamp(bottom+reach,double(viewport.Top),double(viewport.Bottom))<=canvas.Bottom;
    }
    static bool PrecisionSupported(PixelFrame const& base, PixelRect target,
        panel_detail::EdgeGeometry const& shape, double lens, double specular) noexcept
    {
        if (!panel_detail::Unit(lens) && !panel_detail::Unit(specular)) return true;
        const double cw = double(int64_t(base.Bounds.Right) - base.Bounds.Left);
        const double ch = double(int64_t(base.Bounds.Bottom) - base.Bounds.Top);
        const double pw = 1 / (double(base.Width) / cw), ph = 1 / (double(base.Height) / ch);
        // A half-physical-pixel grid maps centers and integer/half-integer shape
        // centers exactly in float under the admitted 32768-span caps. Other
        // grids also budget for cancellation at the full physical geometry scale.
        const bool exactGrid = pw * 2 == std::floor(pw * 2) && ph * 2 == std::floor(ph * 2);
        double scale = std::max({1.0, shape.Radius, shape.Band});
        if (!exactGrid) scale = std::max({scale, cw, ch,
            double(int64_t(target.Right) - target.Left), double(int64_t(target.Bottom) - target.Top)});
        // The rim contains an eighth power and up to 65 byte levels of light.
        // Reserve a conservative 4096-ULP geometry budget instead of claiming
        // support for a band erased by coordinate/radius rounding. This is an
        // admission restriction; it never expands the CPU's outer inclusion.
        return shape.Band >= scale * (4096 * std::numeric_limits<float>::epsilon());
    }
    void BuildRows(PixelFrame const& base, PixelRect target,
        panel_detail::EdgeGeometry const& shape, int const* clip) noexcept
    {
        const double sx = double(base.Width) / (int64_t(base.Bounds.Right) - base.Bounds.Left);
        const double sy = double(base.Height) / (int64_t(base.Bounds.Bottom) - base.Bounds.Top);
        const double pw = 1 / sx, ph = 1 / sy;
        const auto localX = [&](int x) {
            // Preserve the CPU oracle's operation order, including its absolute
            // physical origin. Reassociating these doubles changes boundary ties.
            return base.Bounds.Left + (x + .5) * pw - (target.Left + shape.HalfWidth);
        };
        for (int y = clip[1]; y < clip[3]; ++y)
        {
            auto& row = m_rows[y];
            row = {UINT(clip[0]), UINT(clip[2]), UINT(clip[0]), UINT(clip[2])};
            if (clip[0] >= clip[2]) continue;
            const double localY = base.Bounds.Top + (y + .5) * ph - (target.Top + shape.HalfHeight);
            if (shape.Radius > 0)
            {
                // Convex rows need at most two exact double-SDF searches.
                // Straight rows ordinarily use the clip, but test their endpoints:
                // even the CPU's radius subtraction can round a boundary outside.
                const auto inside = [&](int x) {
                    return panel_detail::EvaluateEdge(shape, localX(x), localY, 0, 0).Distance <= 0;
                };
                if (std::abs(localY) <= shape.HalfHeight - shape.Radius
                    && inside(clip[0]) && inside(clip[2] - 1)) continue;
                int nearest = std::clamp(int(std::floor((target.Left + shape.HalfWidth - base.Bounds.Left) * sx - .5)), clip[0], clip[2] - 1);
                if (nearest + 1 < clip[2] && std::abs(localX(nearest + 1)) < std::abs(localX(nearest))) ++nearest;
                if (!inside(nearest)) { row.First = row.Last = 0; continue; }
                int low = clip[0], high = nearest;
                while (low < high) { const int mid = low + (high - low) / 2; if (inside(mid)) high = mid; else low = mid + 1; }
                row.First = UINT(low);
                low = nearest; high = clip[2];
                while (low < high) { const int mid = low + (high - low) / 2; if (inside(mid)) low = mid + 1; else high = mid; }
                row.Last = UINT(low);
            }
            else
            {
                // Sharp panes can have an active optical band across their
                // medial axis. Match the CPU's discontinuous qx>qy normal choice
                // exactly, rather than allowing float cancellation to flip it.
                const double qy = std::abs(localY) - shape.HalfHeight;
                const auto leftNormal = [&](int x) { const double px = localX(x); return px < 0 && std::abs(px) - shape.HalfWidth > qy; };
                const auto rightNormal = [&](int x) { const double px = localX(x); return px >= 0 && std::abs(px) - shape.HalfWidth > qy; };
                int low = clip[0], high = clip[2];
                while (low < high) { const int mid = low + (high - low) / 2; if (leftNormal(mid)) low = mid + 1; else high = mid; }
                row.LeftNormalEnd = UINT(low) | (localY < 0 ? 0x80000000u : 0);
                low = clip[0]; high = clip[2];
                while (low < high) { const int mid = low + (high - low) / 2; if (rightNormal(mid)) high = mid; else low = mid + 1; }
                row.RightNormalBegin = UINT(low);
            }
        }
    }
    inline static constexpr char Shader[]=R"hlsl(
cbuffer Parameters:register(b0) {
    int4 Size;float4 Physical,Target,Shape,Material,Viewport,Limits;int4 Clip;
};
Texture2D<float4> Source:register(t0);
Buffer<uint4> Rows:register(t1);
float4 Vertex(uint vertex:SV_VertexID):SV_Position {
    float2 p=float2((vertex<<1)&2,vertex&2);
    return float4(p*float2(2,-2)+float2(-1,1),0,1);
}
float4 ReadAt(float2 q) {
    q=clamp(q,Viewport.xy,Viewport.zw);
    // CPU admission proves all sampled in-desktop coordinates are present.
    // Missing source is rejected before Draw; it is never filled with zero.
    q=clamp(q,float2(Limits.x,Limits.z),float2(Limits.y,Limits.w));
    float2 p=clamp(q*float2(Size.xy)/Physical.xy-.5,0,float2(Size.xy-1));
    int2 at=int2(p),next=min(at+1,Size.xy-1);float2 f=p-at;
    float4 a=round(Source.Load(int3(at,0))*255);
    float4 b=round(Source.Load(int3(next.x,at.y,0))*255);
    float4 c=round(Source.Load(int3(at.x,next.y,0))*255);
    float4 d=round(Source.Load(int3(next,0))*255);
    precise float4 top=a+(b-a)*f.x;
    precise float4 bottom=c+(d-c)*f.x;
    return top+(bottom-top)*f.y;
}
float4 Pixel(float4 position:SV_Position):SV_Target {
    int2 at=int2(position.xy);float4 original=Source.Load(int3(at,0));
    if((!Material.y&&!Material.w)||Shape.w<=0)return original;
    if(any(at<Clip.xy)||any(at>=Clip.zw))return original;
    uint4 row=Rows.Load(at.y);
    if(uint(at.x)<row.x||uint(at.x)>=row.y)return original;
    float2 q=position.xy*Physical.zw;
    float2 p=q-(Target.xy+Shape.xy);
    // Equivalent rounded-box SDF, ordered to retain a small radius at an
    // exactly aligned edge instead of losing it in halfExtent-radius.
    float2 boxDistance=abs(p)-Shape.xy;
    float2 edgeq=boxDistance+Shape.z,outside=max(edgeq,0);
    float len=length(outside),distance=len>0?len-Shape.z:max(boxDistance.x,boxDistance.y);
    // CPU-double row spans decide outer inclusion; float distance can round
    // positive at a CPU-included boundary and must not discard its bright rim.
    if(distance < -Shape.w)return original;
    float2 normal=0;
    if(Size.z) {
        if(uint(at.x)<(row.z&0x7fffffff))normal.x=-1;
        else if(uint(at.x)>=row.w)normal.x=1;
        else normal.y=(row.z&0x80000000)?-1:1;
    }
    else if(len>0)normal=outside/len*float2(p.x<0?-1:1,p.y<0?-1:1);
    else if(boxDistance.x>boxDistance.y)normal.x=p.x<0?-1:1;
    else normal.y=p.y<0?-1:1;
    float e=saturate(1+distance/Shape.w);
    float shift=Material.x*Material.y*(e*e*(3-2*e));float2 delta=-normal*shift;
    float4 value=ReadAt(q+delta);
    if(Material.z&&shift) {
        value.b=ReadAt(q+delta*(1-Material.z)).b;
        value.r=ReadAt(q+delta*(1+Material.z)).r;
    }
    float e2=e*e,e4=e2*e2;
    float light=saturate(.45-normal.x*.35-normal.y*.55);
    float alpha=clamp(value.a,0,255);
    float highlight=e4*e4*Material.w*light*65*(alpha/255);
    return floor(float4(clamp(value.rgb+highlight,0,alpha),alpha)+.5)/255;
}
)hlsl";
public:
    // Pipeline initialization throws its HRESULT on failure; Render is noexcept.
    GpuClientCanvasOptics(ID3D11Device* device, ID3D11DeviceContext* context):m_device(device),m_context(context)
    {
        if(!device||!context)Check(E_INVALIDARG);
        Ptr<ID3D11Device> owner;context->GetDevice(&owner);
        if(owner.Get()!=device||context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)Check(E_INVALIDARG);
        auto compile=[](char const* entry,char const* profile){Ptr<ID3DBlob> code,errors;
            const auto hr=D3DCompile(Shader,sizeof(Shader)-1,"NovaBar.GpuClientCanvasOptics",nullptr,nullptr,entry,profile,
                D3DCOMPILE_OPTIMIZATION_LEVEL3|D3DCOMPILE_IEEE_STRICTNESS,0,&code,&errors);
            Check(hr);return code;};
        auto code=compile("Vertex","vs_5_0");Check(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&m_vertex));
        code=compile("Pixel","ps_5_0");Check(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&m_pixel));
        D3D11_BUFFER_DESC b{};b.ByteWidth=sizeof(Parameters);b.Usage=D3D11_USAGE_DEFAULT;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        Check(device->CreateBuffer(&b,nullptr,&m_constants));
        m_rows.reset(new (std::nothrow) RowGeometry[MaximumHeight]);
        if (!m_rows) Check(E_OUTOFMEMORY);
        b.ByteWidth = sizeof(RowGeometry) * MaximumHeight;
        b.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        Check(device->CreateBuffer(&b,nullptr,&m_rowBuffer));
        D3D11_SHADER_RESOURCE_VIEW_DESC rows{};
        rows.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        rows.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        rows.Buffer.NumElements = MaximumHeight;
        Check(device->CreateShaderResourceView(m_rowBuffer.Get(),&rows,&m_rowView));
        D3D11_RASTERIZER_DESC r{};r.FillMode=D3D11_FILL_SOLID;r.CullMode=D3D11_CULL_NONE;r.DepthClipEnable=TRUE;
        Check(device->CreateRasterizerState(&r,&m_raster));
    }
    HRESULT LastError() const noexcept { return m_lastError; }

    // Admission failures leave the caller's target and context untouched.
    // ERROR_NOT_SUPPORTED means source coverage or the optical band's float
    // precision budget cannot be proved. Invalid resource/geometry is E_INVALIDARG;
    // device failures retain their original HRESULT. The native radius12 case is
    // tested; unresolved subpixel bands require the genuine CPU path.
    bool Render(ID3D11ShaderResourceView* source, novabar::PixelFrame const& base, novabar::PixelRect target,
        double radius, double lens, double dispersion, double specular, novabar::PixelRect viewport,
        ID3D11RenderTargetView* destination) noexcept
    {
        m_lastError = E_INVALIDARG;
        try {
        const int64_t cw=int64_t(base.Bounds.Right)-base.Bounds.Left,ch=int64_t(base.Bounds.Bottom)-base.Bounds.Top;
        const int64_t tw=int64_t(target.Right)-target.Left,th=int64_t(target.Bottom)-target.Top;
        if(!source||!destination||base.Width<=0||base.Height<=0||base.Width>16384||base.Height>16384
            ||uint64_t(base.Width)*base.Height>8ull*1024*1024||cw<=0||ch<=0||cw>32768||ch>32768
            ||tw<=0||th<=0||tw>32768||th>32768
            ||viewport.Right<=viewport.Left||viewport.Bottom<=viewport.Top||!std::isfinite(radius)||radius<0
            ||!std::isfinite(lens)||!std::isfinite(dispersion)||!std::isfinite(specular))return false;
        Ptr<ID3D11Resource> inputResource, outputResource;
        if (!SourceMatches(source, base, inputResource)
            || !DestinationMatches(destination, base, outputResource)) return false;
        Ptr<IUnknown> inputIdentity, outputIdentity;
        Check(inputResource.As(&inputIdentity));
        Check(outputResource.As(&outputIdentity));
        if (inputIdentity.Get() == outputIdentity.Get()) return false;
        Check(m_device->GetDeviceRemovedReason());
        auto shape=panel_detail::MakeEdgeGeometry(target,radius);
        if (!SourceCoverage(base.Bounds,target,viewport,shape,panel_detail::Unit(lens),panel_detail::Unit(dispersion)*.18)
            || !PrecisionSupported(base,target,shape,lens,specular))
        {
            m_lastError = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            return false;
        }
        Parameters p;
        p.Size[0]=base.Width;p.Size[1]=base.Height;p.Size[2]=shape.Radius==0;
        p.Physical[0]=float(cw);p.Physical[1]=float(ch);p.Physical[2]=float(double(cw)/base.Width);p.Physical[3]=float(double(ch)/base.Height);
        p.Target[0]=float(int64_t(target.Left)-base.Bounds.Left);p.Target[1]=float(int64_t(target.Top)-base.Bounds.Top);
        p.Target[2]=float(int64_t(target.Right)-base.Bounds.Left);p.Target[3]=float(int64_t(target.Bottom)-base.Bounds.Top);
        p.Shape[0]=float(shape.HalfWidth);p.Shape[1]=float(shape.HalfHeight);p.Shape[2]=float(shape.Radius);p.Shape[3]=float(shape.Band);
        p.Material[0]=float(shape.MaxShift);p.Material[1]=float(panel_detail::Unit(lens));p.Material[2]=float(panel_detail::Unit(dispersion)*.18);p.Material[3]=float(panel_detail::Unit(specular));
        p.Viewport[0]=float(int64_t(viewport.Left)-base.Bounds.Left);p.Viewport[1]=float(int64_t(viewport.Top)-base.Bounds.Top);
        p.Viewport[2]=float(int64_t(viewport.Right)-base.Bounds.Left);p.Viewport[3]=float(int64_t(viewport.Bottom)-base.Bounds.Top);
        const double sx=double(base.Width)/cw,sy=double(base.Height)/ch;
        const auto first=[](double edge,double scale,int maximum){return (std::clamp(std::ceil(edge*scale-.5),0.0,double(maximum-1))+.5)/scale;};
        const auto last=[](double edge,double scale,int maximum){return (std::clamp(std::ceil(edge*scale-.5)-1,0.0,double(maximum-1))+.5)/scale;};
        p.Limits[0]=base.Bounds.Left<=viewport.Left?float(first(double(int64_t(viewport.Left)-base.Bounds.Left),sx,base.Width)):0;
        p.Limits[1]=base.Bounds.Right>=viewport.Right?float(last(double(int64_t(viewport.Right)-base.Bounds.Left),sx,base.Width)):float(cw);
        p.Limits[2]=base.Bounds.Top<=viewport.Top?float(first(double(int64_t(viewport.Top)-base.Bounds.Top),sy,base.Height)):0;
        p.Limits[3]=base.Bounds.Bottom>=viewport.Bottom?float(last(double(int64_t(viewport.Bottom)-base.Bounds.Top),sy,base.Height)):float(ch);
        const auto index=[](double edge,double scale,int maximum){return int(std::clamp(std::ceil(edge*scale-.5),0.0,double(maximum)));};
        p.Clip[0]=index(double(int64_t(std::max({target.Left,base.Bounds.Left,viewport.Left}))-base.Bounds.Left),sx,base.Width);
        p.Clip[1]=index(double(int64_t(std::max({target.Top,base.Bounds.Top,viewport.Top}))-base.Bounds.Top),sy,base.Height);
        p.Clip[2]=index(double(int64_t(std::min({target.Right,base.Bounds.Right,viewport.Right}))-base.Bounds.Left),sx,base.Width);
        p.Clip[3]=index(double(int64_t(std::min({target.Bottom,base.Bounds.Bottom,viewport.Bottom}))-base.Bounds.Top),sy,base.Height);
        BuildRows(base, target, shape, p.Clip);
        // Clear inherited UAV, stream-output and predication hazards before binding
        // the complete pipeline. This does not allocate or synchronize GPU work.
        m_context->ClearState();
        if (p.Clip[1] < p.Clip[3] && p.Clip[0] < p.Clip[2])
        {
            const D3D11_BOX rows{UINT(p.Clip[1]) * sizeof(RowGeometry), 0, 0,
                UINT(p.Clip[3]) * sizeof(RowGeometry), 1, 1};
            m_context->UpdateSubresource(m_rowBuffer.Get(),0,&rows,m_rows.get()+p.Clip[1],0,0);
        }
        m_context->UpdateSubresource(m_constants.Get(),0,nullptr,&p,0,0);
        auto* cb=m_constants.Get();
        auto* targetView=destination;
        m_context->OMSetRenderTargets(1,&targetView,nullptr);m_context->OMSetBlendState(nullptr,nullptr,0xffffffff);m_context->OMSetDepthStencilState(nullptr,0);
        const D3D11_VIEWPORT vp{0,0,float(base.Width),float(base.Height),0,1};m_context->RSSetViewports(1,&vp);m_context->RSSetState(m_raster.Get());
        m_context->IASetInputLayout(nullptr);m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->HSSetShader(nullptr,nullptr,0);m_context->DSSetShader(nullptr,nullptr,0);m_context->GSSetShader(nullptr,nullptr,0);
        m_context->VSSetShader(m_vertex.Get(),nullptr,0);m_context->PSSetShader(m_pixel.Get(),nullptr,0);m_context->PSSetConstantBuffers(0,1,&cb);
        ID3D11ShaderResourceView* sources[] = {source,m_rowView.Get()};
        m_context->PSSetShaderResources(0,2,sources);
        m_context->Draw(3,0);ID3D11ShaderResourceView* empty[2]{};m_context->PSSetShaderResources(0,2,empty);m_context->OMSetRenderTargets(0,nullptr,nullptr);
        Check(m_device->GetDeviceRemovedReason());
        m_lastError = S_OK;
        return true;
        }
        catch (HRESULT error) { m_lastError = error; return false; }
        catch (...) { m_lastError = E_FAIL; return false; }
    }
};

}
