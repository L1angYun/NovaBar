using System.Text;

namespace NovaBar.Core;

/// <summary>
/// 把配置导出成引擎能直接读的扁平格式（<c>%LOCALAPPDATA%\NovaBar\apply.txt</c>）。
///
/// 之所以不让 C++ 侧解析 JSON：引擎是常驻进程、每 200ms 跑一次状态机，
/// 用纯文本 + 逐行解析最省事，也不需要引入 JSON 库。JSON 仍然是唯一真相，
/// 由 UI 负责维护。
/// </summary>
public static class EngineConfigWriter
{
    public static string DefaultPath { get; } = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "NovaBar",
        "apply.txt");

    private static readonly (AppearanceState State, string Key)[] StateKeys =
    [
        (AppearanceState.Desktop, "desktop"),
        (AppearanceState.VisibleWindow, "visible"),
        (AppearanceState.MaximizedWindow, "maximized"),
        (AppearanceState.StartOpened, "start"),
        (AppearanceState.SearchOpened, "search"),
        (AppearanceState.TaskViewOpened, "taskview"),
        (AppearanceState.BatterySaver, "battery"),
    ];

    public static string Build(NovaBarSettings settings, SurfaceKind surface = SurfaceKind.Taskbar)
    {
        ArgumentNullException.ThrowIfNull(settings);
        SettingsStore.Normalize(settings);
        var config = settings.Surface(surface);
        var builder = new StringBuilder();

        builder.AppendLine("# NovaBar engine config (generated)");
        builder.AppendLine($"enabled={(config.Enabled ? 1 : 0)}");
        builder.AppendLine($"surface.start={(settings.Surface(SurfaceKind.StartMenu).Enabled ? 1 : 0)}");
        builder.AppendLine($"surface.notification={(settings.Surface(SurfaceKind.NotificationCenter).Enabled ? 1 : 0)}");

        foreach (var (state, key) in StateKeys)
        {
            if (!config.States.TryGetValue(state, out var entry) || !entry.Enabled)
            {
                continue;
            }

            builder.AppendLine($"{key}={Describe(entry.Active)}");
            if (state is AppearanceState.VisibleWindow or AppearanceState.MaximizedWindow && entry.Inactive is { } inactive)
                builder.AppendLine($"{key}.inactive={Describe(inactive)}");
        }

        AppendRules(builder, config.Rules.ProcessName, "process");
        AppendRules(builder, config.Rules.WindowTitle, "title");
        AppendRules(builder, config.Rules.WindowClass, "class");
        foreach (var (state, key) in StateKeys.Where(item => item.State is AppearanceState.VisibleWindow or AppearanceState.MaximizedWindow))
        {
            var rules = config.State(state).Rules;
            AppendRules(builder, rules.ProcessName, "process", $"rule.{key}");
            AppendRules(builder, rules.WindowTitle, "title", $"rule.{key}");
            AppendRules(builder, rules.WindowClass, "class", $"rule.{key}");
        }
        AppendIgnored(builder, config.IgnoredWindows.ProcessName, "process");
        AppendIgnored(builder, config.IgnoredWindows.WindowTitle, "title");
        AppendIgnored(builder, config.IgnoredWindows.WindowClass, "class");

        return builder.ToString();
    }

    public static void Write(NovaBarSettings settings, string? path = null)
    {
        var target = Path.GetFullPath(path ?? DefaultPath);
        var dir = Path.GetDirectoryName(target)!;
        var taskbar = Build(settings);
        var start = Build(settings, SurfaceKind.StartMenu);
        var notification = Build(settings, SurfaceKind.NotificationCenter);
        Directory.CreateDirectory(dir);
        // Publish opt-in flags last, after the independent surface configs exist.
        WriteAtomic(Path.Combine(dir, "apply.start.txt"), start);
        WriteAtomic(Path.Combine(dir, "apply.notification.txt"), notification);
        WriteAtomic(target, taskbar);
    }

    private static void WriteAtomic(string target, string content)
    {
        var temp = target + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.WriteAllText(temp, content);
            File.Move(temp, target, overwrite: true);
        }
        finally { if (File.Exists(temp)) File.Delete(temp); }
    }

    private static void AppendRules(StringBuilder builder, Dictionary<string, RuleTarget> rules, string kind, string key = "rule")
    {
        foreach (var (pattern, target) in rules)
        {
            if (string.IsNullOrWhiteSpace(pattern))
            {
                continue;
            }

            var safePattern = EscapePattern(pattern);
            builder.AppendLine($"{key}={kind}|{safePattern}|{Describe(target.Active)}");
            if (target.Inactive is { } inactive) builder.AppendLine($"{key}.inactive={kind}|{safePattern}|{Describe(inactive)}");
        }
    }

    private static void AppendIgnored(StringBuilder builder, IEnumerable<string> patterns, string kind)
    {
        foreach (var pattern in patterns) builder.AppendLine($"ignore={kind}|{EscapePattern(pattern)}");
    }

    private static string EscapePattern(string pattern) => pattern.Replace("%", "%25").Replace("|", "%7C").Replace("\r", "%0D").Replace("\n", "%0A");

    /// <summary>生成 <c>accent=x;color=#AARRGGBB;line=0;blur=9;specular=0.3;tint=0.3</c>。</summary>
    private static string Describe(SurfaceAppearance appearance)
    {
        var accent = appearance.Accent switch
        {
            AccentKind.Normal => "normal",
            AccentKind.Opaque => "color",
            AccentKind.Clear => "clear",
            AccentKind.Blur => "blur",
            AccentKind.Acrylic => "acrylic",
            AccentKind.LiquidGlass => "liquid",
            _ => "clear",
        };

        return string.Join(
            ';',
            $"accent={accent}",
            $"color={appearance.Color}",
            $"line={(appearance.ShowLine ? 1 : 0)}",
            $"peek={(appearance.ShowPeek ? 1 : 0)}",
            $"blur={appearance.BlurRadius.ToString("0.##", System.Globalization.CultureInfo.InvariantCulture)}",
            $"specular={appearance.Glass.Specular.ToString("0.##", System.Globalization.CultureInfo.InvariantCulture)}",
            $"tint={appearance.Glass.TintStrength.ToString("0.##", System.Globalization.CultureInfo.InvariantCulture)}",
            $"lens={appearance.Glass.Refraction.ToString("0.##", System.Globalization.CultureInfo.InvariantCulture)}",
            $"dispersion={appearance.Glass.EdgeDispersion.ToString("0.##", System.Globalization.CultureInfo.InvariantCulture)}",
            $"noise={appearance.Glass.Noise.ToString("0.##", System.Globalization.CultureInfo.InvariantCulture)}");
    }
}
