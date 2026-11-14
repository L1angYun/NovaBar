#pragma once
#include "WgcCapture.h"
#include "WallpaperSource.h"

namespace novabar
{
    class GlassFrameRenderer
    {
        PixelRect m_target;
        GlassOptions m_options;
        unsigned m_version = 0;
        bool m_valid = false, m_ready = false;
    public:
        bool Invalidate() { const bool valid = m_valid; m_valid = false; return valid; }
        bool Render(const PixelFrame& frame, PixelRect target, const GlassOptions& options,
                    unsigned version, bool sourceChanged, std::vector<uint8_t>& out, bool& changed)
        {
            const bool sameTarget = target.Left == m_target.Left && target.Top == m_target.Top
                && target.Right == m_target.Right && target.Bottom == m_target.Bottom;
            const bool sameOptions = options.Lens == m_options.Lens && options.Dispersion == m_options.Dispersion
                && options.Noise == m_options.Noise && options.Tint == m_options.Tint
                && options.Blur == m_options.Blur && options.Color == m_options.Color;
            if (m_valid && !sourceChanged && version == m_version && sameTarget && sameOptions)
            {
                changed = false;
                return m_ready;
            }
            changed = true;
            m_valid = false;
            m_ready = RenderGlass(frame, target, target.Right - target.Left, target.Bottom - target.Top, options, out);
            m_target = target; m_options = options; m_version = version; m_valid = true;
            return m_ready;
        }
    };

    class GlassPipeline
    {
        WindowCapture m_capture;
        WallpaperSource m_wallpaper;
        PixelFrame m_frame;
        GlassFrameRenderer m_renderer;
        PixelRect m_target;
        bool m_haveTarget = false, m_haveStaticRead = false;
        uint64_t m_lastSourceCheck = 0, m_lastStaticRead = 0, m_retryAfter = 0;
        HWND m_source = nullptr;
    public:
        bool Live() const { return m_capture.Window() != nullptr; }
        bool Invalidate() { return m_renderer.Invalidate(); }
        bool Render(PixelRect target, const GlassOptions& options, unsigned version, std::vector<uint8_t>& out, bool& changed)
        {
            const auto now = GetTickCount64();
            bool frameChanged = false;
            const bool targetChanged = !m_haveTarget || target.Left != m_target.Left || target.Top != m_target.Top
                || target.Right != m_target.Right || target.Bottom != m_target.Bottom;
            m_target = target; m_haveTarget = true;
            if (targetChanged || now >= m_lastSourceCheck + 2000)
            {
                m_lastSourceCheck = now;
                const HWND source = FindWallpaperWindow(target);
                if (source != m_source)
                {
                    m_capture.Close(); m_source = source; m_frame = {}; m_retryAfter = 0;
                    m_haveStaticRead = false; frameChanged = true;
                }
            }
            if (targetChanged && m_capture.Window()) { m_frame = {}; frameChanged = true; }
            if (m_source && !m_capture.Window() && now >= m_retryAfter)
            {
                try { m_capture.Start(m_source); }
                catch (...) { m_capture.Close(); m_retryAfter = now + 10000; }
            }
            if (m_capture.Window())
            {
                try
                {
                    // Lens displacement only needs at most 58 pixels of padding.
                    PixelRect padded{target.Left, target.Top - 64, target.Right, target.Bottom + 2};
                    PixelFrame frame;
                    if (m_capture.Read(padded, frame)) { m_frame = std::move(frame); frameChanged = true; }
                }
                catch (...) { m_capture.Close(); m_frame = {}; m_retryAfter = now + 10000; m_haveStaticRead = false; frameChanged = true; }
            }
            if (!m_capture.Window() && (!m_haveStaticRead || targetChanged || now >= m_lastStaticRead + 2000))
            {
                m_lastStaticRead = now; m_haveStaticRead = true; frameChanged = true;
                try { if (!m_wallpaper.Read(target, m_frame)) m_frame = {}; }
                catch (...) { m_frame = {}; }
            }
            return m_renderer.Render(m_frame, target, options, version, frameChanged, out, changed);
        }
    };
}
