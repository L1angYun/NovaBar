#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef GetCurrentTime
#include <shobjidl.h>
#include <wincodec.h>
#include <winrt/base.h>
#include <map>
#include <string>
#include "GlassPixels.h"

namespace novabar
{
    class WallpaperSource
    {
        struct Cached { PixelFrame Frame; uint64_t Stamp = 0; };
        std::map<std::wstring, Cached> m_cache;
    public:
        bool Read(PixelRect target, PixelFrame& out)
        {
            RECT rect{target.Left, target.Top, target.Right, target.Bottom};
            MONITORINFO info{sizeof(info)};
            if (!GetMonitorInfoW(MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST), &info)) return false;
            RECT monitor = info.rcMonitor;
            DESKTOP_WALLPAPER_POSITION position = DWPOS_FILL;
            COLORREF background = 0;
            std::wstring filename;
            winrt::com_ptr<IDesktopWallpaper> desktop;
            if (SUCCEEDED(CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(desktop.put()))))
            {
                desktop->GetPosition(&position); desktop->GetBackgroundColor(&background);
                UINT count = 0; desktop->GetMonitorDevicePathCount(&count);
                for (UINT i = 0; i < count; ++i)
                {
                    LPWSTR path = nullptr, paper = nullptr;
                    if (FAILED(desktop->GetMonitorDevicePathAt(i, &path))) continue;
                    RECT candidate{};
                    if (SUCCEEDED(desktop->GetMonitorRECT(path, &candidate)) && EqualRect(&candidate, &monitor)
                        && SUCCEEDED(desktop->GetWallpaper(path, &paper)))
                    {
                        if (paper) filename = paper;
                    }
                    CoTaskMemFree(paper); CoTaskMemFree(path);
                    if (!filename.empty()) break;
                }
            }
            if (filename.empty())
            {
                wchar_t path[32768]{};
                if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, ARRAYSIZE(path), path, 0) || !path[0]) return false;
                filename = path;
            }
            WIN32_FILE_ATTRIBUTE_DATA attr{};
            if (!GetFileAttributesExW(filename.c_str(), GetFileExInfoStandard, &attr)) return false;
            const uint64_t stamp = (static_cast<uint64_t>(attr.ftLastWriteTime.dwHighDateTime) << 32) | attr.ftLastWriteTime.dwLowDateTime;
            auto& cached = m_cache[filename];
            if (cached.Frame.Pixels.empty() || cached.Stamp != stamp)
            {
                winrt::com_ptr<IWICImagingFactory> factory;
                winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put())));
                winrt::com_ptr<IWICBitmapDecoder> decoder;
                winrt::check_hresult(factory->CreateDecoderFromFilename(filename.c_str(), nullptr, GENERIC_READ,
                    WICDecodeMetadataCacheOnLoad, decoder.put()));
                winrt::com_ptr<IWICBitmapFrameDecode> image;
                winrt::check_hresult(decoder->GetFrame(0, image.put()));
                winrt::com_ptr<IWICFormatConverter> converter;
                winrt::check_hresult(factory->CreateFormatConverter(converter.put()));
                winrt::check_hresult(converter->Initialize(image.get(), GUID_WICPixelFormat32bppBGRA,
                    WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
                UINT width = 0, height = 0; converter->GetSize(&width, &height);
                if (!width || !height || width > 32768 || height > 32768 || uint64_t(width) * height > 64 * 1024 * 1024) return false;
                PixelFrame decoded;
                decoded.Width = width; decoded.Height = height;
                decoded.Pixels.resize(static_cast<size_t>(width) * height * 4);
                winrt::check_hresult(converter->CopyPixels(nullptr, width * 4,
                    static_cast<UINT>(decoded.Pixels.size()), decoded.Pixels.data()));
                cached.Frame = std::move(decoded); cached.Stamp = stamp;
                // Keep slideshows bounded, retaining the image just decoded.
                if (m_cache.size() > 8)
                {
                    auto keep = std::move(cached); m_cache.clear(); m_cache.emplace(filename, std::move(keep));
                }
            }
            out = m_cache.at(filename).Frame;
            if (position == DWPOS_SPAN)
                monitor = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                    GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                    GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
            const double width = monitor.right - monitor.left, height = monitor.bottom - monitor.top;
            double scaleX = 1, scaleY = 1;
            if (position == DWPOS_STRETCH) { scaleX = width / out.Width; scaleY = height / out.Height; }
            else if (position == DWPOS_FILL || position == DWPOS_SPAN)
                scaleX = scaleY = std::max(width / out.Width, height / out.Height);
            else if (position == DWPOS_FIT)
                scaleX = scaleY = std::min(width / out.Width, height / out.Height);
            const int imageW = static_cast<int>(std::round(out.Width * scaleX));
            const int imageH = static_cast<int>(std::round(out.Height * scaleY));
            const int left = position == DWPOS_TILE ? monitor.left : monitor.left + static_cast<int>((width - imageW) / 2);
            const int top = position == DWPOS_TILE ? monitor.top : monitor.top + static_cast<int>((height - imageH) / 2);
            out.Bounds = {left, top, left + imageW, top + imageH};
            out.Tile = position == DWPOS_TILE;
            out.Background = 0xFF000000 | (GetRValue(background) << 16) | (GetGValue(background) << 8) | GetBValue(background);
            return true;
        }
    };
}
