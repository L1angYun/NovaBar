namespace NovaBar.Core;

/// <summary>可一键套用的外观预设。</summary>
public sealed record AppearancePreset(
    string Id,
    string Name,
    string Description,
    SurfaceAppearance Appearance);

/// <summary>内置预设库。</summary>
public static class PresetLibrary
{
    public static IReadOnlyList<AppearancePreset> All { get; } =
    [
        new(
            "liquid-glass",
            "液态玻璃",
            "真实背景模糊、折射、色散与高光；薄色底保留背景颜色",
            new SurfaceAppearance
            {
                Accent = AccentKind.LiquidGlass,
                Color = ArgbColor.FromRgb(0, 0, 0, 0x26),
                BlurRadius = 14,
                ShowLine = true,
                Glass = new GlassParameters
                {
                    Refraction = 0.55,
                    Specular = 0.60,
                    EdgeDispersion = 0.18,
                    Noise = 0.0,
                    TintStrength = 0.55,
                },
            }),

        new(
            "liquid-glass-deep",
            "深色液态玻璃",
            "更厚的深色玻璃，适合亮色壁纸",
            new SurfaceAppearance
            {
                Accent = AccentKind.LiquidGlass,
                Color = ArgbColor.FromRgb(0, 0, 0, 0x40),
                BlurRadius = 18,
                ShowLine = true,
                Glass = new GlassParameters
                {
                    Refraction = 0.70,
                    Specular = 0.72,
                    EdgeDispersion = 0.22,
                    Noise = 0.0,
                    TintStrength = 0.72,
                },
            }),

        new(
            "clear",
            "全透明",
            "完全透出壁纸，没有模糊",
            new SurfaceAppearance
            {
                Accent = AccentKind.Clear,
                Color = ArgbColor.Transparent,
            }),

        new(
            "acrylic-dark",
            "深色亚克力",
            "系统同款亚克力：模糊 + 噪声 + 深色底",
            new SurfaceAppearance
            {
                Accent = AccentKind.Acrylic,
                Color = ArgbColor.FromRgb(32, 32, 32, 0xB3),
                BlurRadius = 24,
                ShowLine = true,
            }),

        new(
            "acrylic-light",
            "浅色亚克力",
            "系统同款亚克力，浅色底",
            new SurfaceAppearance
            {
                Accent = AccentKind.Acrylic,
                Color = ArgbColor.FromRgb(243, 243, 243, 0xB3),
                BlurRadius = 24,
                ShowLine = true,
            }),

        new(
            "blur",
            "模糊",
            "只模糊背景，不加色调",
            new SurfaceAppearance
            {
                Accent = AccentKind.Blur,
                Color = ArgbColor.Transparent,
                BlurRadius = 9,
            }),

        new(
            "opaque-dark",
            "不透明（深）",
            "纯色实心，适合追求性能时",
            new SurfaceAppearance
            {
                Accent = AccentKind.Opaque,
                Color = ArgbColor.FromRgb(20, 20, 20),
            }),

        new(
            "system",
            "跟随系统",
            "完全交还给 Windows 自己绘制",
            new SurfaceAppearance
            {
                Accent = AccentKind.Normal,
            }),
    ];

    public static AppearancePreset? Find(string id) =>
        All.FirstOrDefault(p => string.Equals(p.Id, id, StringComparison.OrdinalIgnoreCase));
}
