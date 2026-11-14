#include "../GpuPanelPreprocessor.h"
#include <chrono>
#include <cstdio>
#include <limits>

using namespace novabar;
namespace
{
    PixelFrame Fixture(int width, int height, PixelRect bounds, bool transparent)
    {
        PixelFrame frame; frame.Width = width; frame.Height = height; frame.Bounds = bounds;
        frame.Pixels.resize(static_cast<size_t>(width) * height * 4);
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
        {
            const size_t at = (static_cast<size_t>(y) * width + x) * 4;
            const unsigned alpha = transparent ? (x * 17u + y * 23u) % 256 : 255;
            frame.Pixels[at] = static_cast<uint8_t>((x * 31u + y * 7u) % (alpha + 1));
            frame.Pixels[at + 1] = static_cast<uint8_t>((x * 13u + y * 29u) % (alpha + 1));
            frame.Pixels[at + 2] = static_cast<uint8_t>((x * 7u + y * 19u) % (alpha + 1));
            frame.Pixels[at + 3] = static_cast<uint8_t>(alpha);
        }
        return frame;
    }
    struct Case
    {
        const char* Name; PixelRect Source, Canvas, Viewport; int Width, Height, TextureWidth = 0, TextureHeight = 0;
        double Blur = 14, Tint = .55, Noise = .12; uint32_t Color = 0x26000000;
    };
    const Case Cases[]{
        {"actual Start",{950,0,2468,2160},{955,0,2463,2160},{0,0,3840,2160},1005,1440},
        {"actual side",{2207,0,2891,2160},{2212,0,2886,2160},{0,0,3840,2160},449,1440},
        {"actual notification",{3231,0,3840,2160},{3236,0,3841,2160},{0,0,3840,2160},403,1440},
        {"4K downsample",{0,0,3840,2160},{0,0,3840,2160},{0,0,3840,2160},2560,1440},
        {"4K aligned",{0,0,3840,2160},{0,0,3840,2160},{0,0,3840,2160},3840,2160},
        {"negative screen",{-3840,-2160,0,0},{-3840,-2160,0,0},{-3840,-2160,0,0},2560,1440},
        {"scaled capture",{-3840,-2160,0,0},{-3840,-2160,0,0},{-3840,-2160,0,0},1005,1440,1920,1080},
        {"outside viewport",{-23,-11,1531,2149},{-40,-30,1544,2172},{-23,-11,1531,2149},1005,1440},
        {"colored/no blur",{100,-200,1060,1240},{100,-200,1060,1240},{100,-200,1060,1240},640,960,0,0,0,.73,1,0x8ce4b17c},
        {"colored/max blur",{100,-200,1124,568},{100,-200,1124,568},{100,-200,1124,568},683,512,0,0,256,.73,1,0x8ce4b17c},
    };
    PanelGlassOptions Options(Case const& item)
    {
        PanelGlassOptions options; options.Material = {0, 0, item.Noise, item.Tint, item.Blur, item.Color};
        options.Radius = options.Specular = 0; options.ClipRounded = false;
        options.Canvas = item.Canvas; options.Viewport = item.Viewport; return options;
    }
    bool Near(std::vector<uint8_t> const& actual, std::vector<uint8_t> const& reference)
    {
        if (actual.size() != reference.size()) return false;
        for (size_t i = 0; i < actual.size(); ++i)
            if (std::abs(static_cast<int>(actual[i]) - reference[i]) > 1) return false;
        return true;
    }
}
int main()
{
    int checks = 0, failed = 0;
    const auto check = [&](bool ok, const char* label)
    { ++checks; if (!ok) { ++failed; printf("FAIL: %s\n", label); } };
    // Independently detect hardware availability. Shader or processor startup
    // failures on available hardware are test failures, never a hardware skip.
    Microsoft::WRL::ComPtr<ID3D11Device> probe;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> probeContext;
    D3D_FEATURE_LEVEL level{};
    const HRESULT available = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        probe.GetAddressOf(), &level, probeContext.GetAddressOf());
    if (available == DXGI_ERROR_UNSUPPORTED || available == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE
        || (SUCCEEDED(available) && level < D3D_FEATURE_LEVEL_11_0))
    { printf("SKIP: hardware D3D11 feature level 11 unavailable (HRESULT=%08x level=%x)\n", unsigned(available), unsigned(level)); return 0; }
    check(SUCCEEDED(available), "hardware device preflight");
    if (FAILED(available)) return 1;
    probeContext.Reset(); probe.Reset();
    auto shared = GpuPanelPreprocessor::CreateSharedDevice();
    check(shared != nullptr, "available hardware compiles and initializes GPU base shaders");
    if (!shared) return 1;
    const PixelRect target{100,100,600,700};
    GpuPanelPreprocessor processor(shared);
    for (auto const& item : Cases) for (int transparent = 0; transparent < 2; ++transparent)
    {
        const auto frame = Fixture(item.TextureWidth ? item.TextureWidth : item.Source.Right - item.Source.Left,
            item.TextureHeight ? item.TextureHeight : item.Source.Bottom - item.Source.Top, item.Source, transparent != 0);
        const auto options = Options(item); std::vector<uint8_t> actual, reference;
        check(RenderPanelGlass(frame, target, item.Width, item.Height, options, reference), "CPU parity fixture is valid");
        const bool rendered = processor.Render(frame, target, item.Width, item.Height, options, actual);
        check(rendered, item.Name);
        if (rendered)
        {
            check(Near(actual, reference), "GPU base is within one byte of CPU reference");
            check(processor.LastError() == S_OK, "successful render clears LastError");
        }
    }
    auto tiny = Fixture(8,8,{0,0,8,8},true); auto options = Options(Cases[0]);
    options.Canvas = PixelRect{0,0,8,8}; options.Viewport.reset();
    std::vector<uint8_t> actual, reference;
    GpuPanelPreprocessor absent(nullptr);
    actual = {1,2,3};
    check(!absent.Render(tiny, target,8,8,options,actual) && FAILED(absent.LastError()) && actual.empty(),
        "absent GPU backend returns failure and clears stale output");
    const auto rejected = [&](PixelFrame const& frame, PixelRect bounds, int width, int height, PanelGlassOptions const& style)
    {
        actual = {1,2,3};
        check(!processor.Render(frame,bounds,width,height,style,actual) && FAILED(processor.LastError()) && actual.empty(),
            "invalid or unsupported input fails without stale output");
    };
    auto unsupported = options; unsupported.Canvas.reset(); rejected(tiny,target,8,8,unsupported);
    unsupported = options; unsupported.ClipRounded = true; rejected(tiny,target,8,8,unsupported);
    unsupported = options; unsupported.Radius = 1; rejected(tiny,target,8,8,unsupported);
    unsupported = options; unsupported.Specular = .1; rejected(tiny,target,8,8,unsupported);
    unsupported = options; unsupported.Material.Lens = .1; rejected(tiny,target,8,8,unsupported);
    unsupported = options; unsupported.Material.Dispersion = .1; rejected(tiny,target,8,8,unsupported);
    unsupported = options; unsupported.Viewport = PixelRect{1,1,0,0}; rejected(tiny,target,8,8,unsupported);
    unsupported = options; unsupported.Canvas = PixelRect{0,0,9,8}; rejected(tiny,target,8,8,unsupported);
    unsupported = options; unsupported.Canvas = PixelRect{8,8,0,0}; rejected(tiny,target,8,8,unsupported);
    rejected(tiny,{0,0,0,8},8,8,options); rejected(tiny,target,0,8,options); rejected(tiny,target,16385,8,options);
    auto malformed = tiny; malformed.Pixels.pop_back(); rejected(malformed,target,8,8,options);
    malformed = tiny; malformed.Width = -1; rejected(malformed,target,8,8,options);
    malformed = tiny; malformed.Bounds = {0,0,0,8}; rejected(malformed,target,8,8,options);
    // The broker's composed scene has one texel per physical pixel. A caller
    // with a denser source must use CPU fallback rather than an unbounded
    // shader loop for a very wide kernel.
    auto dense = Fixture(128,128,{0,0,1,1},false);
    unsupported = options; unsupported.Canvas = PixelRect{0,0,1,1}; unsupported.Material.Blur = 256;
    rejected(dense,target,8,8,unsupported);
    unsupported = options; unsupported.Viewport = PixelRect{9,9,10,10};
    check(processor.Render(tiny,target,8,8,unsupported,actual) && actual == std::vector<uint8_t>(256,0),
        "canvas entirely outside viewport produces transparent output");
    options.Material.Blur = std::numeric_limits<double>::quiet_NaN();
    options.Material.Noise = std::numeric_limits<double>::quiet_NaN();
    options.Material.Tint = std::numeric_limits<double>::quiet_NaN();
    check(RenderPanelGlass(tiny,target,8,8,options,reference)
        && processor.Render(tiny,target,8,8,options,actual) && Near(actual,reference), "material sanitization matches CPU");
    options = Options(Cases[0]); options.Canvas = PixelRect{0,0,8,8}; options.Viewport.reset();
    check(processor.Render(tiny,target,8,8,options,actual) && processor.LastError() == S_OK,
        "valid input recovers after rejected input and dimension switches");

    // Four independent target texture sets repeatedly use one shared device.
    // Timings include upload, shader work, blocking map, and output row copies.
    GpuPanelPreprocessor first(shared),second(shared),third(shared),fourth(shared);
    GpuPanelPreprocessor* targets[]{&first,&second,&third,&fourth};
    std::vector<PixelFrame> frames; std::vector<PanelGlassOptions> styles;
    for (int i=0;i<4;++i) { const auto& item=Cases[i<2?i:2];
        frames.push_back(Fixture(item.Source.Right-item.Source.Left,item.Source.Bottom-item.Source.Top,item.Source,false));
        styles.push_back(Options(item)); }
    std::vector<double> elapsed;
    for (int pass=0;pass<43;++pass)
    {
        const auto start=std::chrono::steady_clock::now(); bool ok=true;
        for (int i=0;i<4;++i) {const auto& item=Cases[i<2?i:2];
            ok=targets[i]->Render(frames[i],target,item.Width,item.Height,styles[i],actual)&&ok;}
        if (!ok) { check(false,"four shared-device targets render repeatedly"); break; }
        if(pass>=3)elapsed.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
    }
    check(elapsed.size()==40,"shared-device resource reuse completes forty rounds");
    if(elapsed.size()==40){std::sort(elapsed.begin(),elapsed.end());printf("GPU four-target reused resources: p50=%.3fms p95=%.3fms (40 rounds)\n",elapsed[20],elapsed[38]);}
    printf("GPU base tests: %d checks, %d failures\n",checks,failed);
    return failed ? 1 : 0;
}
