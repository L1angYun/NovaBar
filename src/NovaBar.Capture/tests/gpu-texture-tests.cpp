#include "../GpuPanelPreprocessor.h"
#include <cstdio>

using namespace novabar;
template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

template<class Processor>
bool RenderTexture(Processor& processor, ID3D11ShaderResourceView* source, PixelFrame const& frame,
    PanelGlassOptions const& options, std::vector<uint8_t>& out)
{
    if constexpr (requires { processor.RenderTexture(source, frame, frame.Bounds, 48, 32, options, out); })
        return processor.RenderTexture(source, frame, frame.Bounds, 48, 32, options, out);
    else { out.clear(); return false; }
}

int main()
{
    int checks = 0, failures = 0;
    const auto check = [&](bool ok, char const* label) { ++checks;
        if (!ok) { ++failures; std::printf("FAIL: %s\n", label); } };
    auto shared = GpuPanelPreprocessor::CreateSharedDevice();
    check(shared != nullptr, "hardware GPU base initializes");
    if (!shared) return 1;
    GpuPanelPreprocessor processor(shared);
    PixelFrame frame; frame.Width = 64; frame.Height = 48; frame.Bounds = {-37,-19,27,29};
    frame.Pixels.resize(size_t(frame.Width) * frame.Height * 4);
    for (int y = 0; y < frame.Height; ++y) for (int x = 0; x < frame.Width; ++x)
    {
        const auto at = (size_t(y) * frame.Width + x) * 4;
        const unsigned alpha = unsigned(x * 17 + y * 23) % 256;
        frame.Pixels[at] = uint8_t(unsigned(x * 31 + y * 7) % (alpha + 1));
        frame.Pixels[at + 1] = uint8_t(unsigned(x * 13 + y * 29) % (alpha + 1));
        frame.Pixels[at + 2] = uint8_t(unsigned(x * 7 + y * 19) % (alpha + 1));
        frame.Pixels[at + 3] = uint8_t(alpha);
    }
    const auto upload = [&](ID3D11Device* device, DXGI_FORMAT format, bool swap) {
        auto bytes = frame.Pixels;
        if (swap) for (size_t i = 0; i < bytes.size(); i += 4) std::swap(bytes[i], bytes[i + 2]);
        D3D11_TEXTURE2D_DESC desc{}; desc.Width = frame.Width; desc.Height = frame.Height;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1; desc.Format = format;
        desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{bytes.data(), UINT(frame.Width * 4), 0};
        ComPtr<ID3D11Texture2D> texture; ComPtr<ID3D11ShaderResourceView> view;
        if (SUCCEEDED(device->CreateTexture2D(&desc, &data, texture.GetAddressOf())))
            device->CreateShaderResourceView(texture.Get(), nullptr, view.GetAddressOf());
        return view;
    };
    auto source = upload(shared->Device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, false);
    auto rgba = upload(shared->Device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, true);
    auto srgb = upload(shared->Device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, false);
    check(source != nullptr && rgba != nullptr && srgb != nullptr, "source SRV fixtures initialize");
    auto geometry = frame; geometry.Pixels.clear();
    PanelGlassOptions options; options.Canvas = frame.Bounds; options.Viewport = frame.Bounds;
    options.Radius = options.Specular = 0; options.ClipRounded = false;
    options.Material = {0,0,.12,.55,14,0x263a6381};
    std::vector<uint8_t> expected, actual;
    check(processor.Render(frame, frame.Bounds,48,32,options,expected), "CPU upload establishes existing GPU reference");
    check(RenderTexture(processor,source.Get(),geometry,options,actual), "GPU texture entry point accepts geometry without pixels");
    check(actual == expected, "GPU texture output exactly matches the existing GPU upload pipeline");
    check(RenderTexture(processor,rgba.Get(),geometry,options,actual) && actual == expected,
        "logical RGBA texture channels match BGRA texture channels");
    options.Material.Blur = 0;
    check(processor.Render(frame,frame.Bounds,48,32,options,expected)
        && RenderTexture(processor,source.Get(),geometry,options,actual) && actual == expected,
        "direct GPU source works with zero blur");
    const auto rejects = [&](ID3D11ShaderResourceView* view, PixelFrame const& shape, char const* label) {
        actual = {1,2,3};
        check(!RenderTexture(processor,view,shape,options,actual) && actual.empty(),label);
    };
    rejects(nullptr,geometry,"null SRV fails and clears stale output");
    rejects(srgb.Get(),geometry,"sRGB source is rejected rather than applying an implicit color transform");
    auto invalid = geometry; invalid.Width += 1;
    rejects(source.Get(),invalid,"texture dimensions must match declared source geometry");
    invalid = geometry; invalid.Bounds.Right = invalid.Bounds.Left;
    rejects(source.Get(),invalid,"empty physical source bounds are rejected");
    auto other = GpuPanelPreprocessor::CreateSharedDevice();
    check(other != nullptr,"second device fixture initializes");
    if (other) { auto foreign = upload(other->Device.Get(),DXGI_FORMAT_B8G8R8A8_UNORM,false);
        rejects(foreign.Get(),geometry,"foreign-device SRV is rejected before rendering"); }
    check(RenderTexture(processor,source.Get(),geometry,options,actual) && processor.LastError() == S_OK,
        "valid GPU texture clears errors and recovers after validation failures");
    std::printf("GPU texture tests: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
