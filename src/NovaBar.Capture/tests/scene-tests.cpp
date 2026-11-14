#include <cstdio>
#include <chrono>
#include "../SceneBackdropSource.h"

namespace
{
    // Substitute only WGC/Win32 boundaries: pool ownership, crop caching,
    // exclusion, retry and eviction remain production code under test.
    struct CaptureFixture
    {
        inline static int Starts = 0, Readbacks = 0, Drains = 0;
        inline static int Live = 0, Peak = 0;
        inline static bool FailStart = false;
        inline static std::unordered_set<HWND> Closed;
        HWND Handle = nullptr;
        uint64_t Serial = 0;
        ~CaptureFixture() { Close(); }
        HWND Window() const { return Closed.contains(Handle) ? nullptr : Handle; }
        uint64_t Generation() const { return Serial; }
        void Start(HWND window)
        {
            Close();
            ++Starts;
            if (FailStart) winrt::throw_hresult(E_ACCESSDENIED);
            Handle = window; Closed.erase(window); Serial = 0; ++Live; Peak = std::max(Peak, Live);
        }
        void Close() { if (Handle) --Live; Handle = nullptr; Serial = 0; }
        bool DrainLatest() { ++Drains; if (!Serial) { Serial = 1; return true; } return false; }
        bool Read(novabar::PixelRect region, novabar::PixelFrame& out, bool, bool)
        {
            ++Readbacks; out.Bounds = region;
            out.Width = region.Right - region.Left; out.Height = region.Bottom - region.Top;
            out.Pixels.assign(static_cast<size_t>(out.Width) * out.Height * 4, 255);
            out.Pixels[0] = static_cast<uint8_t>(region.Left); return true;
        }
    };
    struct DesktopFixture
    {
        inline static std::unordered_set<DWORD> DeniedOwners;
        inline static int OwnerOpens = 0;
        struct Owner
        {
            bool Allowed;
            explicit Owner(DWORD pid) : Allowed(!DeniedOwners.contains(pid)) { ++OwnerOpens; }
            bool Alive() const { return Allowed; }
        };
        inline static uint64_t Tick = 100;
        inline static std::vector<novabar::scene_detail::WindowSnapshot> Windows;
        static uint64_t Now() { return Tick; }
        static bool Valid(novabar::scene_detail::WindowSnapshot const& snapshot)
        {
            return std::any_of(Windows.begin(), Windows.end(), [&](auto const& current)
            { return current.Window == snapshot.Window && current.Process == snapshot.Process
                  && novabar::scene_detail::SameRect(current.Rectangle, snapshot.Rectangle); });
        }
        static auto VisibleWindows(novabar::PixelRect) { return Windows; }
    };
    struct StackFixture
    {
        inline static std::vector<novabar::scene_detail::WindowSnapshot> Stack;
        static auto VisibleWindows(novabar::PixelRect)
        {
            auto visible = Stack;
            std::erase_if(visible, [](auto const& item) { return item.Window == reinterpret_cast<HWND>(2); });
            return visible;
        }
        static auto LowerWindows(HWND excluded, novabar::PixelRect)
        {
            auto at = std::find_if(Stack.begin(), Stack.end(), [&](auto const& item) { return item.Window == excluded; });
            return at == Stack.end() ? std::vector<novabar::scene_detail::WindowSnapshot>{}
                : std::vector<novabar::scene_detail::WindowSnapshot>(std::next(at), Stack.end());
        }
        static DWORD Process(HWND window)
        {
            const auto at = std::find_if(Stack.begin(), Stack.end(), [&](auto const& item) { return item.Window == window; });
            return at == Stack.end() ? 0 : at->Process;
        }
    };
}

int main()
{
    int failures = 0;
    const auto check = [&](bool value, const char* name)
    { if (!value) { ++failures; printf("FAIL: %s\n", name); } };
    using namespace novabar;
    {
        PixelFrame storage; storage.Pixels.resize(17*9*4,77);
        const auto* allocation=storage.Pixels.data();
        scene_detail::SceneAccumulator reused({-13,-7,4,2},std::move(storage));
        check(reused.Frame().Pixels.data()==allocation,
            "scene accumulator reuses its owned scratch allocation");
        check(std::all_of(reused.Frame().Pixels.begin(),reused.Frame().Pixels.end(),[](uint8_t value){return value==0;}),
            "reused accumulator clears every old premultiplied pixel before new coverage");
    }
    {
        // Splitting each clip into single pixels disables all SIMD groups and
        // provides an independent segmentation-invariance oracle for fast paths.
        const PixelRect bounds{-13,-7,4,2};
        for(int mode=0;mode<5;++mode)
        {
            PixelFrame top;top.Width=17;top.Height=9;top.Bounds=bounds;top.Pixels.resize(17*9*4);
            auto bottom=top;
            for(size_t i=0;i<top.Pixels.size();i+=4)
            {
                const unsigned alpha=mode==0?255:mode==1?0:mode==2?(i/4%3==0?255:i/4%3==1?128:0):255;
                top.Pixels[i]=static_cast<uint8_t>((i*17)%(alpha+1));
                top.Pixels[i+1]=static_cast<uint8_t>((i*7)%(alpha+1));
                top.Pixels[i+2]=static_cast<uint8_t>((i*3)%(alpha+1));top.Pixels[i+3]=static_cast<uint8_t>(alpha);
                if(mode==4 && i/4%5==0){top.Pixels[i]=top.Pixels[i+2]=0;top.Pixels[i+1]=255;}
                bottom.Pixels[i]=91;bottom.Pixels[i+1]=37;bottom.Pixels[i+2]=11;bottom.Pixels[i+3]=255;
            }
            const std::vector<PixelRect> clips=mode==4?std::vector<PixelRect>{{-12,-6,3,0},{-9,0,4,2}}:std::vector<PixelRect>{bounds};
            std::vector<PixelRect> singles;
            for(auto clip:clips)for(int y=clip.Top;y<clip.Bottom;++y)for(int x=clip.Left;x<clip.Right;++x)singles.push_back({x,y,x+1,y+1});
            scene_detail::SceneAccumulator grouped(bounds),scalar(bounds);
            const unsigned opacity=mode==3?128:255;const uint32_t key=mode==4?RGB(0,255,0):0x1000000;
            grouped.Composite(top,clips,opacity,key);scalar.Composite(top,singles,opacity,key);
            check(grouped.Frame().Pixels==scalar.Frame().Pixels && grouped.Complete()==scalar.Complete()
                && grouped.Needs(bounds)==scalar.Needs(bounds),"grouped opaque/alpha/key/region coverage equals single-pixel composition");
            grouped.Composite(bottom);singles.clear();
            for(int y=bounds.Top;y<bounds.Bottom;++y)for(int x=bounds.Left;x<bounds.Right;++x)singles.push_back({x,y,x+1,y+1});
            scalar.Composite(bottom,singles);
            check(grouped.Frame().Pixels==scalar.Frame().Pixels && grouped.Complete() && scalar.Complete(),
                "grouped covered-skip and scalar premultiplied fallback preserve identical complete scenes");
        }
    }
    {
        SceneUpdate legacy{true,false,S_OK};
        check(!legacy.Profile.Enabled && legacy.Profile.DetailCount==0,
            "three-field scene updates retain disabled zero-cost profiling defaults");
        SceneProfile profile;
        PixelFrame source;source.Width=1517;source.Height=2160;source.Bounds={950,0,2468,2160};
        scene_detail::RecordSourceProfile(profile,source,128,2,3);
        check(profile.Sources==0,"disabled source profiling records no dimensions or counters");
        profile.Enabled=true;
        for(int i=0;i<12;++i)scene_detail::RecordSourceProfile(profile,source,128,2,3);
        check(profile.Sources==12 && profile.NonDirectSources==12 && profile.DetailCount==8,
            "source profiling counts non-direct captures and bounds detailed records to eight");
        check(profile.Details[0].Width==1517 && profile.Details[0].BoundsWidth==1518
            && profile.Details[0].Opacity==128 && !profile.Details[0].Direct
            && profile.Details[0].ReadMs==2 && profile.Details[0].ComposeMs==3,
            "source profiling distinguishes captured texture dimensions from physical crop and opacity");
    }
    {
        // Break caught: composing below a hidden popup's old Z-position omits
        // a newly opened foreground source, then changes background on show.
        const HWND red = reinterpret_cast<HWND>(1), target = reinterpret_cast<HWND>(2);
        const HWND blue = reinterpret_cast<HWND>(3), sibling = reinterpret_cast<HWND>(4);
        StackFixture::Stack = {{sibling,10,{0,0,1,1}}, {red,20,{0,0,1,1}},
                               {target,10,{0,0,1,1}}, {blue,30,{0,0,1,1}}};
        const std::unordered_map<HWND, PixelFrame> pixels{
            {sibling, {1,1,{0,0,1,1},{0,255,0,255}}},
            {red, {1,1,{0,0,1,1},{0,0,255,255}}},
            {blue, {1,1,{0,0,1,1},{255,0,0,255}}}};
        const auto compose = [&](ScenePlacement placement)
        {
            scene_detail::SceneAccumulator scene({0,0,1,1});
            for (auto const& item : scene_detail::BackdropWindows<StackFixture>(target, {0,0,1,1}, placement))
            { if (scene.Complete()) break; scene.Composite(pixels.at(item.Window)); }
            return scene.Frame().Pixels;
        };
        check(compose(ScenePlacement::Actual)==std::vector<uint8_t>({255,0,0,255}),
              "actual placement samples only sources below the popup in current Z order");
        check(compose(ScenePlacement::Promoted)==std::vector<uint8_t>({0,0,255,255}),
              "hidden promoted placement includes a new normal source above its old Z position");
        auto selected = scene_detail::BackdropWindows<StackFixture>(target, {0,0,1,1}, ScenePlacement::Promoted);
        check(selected.size()==2 && selected[0].Window==red && selected[1].Window==blue,
              "promoted source order excludes all popup-owner visuals before compositing");
        StackFixture::Stack = {{target,10,{0,0,1,1}}, {red,20,{0,0,1,1}}, {blue,30,{0,0,1,1}}};
        check(compose(ScenePlacement::Actual)==std::vector<uint8_t>({0,0,255,255}),
              "actual placement retains the promoted backdrop once the native popup is above the new source");
        StackFixture::Stack = {{sibling,10,{0,0,1,1}}, {target,10,{0,0,1,1}}};
        scene_detail::SceneAccumulator absent({0,0,1,1});
        for(auto const& item : scene_detail::BackdropWindows<StackFixture>(target,{0,0,1,1},ScenePlacement::Promoted))
            absent.Composite(pixels.at(item.Window));
        check(!absent.Complete(), "owner-only promoted stack cannot invent a complete background");
    }
    {
        // Break caught: an unqueryable front window consumes every warm-start
        // budget and is reopened every tick, starving ordinary source windows.
        DesktopFixture::Windows = {{reinterpret_cast<HWND>(1), 10, {0, 0, 100, 100}},
                                  {reinterpret_cast<HWND>(2), 20, {0, 0, 100, 100}}};
        DesktopFixture::DeniedOwners = {10}; DesktopFixture::OwnerOpens = 0;
        CaptureFixture::Starts = CaptureFixture::Drains = 0;
        BasicSceneCapturePool<CaptureFixture, DesktopFixture> pool;
        pool.Warm({0, 0, 100, 100}, {});
        check(CaptureFixture::Starts == 1 && CaptureFixture::Drains == 1,
              "inaccessible owner does not starve a normal source in the same warm pass");
        const int opens = DesktopFixture::OwnerOpens;
        pool.Warm({0, 0, 100, 100}, {});
        check(DesktopFixture::OwnerOpens == opens,
              "inaccessible owner discovery backs off instead of reopening each tick");
        DesktopFixture::DeniedOwners.clear();
    }
    {
        // Break caught: background discovery grows WGC resource ownership
        // without bound, or required sources cannot displace idle warm sources.
        DesktopFixture::Windows.clear();
        for (int i = 1; i <= 25; ++i)
            DesktopFixture::Windows.push_back({reinterpret_cast<HWND>(static_cast<intptr_t>(i)),
                static_cast<DWORD>(100 + i), {0, 0, 100, 100}});
        CaptureFixture::Starts = CaptureFixture::Readbacks = CaptureFixture::Live = CaptureFixture::Peak = 0;
        BasicSceneCapturePool<CaptureFixture, DesktopFixture> pool;
        for (int i = 0; i != 24; ++i) pool.Warm({0, 0, 100, 100}, {});
        auto needed = pool.Read(DesktopFixture::Windows.back(), {10, 10, 20, 20});
        check(needed.Ready && CaptureFixture::Starts == 25 && CaptureFixture::Peak == 24,
              "visible demand displaces an idle source while total WGC sessions stay bounded");
    }
    {
        // Break caught: each failed background start consumes no budget and
        // retries every other eligible source in the same foreground cycle.
        DesktopFixture::Windows = {{reinterpret_cast<HWND>(1), 10, {0, 0, 100, 100}},
                                  {reinterpret_cast<HWND>(2), 20, {0, 0, 100, 100}}};
        CaptureFixture::Starts = CaptureFixture::Readbacks = CaptureFixture::Drains = 0;
        CaptureFixture::FailStart = true;
        BasicSceneCapturePool<CaptureFixture, DesktopFixture> pool;
        pool.Warm({0, 0, 100, 100}, {});
        check(CaptureFixture::Starts == 1, "failed WGC starts still consume the background initialization budget");
        const int attempts = CaptureFixture::Starts;
        auto failed = pool.Read(DesktopFixture::Windows.front(), {10, 10, 20, 20});
        check(!failed.Ready && failed.Error == E_ACCESSDENIED && CaptureFixture::Starts == attempts,
              "source failure preserves its HRESULT and backs off before retrying");
        CaptureFixture::FailStart = false;
    }
    {
        // Breaks caught: warming maps the entire desktop, a second target starts
        // another session, or moving back to a static crop waits for new damage.
        const HWND first = reinterpret_cast<HWND>(1), excluded = reinterpret_cast<HWND>(2);
        DesktopFixture::Windows = {{first, 10, {0, 0, 100, 100}}, {excluded, 20, {0, 0, 100, 100}}};
        CaptureFixture::Starts = CaptureFixture::Readbacks = CaptureFixture::Drains = 0;
        BasicSceneCapturePool<CaptureFixture, DesktopFixture> pool;
        pool.Warm({0, 0, 100, 100}, {excluded});
        check(CaptureFixture::Starts == 1 && CaptureFixture::Drains == 1 && CaptureFixture::Readbacks == 0,
              "background prewarm excludes every known target and retains only GPU pixels");
        const auto source = DesktopFixture::Windows.front();
        auto a = pool.Read(source, {10, 10, 20, 20});
        check(a.Ready && a.Frame && a.Frame->Pixels[0] == 10,
              "first target immediately crops a prewarmed static source");
        auto b = pool.Read(source, {30, 10, 40, 20});
        check(b.Ready && b.Frame && b.Frame->Pixels[0] == 30 && CaptureFixture::Starts == 1,
              "second target reuses the same source session at its own coordinates");
        const int copies = CaptureFixture::Readbacks;
        a = pool.Read(source, {10, 10, 20, 20});
        check(a.Ready && CaptureFixture::Readbacks == copies,
              "returning to an unchanged crop needs neither source damage nor another CPU copy");
        CaptureFixture::Closed.insert(first);
        a = pool.Read(source, {10, 10, 20, 20});
        check(a.Ready && CaptureFixture::Starts == 2 && CaptureFixture::Readbacks == copies + 1,
              "a closed capture restarts without reusing pixels from the previous source generation");
        DesktopFixture::Windows.front().Process = 11;
        auto stale = pool.Read(source, {10, 10, 20, 20});
        check(!stale.Ready && stale.Frame == nullptr,
              "reused HWND with a different owner cannot return cached source pixels");
        pool.Warm({0, 0, 100, 100}, {first, excluded});
        const int warmStarts = CaptureFixture::Starts;
        auto lowerTarget = pool.Read(DesktopFixture::Windows.front(), {10, 10, 20, 20});
        check(lowerTarget.Ready && lowerTarget.Frame && CaptureFixture::Starts == warmStarts + 1,
              "actual lower target can supply demanded pixels despite exclusion from background prewarm");
        const int demandStarts = CaptureFixture::Starts;
        pool.Warm({0, 0, 100, 100}, {first, excluded});
        lowerTarget = pool.Read(DesktopFixture::Windows.front(), {10, 10, 20, 20});
        check(lowerTarget.Ready && CaptureFixture::Starts == demandStarts,
              "background exclusion preserves a recently demanded lower-target capture session");
    }
    {
        // Break caught: interpreting captured premultiplied red as straight alpha,
        // or compositing the lower blue layer above the translucent red layer.
        scene_detail::SceneAccumulator scene({0, 0, 2, 1});
        PixelFrame top{2, 1, {0, 0, 2, 1}, {0, 0, 128, 128, 0, 0, 0, 0}};
        PixelFrame bottom{2, 1, {0, 0, 2, 1}, {255, 0, 0, 255, 255, 0, 0, 255}};
        check(scene.Composite(top), "accept translucent foreground layer");
        check(scene.Needs({0, 0, 2, 1}), "translucency leaves lower scene required");
        check(scene.Composite(bottom), "accept lower opaque layer");
        check(scene.Complete(), "opaque lower layer closes remaining coverage");
        const auto& p = scene.Frame().Pixels;
        check(p == std::vector<uint8_t>({127, 0, 128, 255, 255, 0, 0, 255}),
              "premultiplied foreground and true z-order preserve literal colors");
    }
    {
        // Literal golden pixels protect non-integer source scaling when capture
        // and physical crop sizes differ, including clipped negative origins,
        // premultiplication, global opacity, and an opaque color-key hole.
        const PixelRect bounds{-5,-3,2,2};
        PixelFrame upper{3,2,{-6,-4,3,3},
            {0,0,255,255, 0,255,0,255, 128,0,0,128,
             0,0,0,0, 40,60,80,255, 255,255,255,255}};
        PixelFrame lower;lower.Width=7;lower.Height=5;lower.Bounds=bounds;lower.Pixels.resize(7*5*4);
        for(size_t i=0;i<lower.Pixels.size();i+=4){lower.Pixels[i]=100;lower.Pixels[i+3]=255;}
        scene_detail::SceneAccumulator scene(bounds);
        check(scene.Composite(upper,{{-4,-2,2,2}},128,RGB(0,255,0))&&!scene.Complete(),
            "scaled translucent source retains incomplete clipped coverage");
        check(scene.Composite(lower)&&scene.Complete(),"scaled source preserves lower opaque coverage");
        auto expected=lower.Pixels;
        const auto pixel=[&](int x,int y,uint8_t b,uint8_t g,uint8_t r){const size_t at=(y*7+x)*4;
            expected[at]=b;expected[at+1]=g;expected[at+2]=r;};
        for(int y:{1,2}){pixel(1,y,50,0,128);pixel(5,y,139,0,0);pixel(6,y,139,0,0);}
        for(int y:{3,4}){for(int x:{2,3,4})pixel(x,y,70,30,40);
            pixel(5,y,178,128,128);pixel(6,y,178,128,128);}
        check(scene.Frame().Pixels==expected,
            "scaled physical mapping preserves literal clipped color-key and premultiplied golden pixels");
    }
    {
        // Break caught: requesting an underlying capture even when every sample
        // that intersects it is already covered by an opaque upper window.
        scene_detail::SceneAccumulator scene({-2, -1, 0, 0});
        PixelFrame left{1, 1, {-2, -1, -1, 0}, {12, 34, 56, 255}};
        scene.Composite(left);
        check(!scene.Needs({-2, -1, -1, 0}), "opaque local coverage skips hidden captures");
        check(scene.Needs({-1, -1, 0, 0}), "uncovered neighbor remains required");
        check(!scene.Complete(), "partially covered scene is not ready");
        check(scene.Frame().Pixels[0] == 12 && scene.Frame().Pixels[3] == 255,
              "negative physical coordinates map to first target pixel");
    }
    {
        // Break caught: ignoring explicit window regions or treating transparent
        // rounded pixels as opaque black, hiding the desktop underneath them.
        scene_detail::SceneAccumulator scene({0, 0, 2, 2});
        PixelFrame upper{2, 2, {0, 0, 2, 2},
            {0, 0, 0, 0, 0, 128, 0, 128, 0, 255, 0, 255, 0, 255, 0, 255}};
        scene.Composite(upper, {{0, 0, 2, 1}});
        PixelFrame lower{2, 2, {0, 0, 2, 2},
            {50, 0, 0, 255, 50, 0, 0, 255, 50, 0, 0, 255, 50, 0, 0, 255}};
        scene.Composite(lower);
        check(scene.Frame().Pixels == std::vector<uint8_t>({50, 0, 0, 255, 25, 128, 0, 255,
                                                          50, 0, 0, 255, 50, 0, 0, 255}),
              "rounded alpha and explicit region preserve lower pixels");
    }
    {
        scene_detail::SceneAccumulator scene({0, 0, 1, 1});
        PixelFrame invalid{2, 1, {0, 0, 2, 1}, {1, 2, 3, 255}};
        check(!scene.Composite(invalid) && !scene.Complete(), "incomplete capture buffer is rejected");
        check(!scene.Needs({2, 2, 3, 3}), "nonintersecting windows require no capture");
        check(!scene.Needs({2, 0, 3, 1}), "horizontally disjoint source requires no capture");
    }
    {
        scene_detail::SceneAccumulator scene({0, 0, 2, 1});
        PixelFrame upper{2, 1, {0, 0, 2, 1}, {0, 0, 255, 255, 0, 255, 0, 255}};
        PixelFrame lower{2, 1, {0, 0, 2, 1}, {255, 0, 0, 255, 255, 0, 0, 255}};
        scene.Composite(upper, {}, 128, RGB(0, 255, 0)); scene.Composite(lower);
        check(scene.Frame().Pixels == std::vector<uint8_t>({127, 0, 128, 255, 255, 0, 0, 255}),
              "global layer opacity and color-key apply outside raw capture surface");
    }
    {
        PixelFrame layer; layer.Width = 1000; layer.Height = 900; layer.Bounds = {0, 0, 1000, 900};
        layer.Pixels.resize(1000 * 900 * 4, 255);
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 16; ++i)
        { scene_detail::SceneAccumulator scene(layer.Bounds); scene.Composite(layer); check(scene.Complete(), "large opaque scene closes coverage"); }
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 16;
        printf("scene compose 1000x900: %.2f ms\n", elapsed);
    }
    printf("scene tests: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}
