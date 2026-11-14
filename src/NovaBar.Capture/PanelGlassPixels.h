#pragma once
#include "../NovaBar.XamlBridge/GlassPixels.h"
#include <optional>

namespace novabar
{
    struct PanelGlassOptions
    {
        GlassOptions Material;
        double Radius = 16;
        double Specular = .35;
        // Native shell hosts may own the final rounded clip. Keep the optical
        // bevel radius independent so we do not punch a second transparent
        // corner through their backing surface.
        bool ClipRounded = true;
        // Only physical pixels outside this desktop rectangle may be absent
        // from the scene. Without a viewport the full target stays mandatory.
        std::optional<PixelRect> Viewport;
        // Stable screen-space backing extent. The target still describes the
        // moving material and its lens; the native host clips the final shape.
        std::optional<PixelRect> Canvas;
    };

    namespace panel_detail
    {
        inline double Unit(double value)
        {
            return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
        }

        inline int BlurRadius(double value)
        {
            return std::isfinite(value) ? static_cast<int>(std::clamp(value * .25, 0.0, 64.0)) : 0;
        }

        // The scene arrives in premultiplied BGRA. Blur all four channels so
        // transparent window edges cannot acquire opaque black fringes.
        inline void Blur(std::vector<uint8_t>& pixels, int width, int height, int radiusX, int radiusY)
        {
            if (!radiusX && !radiusY) return;
            std::vector<uint8_t> scratch(pixels.size());
            const size_t stride = static_cast<size_t>(width) * 4;
            if (radiusX)
            {
                const float reciprocal = 1.0f / (radiusX * 2 + 1);
                for (int y = 0; y < height; ++y)
                {
                    const auto row = pixels.data() + static_cast<size_t>(y) * stride;
                    auto result = scratch.data() + static_cast<size_t>(y) * stride;
                    int sums[4]{row[0] * (radiusX + 1), row[1] * (radiusX + 1),
                        row[2] * (radiusX + 1), row[3] * (radiusX + 1)};
                    for (int x = 1; x <= radiusX; ++x)
                    {
                        const auto at = row + static_cast<size_t>(std::min(x, width - 1)) * 4;
                        for (int c = 0; c != 4; ++c) sums[c] += at[c];
                    }
                    for (int x = 0; x < width; ++x)
                    {
                        const auto remove = row + static_cast<size_t>(std::max(x - radiusX, 0)) * 4;
                        const auto add = row + static_cast<size_t>(std::min(x + radiusX + 1, width - 1)) * 4;
                        for (int c = 0; c != 4; ++c)
                        {
                            result[static_cast<size_t>(x) * 4 + c] = static_cast<uint8_t>(sums[c] * reciprocal + .5f);
                            sums[c] += add[c] - remove[c];
                        }
                    }
                }
                pixels.swap(scratch);
            }
            if (radiusY)
            {
                const float reciprocal = 1.0f / (radiusY * 2 + 1);
                std::vector<int> sums(stride);
                for (size_t i = 0; i < stride; ++i) sums[i] = pixels[i] * (radiusY + 1);
                for (int y = 1; y <= radiusY; ++y)
                {
                    const auto row = pixels.data() + static_cast<size_t>(std::min(y, height - 1)) * stride;
                    for (size_t i = 0; i < stride; ++i) sums[i] += row[i];
                }
                for (int y = 0; y < height; ++y)
                {
                    const auto remove = pixels.data() + static_cast<size_t>(std::max(y - radiusY, 0)) * stride;
                    const auto add = pixels.data() + static_cast<size_t>(std::min(y + radiusY + 1, height - 1)) * stride;
                    auto result = scratch.data() + static_cast<size_t>(y) * stride;
                    for (size_t i = 0; i < stride; ++i)
                    {
                        result[i] = static_cast<uint8_t>(sums[i] * reciprocal + .5f);
                        sums[i] += add[i] - remove[i];
                    }
                }
                pixels.swap(scratch);
            }
        }

        struct Sample { float B, G, R, A; };

        struct EdgeGeometry
        {
            double HalfWidth, HalfHeight, Radius, Band, MaxShift, Inset;
        };
        inline EdgeGeometry MakeEdgeGeometry(PixelRect target, double radius)
        {
            const double w = static_cast<double>(static_cast<int64_t>(target.Right)-target.Left);
            const double h = static_cast<double>(static_cast<int64_t>(target.Bottom)-target.Top);
            radius = std::isfinite(radius) ? std::clamp(radius,0.0,std::min(w,h)*.5) : 0;
            const double desired = std::clamp(std::min(w,h)*.12,12.0,64.0);
            const double band = radius>0 ? std::min(desired,radius) : desired;
            return {w*.5,h*.5,radius,band,std::min(36.0,band*.5),std::max(radius,band)};
        }
        struct EdgeField
        {
            double Distance, Nx, Ny, Shift, Dx, Dy;
            float Highlight;
        };
        inline EdgeField EvaluateEdge(EdgeGeometry const& geometry, double x, double y,
            double lens, float specular)
        {
            const double qx=std::abs(x)-(geometry.HalfWidth-geometry.Radius);
            const double qy=std::abs(y)-(geometry.HalfHeight-geometry.Radius);
            const double ox=std::max(qx,0.0), oy=std::max(qy,0.0);
            const double length=std::sqrt(ox*ox+oy*oy);
            const double distance=length+std::min(std::max(qx,qy),0.0)-geometry.Radius;
            double nx=0,ny=0;
            if(length>0) { nx=ox/length*(x<0?-1:1); ny=oy/length*(y<0?-1:1); }
            else if(qx>qy) nx=x<0?-1:1;
            else ny=y<0?-1:1;
            const double edge=std::clamp(1+distance/geometry.Band,0.0,1.0);
            const double shift=geometry.MaxShift*lens*(edge*edge*(3-2*edge));
            const float edge2=static_cast<float>(edge*edge), edge4=edge2*edge2;
            const float light=static_cast<float>(std::clamp(.45-nx*.35-ny*.55,0.0,1.0));
            return {distance,nx,ny,shift,-nx*shift,-ny*shift,edge4*edge4*specular*light*65};
        }

        struct Sampler
        {
            const uint8_t* Pixels;
            int Width, Height;
            double Left, Top, ScaleX, ScaleY;

            // Source and target bounds name physical pixel edges. The -.5
            // maps physical pixel centers to their matching texture centers.
            Sample Read(double screenX, double screenY) const
            {
                double px = (screenX - Left) * ScaleX - .5;
                double py = (screenY - Top) * ScaleY - .5;
                if (px < -.5 || py < -.5 || px > Width - .5 || py > Height - .5) return {};
                px = std::clamp(px, 0.0, static_cast<double>(Width - 1));
                py = std::clamp(py, 0.0, static_cast<double>(Height - 1));
                const int x = static_cast<int>(px), y = static_cast<int>(py);
                const int nextX = std::min(x + 1, Width - 1), nextY = std::min(y + 1, Height - 1);
                const float fx = static_cast<float>(px - x), fy = static_cast<float>(py - y);
                const auto a = Pixels + (static_cast<size_t>(y) * Width + x) * 4;
                const auto b = Pixels + (static_cast<size_t>(y) * Width + nextX) * 4;
                const auto c = Pixels + (static_cast<size_t>(nextY) * Width + x) * 4;
                const auto d = Pixels + (static_cast<size_t>(nextY) * Width + nextX) * 4;
                const auto value = [&](int channel)
                {
                    const float top = a[channel] + (b[channel] - a[channel]) * fx;
                    const float bottom = c[channel] + (d[channel] - c[channel]) * fx;
                    return top + (bottom - top) * fy;
                };
                return {value(0), value(1), value(2), value(3)};
            }
        };
    }

    inline int PanelCapturePadding(const PanelGlassOptions& options)
    {
        // Finite-support lens: 36 physical pixels, chromatic spread up to
        // 18%, one box-blur support radius, and a bilinear interpolation guard.
        const double displacement = 36 * panel_detail::Unit(options.Material.Lens)
            * (1 + .18 * panel_detail::Unit(options.Material.Dispersion));
        return std::min(128, static_cast<int>(std::ceil(displacement))
            + panel_detail::BlurRadius(options.Material.Blur) + 2);
    }

    inline bool RenderPanelGlass(const PixelFrame& frame, PixelRect target, int width, int height,
                                 const PanelGlassOptions& options, std::vector<uint8_t>& out)
    {
        const int64_t targetWidth = static_cast<int64_t>(target.Right) - target.Left;
        const int64_t targetHeight = static_cast<int64_t>(target.Bottom) - target.Top;
        const PixelRect canvas = options.Canvas.value_or(target);
        const int64_t canvasWidth = static_cast<int64_t>(canvas.Right) - canvas.Left;
        const int64_t canvasHeight = static_cast<int64_t>(canvas.Bottom) - canvas.Top;
        const int64_t sceneWidth = static_cast<int64_t>(frame.Bounds.Right) - frame.Bounds.Left;
        const int64_t sceneHeight = static_cast<int64_t>(frame.Bounds.Bottom) - frame.Bounds.Top;
        if (width <= 0 || height <= 0 || width > 16384 || height > 16384
            || static_cast<uint64_t>(width) * height > 32 * 1024 * 1024
            || frame.Width <= 0 || frame.Height <= 0 || frame.Width > 16384 || frame.Height > 16384
            || static_cast<uint64_t>(frame.Width) * frame.Height > 32 * 1024 * 1024
            || frame.Pixels.size() != static_cast<size_t>(frame.Width) * frame.Height * 4
            || targetWidth <= 0 || targetHeight <= 0 || canvasWidth <= 0 || canvasHeight <= 0
            || sceneWidth <= 0 || sceneHeight <= 0) return false;
        PixelRect visible = canvas;
        if (options.Viewport)
        {
            const auto viewport = *options.Viewport;
            if (viewport.Right <= viewport.Left || viewport.Bottom <= viewport.Top) return false;
            visible = {std::max(canvas.Left, viewport.Left), std::max(canvas.Top, viewport.Top),
                std::min(canvas.Right, viewport.Right), std::min(canvas.Bottom, viewport.Bottom)};
        }
        if (visible.Right <= visible.Left || visible.Bottom <= visible.Top)
        { out.assign(static_cast<size_t>(width)*height*4, 0); return true; }
        if (visible.Left < frame.Bounds.Left || visible.Top < frame.Bounds.Top
            || visible.Right > frame.Bounds.Right || visible.Bottom > frame.Bounds.Bottom) return false;

        const double sourceScaleX = static_cast<double>(frame.Width) / sceneWidth;
        const double sourceScaleY = static_cast<double>(frame.Height) / sceneHeight;
        const int physicalBlur = panel_detail::BlurRadius(options.Material.Blur);
        std::vector<uint8_t> blurred;
        const uint8_t* source = frame.Pixels.data();
        if (physicalBlur)
        {
            blurred = frame.Pixels;
            const int radiusX = std::min(frame.Width, static_cast<int>(std::ceil(physicalBlur * sourceScaleX)));
            const int radiusY = std::min(frame.Height, static_cast<int>(std::ceil(physicalBlur * sourceScaleY)));
            panel_detail::Blur(blurred, frame.Width, frame.Height, radiusX, radiusY);
            source = blurred.data();
        }
        const panel_detail::Sampler sampler{source, frame.Width, frame.Height,
            static_cast<double>(frame.Bounds.Left), static_cast<double>(frame.Bounds.Top), sourceScaleX, sourceScaleY};
        const auto geometry = panel_detail::MakeEdgeGeometry(target,options.Radius);
        const double halfWidth = geometry.HalfWidth, halfHeight = geometry.HalfHeight;
        const double pixelWidth = static_cast<double>(canvasWidth) / width;
        const double pixelHeight = static_cast<double>(canvasHeight) / height;
        // The rounded SDF's nearest-edge normal is undefined at its inner
        // medial axis. End the bevel before that axis so its displacement is
        // already zero when the normal switches between straight edges.
        // Radius zero deliberately retains a sharp rectangular corner.
        const double radius = geometry.Radius;
        // Smoothstep's maximum slope is 1.5. Keep the sample map ordered,
        // including the red channel's maximum 1.18 dispersion multiplier.
        const double lens = panel_detail::Unit(options.Material.Lens);
        const double dispersion = panel_detail::Unit(options.Material.Dispersion) * .18;
        const float specular = static_cast<float>(panel_detail::Unit(options.Specular));
        const float tint = static_cast<float>(((options.Material.Color >> 24) & 255) / 255.0
            * panel_detail::Unit(options.Material.Tint));
        const float noise = static_cast<float>(panel_detail::Unit(options.Material.Noise) * 8);
        const float tintColor[3]{static_cast<float>(options.Material.Color & 255),
            static_cast<float>((options.Material.Color >> 8) & 255),
            static_cast<float>((options.Material.Color >> 16) & 255)};
        out.assign(static_cast<size_t>(width) * height * 4, 0);
        const double textureStartX = (static_cast<double>(canvas.Left) - frame.Bounds.Left) * sourceScaleX;
        const double textureStartY = (static_cast<double>(canvas.Top) - frame.Bounds.Top) * sourceScaleY;
        const bool aligned = pixelWidth * sourceScaleX == 1 && pixelHeight * sourceScaleY == 1
            && std::abs(textureStartX - std::round(textureStartX)) < 1e-8
            && std::abs(textureStartY - std::round(textureStartY)) < 1e-8;
        const int sourceLeft = aligned ? static_cast<int>(std::round(textureStartX)) : 0;
        const int sourceTop = aligned ? static_cast<int>(std::round(textureStartY)) : 0;
        const double interiorInset = geometry.Inset;
        const bool flat = radius == 0 && lens == 0 && specular == 0;
        const auto write = [&](panel_detail::Sample sample, float coverage, float highlight, int x, int y)
        {
            const float alpha = std::clamp(sample.A, 0.0f, 255.0f);
            const float alphaUnit = alpha * (1.0f / 255);
            const uint32_t hash = (static_cast<uint32_t>(x) * 374761393u
                + static_cast<uint32_t>(y) * 668265263u) ^ 0x9e3779b9u;
            const float grain = (static_cast<int>((hash ^ (hash >> 13)) & 255) * (1.0f / 255) - .5f) * noise;
            const auto at = (static_cast<size_t>(y) * width + x) * 4;
            const float colors[]{sample.B, sample.G, sample.R};
            for (int c = 0; c != 3; ++c)
            {
                const float color = colors[c] * (1 - tint)
                    + (tintColor[c] * tint + grain + highlight) * alphaUnit;
                out[at + c] = static_cast<uint8_t>(std::clamp(color, 0.0f, alpha) * coverage + .5f);
            }
            out[at + 3] = static_cast<uint8_t>(alpha * coverage + .5f);
        };

        for (int y = 0; y < height; ++y)
        {
            const double sy = canvas.Top + (y + .5) * pixelHeight;
            const double localY = sy - (target.Top + halfHeight);
            if (options.Viewport && (sy < options.Viewport->Top || sy >= options.Viewport->Bottom)) continue;
            const bool insideRow = std::abs(localY) <= halfHeight - interiorInset;
            for (int x = 0; x < width; ++x)
            {
                const double sx = canvas.Left + (x + .5) * pixelWidth;
                const double localX = sx - (target.Left + halfWidth);
                if (options.Viewport && (sx < options.Viewport->Left || sx >= options.Viewport->Right)) continue;
                const bool outsideMaterial = options.Canvas
                    && (sx < target.Left || sx >= target.Right || sy < target.Top || sy >= target.Bottom);
                if (outsideMaterial || flat || (insideRow && std::abs(localX) <= halfWidth - interiorInset))
                {
                    if (aligned)
                    {
                        const auto pixel = source + (static_cast<size_t>(sourceTop + y) * frame.Width + sourceLeft + x) * 4;
                        write({static_cast<float>(pixel[0]), static_cast<float>(pixel[1]),
                               static_cast<float>(pixel[2]), static_cast<float>(pixel[3])}, 1, 0, x, y);
                    }
                    else write(sampler.Read(sx, sy), 1, 0, x, y);
                    continue;
                }
                const auto field=panel_detail::EvaluateEdge(geometry,localX,localY,lens,specular);
                const double distance=field.Distance;
                if (options.Canvas && distance > 0)
                { write(sampler.Read(sx, sy), 1, 0, x, y); continue; }
                const double nx=field.Nx,ny=field.Ny;
                const double antialiasWidth = std::sqrt(nx * nx * pixelWidth * pixelWidth
                    + ny * ny * pixelHeight * pixelHeight);
                const float coverage = !options.Canvas && options.ClipRounded
                    ? static_cast<float>(std::clamp(.5 - distance / antialiasWidth, 0.0, 1.0)) : 1.0f;
                if (coverage <= 0) continue;

                const double shift=field.Shift,dx=field.Dx,dy=field.Dy;
                auto sample = sampler.Read(sx + dx, sy + dy);
                if (dispersion && shift)
                {
                    sample.B = sampler.Read(sx + dx * (1 - dispersion), sy + dy * (1 - dispersion)).B;
                    sample.R = sampler.Read(sx + dx * (1 + dispersion), sy + dy * (1 + dispersion)).R;
                }
                write(sample, coverage, field.Highlight, x, y);
            }
        }
        return true;
    }
}
