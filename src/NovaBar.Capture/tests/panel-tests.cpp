#include <cstdio>
#include <limits>
#include <chrono>
#include "../PanelGlassPixels.h"
#include "../CaptureProtocol.h"
#include "../ClientCanvasOptics.h"

using namespace novabar;

static PixelFrame Constant(int width, int height, uint8_t b, uint8_t g, uint8_t r, uint8_t alpha = 255)
{
    PixelFrame frame; frame.Width = width; frame.Height = height; frame.Bounds = {0, 0, width, height};
    frame.Pixels.resize(static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i < frame.Pixels.size(); i += 4)
    { frame.Pixels[i] = b; frame.Pixels[i + 1] = g; frame.Pixels[i + 2] = r; frame.Pixels[i + 3] = alpha; }
    return frame;
}

int main()
{
    int failed = 0, checks = 0;
    const auto check = [&](bool ok, const char* label)
    { ++checks; if (!ok) { ++failed; std::printf("FAIL: %s\n", label); } };
    PanelGlassOptions plain; plain.Material = {0, 0, 0, 0, 0, 0}; plain.Radius = plain.Specular = 0;
    std::vector<uint8_t> pixels, reference;
    capture::Request clipRequest;
    clipRequest.Pid = 1; clipRequest.TargetId = clipRequest.Revision = 1;
    clipRequest.CoordinateHwnd = clipRequest.TopLevelHwnd = 1; clipRequest.Bounds = {0,0,16,16};
    check(clipRequest.Flags == 0 && capture::detail::ValidRequest(clipRequest, false),
          "protocol-v1 zero flags retain the legacy request and clip contract");
    clipRequest.Flags = capture::HostClipsCorners;
    check(capture::detail::ValidRequest(clipRequest, false), "native host corner clip flag is accepted without changing protocol layout");
    clipRequest.Flags |= 0x80000000u;
    check(!capture::detail::ValidRequest(clipRequest, false), "unknown request flags cannot enter the broker");

    // Catches nearest-neighbor sampling and sample-corner offsets. At the
    // center of these four literal pixels, bilinear interpolation is 135.
    PixelFrame four; four.Width = four.Height = 2; four.Bounds = {0, 0, 2, 2};
    four.Pixels = {0,0,0,255, 100,100,100,255, 200,200,200,255, 240,240,240,255};
    check(RenderPanelGlass(four, {0,0,2,2}, 1, 1, plain, pixels), "bilinear fixture renders");
    check(pixels == std::vector<uint8_t>({135,135,135,255}), "bilinear sample preserves the center of four source pixels");

    auto translucent = Constant(8, 8, 25, 50, 75, 128);
    check(RenderPanelGlass(translucent, {0,0,8,8}, 8, 8, plain, pixels), "premultiplied scene renders");
    check(pixels == translucent.Pixels, "identity preserves premultiplied input rather than multiplying its alpha twice");

    auto source = Constant(256, 256, 0, 0, 0);
    for (int y = 0; y < 256; ++y) for (int x = 0; x < 256; ++x)
    { const auto at = (static_cast<size_t>(y) * 256 + x) * 4; source.Pixels[at] = source.Pixels[at+2] = static_cast<uint8_t>(x); source.Pixels[at+1] = static_cast<uint8_t>(y); }
    check(RenderPanelGlass(source, {40,40,140,140}, 100, 100, plain, reference), "physical gradient fixture renders");
    check(reference.size() == 40000 && reference[(50 * 100 + 2) * 4] == 42
          && reference[(2 * 100 + 50) * 4 + 1] == 42, "identity uses actual desktop coordinates");
    auto lens = plain; lens.Material.Lens = 1;
    check(RenderPanelGlass(source, {40,40,140,140}, 100, 100, lens, pixels), "lens fixture renders");
    if (pixels.size() == 40000 && reference.size() == 40000)
    {
        check(pixels[(50 * 100 + 2) * 4] > reference[(50 * 100 + 2) * 4], "left lens refracts along the horizontal normal");
        check(pixels[(2 * 100 + 50) * 4 + 1] > reference[(2 * 100 + 50) * 4 + 1], "top lens refracts along the vertical normal");
        check(pixels[(50 * 100 + 97) * 4] < reference[(50 * 100 + 97) * 4], "right lens reverses horizontal refraction");
        check(pixels[(97 * 100 + 50) * 4 + 1] < reference[(97 * 100 + 50) * 4 + 1], "bottom lens reverses vertical refraction");
        check(pixels[(50 * 100 + 50) * 4] == 90, "flat center retains its source pixel");
    }
    lens.Radius = 20;
    RenderPanelGlass(source, {40,40,140,140}, 100, 100, lens, pixels);
    check(pixels.size() == 40000 && pixels[(6 * 100 + 6) * 4] > 46
          && pixels[(6 * 100 + 6) * 4 + 1] > 46, "rounded corner refracts along both normal components");

    // A convex glass edge must not reverse a smooth backdrop ramp, even at
    // maximum refraction and red-channel dispersion. Excessive displacement
    // over a narrow bevel folds the sample map back on itself.
    auto maximumLens = lens; maximumLens.Material.Dispersion = 1;
    RenderPanelGlass(source, {40,40,140,140}, 100, 100, maximumLens, pixels);
    bool ordered = pixels.size() == 40000;
    for (int x=1;ordered && x<100;++x) for (int channel : {0,2})
        ordered = ordered && pixels[(50*100+x)*4+channel] >= pixels[(50*100+x-1)*4+channel];
    check(ordered, "maximum lens and dispersion preserve horizontal ramp ordering through both edges");
    ordered = pixels.size() == 40000;
    for (int y=1;ordered && y<100;++y)
        ordered = pixels[(y*100+50)*4+1] >= pixels[((y-1)*100+50)*4+1];
    check(ordered, "maximum lens and dispersion preserve vertical ramp ordering through both edges");

    // Match the live Start panel's 1251x1299 physical geometry and 18px
    // corner. Neighboring pixels across the inner corner diagonal must not
    // jump to unrelated backdrop coordinates when the nearest edge changes.
    auto cornerScene = Constant(1500, 1500, 0, 0, 0);
    for(int y=0;y<1500;++y) for(int x=0;x<1500;++x)
    {const auto at=(static_cast<size_t>(y)*1500+x)*4;cornerScene.Pixels[at]=static_cast<uint8_t>(x);cornerScene.Pixels[at+1]=static_cast<uint8_t>(y);}
    auto cornerLens = plain; cornerLens.Radius = 18; cornerLens.Material.Lens = .55;
    RenderPanelGlass(cornerScene, {100,100,1351,1399}, 1251, 1299, cornerLens, pixels);
    bool continuous = pixels.size() == static_cast<size_t>(1251)*1299*4;
    if (continuous) for(int channel : {0,1})
        continuous = continuous && std::abs(int(pixels[(24*1251+23)*4+channel])-int(pixels[(23*1251+24)*4+channel]))<=2;
    check(continuous, "rounded inner diagonal has continuous backdrop coordinates at physical resolution");
    RenderPanelGlass(cornerScene, {100,100,1351,1399}, 834, 866, cornerLens, pixels);
    continuous = pixels.size() == static_cast<size_t>(834)*866*4;
    if (continuous) for(int channel : {0,1})
        continuous = continuous && std::abs(int(pixels[(16*834+15)*4+channel])-int(pixels[(15*834+16)*4+channel]))<=3;
    check(continuous, "rounded inner diagonal remains continuous with 150 percent raster scaling");
    reference = pixels;
    auto nativeCorner = cornerLens; nativeCorner.ClipRounded = false;
    RenderPanelGlass(cornerScene, {100,100,1351,1399}, 834, 866, nativeCorner, pixels);
    bool nativeOpaque = pixels.size() == reference.size();
    if (nativeOpaque) for (const auto corner : {0,833,865*834,866*834-1})
        nativeOpaque = nativeOpaque && pixels[static_cast<size_t>(corner)*4+3] == 255;
    check(nativeOpaque, "native rounded host receives opaque backdrop pixels at every corner without a second alpha hole");
    bool sameOptics = pixels.size() == reference.size();
    if (sameOptics) for(size_t at=0;at<pixels.size();at+=4) if(reference[at+3]==255)
        for(int channel=0;channel!=4;++channel) sameOptics = sameOptics && pixels[at+channel]==reference[at+channel];
    check(sameOptics, "delegating corner clipping to the host preserves identical optical samples inside the rounded boundary");
    auto nativeTranslucent = plain; nativeTranslucent.Radius = 4; nativeTranslucent.ClipRounded = false;
    RenderPanelGlass(translucent, {0,0,8,8}, 8, 8, nativeTranslucent, pixels);
    check(pixels == translucent.Pixels, "native host clipping preserves source premultiplied alpha rather than inventing opacity");

    auto white = Constant(100, 100, 200, 200, 200);
    auto rounded = plain; rounded.Radius = 20;
    RenderPanelGlass(white, {0,0,100,100}, 100, 100, rounded, pixels);
    check(pixels.size() == 40000 && pixels[3] == 0 && pixels[0] == 0, "rounded corner has transparent premultiplied pixels");
    if (pixels.size() == 40000)
    {
        const auto edge = (8 * 100 + 3) * 4;
        check(pixels[edge + 3] >= 98 && pixels[edge + 3] <= 100
              && pixels[edge] >= 77 && pixels[edge] <= 79, "rounded edge applies subpixel coverage to color and alpha");
        check(pixels[(50 * 100 + 50) * 4] == 200 && pixels[(50 * 100 + 50) * 4 + 3] == 255, "rounded clip leaves center opaque");
    }

    auto visibleDesktop = plain; visibleDesktop.Viewport = PixelRect{0,0,100,100};
    check(RenderPanelGlass(white, {90,10,110,30}, 20, 20, visibleDesktop, pixels),
          "partially offscreen panel accepts a complete visible backdrop intersection");
    bool visiblePixels = pixels.size() == 20*20*4;
    if (visiblePixels) for(int y=0;y<20;++y) for(int x=0;x<20;++x)
    {
        const size_t at=(y*20+x)*4;
        visiblePixels = visiblePixels && pixels[at+3] == (x<10?255:0)
            && pixels[at] == (x<10?200:0);
    }
    check(visiblePixels, "offscreen output is transparent while visible pixels retain their real scene color");
    check(RenderPanelGlass(white, {90,10,111,31}, 14, 14, visibleDesktop, pixels),
          "partially offscreen panel renders with non-unit physical output scale");
    visiblePixels = pixels.size() == 14*14*4;
    if (visiblePixels) for(int y=0;y<14;++y) for(int x=0;x<14;++x)
        visiblePixels = visiblePixels && pixels[(y*14+x)*4+3] == (90+(x+.5)*1.5<100?255:0);
    check(visiblePixels, "150 percent output scale tests viewport in physical coordinates");
    auto negativeScene = white; negativeScene.Bounds = {-100,-50,0,50};
    visibleDesktop.Viewport = negativeScene.Bounds;
    check(RenderPanelGlass(negativeScene, {-110,-20,-80,20}, 30, 40, visibleDesktop, pixels),
          "negative monitor origin permits only the physically offscreen left portion");
    visiblePixels = pixels.size() == 30*40*4;
    if(visiblePixels) for(int y=0;y<40;++y) for(int x=0;x<30;++x)
        visiblePixels = visiblePixels && pixels[(y*30+x)*4+3] == (x<10?0:255);
    check(visiblePixels, "negative aligned source offsets never read outside the visible scene");
    visibleDesktop.Viewport = PixelRect{0,0,120,100};
    check(!RenderPanelGlass(white, {90,10,110,30}, 20, 20, visibleDesktop, pixels),
          "viewport allowance still rejects any missing visible desktop pixels");
    visibleDesktop.Viewport = PixelRect{0,0,100,100};
    check(RenderPanelGlass(white, {110,10,130,30}, 20, 20, visibleDesktop, pixels)
          && std::all_of(pixels.begin(),pixels.end(),[](uint8_t value){return value==0;}),
          "entirely offscreen output stays transparent without fabricated background");
    visibleDesktop.Viewport = PixelRect{100,0,0,100};
    check(!RenderPanelGlass(white, {10,10,30,30}, 20, 20, visibleDesktop, pixels),
          "inverted physical viewport is rejected");
    auto fullOptics = plain; fullOptics.Radius = 18; fullOptics.Material.Lens = .55;
    RenderPanelGlass(source, {10,10,110,90}, 100, 80, fullOptics, reference);
    auto clippedGradient = white;
    for(int y=0;y<100;++y) for(int x=0;x<100;++x)
    {const size_t at=(y*100+x)*4;clippedGradient.Pixels[at]=clippedGradient.Pixels[at+2]=static_cast<uint8_t>(x);clippedGradient.Pixels[at+1]=static_cast<uint8_t>(y);}
    fullOptics.Viewport = PixelRect{0,0,100,100};
    const bool partialOptics = RenderPanelGlass(clippedGradient, {10,10,110,90}, 100, 80, fullOptics, pixels);
    check(partialOptics && reference.size()==pixels.size()
          && pixels[(40*100+89)*4]==reference[(40*100+89)*4],
          "offscreen clipping retains the full panel optical geometry instead of moving its right lens edge");

    // Break caught: an offscreen animated material punches transparent holes
    // in the backing bitmap that later move onscreen with the native host.
    auto canvas = plain; canvas.Canvas = PixelRect{10,10,30,30};
    canvas.Viewport = PixelRect{0,0,256,256};
    check(RenderPanelGlass(source, {300,10,320,30}, 20, 20, canvas, pixels),
          "visible world canvas renders even while the material is entirely offscreen");
    check(pixels.size()==1600 && pixels[0]==10 && pixels[1]==10 && pixels[3]==255
          && pixels[(20*20-1)*4]==29 && pixels[(20*20-1)*4+1]==29 && pixels[(20*20-1)*4+3]==255,
          "offscreen material leaves opaque real pixels at the canvas world coordinates");
    reference = pixels;
    check(RenderPanelGlass(source, {250,10,270,30}, 20, 20, canvas, pixels) && pixels == reference,
          "partially offscreen material cannot introduce alpha holes into its visible world canvas");
    check(RenderPanelGlass(source, {20,10,40,30}, 20, 20, canvas, pixels) && pixels == reference,
          "moving an unpowered lens across the canvas does not move its world background");
    canvas.Canvas = PixelRect{10,10,40,40};
    check(RenderPanelGlass(source, {300,10,320,30}, 20, 20, canvas, pixels)
          && pixels[0]==10 && pixels[1]==10 && pixels[(20*20-1)*4]==39 && pixels[(20*20-1)*4+1]==39,
          "world canvas uses its own physical bounds with a non-unit output scale");
    auto canvasLens = lens; canvasLens.ClipRounded = false;
    RenderPanelGlass(source, {40,40,140,140}, 100, 100, canvasLens, reference);
    canvasLens.Canvas = PixelRect{20,20,160,160}; canvasLens.Viewport = PixelRect{0,0,256,256};
    check(RenderPanelGlass(source, {40,40,140,140}, 140, 140, canvasLens, pixels)
          && pixels[(70*140+22)*4]==reference[(50*100+2)*4]
          && pixels[0]==20 && pixels[1]==20 && pixels[3]==255,
          "world canvas preserves material lens optics while pixels outside it retain the true backdrop");
    canvas.Material.Color=0xFF204080; canvas.Material.Tint=1;
    check(RenderPanelGlass(source, {300,10,320,30}, 20, 20, canvas, pixels)
          && pixels[0]==128 && pixels[1]==64 && pixels[2]==32 && pixels[3]==255,
          "material tint also applies to real canvas samples outside the current lens");
    canvas.Canvas = PixelRect{90,10,110,30}; canvas.Viewport = PixelRect{0,0,120,100};
    check(!RenderPanelGlass(white, {300,10,320,30}, 20, 20, canvas, pixels),
          "world canvas rejects missing visible scene pixels even when the lens is offscreen");

    lens.Radius = 0; lens.Material.Dispersion = 1;
    RenderPanelGlass(source, {40,40,140,140}, 100, 100, lens, pixels);
    check(pixels.size() == 40000 && pixels[(50 * 100 + 2) * 4] != pixels[(50 * 100 + 2) * 4 + 2], "dispersion samples distinct red and blue positions");

    auto tint = plain; tint.Material.Color = 0xFF204080; tint.Material.Tint = 1;
    RenderPanelGlass(white, {0,0,100,100}, 100, 100, tint, pixels);
    check(pixels.size() == 40000 && pixels[0] == 128 && pixels[1] == 64 && pixels[2] == 32 && pixels[3] == 255, "full tint retains requested BGRA channel order");
    auto noisy = plain; noisy.Material.Noise = 1;
    RenderPanelGlass(white, {0,0,100,100}, 100, 100, noisy, pixels);
    RenderPanelGlass(white, {0,0,100,100}, 100, 100, noisy, reference);
    check(pixels == reference && pixels != white.Pixels, "noise is visible and deterministic");

    auto blurred = plain; blurred.Material.Blur = 12;
    RenderPanelGlass(translucent, {0,0,8,8}, 8, 8, blurred, pixels);
    check(pixels == translucent.Pixels, "blur preserves a translucent constant scene's premultiplied color and alpha");
    auto checker = Constant(24, 24, 0, 0, 0);
    for (int y=0;y<24;++y) for(int x=0;x<24;++x)
    {const auto at=(y*24+x)*4;checker.Pixels[at]=checker.Pixels[at+1]=checker.Pixels[at+2]=((x+y)&1)?255:0;}
    RenderPanelGlass(checker, {0,0,24,24}, 24, 24, blurred, pixels);
    check(pixels.size() == 2304 && pixels[(12*24+12)*4] > 80 && pixels[(12*24+12)*4] < 180, "blur combines neighboring real pixels");
    blurred.Radius = 10;
    RenderPanelGlass(checker, {0,0,24,24}, 24, 24, blurred, pixels);
    check(pixels.size() == 2304 && pixels[3] == 0, "blur does not spread opacity beyond the rounded clip");
    auto highlight = rounded; highlight.Specular = 1;
    RenderPanelGlass(white, {0,0,100,100}, 100, 100, highlight, pixels);
    check(pixels.size() == 40000 && pixels[(1*100+50)*4] > 200, "specular light responds at the top edge");

    auto maximum = lens; maximum.Material.Blur = 10000;
    check(PanelCapturePadding(maximum) >= 105 && PanelCapturePadding(maximum) <= 128, "capture padding covers bounded blur and chromatic displacement");
    maximum.Material.Lens = std::numeric_limits<double>::quiet_NaN();
    maximum.Material.Dispersion = std::numeric_limits<double>::infinity();
    check(PanelCapturePadding(maximum) >= 0 && PanelCapturePadding(maximum) <= 128, "nonfinite optics keep padding bounded");
    check(!RenderPanelGlass({}, {0,0,10,10}, 10, 10, plain, pixels), "missing scene is rejected");
    check(!RenderPanelGlass(white, {0,0,100,100}, 0, 100, plain, pixels), "zero output dimension is rejected");
    check(!RenderPanelGlass(white, {0,0,100,100}, 20000, 100, plain, pixels), "oversized output is rejected");
    check(!RenderPanelGlass(white, {10,0,0,100}, 10, 100, plain, pixels), "inverted target is rejected");
    check(!RenderPanelGlass(white, {-1,0,99,100}, 100, 100, plain, pixels), "missing target pixels are rejected rather than replaced by a synthetic background");
    auto bad = white; bad.Pixels.pop_back();
    check(!RenderPanelGlass(bad, {0,0,100,100}, 100, 100, plain, pixels), "malformed source stride is rejected");

    auto large = Constant(1200, 1100, 80, 100, 120);
    {
        auto base = Constant(256,256,40,60,80);
        const auto original = base.Pixels;
        std::vector<uint8_t> first, moved, again;
        ClientOpticsStats stats;
        check(RenderClientCanvasOptics(base,{40,40,140,140},12,.55,.18,.6,{0,0,256,256},first,&stats)
            && first[(40*256+90)*4]>40 && stats.EdgePixels>0 && stats.EdgePixels<10000,
            "client optics draws the current specular edge using a finite pixel band");
        auto shared=plain; shared.Radius=12; shared.Specular=.6; shared.Material.Lens=.55; shared.Material.Dispersion=.18;
        shared.ClipRounded=false; shared.Canvas=base.Bounds; shared.Viewport=base.Bounds;
        check(RenderPanelGlass(base,{40,40,140,140},256,256,shared,again) && again==first,
            "client optics and legacy canvas rendering share identical edge geometry and specular samples");
        check(RenderClientCanvasOptics(base,{60,70,160,170},12,.55,.18,.6,{0,0,256,256},moved)
            && moved[(40*256+90)*4]==40 && moved[(70*256+110)*4]>40,
            "moving client optics restores the old highlight before drawing the current edge");
        check(RenderClientCanvasOptics(base,{40,40,140,140},12,.55,.18,.6,{0,0,256,256},again)
            && again==first && base.Pixels==original,
            "client optics never accumulates refraction or mutates its immutable base");
        check(RenderClientCanvasOptics(base,{40,40,140,140},12,0,0,0,{0,0,256,256},again)
            && again==original, "client optics does not apply base tint or noise a second time");
        check(RenderClientCanvasOptics(source,{40,40,140,140},12,1,1,0,{0,0,256,256},again)
            && again[(90*256+40)*4]>40 && again[(90*256+40)*4+2]>again[(90*256+40)*4],
            "client edge dispersion samples distinct genuine red and blue coordinates");
        check(RenderClientCanvasOptics(source,{40,40,140,140},20,1,0,0,{0,0,256,256},again)
            && again[(47*256+47)*4]>47 && again[(47*256+47)*4+1]>47,
            "client rounded corner refracts along both current normal components");
        check(RenderClientCanvasOptics(translucent,{0,0,8,8},4,1,1,1,{0,0,8,8},again),
            "client optics accepts a premultiplied translucent source");
        bool premultiplied = again.size()==translucent.Pixels.size();
        for(size_t i=0;premultiplied && i<again.size();i+=4)
            premultiplied = again[i]<=again[i+3] && again[i+1]<=again[i+3] && again[i+2]<=again[i+3];
        check(premultiplied, "client corner highlights preserve premultiplied channel bounds");
        auto negative = base; negative.Bounds={-256,-128,0,128};
        check(RenderClientCanvasOptics(negative,{-250,-120,-150,-20},12,.55,.18,.6,{-256,-128,0,128},again)
            && again[(8*256+56)*4]>40,
            "client optics uses negative physical viewport coordinates");
        auto limited = Constant(100,100,40,60,80); limited.Bounds={100,100,200,200};
        check(!RenderClientCanvasOptics(limited,{198,110,298,190},12,1,1,0,{0,0,3840,2160},again),
            "client optics rejects an in-desktop sample missing from the immutable canvas");
        check(RenderClientCanvasOptics(limited,{198,110,298,190},12,1,1,0,{100,100,200,200},again)
            && again[(40*100+99)*4+3]==255,
            "client optics clamps only at real desktop edges without punching alpha holes");
        auto invalidBase=base; invalidBase.Pixels.pop_back();
        check(!RenderClientCanvasOptics(invalidBase,{40,40,140,140},12,1,1,1,{0,0,256,256},again),
            "client optics rejects truncated base pixels before processing");
        check(!RenderClientCanvasOptics(base,{40,40,140,140},12,1,1,1,{0,0,256,256},base.Pixels)
            && base.Pixels==original, "client optics rejects source-output aliasing without changing the source");
    }
    {
        struct Benchmark { const char* Name; int Width,Height; PixelRect Canvas,Target; };
        // Canonical canvases/output sizes read from the current 150% DPI
        // requests; material bounds match their recorded visible positions.
        const Benchmark cases[]{
            {"start-main",1005,1440,{955,0,2463,2160},{1083,771,2334,2070}},
            {"start-side",449,1440,{2212,0,2886,2160},{2340,771,2757,2070}},
            {"notification",403,1440,{3236,0,3840,2160},{3318,12,3822,1872}},
            {"calendar",403,1440,{3236,0,3840,2160},{3318,1890,3822,2070}}};
        for(auto const& item:cases)
        {
            auto base=Constant(item.Width,item.Height,80,100,120); base.Bounds=item.Canvas;
            std::vector<uint8_t> output; ClientOpticsStats stats;
            bool valid=RenderClientCanvasOptics(base,item.Target,12,.55,.18,.6,{0,0,3840,2160},output,&stats);
            std::vector<double> elapsed; unsigned checksum=0;
            for(int i=0;i<40;++i)
            {
                auto target=item.Target; const int delta=i%7-3; target.Top+=delta; target.Bottom+=delta;
                const auto began=std::chrono::steady_clock::now();
                valid=RenderClientCanvasOptics(base,target,12,.55,.18,.6,{0,0,3840,2160},output,&stats)&&valid;
                elapsed.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count());
                checksum+=output[(static_cast<size_t>(item.Height/2)*item.Width+item.Width/2)*4];
            }
            std::sort(elapsed.begin(),elapsed.end());
            check(valid,"current-canvas client optics benchmark samples remain geometrically valid");
            std::printf("client optics %s output=%dx%d canvas=%d,%d,%d,%d target=%d,%d,%d,%d radius=12 calls=40 edge=%zu p50=%.3fms p95=%.3fms checksum=%u\n",
                item.Name,item.Width,item.Height,item.Canvas.Left,item.Canvas.Top,item.Canvas.Right,item.Canvas.Bottom,
                item.Target.Left,item.Target.Top,item.Target.Right,item.Target.Bottom,stats.EdgePixels,elapsed[20],elapsed[37],checksum);
        }
    }
    auto optical = PanelGlassOptions{}; optical.Material.Blur = 9; optical.Material.Color = 0x60203050;
    const auto start = std::chrono::steady_clock::now();
    for (int i=0;i<8;++i) RenderPanelGlass(large, {100,100,1100,1000}, 1000, 900, optical, pixels);
    const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/8;
    std::printf("panel render 1000x900: %.2f ms; %d checks, %d failed\n", ms, checks, failed);
    return failed ? 1 : 0;
}
