#include <cstdio>
#include <cmath>
#include <future>
#include <exception>
#include "../WgcCapture.h"
#include "../WallpaperSource.h"
#include "../GlassPipeline.h"
#include "../bridge.cpp"
#include "../../NovaBar.Capture/CaptureFrameWriter.h"

int main(int argc, char**)
{
    int failed = 0;
    for (const double amount : {0.0, 0.2, 1.0})
    {
        const auto spec = ParseSpec(L"accent=liquid;lens=" + std::to_wstring(amount));
        if (std::abs(spec.Lens - amount) > 0.00001)
        {
            printf("FAIL: lens %.2f was parsed as %.2f\n", amount, spec.Lens);
            ++failed;
        }
    }
    const auto check = [&](bool ok, const char* name) { if (!ok) { printf("FAIL: %s\n", name); ++failed; } };
    {
        using namespace novabar::client_detail;
        SurfaceVisibility popup;
        popup.Loaded = popup.ElementVisible = popup.CoordinateVisible = popup.TopVisible = popup.Owned = true;
        popup.CloakKnown = true;
        popup.HostVisible = 0; popup.WindowVisible = 0;
        check(EffectiveVisibility(popup) && Activity(true, true, EffectiveVisibility(popup)) == SurfaceActivity::Visible
            && SourceInterval(true, EffectiveVisibility(popup)) == 8,
            "an owned visible native popup runs visible capture despite stale XAML host and CoreWindow flags on cold open");
        popup.HostVisible = popup.WindowVisible = 1;
        popup.Cloaked = true;
        check(!EffectiveVisibility(popup), "a cloaked native popup remains hidden warm even if XAML reports visible");
        popup.Cloaked = false; popup.Iconic = true;
        check(!EffectiveVisibility(popup), "an iconic native popup never becomes a visible capture target");
        popup.Iconic = false; popup.CoordinateVisible = false;
        check(!EffectiveVisibility(popup), "a hidden native coordinate host remains hidden warm");
        popup.CoordinateVisible = true; popup.TopVisible = false;
        check(!EffectiveVisibility(popup), "a hidden native root remains hidden warm");
        popup.TopVisible = true; popup.Owned = false;
        check(!EffectiveVisibility(popup), "an unrelated native root cannot promote an element into visible capture");
        popup.Owned = true; popup.ElementVisible = false;
        check(!EffectiveVisibility(popup), "a collapsed element cannot become visible through its native host");
        popup.ElementVisible = true; popup.Loaded = false;
        check(!EffectiveVisibility(popup), "an unloaded measured element only prepares its hidden source");
        popup.Loaded = true; popup.CloakKnown = false;
        check(!EffectiveVisibility(popup), "an unknown native cloaking query fails closed instead of promoting a hidden popup");
    }
    {
        novabar::client_detail::PresentationDriver driver;
        check(!driver.Enter(true, 1) && !driver.Enter(false), "a stopped client ignores queued rendering and heartbeat callbacks");
        driver.Start();
        check(driver.Enter(true, 100), "a panel can present on a XAML rendering frame without a timer tick");
        check(!driver.Enter(false) && !driver.Enter(true, 101), "presentation callbacks cannot reenter source or optics work");
        driver.Leave();
        check(!driver.Enter(true, 100), "the same XAML rendering frame cannot upload twice");
        check(driver.Enter(true, 101), "the next XAML rendering frame can update the native pane pose");
        driver.Stop();
        driver.Leave();
        check(!driver.Enter(true, 102) && !driver.Enter(false), "stopping during publication blocks remaining queued callbacks");
        driver.Start();
        check(driver.Enter(true, 100), "restarting a client resets its previous rendering frame identity");
        driver.Leave();
        check(novabar::client_detail::SourceInterval(true, false) == 100
            && novabar::client_detail::SourceInterval(true, true) == 8
            && novabar::client_detail::SourceInterval(false, false) == 16,
            "hidden panels poll slowly while visible animated sources and taskbars retain their source cadence");
        const auto opening = novabar::client_detail::SelectPresentationWork(true, true, true);
        check(opening.Present && opening.Source,
            "the first visible rendering frame presents its compatible hidden base before registering visibility");
        const auto replacement = novabar::client_detail::SelectPresentationWork(false, true, false);
        check(!replacement.Present && replacement.Source,
            "a different surface epoch must register its new source before presenting pixels");
    }
    {
        using Action = novabar::client_detail::RequestWriteFailure;
        for (const DWORD error : {ERROR_ACCESS_DENIED, ERROR_SHARING_VIOLATION, ERROR_LOCK_VIOLATION, ERROR_IO_PENDING})
        {
            novabar::client_detail::RequestWriteState writes;
            writes.Succeeded(1000);
            check(writes.Failed(error) == Action::Retry && writes.RetryPending && writes.LastSuccessTick == 1000,
                "a transient request replacement failure preserves the successful publication for retry");
            check(writes.CanPresent(true, 1100), "a transient write failure keeps recent real glass in flight");
            check(!writes.CanPresent(true, 1201),
                "an old request cannot renew visible glass indefinitely while request replacement keeps failing");
            check(writes.CanPresent(false, 6000), "request write retry may preserve a hidden bitmap without displaying it");
            writes.Succeeded(1216);
            check(!writes.RetryPending && writes.LastSuccessTick == 1216 && writes.CanPresent(true, 1232),
                "a recovered request write resumes presentation without replacing the surface epoch");
        }
        novabar::client_detail::RequestWriteState invalid;
        check(invalid.Failed(ERROR_INVALID_DATA) == Action::Cancel && invalid.Failed(ERROR_INVALID_PARAMETER) == Action::Cancel,
            "invalid request geometry still cancels capture instead of retrying forever");
        novabar::client_detail::RequestWriteState neverPublished;
        neverPublished.Failed(ERROR_ACCESS_DENIED);
        check(!neverPublished.CanPresent(true, 100), "a failed first request cannot claim an existing real-glass publication");
        using novabar::client_detail::NeedsMotionRegistration;
        uint64_t successfulRevision = 1;
        const uint64_t newRevision = 2;
        check(NeedsMotionRegistration(true, true, newRevision, successfulRevision, true),
            "a material epoch change registers its first motion even when the pane is static");
        // A failed replacement keeps the last successful request at revision
        // one, while remembered geometry already belongs to revision two.
        check(NeedsMotionRegistration(false, true, newRevision, successfulRevision, true),
            "retrying a new epoch must register its first motion after a failed unchanged-pose write");
        successfulRevision = newRevision;
        check(!NeedsMotionRegistration(false, true, newRevision, successfulRevision, true),
            "a successfully registered static epoch does not duplicate motion entries on heartbeat");
        check(NeedsMotionRegistration(false, true, newRevision, successfulRevision, false)
            && NeedsMotionRegistration(false, false, newRevision, successfulRevision, true),
            "real movement and an unpublished surface still register motion");
    }
    {
        using namespace novabar;
        using namespace novabar::client_detail;
        PixelFrame immutable;
        immutable.Width = immutable.Height = 96; immutable.Bounds = {0, 0, 96, 96};
        immutable.Pixels.resize(96 * 96 * 4);
        for (int y = 0; y < 96; ++y) for (int x = 0; x < 96; ++x)
        {
            const size_t at = (y * 96 + x) * 4;
            immutable.Pixels[at] = static_cast<uint8_t>(x * 2);
            immutable.Pixels[at + 1] = static_cast<uint8_t>(y * 2);
            immutable.Pixels[at + 2] = static_cast<uint8_t>((x + y) % 192);
            immutable.Pixels[at + 3] = 255;
        }
        capture::Request current;
        current.Revision = 5; current.Flags = capture::HostClipsCorners | capture::ClientOptics;
        current.Bounds = {20, 20, 70, 70}; current.Radius = 10; current.Specular = .5;
        current.Material.Lens = .6; current.Material.Dispersion = .2;
        check(capture::detail::SetCanvasBounds(current, immutable.Bounds), "prepare current-optics integration canvas");
        CurrentCanvasOptics optics;
        bool changed = false;
        optics.SetBase(immutable, current.Revision, 17);
        check(optics.Apply(current, immutable.Bounds, changed) && changed, "a new immutable base gets current local optics before presentation");
        std::vector<uint8_t> expected;
        RenderClientCanvasOptics(immutable, current.Bounds, current.Radius, current.Material.Lens,
            current.Material.Dispersion, current.Specular, immutable.Bounds, expected);
        check(optics.Pixels() == expected && optics.Base().Pixels == immutable.Pixels,
            "local optics samples the immutable prepared base without altering it");
        const auto previousPosePixels = optics.Pixels();
        current.Bounds = {25, 30, 75, 80};
        capture::detail::SetCanvasBounds(current, immutable.Bounds);
        check(optics.Apply(current, immutable.Bounds, changed) && changed,
            "current native motion rerenders optics without waiting for another broker frame");
        RenderClientCanvasOptics(immutable, current.Bounds, current.Radius, current.Material.Lens,
            current.Material.Dispersion, current.Specular, immutable.Bounds, expected);
        check(optics.Pixels() == expected && optics.Pixels() != previousPosePixels && optics.Base().Pixels == immutable.Pixels,
            "moving optics restores the old edge and never accumulates lens or highlight into the base");
        const auto allocation = optics.Pixels().data();
        check(optics.Apply(current, immutable.Bounds, changed) && !changed && optics.Pixels().data() == allocation,
            "an unchanged base and native pose reuse the existing pixel allocation without another optics pass");
        auto updated = immutable; updated.Pixels[0] = 231;
        optics.SetBase(updated, current.Revision, 18);
        check(optics.Apply(current, immutable.Bounds, changed) && changed && optics.Pixels()[0] == 231,
            "new source pixels refresh the current pose even when the native bounds stay fixed");
        auto wrongEpoch = current; ++wrongEpoch.Revision;
        check(!optics.Apply(wrongEpoch, immutable.Bounds, changed), "a local optics cache never crosses a material epoch");
        current.Visible = 0;
        check(CanRetainBaseCanvas(optics.HasBase(), 1000, 2500)
            && !CanRetainBaseCanvas(optics.HasBase(), 1000, 2501),
            "an immutable base expires at the protocol deadline even while the XAML visible flag remains false");
        if (!CanRetainBaseCanvas(optics.HasBase(), 1000, 2501)) optics.Clear();
        check(!optics.HasBase() && !optics.Apply(current, immutable.Bounds, changed),
            "source loss clears the immutable base and prevents presenting current optics from expired pixels");
        capture::Frame frame;
        frame.Status = capture::Status::Ready; frame.Revision = current.Revision; frame.Serial = 17;
        frame.Width = frame.Height = 96; frame.BitmapBounds = immutable.Bounds;
        frame.PayloadFlags = capture::BaseCanvasPayload;
        check(CompatibleBaseCanvas(current, frame), "base payload is accepted without inventing a producer rendered pose");
        frame.MotionSequence = 1;
        check(!CompatibleBaseCanvas(current, frame), "base payload with a producer material pose is rejected");
        frame.MotionSequence = 0; frame.PayloadFlags = 0;
        check(!CompatibleBaseCanvas(current, frame), "legacy finished optics cannot masquerade as an immutable base payload");
        auto legacyMode = current; legacyMode.Flags &= ~capture::ClientOptics;
        SurfaceLayout layout{30, 50, 50, 1, 10};
        check(!SameSurfaceEpoch(current, legacyMode, layout, layout), "switching client optics mode discards the previous canvas epoch");
        auto gpuMode = current; gpuMode.Flags |= capture::GpuBase;
        check(!SameSurfaceEpoch(current, gpuMode, layout, layout),
            "switching CPU and GPU transport cannot reuse an old source epoch");
        frame.PayloadFlags = capture::BaseCanvasPayload | capture::GpuBasePayload;
        frame.Generation = 9; frame.ReadyFenceValue = 1; frame.TextureSlot = 0;
        frame.PublishedTick = GetTickCount64();
        check(CompatibleGpuBaseCanvas(gpuMode, frame), "explicit GPU metadata retains exact canvas geometry without CPU pixels");
        check(!CompatibleGpuBaseCanvas(current, frame), "GPU metadata requires an explicit GPU request");
        frame.BitmapBounds.Left += 1;
        check(!CompatibleGpuBaseCanvas(gpuMode, frame), "GPU metadata cannot cross canvas geometry");
    }
    {
        using namespace novabar;
        using namespace novabar::client_detail;
        const PixelRect desktop{0, 0, 3840, 2160};
        const PixelRect notificationHost{3300, 0, 3840, 2088};
        const auto notification = StableCanvas(notificationHost, desktop, 3300, 540, true);
        check(SameRect(notification, {3236, 0, 3840, 2160}),
            "narrow notification hosts prepare their full visible travel column before opening");
        const PixelRect startHost{0, 0, 3840, 2088};
        const auto start = StableCanvas(startHost, desktop, 1190, 1460, true);
        check(SameRect(start, {1062, 0, 2778, 2160}),
            "wide Start hosts use the final layout column rather than their animated translation");
        check(SameRect(StableCanvas(startHost, desktop, 0, 0, false), desktop),
            "an unavailable layout offset safely prepares the native host viewport");
        const PixelRect negativeDesktop{-1920, -200, 1920, 1960};
        check(SameRect(StableCanvas({-1920, -200, -1400, 1900}, negativeDesktop, -1920, 520, true),
            {-1920, -200, -1336, 1960}), "world canvas clamps correctly on negative desktop coordinates");
        // Only the on-screen portion of an opening panel needs canvas pixels.
        const PixelRect opening{3300, 2100, 3840, 2700};
        check(notification.Left <= opening.Left && notification.Right >= opening.Right
            && notification.Top <= opening.Top && notification.Bottom == desktop.Bottom,
            "partially offscreen vertical animation retains every visible desktop sample");
        check(CanPrepareGeometry(false, 400, 3, true) && !CanPrepareGeometry(false, 0, 3, true)
            && !CanPrepareGeometry(false, 400, 3, false),
            "a measured unloaded panel may prepare pixels while an unmeasured surface waits");
        const auto matrix = CanvasBrushMatrix({100, 0, 1100, 2160}, {250, 300, 650, 900});
        const double localX = (550.0 - 250) / 400, localY = (600.0 - 300) / 600;
        const double imageX = (localX - matrix.OffsetX) / matrix.M11;
        const double imageY = (localY - matrix.OffsetY) / matrix.M22;
        check(std::abs(imageX - .45) < .000001 && std::abs(imageY - 600.0 / 2160) < .000001,
            "the brush inverse maps a visible point to its actual world canvas sample");
        const auto movedMatrix = CanvasBrushMatrix({100, 0, 1100, 2160}, {250, 600, 650, 1200});
        check(std::abs(((600.0 - 600) / 600 - movedMatrix.OffsetY) / movedMatrix.M22 - imageY) < .000001,
            "moving the pane changes its crop without stretching the captured world");
        for (const auto material : {PixelRect{250, 300, 650, 900}, PixelRect{250, 600, 550, 1050},
                                   PixelRect{-150, -300, 450, 600}})
        {
            const PixelRect canvas{100, 0, 1100, 2160};
            // XAML first stretches the image into the measured DIP size,
            // then applies Brush.Transform in that local coordinate space.
            const double dipWidth = 400, dipHeight = 600;
            const double materialWidth = material.Right - material.Left, materialHeight = material.Bottom - material.Top;
            const double worldX = 350, worldY = 700;
            const double localDipX = (worldX - material.Left) * dipWidth / materialWidth;
            const double localDipY = (worldY - material.Top) * dipHeight / materialHeight;
            const auto absolute = CanvasBrushAbsoluteMatrix(canvas, material, dipWidth, dipHeight);
            const double sourceX = (localDipX - absolute.OffsetX) / absolute.M11 / dipWidth;
            const double sourceY = (localDipY - absolute.OffsetY) / absolute.M22 / dipHeight;
            check(std::abs(sourceX - (worldX - canvas.Left) / (canvas.Right - canvas.Left)) < .000001
                && std::abs(sourceY - (worldY - canvas.Top) / (canvas.Bottom - canvas.Top)) < .000001,
                "an absolute DIP brush transform samples the same world point through DPI, scale, and offscreen motion");
            const auto relative = CanvasBrushMatrix(canvas, material);
            check(std::abs(absolute.M11 - relative.M11) < .000001 && std::abs(absolute.M22 - relative.M22) < .000001
                && std::abs(absolute.OffsetX - relative.OffsetX * dipWidth) < .000001
                && std::abs(absolute.OffsetY - relative.OffsetY * dipHeight) < .000001,
                "the absolute matrix matches XAML's documented conversion from relative brush coordinates");
        }
        capture::Request first;
        first.CoordinateHwnd = 10; first.TopLevelHwnd = 20; first.Scale = 1.5;
        first.Bounds = {3300, 1500, 3840, 1503}; first.Radius = 18;
        check(capture::detail::SetCanvasBounds(first, notification), "notification canvas fits the capture protocol");
        auto opened = first; opened.Bounds = {3300, 900, 3840, 2100}; opened.Radius = 16;
        capture::detail::SetCanvasBounds(opened, notification);
        SurfaceLayout hiddenLayout{30, 360, 2, 1.5, 12}, openLayout{30, 360, 800, 1.5, 10};
        check(SameSurfaceEpoch(first, opened, hiddenLayout, openLayout),
            "a hidden tiny panel and its opened height and corners share one world canvas epoch");
        auto changed = opened; capture::detail::SetCanvasBounds(changed, {3200, 0, 3840, 2160});
        check(!SameSurfaceEpoch(first, changed, hiddenLayout, openLayout), "a changed canvas rejects cached world pixels");
        changed = opened; changed.CoordinateHwnd = 11;
        check(!SameSurfaceEpoch(first, changed, hiddenLayout, openLayout), "a replacement canvas owner rejects cached pixels");
        auto changedLayout = openLayout; changedLayout.Raster = 2;
        check(!SameSurfaceEpoch(first, opened, hiddenLayout, changedLayout), "a canvas DPI change rejects cached pixels");
        changed = opened; changed.Material.Blur += 1;
        check(!SameSurfaceEpoch(first, changed, hiddenLayout, openLayout), "a canvas material change rejects cached pixels");
        PendingCanvasField pending;
        pending.Observe(true, false, 5000);
        pending.Observe(true, false, 5100);
        check(pending.Tick == 5000 && CanRetainCanvasPresentation(true, true, false, 5100, 1000, pending.Tick, 5100),
            "a fresh hidden world canvas is eligible during the first visible field request despite its old material pose");
        check(CanRetainCanvasPresentation(true, true, false, 5201, 1000, pending.Tick, 5201),
            "an old material field does not expire fresh complete world pixels during ordinary opening motion");
        check(CanRetainCanvasPresentation(true, true, true, 1000, 1000, 0, 1201)
            && CanRetainCanvasPresentation(true, true, true, 1000, 1000, 0, 1218),
            "a ready world canvas survives the observed 201 and 218 millisecond producer cadence");
        check(CanRetainCanvasPresentation(true, true, false, 1000, 10, 10, 2500)
            && !CanRetainCanvasPresentation(true, true, false, 1000, 10, 10, 2501),
            "world canvas source freshness follows the protocol 1500 millisecond inclusive boundary");
        check(!CanRetainCanvasPresentation(true, true, true, 0, 1000, 0, 1000)
            && !CanRetainCanvasPresentation(true, true, true, 1001, 1000, 0, 1000)
            && !CanRetainCanvasPresentation(false, true, true, 1000, 1000, 0, 1000),
            "missing, future, and absent canvas publications remain unavailable");
        check(CanRetainCanvasPresentation(true, false, false, 1000, 10, 10, 10000),
            "hidden canvas bitmap retention remains independent from visible source expiry");
        pending.Observe(true, true, 5210);
        check(!pending.Tick && CanRetainCanvasPresentation(true, true, true, 5220, 1000, pending.Tick, 5220),
            "an actual matching material field clears its pending deadline");
        pending.Observe(false, false, 6000);
        check(!pending.Tick, "a hidden canvas starts a new bounded field interval on its next opening");
    }
    {
        // Drive the actual reader/writer without a window or capture source.
        // This also verifies that discarding a bitmap resets the reader's
        // serial, so a static producer can rehydrate the same serial later.
        using namespace novabar;
        using namespace novabar::capture;
        const auto fixture = std::filesystem::temp_directory_path()
            / (L"novabar-client-retention-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(fixture);
        Request request;
        request.Pid = GetCurrentProcessId(); request.TargetId = 77; request.Revision = 1;
        request.CoordinateHwnd = request.TopLevelHwnd = 1;
        request.Bounds = {0, 0, 4, 4}; request.MotionSequence = 1; request.HeartbeatTick = GetTickCount64();
        request.Flags = KeepWarm;
        check(detail::SetCanvasBounds(request, request.Bounds) && WriteRequest(fixture.wstring(), request),
            "prepare the world canvas lifecycle fixture");
        detail::FrameWriter writer;
        check(writer.Ensure(fixture.wstring(), request.Pid, request.TargetId, 64), "create the world canvas frame mapping");
        const std::vector<uint8_t> pixels(64, 127);
        check(writer.Publish(1, Status::Ready, S_OK, 4, 4, pixels, true, request.Bounds, 1, request.Bounds),
            "publish the lifecycle fixture pixels");
        FrameReader reader;
        Frame frame;
        check(reader.ReadLatest(fixture.wstring(), request.Pid, request.TargetId, 1, frame), "read initial world canvas pixels");
        const auto serial = frame.Serial;
        check(writer.Publish(1, Status::Ready, S_OK, 4, 4, pixels, false, request.Bounds, 1, request.Bounds),
            "renew a static world canvas without changing its serial");
        check(!reader.ReadLatest(fixture.wstring(), request.Pid, request.TargetId, 1, frame)
            && frame.Status == Status::Ready && frame.Serial == serial && frame.Pixels.empty(),
            "a static heartbeat updates metadata without uploading duplicate pixels");
        reader.Close();
        check(reader.ReadLatest(fixture.wstring(), request.Pid, request.TargetId, 1, frame)
            && frame.Serial == serial && frame.Pixels == pixels,
            "closing the reader with a discarded bitmap restores unchanged producer pixels");
        for (const auto failure : {Status::Unavailable, Status::InvalidRequest})
        {
            check(writer.Publish(1, failure, E_FAIL, 0, 0, {}), "publish a genuine producer failure");
            check(!reader.ReadLatest(fixture.wstring(), request.Pid, request.TargetId, 1, frame)
                && frame.Status == failure && frame.Pixels.empty(),
                "a genuine source failure reaches the client immediately despite its fresh timestamp");
        }
        reader.Close(); writer.Close(); CancelRequest(fixture.wstring(), request.Pid, request.TargetId);
        std::filesystem::remove(fixture);
    }
    {
        HRESULT error = S_OK;
        try { throw winrt::hresult_error(E_ACCESSDENIED); }
        catch (...) { error = novabar::client_detail::DiagnosticHresult(); }
        check(error == E_ACCESSDENIED, "lifecycle diagnostics preserve the caught WinRT HRESULT");
        // Fail cleanly without invoking Windows crash UI if the diagnostic
        // converter accidentally terminates on a nonstandard exception.
        const auto previousTerminate = std::set_terminate([]
        {
            fputs("FAIL: lifecycle diagnostics must preserve catch-all protection for unknown exceptions\n", stdout);
            fflush(stdout); ExitProcess(1);
        });
        try { throw 7; }
        catch (...) { error = novabar::client_detail::DiagnosticHresult(); }
        std::set_terminate(previousTerminate);
        check(error == E_FAIL, "unknown caught exceptions become diagnostic E_FAIL without terminating capture cleanup");
    }
    {
        // Regressions: animated bounds used to reset the surface epoch and
        // hidden panels cancelled their real-scene source on every close.
        novabar::capture::Request first;
        first.CoordinateHwnd = 10; first.TopLevelHwnd = 20;
        first.Bounds = {100, 200, 500, 800}; first.Radius = 18;
        novabar::client_detail::SurfaceLayout layout{30, 400, 600, 1.5, 12};
        auto moved = first; moved.Bounds = {120, 230, 500, 800}; moved.Scale = 1.425; moved.Radius = 17.1;
        check(novabar::client_detail::SameSurfaceEpoch(first, moved, layout, layout),
            "opening translation and visual scale preserve the real-glass surface epoch");
        auto changedLayout = layout; changedLayout.Root = 31;
        check(!novabar::client_detail::SameSurfaceEpoch(first, first, layout, changedLayout),
            "a different XAML root discards the previous surface presentation");
        changedLayout = layout; changedLayout.Width = 401;
        check(!novabar::client_detail::SameSurfaceEpoch(first, first, layout, changedLayout),
            "a true layout resize cannot reuse the previous surface epoch");
        changedLayout = layout; changedLayout.Raster = 2;
        check(!novabar::client_detail::SameSurfaceEpoch(first, first, layout, changedLayout),
            "a DPI change cannot reuse the previous surface epoch");
        auto restyled = first; restyled.Material.Lens = .9;
        check(!novabar::client_detail::SameSurfaceEpoch(first, restyled, layout, layout),
            "a changed liquid material cannot reuse the previous surface epoch");
        auto replacedOwner = first; replacedOwner.CoordinateHwnd = 11;
        check(!novabar::client_detail::SameSurfaceEpoch(first, replacedOwner, layout, layout),
            "a replacement native island cannot reuse the previous surface epoch");
        replacedOwner = first; replacedOwner.TopLevelHwnd = 21;
        check(!novabar::client_detail::SameSurfaceEpoch(first, replacedOwner, layout, layout),
            "a replacement top-level owner cannot reuse the previous surface epoch");
        using Activity = novabar::client_detail::SurfaceActivity;
        check(novabar::client_detail::Activity(true, true, false) == Activity::HiddenWarm,
            "closing a known panel keeps its real-scene source warm");
        check(novabar::client_detail::Activity(true, false, false) == Activity::Waiting,
            "an unmeasured panel waits without pretending to have capture geometry");
        check(novabar::client_detail::Activity(true, true, false, true) == Activity::Stop,
            "stopping a hidden panel cancels its capture lifetime");
        check(novabar::client_detail::CanReuseHiddenGeometry(false, 400, 600, true),
            "an unloaded panel may warm its remembered geometry without presenting it");
        check(novabar::client_detail::CanReuseHiddenGeometry(true, 0, 600, true)
            && novabar::client_detail::CanReuseHiddenGeometry(true, 400, 0, true),
            "a temporarily unmeasured panel may keep its known real-scene source warm");
        check(novabar::client_detail::CanReuseHiddenGeometry(true, 400, 600, false),
            "an explicitly hidden XAML host may retain its known capture geometry");
        check(!novabar::client_detail::CanReuseHiddenGeometry(true, 400, 600, true),
            "a visible measured panel with a failed mapping cannot silently retain an old bitmap forever");
        check(novabar::client_detail::CanRetainPresentation(true, true, false, 1000, 900, 1100),
            "a recent completed motion remains glass while the next real-scene result is in flight");
        check(!novabar::client_detail::CanRetainPresentation(true, true, false, 1100, 899, 1100),
            "a newly published but over-200ms-old motion cannot prolong the previous position");
        check(!novabar::client_detail::CanRetainPresentation(true, true, true, 899, 899, 1100),
            "a stalled source cannot leave a visible real-glass bitmap indefinitely");
        check(novabar::client_detail::CanRetainPresentation(true, false, false, 1000, 900, 6000),
            "a hidden panel keeps its bitmap without presenting expired pixels on screen");
        novabar::client_detail::PublicationGeneration publication;
        using Result = novabar::client_detail::NotificationResult;
        check(novabar::client_detail::NotifyCurrent(publication, {}, nullptr, false) == Result::Accepted,
            "pending brush bootstrap without a callback remains in the current lifecycle");
        check(novabar::client_detail::NotifyCurrent(publication, [&](auto const&, bool) { publication.Invalidate(); }, nullptr, false)
            == Result::Superseded, "a synchronous restyle cancels pending brush continuation");
        check(novabar::client_detail::NotifyCurrent(publication, [&](auto const&, bool) { publication.Invalidate(); throw 1; }, nullptr, false)
            == Result::Superseded, "callback invalidation takes priority over failure cleanup of a new lifecycle");
        check(novabar::client_detail::NotifyCurrent(publication, [](auto const&, bool) { throw 1; }, nullptr, false) == Result::Failed,
            "callback failure without a lifecycle transition requests normal cleanup");
        const auto beforeStop = publication.Capture();
        publication.Invalidate();
        check(!publication.Current(beforeStop),
            "a synchronous Stop callback cannot let its outer tick recreate the cancelled request");
        const auto beforeRestyle = publication.Capture();
        publication.Invalidate();
        check(!publication.Current(beforeRestyle),
            "a synchronous appearance change prevents an older tick from publishing obsolete work");
    }
    {
        // Exercise the production registration lease with ordinary WinRT
        // identities; no shell injection or XAML window is needed.
        struct NativeShapeFixture : winrt::implements<NativeShapeFixture, winrt::Windows::Foundation::IStringable>
        { winrt::hstring ToString() { return L"native taskbar shape fixture"; } };
        auto native = winrt::make_self<NativeShapeFixture>();
        winrt::Windows::Foundation::IInspectable shape = native.as<winrt::Windows::Foundation::IStringable>();
        TaskbarShapeOwnership firstSite, anotherSite;
        check(firstSite.Claim(101, shape), "first native taskbar registration owns its shape");
        check(!anotherSite.Claim(202, shape), "a second Explorer diagnostics site cannot register the same taskbar shape");
        check(firstSite.Claim(101, shape), "repeated taskbar adds reuse the existing owner's lease");
        anotherSite.Remove(202);
        check(!anotherSite.Claim(202, shape), "removing a non-owned diagnostics handle cannot release another site's taskbar");
        firstSite.Remove(101);
        check(anotherSite.Claim(202, shape), "taskbar removal releases the lease for a later owner");
        anotherSite.Remove(202);
        {
            TaskbarShapeOwnership temporarySite;
            check(temporarySite.Claim(303, shape), "a temporary native watcher can acquire the taskbar lease");
        }
        check(firstSite.Claim(101, shape), "destroying a native watcher releases its process-wide shape lease");
        const auto weak = winrt::make_weak(shape);
        shape = nullptr; native = nullptr;
        check(!weak.get(), "native registration leases never keep a XAML-equivalent object alive");
        auto secondNative = winrt::make_self<NativeShapeFixture>();
        auto thirdNative = winrt::make_self<NativeShapeFixture>();
        auto secondShape = secondNative.as<winrt::Windows::Foundation::IStringable>();
        auto thirdShape = thirdNative.as<winrt::Windows::Foundation::IStringable>();
        check(firstSite.Claim(404, secondShape) && anotherSite.Claim(505, thirdShape),
            "different taskbar shapes can have independent native owners");
    }
    {
        struct Owner { bool Alive = true; };
        novabar::surface_detail::WeakOwners<Owner> owners;
        const auto alive = [](auto const& owner, DWORD) { return owner->Alive; };
        int identity = 0;
        auto first = std::make_shared<Owner>(), duplicate = std::make_shared<Owner>();
        check(owners.Claim(10, &identity, first, alive), "first diagnostics watcher owns the surface");
        check(!owners.Claim(10, &identity, duplicate, alive), "another watcher cannot install a second client on the same UI surface");
        check(owners.Claim(11, &identity, duplicate, alive), "surface ownership is isolated by UI thread");
        first->Alive = false;
        check(owners.Claim(10, &identity, duplicate, alive), "a stopped watcher releases its weak surface ownership");
        auto transient = std::make_shared<Owner>();
        std::weak_ptr<Owner> weak = transient;
        check(owners.Claim(12, &identity, transient, alive), "temporary surface owner can register");
        transient.reset();
        check(weak.expired() && owners.Claim(12, &identity, duplicate, alive), "registry does not retain a dead watcher and permits replacement");
    }
    {
        bool applying = false;
        {
            novabar::surface_detail::ApplyingGuard outer(applying);
            { novabar::surface_detail::ApplyingGuard inner(applying); }
            check(applying, "nested frame callbacks keep the outer property enforcement guard active");
        }
        check(!applying, "property enforcement guard restores its original state");
        check(novabar::surface_detail::IsControlCenterTintOverlay(L"", L"L1Grid", true, true), "known Quick Settings tint overlay is selected");
        check(!novabar::surface_detail::IsControlCenterTintOverlay(L"ButtonBorder", L"L1Grid", true, true)
            && !novabar::surface_detail::IsControlCenterTintOverlay(L"", L"ButtonsGrid", true, true)
            && !novabar::surface_detail::IsControlCenterTintOverlay(L"", L"L1Grid", false, true)
            && !novabar::surface_detail::IsControlCenterTintOverlay(L"", L"L1Grid", true, false),
            "button backgrounds and unrelated unnamed borders are excluded from tint overlay styling");
        check(novabar::surface_detail::IsControlCenterBackdrop(L"ControlCenterRegion", true),
            "native Quick Settings Acrylic ancestor is selected for liquid-only clearing");
        check(!novabar::surface_detail::IsControlCenterBackdrop(L"ControlCenterRegion", false)
            && !novabar::surface_detail::IsControlCenterBackdrop(L"RootContent", true)
            && !novabar::surface_detail::IsControlCenterBackdrop(L"MediaTransportControlsRegion", true),
            "native backdrop clearing excludes unrelated grids and other regions");
        using Action = novabar::surface_detail::BackgroundAction;
        using Accent = novabar::ShellAccent;
        const auto primaryRegion = [](Accent accent)
        { return novabar::surface_detail::SelectBackground(accent, false, false, true, false); };
        const auto movingRoot = [](Accent accent)
        { return novabar::surface_detail::SelectBackground(accent, false, false, false, true); };
        check(primaryRegion(Accent::LiquidGlass) == Action::Liquid,
            "stable ControlCenterRegion owns the only liquid backdrop client");
        check(movingRoot(Accent::LiquidGlass) == Action::Clear,
            "navigation-animated RootGridBorder stays transparent under liquid mode");
        check(primaryRegion(Accent::Normal) == Action::Restore && movingRoot(Accent::Normal) == Action::Restore,
            "normal restores both outer native and inner custom background locals");
        check(primaryRegion(Accent::Color) == Action::Restore && movingRoot(Accent::Color) == Action::Material,
            "explicit color keeps its existing inner material and restores native outer background");
        check(primaryRegion(Accent::Acrylic) == Action::Restore && movingRoot(Accent::Acrylic) == Action::Material,
            "explicit acrylic keeps its existing inner material and restores native outer background");
        check(primaryRegion(Accent::Clear) == Action::Restore && movingRoot(Accent::Clear) == Action::Material,
            "explicit clear preserves its previous inner material behavior");
        check(novabar::surface_detail::SelectBackground(Accent::LiquidGlass, true, true, false, false) == Action::Clear
            && novabar::surface_detail::SelectBackground(Accent::Acrylic, true, true, false, false) == Action::Restore,
            "the separate L1 tint overlay stays liquid-only without another capture client");
        struct IdentityFixture : winrt::implements<IdentityFixture,
            winrt::Windows::Foundation::IStringable, winrt::Windows::Foundation::IClosable>
        {
            winrt::hstring ToString() { return L"surface identity fixture"; }
            void Close() {}
        };
        auto identity = winrt::make_self<IdentityFixture>();
        winrt::Windows::Foundation::IInspectable first = identity.as<winrt::Windows::Foundation::IStringable>();
        winrt::Windows::Foundation::IInspectable second = identity.as<winrt::Windows::Foundation::IClosable>();
        check(winrt::get_abi(first) != winrt::get_abi(second)
            && novabar::surface_detail::CanonicalIdentity(first) == novabar::surface_detail::CanonicalIdentity(second),
            "different interface pointers resolve to one canonical COM surface identity");
    }
    {
        novabar::ShellSurfaceStyle style;
        style.Accent = novabar::ShellAccent::LiquidGlass;
        style.ShowLine = true;
        check(!novabar::UsesSystemShellBorder(style), "liquid owns its edge even when imported taskbar line setting is enabled");
        style.ShowLine = false;
        check(!novabar::UsesSystemShellBorder(style), "liquid has no second system outline when line is disabled");
        style.Accent = novabar::ShellAccent::Normal;
        check(novabar::UsesSystemShellBorder(style), "normal restores the system border regardless of line setting");
        style.Accent = novabar::ShellAccent::Acrylic;
        style.ShowLine = true;
        check(novabar::UsesSystemShellBorder(style), "explicit acrylic preserves enabled system outline");
        style.ShowLine = false;
        check(!novabar::UsesSystemShellBorder(style), "explicit acrylic preserves disabled outline behavior");
    }
    bool callbackEscaped = false;
    try
    {
        std::function<void(novabar::client_detail::Brush const&, bool)> throwing = [](auto const&, bool) { throw std::runtime_error("detached XAML target"); };
        novabar::client_detail::Notify(throwing, nullptr, false);
    }
    catch (...) { callbackEscaped = true; }
    check(!callbackEscaped, "consumer callback exceptions cannot escape into shell recovery");
    {
        VisualElement element{};
        check(!IsTaskbarRootAdd(element, Add), "missing element type does not claim a thread");
        for (const auto type : {L"Microsoft.UI.Xaml.Controls.Grid", L"Windows.UI.Xaml.Controls.Grid",
                                L"Windows.UI.Xaml.Hosting.DesktopWindowXamlSource", L"Taskbar.TaskbarBackground"})
        {
            element.Type = const_cast<wchar_t*>(type);
            check(!IsTaskbarRootAdd(element, Add), "only a positive taskbar root claims an unknown thread");
        }
        element.Type = const_cast<wchar_t*>(L"Taskbar.TaskbarFrame");
        check(IsTaskbarRootAdd(element, Add), "taskbar root add claims its UI thread");
        check(!IsTaskbarRootAdd(element, Remove), "taskbar root removal does not create a watcher");
    }
    {
        // An unrelated Explorer island must not start its own bridge poller.
        // Keep the callback thread alive until counting so its one thread is
        // accounted for separately from an accidentally created watcher.
        const auto threadCount = []
        {
            unsigned count = 0;
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 entry{sizeof(entry)};
            if (snapshot != INVALID_HANDLE_VALUE)
            {
                if (Thread32First(snapshot, &entry)) do
                {
                    if (entry.th32OwnerProcessID == GetCurrentProcessId()) ++count;
                } while (Thread32Next(snapshot, &entry));
                CloseHandle(snapshot);
            }
            return count;
        };
        const auto originalStatePath = g_statePath;
        const bool originalTaskbarHost = g_taskbarHost;
        g_statePath = L"Z:\\novabar-nonexistent-test-state.txt";
        g_taskbarHost = true;
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        {
            const auto watcher = winrt::make_self<Watcher>(winrt::com_ptr<IXamlDiagnostics>{});
            const unsigned before = threadCount();
            std::promise<HRESULT> completed;
            std::promise<void> release;
            auto released = release.get_future();
            std::thread callback([&]
            {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                VisualElement element{};
                element.Type = const_cast<wchar_t*>(L"Microsoft.UI.Xaml.Controls.Grid");
                completed.set_value(watcher->OnVisualTreeChange({}, element, Add));
                released.wait();
                winrt::uninit_apartment();
            });
            check(completed.get_future().get() == S_OK, "unrelated Explorer callback succeeds");
            check(threadCount() == before + 1, "unrelated Explorer island does not create a watcher poller");
            release.set_value();
            callback.join();
        }
        winrt::uninit_apartment();
        g_statePath = originalStatePath;
        g_taskbarHost = originalTaskbarHost;
    }
    check(ParseSpec(L"lens=nan;dispersion=inf;noise=-5;blur=-2").Lens == .55, "nonfinite lens rejected");
    check(ParseSpec(L"lens=2;dispersion=0;noise=0").Lens == 1, "lens bounded");
    check(ParseSpec(L"dispersion=.7;noise=.4").Dispersion == .7, "dispersion parsed");
    check(ParseSpec(L"accent=blur").Accent == Accent::Blur, "blur distinct from acrylic");
    check(SheenStrength(ParseSpec(L"accent=clear;specular=1")) == 0, "clear has no glass sheen");
    check(SheenStrength(ParseSpec(L"accent=normal;specular=1")) == 0, "normal restores sheen");
    check(SheenStrength(ParseSpec(L"accent=liquid;specular=.6")) == .6, "glass retains chosen sheen");
    wchar_t temporary[MAX_PATH]{}; GetTempPathW(ARRAYSIZE(temporary), temporary);
    const auto testPath = std::wstring(temporary) + L"novabar-bridge-test-" + std::to_wstring(GetCurrentProcessId()) + L".txt";
    FILE* testFile = nullptr; _wfopen_s(&testFile, testPath.c_str(), L"w, ccs=UTF-8");
    if (testFile) { fputws(L"accent=color;color=#FF123456;lens=.2", testFile); fclose(testFile); }
    HANDLE exclusive = CreateFileW(testPath.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    Spec read;
    check(!TryReadSpecFile(testPath, read), "sharing failure remains retryable");
    if (exclusive != INVALID_HANDLE_VALUE) CloseHandle(exclusive);
    check(TryReadSpecFile(testPath, read) && read.Color == 0xFF123456, "same file succeeds after temporary sharing failure");
    DeleteFileW(testPath.c_str());
    novabar::PixelFrame source;
    source.Width = 80; source.Height = 100; source.Bounds = {-80, -100, 0, 0};
    source.Pixels.resize(80 * 100 * 4);
    for (int y = 0; y < 100; ++y) for (int x = 0; x < 80; ++x)
    {
        const auto at = (y * 80 + x) * 4;
        source.Pixels[at] = source.Pixels[at + 1] = source.Pixels[at + 2] = static_cast<uint8_t>(y * 2);
        source.Pixels[at + 3] = 255;
    }
    novabar::GlassOptions options; options.Lens = options.Dispersion = options.Noise = options.Blur = options.Tint = 0;
    std::vector<uint8_t> plain, lensed, dispersed, blurred;
    check(novabar::RenderGlass(source, {-80, -40, 0, 0}, 80, 40, options, plain), "render negative monitor coordinates");
    check(plain[0] == 120 && plain[80 * 39 * 4] == 198, "sample the region behind taskbar");
    options.Lens = 1;
    novabar::RenderGlass(source, {-80, -40, 0, 0}, 80, 40, options, lensed);
    check(lensed[0] == 28 && lensed != plain, "refraction controls displacement");
    options.Dispersion = 1;
    novabar::RenderGlass(source, {-80, -40, 0, 0}, 80, 40, options, dispersed);
    check(dispersed[0] != dispersed[2], "dispersion separates RGB channels");
    options.Lens = 0; options.Blur = 40;
    novabar::RenderGlass(source, {-80, -40, 0, 0}, 80, 40, options, blurred);
    check(blurred[0] > plain[0], "blur radius changes edge samples");
    novabar::GlassFrameRenderer cached;
    std::vector<uint8_t> cachedPixels;
    bool changed = false;
    const novabar::PixelRect cachedTarget{-80, -40, 0, 0};
    check(cached.Render(source, cachedTarget, options, 1, true, cachedPixels, changed) && changed,
          "first static frame renders");
    cachedPixels = {11, 22, 33};
    check(cached.Render(source, cachedTarget, options, 1, false, cachedPixels, changed) && !changed
          && cachedPixels == std::vector<uint8_t>({11, 22, 33}), "unchanged ready frame preserves output without rendering");
    check(cached.Render(source, cachedTarget, options, 2, false, cachedPixels, changed) && changed,
          "appearance version forces a new frame even with identical options");
    options.Noise = .25;
    check(cached.Render(source, cachedTarget, options, 2, false, cachedPixels, changed) && changed,
          "changed rendering options invalidate cached pixels");
    check(cached.Render(source, {-79, -40, 1, 0}, options, 2, false, cachedPixels, changed) && changed,
          "changed desktop target invalidates cached pixels");
    check(cached.Render(source, cachedTarget, options, 2, true, cachedPixels, changed) && changed,
          "new captured or refreshed static source invalidates cached pixels");
    check(cached.Invalidate() && !cached.Invalidate(), "obscuration invalidates a cached result once");
    check(cached.Render(source, cachedTarget, options, 2, false, cachedPixels, changed) && changed,
          "returning from obscuration restores a paused frame");
    const novabar::PixelFrame missing;
    check(!cached.Render(missing, cachedTarget, options, 2, true, cachedPixels, changed) && changed,
          "source loss transitions to material fallback");
    check(!cached.Render(missing, cachedTarget, options, 2, false, cachedPixels, changed) && !changed,
          "unchanged missing frame does not queue repeated fallback work");
    source.Pixels.pop_back();
    check(!novabar::RenderGlass(source, {-80, -40, 0, 0}, 80, 40, options, plain), "reject truncated frame buffer");
    if (argc > 1)
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        RECT taskbar{}; GetWindowRect(FindWindowW(L"Shell_TrayWnd", nullptr), &taskbar);
        const novabar::PixelRect target{taskbar.left, taskbar.top - 64, taskbar.right, taskbar.bottom};
        const HWND wallpaper = novabar::FindWallpaperWindow(target);
        printf("Wallpaper HWND: %p\n", wallpaper);
        if (!wallpaper) { puts("SKIP: Wallpaper Engine window is absent"); return failed ? 1 : 0; }
        try
        {
            novabar::WindowCapture capture; capture.Start(wallpaper);
            int frames = 0, changes = 0; uint64_t lastHash = 0;
            const auto until = GetTickCount64() + 2500;
            while (GetTickCount64() < until)
            {
                novabar::PixelFrame pixels;
                if (capture.Read(target, pixels, true))
                {
                    uint64_t hash = 1469598103934665603ull;
                    for (const auto pixel : pixels.Pixels) hash = (hash ^ pixel) * 1099511628211ull;
                    if (lastHash && hash != lastHash) ++changes;
                    lastHash = hash; ++frames;
                }
                Sleep(30);
            }
            printf("WGC: frames=%d changed=%d\n", frames, changes);
            // WGC sends a single frame when the desktop is static or the
            // wallpaper app is paused. Animation is covered by owned fixtures.
            check(frames > 0, "WGC returns a genuine wallpaper window frame");
            novabar::PixelFrame retained;
            auto recrop = target; --recrop.Right;
            check(capture.Read(recrop, retained, true) && retained.Width > 0 && retained.Height > 0
                  && retained.Pixels.size() == static_cast<size_t>(retained.Width) * retained.Height * 4,
                  "static wallpaper can be recropped through retained capture pixels");
        }
        catch (const winrt::hresult_error& e) { printf("FAIL: capture HRESULT %08X\n", static_cast<unsigned>(e.code())); ++failed; }
    }
    if (!failed) puts("PASS: bridge parsing, rendering, capture checks");
    return failed ? 1 : 0;
}
