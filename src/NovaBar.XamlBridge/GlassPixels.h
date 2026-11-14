#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace novabar
{
    struct PixelRect { int Left = 0, Top = 0, Right = 0, Bottom = 0; };
    struct PixelFrame
    {
        int Width = 0, Height = 0;
        PixelRect Bounds;
        std::vector<uint8_t> Pixels;
        bool Tile = false;
        uint32_t Background = 0xFF000000;
    };
    struct GlassOptions
    {
        double Lens = .55, Dispersion = .18, Noise = .12, Tint = .3, Blur = 0;
        uint32_t Color = 0;
    };

    inline void BoxBlur(std::vector<uint8_t>& pixels, int width, int height, int radius)
    {
        if (radius <= 0) return;
        std::vector<uint8_t> scratch(pixels.size());
        for (int pass = 0; pass != 2; ++pass)
        {
            const int rows = pass ? width : height;
            const int columns = pass ? height : width;
            const auto index = [=](int row, int col)
            { return static_cast<size_t>(pass ? col * width + row : row * width + col) * 4; };
            for (int row = 0; row < rows; ++row)
            {
                int sum[3]{};
                for (int col = -radius; col <= radius; ++col)
                {
                    const auto at = index(row, std::clamp(col, 0, columns - 1));
                    for (int c = 0; c < 3; ++c) sum[c] += pixels[at + c];
                }
                for (int col = 0; col < columns; ++col)
                {
                    const auto at = index(row, col);
                    for (int c = 0; c < 3; ++c) scratch[at + c] = static_cast<uint8_t>(sum[c] / (2 * radius + 1));
                    scratch[at + 3] = 255;
                    const auto remove = index(row, std::clamp(col - radius, 0, columns - 1));
                    const auto add = index(row, std::clamp(col + radius + 1, 0, columns - 1));
                    for (int c = 0; c < 3; ++c) sum[c] += pixels[add + c] - pixels[remove + c];
                }
            }
            pixels.swap(scratch);
        }
    }

    // Coordinates are desktop pixels, so secondary monitors need no special
    // primary-screen arithmetic. Alpha from captured surfaces is composited
    // over the desktop color instead of turning transparent surfaces black.
    inline bool RenderGlass(const PixelFrame& frame, PixelRect target, int width, int height,
                            const GlassOptions& options, std::vector<uint8_t>& out)
    {
        if (width <= 0 || height <= 0 || width > 16384 || height > 16384
            || static_cast<uint64_t>(width) * height > 32 * 1024 * 1024
            || frame.Width <= 0 || frame.Height <= 0
            || frame.Bounds.Right <= frame.Bounds.Left || frame.Bounds.Bottom <= frame.Bounds.Top
            || frame.Pixels.size() != static_cast<size_t>(frame.Width) * frame.Height * 4) return false;
        out.resize(static_cast<size_t>(width) * height * 4);
        const auto unit = [](double v) { return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.0; };
        const double lens = unit(options.Lens), dispersion = unit(options.Dispersion);
        const double tint = ((options.Color >> 24) & 255) / 255.0 * unit(options.Tint);
        const double noise = unit(options.Noise) * 8.0;
        const auto sample = [&](double sx, double sy, int channel) -> double
        {
            double px = (sx - frame.Bounds.Left) * frame.Width / (frame.Bounds.Right - frame.Bounds.Left);
            double py = (sy - frame.Bounds.Top) * frame.Height / (frame.Bounds.Bottom - frame.Bounds.Top);
            if (frame.Tile)
            {
                px -= std::floor(px / frame.Width) * frame.Width;
                py -= std::floor(py / frame.Height) * frame.Height;
            }
            else if (px < 0 || py < 0 || px >= frame.Width || py >= frame.Height)
                return (frame.Background >> (channel * 8)) & 255;
            const int x = std::clamp(static_cast<int>(px), 0, frame.Width - 1);
            const int y = std::clamp(static_cast<int>(py), 0, frame.Height - 1);
            const auto at = (static_cast<size_t>(y) * frame.Width + x) * 4;
            const double alpha = frame.Pixels[at + 3] / 255.0;
            return frame.Pixels[at + channel] * alpha + ((frame.Background >> (channel * 8)) & 255) * (1 - alpha);
        };
        for (int y = 0; y < height; ++y)
        {
            const double edge = std::exp(-4.5 * y / height);
            const double shift = -46.0 * lens * edge;
            const double sy = target.Top + static_cast<double>(y) * (target.Bottom - target.Top) / height;
            for (int x = 0; x < width; ++x)
            {
                const double sx = target.Left + static_cast<double>(x) * (target.Right - target.Left) / width;
                const auto at = (static_cast<size_t>(y) * width + x) * 4;
                // Stable noise avoids flicker while preserving every live frame.
                const uint32_t hash = (static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u) ^ 0x9e3779b9u;
                const double grain = (static_cast<int>((hash ^ (hash >> 13)) & 255) / 255.0 - .5) * noise;
                for (int c = 0; c < 3; ++c)
                {
                    const double chromatic = c == 2 ? shift * dispersion * .25 : c == 0 ? -shift * dispersion * .25 : 0;
                    const double color = sample(sx, sy + shift + chromatic, c) * (1 - tint)
                        + ((options.Color >> (c * 8)) & 255) * tint + grain;
                    out[at + c] = static_cast<uint8_t>(std::clamp(color, 0.0, 255.0));
                }
                out[at + 3] = 255;
            }
        }
        const int radius = std::isfinite(options.Blur) ? std::clamp(static_cast<int>(options.Blur * .25), 0, 64) : 0;
        BoxBlur(out, width, height, radius);
        return true;
    }
}
