#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstring>
#include <memory>
#include <new>
#include <array>
#include "PanelGlassPixels.h"

namespace novabar
{
    // Broker MTA only. One device/pipeline is shared; targets own their textures.
    // Input is a CPU or GPU composed scene from validated source windows.
    class GpuPanelPreprocessor
    {
        template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
        struct alignas(16) Parameters
        {
            int Size[4]{}, Radius[4]{};
            float Mapping[4]{};
            int Clip[4]{};
            float TintNoise[4]{}, TintColor[4]{};
        };
        static_assert(sizeof(Parameters) == 96);
        static void Check(HRESULT error) { if (FAILED(error)) throw error; }
        inline static constexpr char Shader[] = R"hlsl(
cbuffer Parameters : register(b0) {
    int4 Size; int4 Radius; float4 Mapping; int4 Clip;
    float4 TintNoise; float4 TintColor;
};
Texture2D<float4> Input : register(t0);
Buffer<int4> TaskbarRows : register(t1);
float4 Vertex(uint vertex : SV_VertexID) : SV_Position {
    float2 p = float2((vertex << 1) & 2, vertex & 2);
    return float4(p * float2(2,-2) + float2(-1,1), 0, 1);
}
float4 Blur(float4 position : SV_Position) : SV_Target {
    int2 at = int2(position.xy);
    int radius = Radius.z == 0 ? Radius.x : Radius.y;
    precise float4 sum = 0;
    [loop] for (int i = -radius; i <= radius; ++i) {
        int2 xy = clamp(at + (Radius.z == 0 ? int2(i,0) : int2(0,i)), int2(0,0), Size.xy - 1);
        sum += round(Input.Load(int3(xy,0)) * 255.0);
    }
    // Match the CPU's byte rounding after each separable pass.
    precise float reciprocal = 1.0 / (2 * radius + 1);
    return floor(sum * reciprocal + 0.5) / 255.0;
}
float4 Base(float4 position : SV_Position) : SV_Target {
    int2 pixel = int2(position.xy);
    if (pixel.x < Clip.x || pixel.y < Clip.y || pixel.x >= Clip.z || pixel.y >= Clip.w) return 0;
    float2 p = Mapping.zw + position.xy * Mapping.xy - 0.5;
    float4 sample = 0;
    if (all(p >= -0.5) && all(p <= float2(Size.xy) - 0.5)) {
        p = clamp(p, float2(0,0), float2(Size.xy - 1));
        int2 at = int2(p), next = min(at + 1, Size.xy - 1);
        float2 fraction = p - at;
        float4 a = round(Input.Load(int3(at,0)) * 255.0);
        float4 b = round(Input.Load(int3(next.x,at.y,0)) * 255.0);
        float4 c = round(Input.Load(int3(at.x,next.y,0)) * 255.0);
        float4 d = round(Input.Load(int3(next,0)) * 255.0);
        precise float4 top = a + (b - a) * fraction.x;
        precise float4 bottom = c + (d - c) * fraction.x;
        sample = top + (bottom - top) * fraction.y;
    }
    uint hash = (uint(pixel.x) * 374761393u + uint(pixel.y) * 668265263u) ^ 0x9e3779b9u;
    precise float grain = (float((hash ^ (hash >> 13)) & 255u) * (1.0 / 255.0) - 0.5) * TintNoise.y;
    float alpha = clamp(sample.a, 0, 255);
    precise float alphaUnit = alpha * (1.0 / 255.0);
    precise float3 color = sample.rgb * (1 - TintNoise.x) + (TintColor.rgb * TintNoise.x + grain) * alphaUnit;
    return floor(float4(clamp(color, 0, alpha), alpha) + 0.5) / 255.0;
}
float TaskbarSample(int x, int y, uint channel) {
    if (x < 0 || y < 0 || x >= Size.x || y >= Size.y) return Mapping[channel];
    float4 pixel = round(Input.Load(int3(x,y,0)) * 255.0);
    precise float alpha = pixel.a / 255.0;
    return pixel[channel] * alpha + Mapping[channel] * (1.0 - alpha);
}
float4 Taskbar(float4 position : SV_Position) : SV_Target {
    int2 at = int2(position.xy);
    int x = Clip.x + at.x;
    int3 rows = TaskbarRows.Load(at.y).xyz;
    float3 sample = float3(TaskbarSample(x,rows.x,0),TaskbarSample(x,rows.y,1),TaskbarSample(x,rows.z,2));
    uint hash = (uint(at.x) * 374761393u + uint(at.y) * 668265263u) ^ 0x9e3779b9u;
    precise float grain = (float((hash ^ (hash >> 13)) & 255u) / 255.0 - 0.5) * TintNoise.y;
    precise float3 color = sample * (1.0 - TintNoise.x) + TintColor.rgb * TintNoise.x + grain;
    return float4(floor(clamp(color,0,255)) / 255.0,1);
}
float4 TaskbarBlur(float4 position : SV_Position) : SV_Target {
    int2 at = int2(position.xy);
    uint3 sum = 0;
    int radius = Radius.x;
    [loop] for(int i=-radius;i<=radius;++i) {
        int2 xy = clamp(at + (Radius.z == 0 ? int2(i,0) : int2(0,i)),int2(0,0),Size.xy-1);
        sum += uint3(round(Input.Load(int3(xy,0)).rgb * 255.0));
    }
    // Legacy RenderGlass floors each box pass. Panel blur retains its rounding.
    return float4(float3(sum / uint(2 * radius + 1)) / 255.0,1);
}
)hlsl";
    public:
        struct SharedDevice
        {
            ComPtr<ID3D11Device> Device;
            ComPtr<ID3D11DeviceContext> Context;
            ComPtr<ID3D11VertexShader> Vertex;
            ComPtr<ID3D11PixelShader> Blur, Base, Taskbar, TaskbarBlur;
            ComPtr<ID3D11Buffer> Constants;
            ComPtr<ID3D11RasterizerState> Rasterizer;
        };
        static std::shared_ptr<SharedDevice> CreateSharedDevice() noexcept
        {
            try
            {
                auto shared = std::make_shared<SharedDevice>();
                D3D_FEATURE_LEVEL level{};
                Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                    shared->Device.GetAddressOf(), &level, shared->Context.GetAddressOf()));
                if (level < D3D_FEATURE_LEVEL_11_0) return {};
                const auto compile = [](const char* entry, const char* profile)
                {
                    ComPtr<ID3DBlob> code, errors;
                    Check(D3DCompile(Shader, sizeof(Shader) - 1, "NovaBar.GpuPanelPreprocessor", nullptr, nullptr,
                        entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS, 0,
                        code.GetAddressOf(), errors.GetAddressOf()));
                    return code;
                };
                auto code = compile("Vertex", "vs_5_0");
                Check(shared->Device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(),
                    nullptr, shared->Vertex.GetAddressOf()));
                code = compile("Blur", "ps_5_0");
                Check(shared->Device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
                    nullptr, shared->Blur.GetAddressOf()));
                code = compile("Base", "ps_5_0");
                Check(shared->Device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
                    nullptr, shared->Base.GetAddressOf()));
                code = compile("Taskbar", "ps_5_0");
                Check(shared->Device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
                    nullptr, shared->Taskbar.GetAddressOf()));
                code = compile("TaskbarBlur", "ps_5_0");
                Check(shared->Device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
                    nullptr, shared->TaskbarBlur.GetAddressOf()));
                D3D11_BUFFER_DESC constants{};
                constants.ByteWidth = sizeof(Parameters); constants.Usage = D3D11_USAGE_DEFAULT;
                constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                Check(shared->Device->CreateBuffer(&constants, nullptr, shared->Constants.GetAddressOf()));
                D3D11_RASTERIZER_DESC rasterizer{};
                rasterizer.FillMode = D3D11_FILL_SOLID; rasterizer.CullMode = D3D11_CULL_NONE; rasterizer.DepthClipEnable = TRUE;
                Check(shared->Device->CreateRasterizerState(&rasterizer, shared->Rasterizer.GetAddressOf()));
                return shared;
            }
            catch (...) { return {}; }
        }
        explicit GpuPanelPreprocessor(std::shared_ptr<SharedDevice> const& shared) : m_shared(shared) {}
        GpuPanelPreprocessor(GpuPanelPreprocessor const&) = delete;
        GpuPanelPreprocessor& operator=(GpuPanelPreprocessor const&) = delete;

        bool Render(PixelFrame const& frame, PixelRect target, int width, int height,
            PanelGlassOptions const& options, std::vector<uint8_t>& out) noexcept
        { return RenderImpl(nullptr, frame, target, width, height, options, out); }

        // The source is already on the broker device. Geometry carries only
        // physical bounds and dimensions; no source readback/upload is needed.
        bool RenderTexture(ID3D11ShaderResourceView* source, PixelFrame const& geometry,
            PixelRect target, int width, int height, PanelGlassOptions const& options,
            std::vector<uint8_t>& out) noexcept
        {
            if (!source) { m_error = E_INVALIDARG; out.clear(); return false; }
            return RenderImpl(source, geometry, target, width, height, options, out);
        }
        // GPU transport owns the target and its reuse fences. This path stops
        // before staging allocation, CPU readback or pixel-vector publication.
        bool RenderTextureToTarget(ID3D11ShaderResourceView* source, PixelFrame const& geometry,
            PixelRect target, int width, int height, PanelGlassOptions const& options,
            ID3D11RenderTargetView* destination) noexcept
        {
            if (!source || !destination) { m_error = E_INVALIDARG; return false; }
            std::vector<uint8_t> unused;
            return RenderImpl(source, geometry, target, width, height, options, unused, destination);
        }
        bool RenderTaskbarTexture(ID3D11ShaderResourceView* source, PixelFrame const& frame,
            PixelRect target, int width, int height, GlassOptions const& options, std::vector<uint8_t>& out) noexcept
        {
            try
            {
                if (!source || width <= 0 || height <= 0 || width > 16384 || height > 16384
                    || uint64_t(width) * height > 32 * 1024 * 1024 || frame.Width <= 0 || frame.Height <= 0
                    || frame.Width > 16384 || frame.Height > 16384 || uint64_t(frame.Width) * frame.Height > 32 * 1024 * 1024
                    || int64_t(target.Right) - target.Left != width || int64_t(target.Bottom) - target.Top != height)
                    throw HRESULT(E_INVALIDARG);
                if (frame.Tile || int64_t(frame.Bounds.Right) - frame.Bounds.Left != frame.Width
                    || int64_t(frame.Bounds.Bottom) - frame.Bounds.Top != frame.Height) throw HRESULT(E_NOTIMPL);
                if (!m_shared || !m_shared->Device || !m_shared->Context || !m_shared->Taskbar || !m_shared->TaskbarBlur)
                    throw HRESULT(E_NOINTERFACE);
                ValidateTexture(source, frame); EnsureTaskbar(width,height); EnsureOutput(width,height);
                const auto unit=[](double value) { return std::isfinite(value)?std::clamp(value,0.0,1.0):0.0; };
                const double lens=unit(options.Lens),dispersion=unit(options.Dispersion);
                Parameters parameters;
                parameters.Size[0]=frame.Width;parameters.Size[1]=frame.Height;
                parameters.Size[2]=width;parameters.Size[3]=height;
                const int64_t offset=int64_t(target.Left)-frame.Bounds.Left;
                if(offset<INT_MIN || offset>INT_MAX)throw HRESULT(E_INVALIDARG);
                parameters.Clip[0]=int(offset);
                parameters.TintNoise[0]=float(((options.Color>>24)&255)/255.0*unit(options.Tint));
                parameters.TintNoise[1]=float(unit(options.Noise)*8.0);
                parameters.TintColor[0]=float((options.Color>>16)&255);parameters.TintColor[1]=float((options.Color>>8)&255);parameters.TintColor[2]=float(options.Color&255);
                parameters.Mapping[0]=float((frame.Background>>16)&255);parameters.Mapping[1]=float((frame.Background>>8)&255);parameters.Mapping[2]=float(frame.Background&255);
                const int radius=std::isfinite(options.Blur)?std::clamp(static_cast<int>(options.Blur*.25),0,64):0;
                parameters.Radius[0]=radius;
                m_taskbarRows.resize(height);
                for(int y=0;y<height;++y)
                {
                    const double edge=std::exp(-4.5*y/height),shift=-46.0*lens*edge;
                    const double sy=target.Top+static_cast<double>(y)*(target.Bottom-target.Top)/height;
                    for(int channel=0;channel<3;++channel)
                    {
                        const double chromatic=channel==2?shift*dispersion*.25:channel==0?-shift*dispersion*.25:0;
                        const double py=(sy+shift+chromatic-frame.Bounds.Top)*frame.Height/(frame.Bounds.Bottom-frame.Bounds.Top);
                        m_taskbarRows[y][2-channel]=py<0 || py>=frame.Height?-1:std::clamp(static_cast<int>(py),0,frame.Height-1);
                    }
                    m_taskbarRows[y][3]=0;
                }
                auto* context=m_shared->Context.Get();D3D11_MAPPED_SUBRESOURCE mapped{};
                Check(context->Map(m_taskbarRowBuffer.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped));
                memcpy(mapped.pData,m_taskbarRows.data(),size_t(height)*sizeof(m_taskbarRows[0]));context->Unmap(m_taskbarRowBuffer.Get(),0);
                context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                context->VSSetShader(m_shared->Vertex.Get(),nullptr,0);auto* constants=m_shared->Constants.Get();context->PSSetConstantBuffers(0,1,&constants);
                context->RSSetState(m_shared->Rasterizer.Get());context->OMSetBlendState(nullptr,nullptr,0xffffffff);context->OMSetDepthStencilState(nullptr,0);
                auto* rows=m_taskbarRowView.Get();context->PSSetShaderResources(1,1,&rows);
                Draw(source,radius?m_taskbarBaseTarget.Get():m_outputTarget.Get(),m_shared->Taskbar.Get(),width,height,parameters);
                ID3D11ShaderResourceView* empty=nullptr;context->PSSetShaderResources(1,1,&empty);
                if(radius)
                {
                    parameters.Size[0]=width;parameters.Size[1]=height;
                    Draw(m_taskbarBaseView.Get(),m_taskbarHorizontalTarget.Get(),m_shared->TaskbarBlur.Get(),width,height,parameters);
                    parameters.Radius[2]=1;
                    Draw(m_taskbarHorizontalView.Get(),m_outputTarget.Get(),m_shared->TaskbarBlur.Get(),width,height,parameters);
                }
                out.resize(size_t(width)*height*4);
                context->CopyResource(m_staging.Get(),m_output.Get());Check(context->Map(m_staging.Get(),0,D3D11_MAP_READ,0,&mapped));
                for(int y=0;y<height;++y)memcpy(out.data()+size_t(y)*width*4,static_cast<uint8_t const*>(mapped.pData)+size_t(y)*mapped.RowPitch,size_t(width)*4);
                context->Unmap(m_staging.Get(),0);m_error=S_OK;return true;
            }
            catch(HRESULT error){m_error=error;}
            catch(std::bad_alloc const&){m_error=E_OUTOFMEMORY;}
            catch(...){m_error=E_FAIL;}
            out.clear();return false;
        }
        HRESULT LastError() const { return m_error; }
    private:
        bool RenderImpl(ID3D11ShaderResourceView* textureSource, PixelFrame const& frame,
            PixelRect target, int width, int height, PanelGlassOptions const& options,
            std::vector<uint8_t>& out, ID3D11RenderTargetView* destination = nullptr) noexcept
        {
            try
            {
                Validate(frame, target, width, height, options, textureSource == nullptr);
                if (!m_shared || !m_shared->Device || !m_shared->Context) throw HRESULT(E_NOINTERFACE);
                Check(m_shared->Device->GetDeviceRemovedReason());
                if (textureSource) ValidateTexture(textureSource, frame);
                if (destination) ValidateRenderTarget(destination, width, height, textureSource);
                const auto canvas = *options.Canvas;
                const auto viewport = options.Viewport.value_or(canvas);
                const double sourceScaleX = static_cast<double>(frame.Width)
                    / (static_cast<int64_t>(frame.Bounds.Right) - frame.Bounds.Left);
                const double sourceScaleY = static_cast<double>(frame.Height)
                    / (static_cast<int64_t>(frame.Bounds.Bottom) - frame.Bounds.Top);
                const double pixelWidth = static_cast<double>(static_cast<int64_t>(canvas.Right) - canvas.Left) / width;
                const double pixelHeight = static_cast<double>(static_cast<int64_t>(canvas.Bottom) - canvas.Top) / height;
                Parameters parameters;
                parameters.Size[0] = frame.Width; parameters.Size[1] = frame.Height;
                parameters.Size[2] = width; parameters.Size[3] = height;
                // Match CPU double screen-coordinate clipping, even at large origins.
                parameters.Clip[0] = FirstPixel(canvas.Left, pixelWidth, width, viewport.Left);
                parameters.Clip[1] = FirstPixel(canvas.Top, pixelHeight, height, viewport.Top);
                parameters.Clip[2] = FirstPixel(canvas.Left, pixelWidth, width, viewport.Right);
                parameters.Clip[3] = FirstPixel(canvas.Top, pixelHeight, height, viewport.Bottom);
                if (parameters.Clip[0] >= parameters.Clip[2] || parameters.Clip[1] >= parameters.Clip[3])
                {
                    if (destination)
                    {
                        const float transparent[4]{};
                        m_shared->Context->SetPredication(nullptr, FALSE);
                        m_shared->Context->ClearRenderTargetView(destination, transparent);
                        Check(m_shared->Device->GetDeviceRemovedReason());
                    }
                    else out.assign(static_cast<size_t>(width) * height * 4, 0);
                    m_error = S_OK; return true;
                }
                const int physicalBlur = panel_detail::BlurRadius(options.Material.Blur);
                parameters.Radius[0] = std::min(frame.Width, static_cast<int>(std::ceil(physicalBlur * sourceScaleX)));
                parameters.Radius[1] = std::min(frame.Height, static_cast<int>(std::ceil(physicalBlur * sourceScaleY)));
                // Normal composed scenes have unit source scale. Retain the
                // CPU path for unusually dense sources rather than issuing a
                // shader loop wider than the supported physical blur kernel.
                if (parameters.Radius[0] > 64 || parameters.Radius[1] > 64) throw HRESULT(E_NOTIMPL);
                parameters.Mapping[0] = static_cast<float>(pixelWidth * sourceScaleX);
                parameters.Mapping[1] = static_cast<float>(pixelHeight * sourceScaleY);
                parameters.Mapping[2] = static_cast<float>((static_cast<double>(canvas.Left) - frame.Bounds.Left) * sourceScaleX);
                parameters.Mapping[3] = static_cast<float>((static_cast<double>(canvas.Top) - frame.Bounds.Top) * sourceScaleY);
                parameters.TintNoise[0] = static_cast<float>(((options.Material.Color >> 24) & 255)
                    / 255.0 * panel_detail::Unit(options.Material.Tint));
                parameters.TintNoise[1] = static_cast<float>(panel_detail::Unit(options.Material.Noise) * 8);
                // BGRA textures expose logical RGBA values to HLSL.
                parameters.TintColor[0] = static_cast<float>((options.Material.Color >> 16) & 255);
                parameters.TintColor[1] = static_cast<float>((options.Material.Color >> 8) & 255);
                parameters.TintColor[2] = static_cast<float>(options.Material.Color & 255);
                EnsureSource(frame.Width, frame.Height, textureSource == nullptr);
                if (!destination) { EnsureOutput(width, height); out.resize(static_cast<size_t>(width) * height * 4); }
                auto* context = m_shared->Context.Get();
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (!textureSource)
                {
                    Check(context->Map(m_upload.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
                    for (int y = 0; y < frame.Height; ++y)
                        std::memcpy(static_cast<uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                            frame.Pixels.data() + static_cast<size_t>(y) * frame.Width * 4, static_cast<size_t>(frame.Width) * 4);
                    context->Unmap(m_upload.Get(), 0);
                }
                context->IASetInputLayout(nullptr); context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                context->SetPredication(nullptr, FALSE);
                context->HSSetShader(nullptr, nullptr, 0); context->DSSetShader(nullptr, nullptr, 0);
                context->GSSetShader(nullptr, nullptr, 0);
                context->VSSetShader(m_shared->Vertex.Get(), nullptr, 0);
                auto* constants = m_shared->Constants.Get(); context->PSSetConstantBuffers(0, 1, &constants);
                context->RSSetState(m_shared->Rasterizer.Get());
                context->OMSetBlendState(nullptr, nullptr, 0xffffffff); context->OMSetDepthStencilState(nullptr, 0);
                auto* source = textureSource ? textureSource : m_uploadView.Get();
                if (physicalBlur)
                {
                    Draw(source, m_horizontalTarget.Get(), m_shared->Blur.Get(), frame.Width, frame.Height, parameters);
                    source = m_horizontalView.Get(); parameters.Radius[2] = 1;
                    Draw(source, m_verticalTarget.Get(), m_shared->Blur.Get(), frame.Width, frame.Height, parameters);
                    source = m_verticalView.Get();
                }
                Draw(source, destination ? destination : m_outputTarget.Get(), m_shared->Base.Get(), width, height, parameters);
                if (destination)
                { Check(m_shared->Device->GetDeviceRemovedReason()); m_error = S_OK; return true; }
                context->CopyResource(m_staging.Get(), m_output.Get());
                Check(context->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
                for (int y = 0; y < height; ++y)
                    std::memcpy(out.data() + static_cast<size_t>(y) * width * 4,
                        static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                        static_cast<size_t>(width) * 4);
                context->Unmap(m_staging.Get(), 0); m_error = S_OK; return true;
            }
            catch (HRESULT error) { m_error = error; }
            catch (std::bad_alloc const&) { m_error = E_OUTOFMEMORY; }
            catch (...) { m_error = E_FAIL; }
            out.clear(); return false;
        }
        std::shared_ptr<SharedDevice> m_shared;
        HRESULT m_error = S_OK;
        int m_sourceWidth = 0, m_sourceHeight = 0, m_outputWidth = 0, m_outputHeight = 0;
        ComPtr<ID3D11Texture2D> m_upload, m_horizontal, m_vertical, m_output, m_staging;
        ComPtr<ID3D11ShaderResourceView> m_uploadView, m_horizontalView, m_verticalView;
        ComPtr<ID3D11RenderTargetView> m_horizontalTarget, m_verticalTarget, m_outputTarget;
        int m_taskbarWidth=0,m_taskbarHeight=0;
        ComPtr<ID3D11Texture2D> m_taskbarBase,m_taskbarHorizontal;
        ComPtr<ID3D11ShaderResourceView> m_taskbarBaseView,m_taskbarHorizontalView,m_taskbarRowView;
        ComPtr<ID3D11RenderTargetView> m_taskbarBaseTarget,m_taskbarHorizontalTarget;
        ComPtr<ID3D11Buffer> m_taskbarRowBuffer;
        std::vector<std::array<int,4>> m_taskbarRows;

        void EnsureTaskbar(int width,int height)
        {
            if(m_taskbarWidth==width && m_taskbarHeight==height)return;
            ComPtr<ID3D11Texture2D> base,horizontal;ComPtr<ID3D11ShaderResourceView> baseView,horizontalView,rowView;
            ComPtr<ID3D11RenderTargetView> baseTarget,horizontalTarget;ComPtr<ID3D11Buffer> rows;
            Texture(width,height,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET,D3D11_USAGE_DEFAULT,0,base,&baseView,&baseTarget);
            Texture(width,height,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET,D3D11_USAGE_DEFAULT,0,horizontal,&horizontalView,&horizontalTarget);
            D3D11_BUFFER_DESC desc{};desc.ByteWidth=UINT(size_t(height)*sizeof(std::array<int,4>));desc.Usage=D3D11_USAGE_DYNAMIC;
            desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
            Check(m_shared->Device->CreateBuffer(&desc,nullptr,rows.GetAddressOf()));
            D3D11_SHADER_RESOURCE_VIEW_DESC view{};view.Format=DXGI_FORMAT_R32G32B32A32_SINT;view.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;view.Buffer.NumElements=height;
            Check(m_shared->Device->CreateShaderResourceView(rows.Get(),&view,rowView.GetAddressOf()));
            m_taskbarBase=std::move(base);m_taskbarHorizontal=std::move(horizontal);m_taskbarBaseView=std::move(baseView);m_taskbarHorizontalView=std::move(horizontalView);
            m_taskbarBaseTarget=std::move(baseTarget);m_taskbarHorizontalTarget=std::move(horizontalTarget);m_taskbarRowBuffer=std::move(rows);m_taskbarRowView=std::move(rowView);
            m_taskbarWidth=width;m_taskbarHeight=height;
        }

        static int FirstPixel(int origin, double step, int count, int edge)
        {
            int first = 0, last = count;
            while (first < last)
            {
                const int middle = first + (last - first) / 2;
                if (origin + (middle + .5) * step < edge) first = middle + 1;
                else last = middle;
            }
            return first;
        }
        static void Validate(PixelFrame const& frame, PixelRect target, int width, int height, PanelGlassOptions const& options, bool requirePixels)
        {
            constexpr uint64_t MaxPixels = 32ull * 1024 * 1024;
            const auto validRect = [](PixelRect rect) { return rect.Right > rect.Left && rect.Bottom > rect.Top; };
            if (!options.Canvas || options.ClipRounded || options.Radius != 0 || options.Specular != 0
                || options.Material.Lens != 0 || options.Material.Dispersion != 0
                || !validRect(target) || !validRect(*options.Canvas) || !validRect(frame.Bounds)
                || (options.Viewport && !validRect(*options.Viewport))
                || width <= 0 || height <= 0 || width > 16384 || height > 16384
                || static_cast<uint64_t>(width) * height > MaxPixels
                || frame.Width <= 0 || frame.Height <= 0 || frame.Width > 16384 || frame.Height > 16384
                || static_cast<uint64_t>(frame.Width) * frame.Height > MaxPixels
                || (requirePixels && frame.Pixels.size() != static_cast<size_t>(frame.Width) * frame.Height * 4))
                throw HRESULT(E_INVALIDARG);
            auto visible = *options.Canvas;
            if (options.Viewport)
            {
                visible.Left = std::max(visible.Left, options.Viewport->Left);
                visible.Top = std::max(visible.Top, options.Viewport->Top);
                visible.Right = std::min(visible.Right, options.Viewport->Right);
                visible.Bottom = std::min(visible.Bottom, options.Viewport->Bottom);
            }
            if (validRect(visible) && (visible.Left < frame.Bounds.Left || visible.Top < frame.Bounds.Top
                || visible.Right > frame.Bounds.Right || visible.Bottom > frame.Bounds.Bottom))
                throw HRESULT(E_INVALIDARG);
        }
        void ValidateTexture(ID3D11ShaderResourceView* source, PixelFrame const& frame) const
        {
            ComPtr<ID3D11Device> device; source->GetDevice(device.GetAddressOf());
            if (device.Get() != m_shared->Device.Get()) throw HRESULT(E_INVALIDARG);
            D3D11_SHADER_RESOURCE_VIEW_DESC view{}; source->GetDesc(&view);
            if (view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || view.Texture2D.MostDetailedMip != 0
                || (view.Format != DXGI_FORMAT_B8G8R8A8_UNORM && view.Format != DXGI_FORMAT_R8G8B8A8_UNORM))
                throw HRESULT(E_INVALIDARG);
            ComPtr<ID3D11Resource> resource; source->GetResource(resource.GetAddressOf());
            ComPtr<ID3D11Texture2D> texture; Check(resource.As(&texture));
            D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
            if (desc.Width != static_cast<UINT>(frame.Width) || desc.Height != static_cast<UINT>(frame.Height)
                || desc.ArraySize != 1 || desc.SampleDesc.Count != 1)
                throw HRESULT(E_INVALIDARG);
        }
        void ValidateRenderTarget(ID3D11RenderTargetView* target, int width, int height,
            ID3D11ShaderResourceView* source) const
        {
            ComPtr<ID3D11Device> device; target->GetDevice(device.GetAddressOf());
            if (device.Get() != m_shared->Device.Get()) throw HRESULT(E_INVALIDARG);
            D3D11_RENDER_TARGET_VIEW_DESC view{}; target->GetDesc(&view);
            if (view.Format != DXGI_FORMAT_B8G8R8A8_UNORM || view.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D
                || view.Texture2D.MipSlice != 0) throw HRESULT(E_INVALIDARG);
            ComPtr<ID3D11Resource> resource; target->GetResource(resource.GetAddressOf());
            ComPtr<ID3D11Texture2D> texture; Check(resource.As(&texture));
            D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
            if (desc.Width != static_cast<UINT>(width) || desc.Height != static_cast<UINT>(height)
                || desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || desc.MipLevels != 1 || desc.ArraySize != 1
                || desc.SampleDesc.Count != 1 || desc.Usage != D3D11_USAGE_DEFAULT || desc.CPUAccessFlags
                || !(desc.BindFlags & D3D11_BIND_RENDER_TARGET)) throw HRESULT(E_INVALIDARG);
            if (source)
            {
                ComPtr<ID3D11Resource> input; source->GetResource(input.GetAddressOf());
                // Broker scene outputs are private. A shared allocation can
                // reopen with a distinct COM identity, defeating pointer-only
                // feedback checks; do not admit it on this direct-output path.
                ComPtr<ID3D11Texture2D> inputTexture; Check(input.As(&inputTexture));
                D3D11_TEXTURE2D_DESC inputDesc{}; inputTexture->GetDesc(&inputDesc);
                if (inputDesc.MiscFlags) throw HRESULT(E_INVALIDARG);
                if (input.Get() == resource.Get()) throw HRESULT(E_INVALIDARG);
            }
        }
        void Texture(int width, int height, UINT bindings, D3D11_USAGE usage, UINT cpuAccess,
            ComPtr<ID3D11Texture2D>& texture, ComPtr<ID3D11ShaderResourceView>* view = nullptr,
            ComPtr<ID3D11RenderTargetView>* target = nullptr)
        {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = static_cast<UINT>(width); desc.Height = static_cast<UINT>(height);
            desc.MipLevels = desc.ArraySize = 1; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1; desc.Usage = usage; desc.BindFlags = bindings; desc.CPUAccessFlags = cpuAccess;
            Check(m_shared->Device->CreateTexture2D(&desc, nullptr, texture.GetAddressOf()));
            if (view) Check(m_shared->Device->CreateShaderResourceView(texture.Get(), nullptr, view->GetAddressOf()));
            if (target) Check(m_shared->Device->CreateRenderTargetView(texture.Get(), nullptr, target->GetAddressOf()));
        }
        void EnsureSource(int width, int height, bool requireUpload)
        {
            if (m_sourceWidth == width && m_sourceHeight == height)
            {
                if (requireUpload && !m_upload)
                    Texture(width, height, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DYNAMIC,
                        D3D11_CPU_ACCESS_WRITE, m_upload, &m_uploadView);
                return;
            }
            ComPtr<ID3D11Texture2D> upload, horizontal, vertical;
            ComPtr<ID3D11ShaderResourceView> uploadView, horizontalView, verticalView;
            ComPtr<ID3D11RenderTargetView> horizontalTarget, verticalTarget;
            if (requireUpload) Texture(width, height, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DYNAMIC,
                D3D11_CPU_ACCESS_WRITE, upload, &uploadView);
            Texture(width, height, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                D3D11_USAGE_DEFAULT, 0, horizontal, &horizontalView, &horizontalTarget);
            Texture(width, height, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                D3D11_USAGE_DEFAULT, 0, vertical, &verticalView, &verticalTarget);
            m_upload = std::move(upload); m_horizontal = std::move(horizontal); m_vertical = std::move(vertical);
            m_uploadView = std::move(uploadView); m_horizontalView = std::move(horizontalView); m_verticalView = std::move(verticalView);
            m_horizontalTarget = std::move(horizontalTarget); m_verticalTarget = std::move(verticalTarget);
            m_sourceWidth = width; m_sourceHeight = height;
        }
        void EnsureOutput(int width, int height)
        {
            if (m_outputWidth == width && m_outputHeight == height) return;
            ComPtr<ID3D11Texture2D> output, staging; ComPtr<ID3D11RenderTargetView> target;
            Texture(width, height, D3D11_BIND_RENDER_TARGET, D3D11_USAGE_DEFAULT, 0, output, nullptr, &target);
            Texture(width, height, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, staging);
            m_output = std::move(output); m_staging = std::move(staging); m_outputTarget = std::move(target);
            m_outputWidth = width; m_outputHeight = height;
        }
        void Draw(ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target, ID3D11PixelShader* shader,
            int width, int height, Parameters const& parameters)
        {
            auto* context = m_shared->Context.Get();
            context->UpdateSubresource(m_shared->Constants.Get(), 0, nullptr, &parameters, 0, 0);
            const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
            context->RSSetViewports(1, &viewport); context->OMSetRenderTargets(1, &target, nullptr);
            context->PSSetShaderResources(0, 1, &source); context->PSSetShader(shader, nullptr, 0); context->Draw(3, 0);
            ID3D11ShaderResourceView* empty = nullptr;
            context->PSSetShaderResources(0, 1, &empty); context->OMSetRenderTargets(0, nullptr, nullptr);
        }
    };
}
