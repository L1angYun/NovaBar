using System.Text.Json;
using System.Text.Json.Serialization;

namespace NovaBar.Core;

/// <summary>液态玻璃材质参数。仅在 <see cref="AccentKind.LiquidGlass"/> 下生效。</summary>
public sealed class GlassParameters
{
    /// <summary>折射强度（0-1）。越大边缘越"厚"，背景被弯折得越明显。</summary>
    public double Refraction { get; set; } = 0.55;

    /// <summary>高光强度（0-1）。沿法线的方向光。</summary>
    public double Specular { get; set; } = 0.35;

    /// <summary>边缘色散（0-1）。RGB 三通道微分离，产生彩色描边。</summary>
    public double EdgeDispersion { get; set; } = 0.18;

    /// <summary>噪声强度（0-1）。抑制色带，让玻璃有颗粒感。</summary>
    public double Noise { get; set; } = 0.12;

    /// <summary>色调浓度（0-1）。0 = 完全跟随背景，1 = 完全由 <see cref="SurfaceAppearance.Color"/> 决定。</summary>
    public double TintStrength { get; set; } = 0.30;

    /// <summary>圆角半径（DIP），0 = 跟随系统。</summary>
    public double CornerRadius { get; set; }

    public GlassParameters Clone() => (GlassParameters)MemberwiseClone();

    internal void Validate()
    {
        Refraction = Clamp01(Refraction);
        Specular = Clamp01(Specular);
        EdgeDispersion = Clamp01(EdgeDispersion);
        Noise = Clamp01(Noise);
        TintStrength = Clamp01(TintStrength);
        CornerRadius = double.IsFinite(CornerRadius) ? Math.Clamp(CornerRadius, 0, 64) : 0;
    }

    private static double Clamp01(double v) => double.IsFinite(v) ? Math.Clamp(v, 0, 1) : 0;
}

/// <summary>一个界面在某一状态下的外观。</summary>
public sealed class SurfaceAppearance
{
    public AccentKind Accent { get; set; } = AccentKind.Clear;

    public ArgbColor Color { get; set; } = ArgbColor.Transparent;

    public bool ShowPeek { get; set; }

    public bool ShowLine { get; set; }

    /// <summary>模糊半径（DIP）。Blur / Acrylic / LiquidGlass 使用。</summary>
    public double BlurRadius { get; set; } = 9.0;

    public GlassParameters Glass { get; set; } = new();

    public SurfaceAppearance Clone() => new()
    {
        Accent = Accent,
        Color = Color,
        ShowPeek = ShowPeek,
        ShowLine = ShowLine,
        BlurRadius = BlurRadius,
        Glass = Glass.Clone(),
    };

    internal void Validate()
    {
        if (!Enum.IsDefined(Accent)) Accent = AccentKind.Clear;
        BlurRadius = double.IsFinite(BlurRadius) ? Math.Clamp(BlurRadius, 0, 750) : 9;
        Glass ??= new();
        Glass.Validate();
    }
}

/// <summary>某个状态下可开关的外观（对应 TTB 的 OptionalTaskbarAppearance）。</summary>
public sealed class StateAppearance
{
    public bool Enabled { get; set; } = true;

    public SurfaceAppearance Active { get; set; } = new();

    /// <summary>窗口失焦时的外观。<c>null</c> 表示与 <see cref="Active"/> 相同。</summary>
    public SurfaceAppearance? Inactive { get; set; }

    /// <summary>仅此窗口状态的规则；未命中时再使用界面的通用规则。</summary>
    public SurfaceRules Rules { get; set; } = new();

    public StateAppearance Clone() => new()
    {
        Enabled = Enabled,
        Active = Active.Clone(),
        Inactive = Inactive?.Clone(),
        Rules = Rules?.Clone() ?? new(),
    };
}

/// <summary>一条规则命中的目标外观。</summary>
public sealed class RuleTarget
{
    public SurfaceAppearance Active { get; set; } = new();

    public SurfaceAppearance? Inactive { get; set; }

    public RuleTarget Clone() => new()
    {
        Active = Active.Clone(),
        Inactive = Inactive?.Clone(),
    };
}

/// <summary>按进程名 / 窗口标题 / 窗口类名匹配的规则集合。</summary>
public sealed class SurfaceRules
{
    public Dictionary<string, RuleTarget> ProcessName { get; set; } = new(StringComparer.OrdinalIgnoreCase);

    public Dictionary<string, RuleTarget> WindowTitle { get; set; } = new(StringComparer.OrdinalIgnoreCase);

    public Dictionary<string, RuleTarget> WindowClass { get; set; } = new(StringComparer.OrdinalIgnoreCase);

    public bool IsEmpty => ProcessName.Count == 0 && WindowTitle.Count == 0 && WindowClass.Count == 0;

    public SurfaceRules Clone()
    {
        var copy = new SurfaceRules();
        foreach (var (k, v) in ProcessName)
        {
            copy.ProcessName[k] = v.Clone();
        }

        foreach (var (k, v) in WindowTitle)
        {
            copy.WindowTitle[k] = v.Clone();
        }

        foreach (var (k, v) in WindowClass)
        {
            copy.WindowClass[k] = v.Clone();
        }

        return copy;
    }
}

/// <summary>被忽略的窗口（命中后回退到桌面外观）。</summary>
public sealed class WindowFilter
{
    public List<string> ProcessName { get; set; } = new();

    public List<string> WindowTitle { get; set; } = new();

    public List<string> WindowClass { get; set; } = new();

    public bool IsEmpty => ProcessName.Count == 0 && WindowTitle.Count == 0 && WindowClass.Count == 0;
}

/// <summary>一个界面的完整配置。</summary>
public sealed class SurfaceConfig
{
    public bool Enabled { get; set; } = true;

    public Dictionary<AppearanceState, StateAppearance> States { get; set; } = new();

    public SurfaceRules Rules { get; set; } = new();

    public WindowFilter IgnoredWindows { get; set; } = new();

    public StateAppearance State(AppearanceState state)
    {
        if (!States.TryGetValue(state, out var entry))
        {
            entry = new StateAppearance();
            States[state] = entry;
        }

        return entry;
    }

    public SurfaceConfig Clone()
    {
        var copy = new SurfaceConfig
        {
            Enabled = Enabled,
            Rules = Rules.Clone(),
            States = States.ToDictionary(kv => kv.Key, kv => kv.Value.Clone()),
            IgnoredWindows = new WindowFilter
            {
                ProcessName = [.. IgnoredWindows.ProcessName],
                WindowTitle = [.. IgnoredWindows.WindowTitle],
                WindowClass = [.. IgnoredWindows.WindowClass],
            },
        };

        return copy;
    }
}

/// <summary>全局选项。</summary>
public sealed class GlobalOptions
{
    private UpdateSourcePreference _updateSource;

    [JsonConverter(typeof(UpdateSourceSettingConverter))]
    public UpdateSourcePreference UpdateSource
    {
        get => _updateSource;
        set => _updateSource = Enum.IsDefined(value) ? value : UpdateSourcePreference.Auto;
    }

    /// <summary>启动配置中心后在后台检查稳定版本；默认关闭。</summary>
    public bool CheckUpdatesOnStartup { get; set; }

    public bool StartWithWindows { get; set; }

    public bool ShowTrayIcon { get; set; } = true;

    public bool CloseToTray { get; set; } = true;

    /// <summary>关掉后配置只读，不写盘。</summary>
    public bool DisableSaving { get; set; }

    public string LogLevel { get; set; } = "warn";

    public string Language { get; set; } = "zh-CN";

    /// <summary>引擎启动时自动把配置应用到已发现的界面。</summary>
    public bool AutoApply { get; set; } = true;
}

/// <summary>配置文件根对象。</summary>
public sealed class NovaBarSettings
{
    public int SchemaVersion { get; set; } = 1;

    public Dictionary<SurfaceKind, SurfaceConfig> Surfaces { get; set; } = new();

    public GlobalOptions Global { get; set; } = new();

    public SurfaceConfig Surface(SurfaceKind kind)
    {
        if (!Surfaces.TryGetValue(kind, out var cfg))
        {
            cfg = new SurfaceConfig { Enabled = kind == SurfaceKind.Taskbar };
            Surfaces[kind] = cfg;
        }

        return cfg;
    }

    public static NovaBarSettings CreateDefault()
    {
        var settings = new NovaBarSettings();

        foreach (var kind in Enum.GetValues<SurfaceKind>())
        {
            var cfg = settings.Surface(kind);
            foreach (var state in Enum.GetValues<AppearanceState>())
            {
                cfg.States[state] = new StateAppearance
                {
                    Enabled = state is not AppearanceState.BatterySaver,
                    Active = new SurfaceAppearance
                    {
                        Accent = AccentKind.LiquidGlass,
                        Color = ArgbColor.FromRgb(0, 0, 0, 0x26),
                        BlurRadius = 14,
                        ShowLine = true,
                        Glass = new GlassParameters
                        {
                            Specular = 0.60,
                            TintStrength = 0.55,
                        },
                    },
                };
            }
        }

        // 省电模式默认关闭（与 TTB 一致）
        settings.Surface(SurfaceKind.Taskbar).State(AppearanceState.BatterySaver).Enabled = false;

        return settings;
    }

    public NovaBarSettings Clone()
    {
        var copy = new NovaBarSettings
        {
            SchemaVersion = SchemaVersion,
            Global = new GlobalOptions
            {
                StartWithWindows = Global.StartWithWindows,
                ShowTrayIcon = Global.ShowTrayIcon,
                CloseToTray = Global.CloseToTray,
                DisableSaving = Global.DisableSaving,
                LogLevel = Global.LogLevel,
                Language = Global.Language,
                AutoApply = Global.AutoApply,
                CheckUpdatesOnStartup = Global.CheckUpdatesOnStartup,
                UpdateSource = Global.UpdateSource,
            },
            Surfaces = Surfaces.ToDictionary(kv => kv.Key, kv => kv.Value.Clone()),
        };

        return copy;
    }
}

// An obsolete source value must not make the rest of an imported settings file unreadable.
internal sealed class UpdateSourceSettingConverter : JsonConverter<UpdateSourcePreference>
{
    public override UpdateSourcePreference Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options)
    {
        if (reader.TokenType == JsonTokenType.String)
            return reader.GetString()?.ToLowerInvariant() switch
            {
                "github" => UpdateSourcePreference.GitHub,
                "gitee" => UpdateSourcePreference.Gitee,
                _ => UpdateSourcePreference.Auto,
            };
        if (reader.TokenType == JsonTokenType.Number && reader.TryGetInt32(out var value) && Enum.IsDefined((UpdateSourcePreference)value))
            return (UpdateSourcePreference)value;
        reader.Skip();
        return UpdateSourcePreference.Auto;
    }

    public override void Write(Utf8JsonWriter writer, UpdateSourcePreference value, JsonSerializerOptions options) =>
        writer.WriteStringValue(value switch
        {
            UpdateSourcePreference.GitHub => "github",
            UpdateSourcePreference.Gitee => "gitee",
            _ => "auto",
        });
}
