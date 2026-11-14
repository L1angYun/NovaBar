using System.Text.Json;

namespace NovaBar.Core;

/// <summary>TTB 配置迁移结果。</summary>
public sealed record TtbImportResult(NovaBarSettings Settings, IReadOnlyList<string> Warnings);

/// <summary>
/// 把 TranslucentTB 的 <c>settings.json</c> 映射成 NovaBar 配置，迁移到任务栏界面。
/// 字段名与语义刻意保持一一对应，避免"换了个工具就全没了"。
/// </summary>
public static class TtbImporter
{
    private static readonly Dictionary<string, AccentKind> AccentMap = new(StringComparer.OrdinalIgnoreCase)
    {
        ["normal"] = AccentKind.Normal,
        ["opaque"] = AccentKind.Opaque,
        ["clear"] = AccentKind.Clear,
        ["blur"] = AccentKind.Blur,
        ["acrylic"] = AccentKind.Acrylic,
    };

    public static TtbImportResult ImportFile(string path)
    {
        var json = File.ReadAllText(path);
        return Import(json);
    }

    public static TtbImportResult Import(string json)
    {
        var warnings = new List<string>();
        var settings = NovaBarSettings.CreateDefault();
        var taskbar = settings.Surface(SurfaceKind.Taskbar);

        using var doc = JsonDocument.Parse(json, new JsonDocumentOptions
        {
            CommentHandling = JsonCommentHandling.Skip,
            AllowTrailingCommas = true,
        });

        var root = doc.RootElement;

        ReadPlain(root, "desktop_appearance", AppearanceState.Desktop, taskbar, warnings);
        ReadRuled(root, "visible_window_appearance", AppearanceState.VisibleWindow, taskbar, warnings);
        ReadRuled(root, "maximized_window_appearance", AppearanceState.MaximizedWindow, taskbar, warnings);
        ReadPlain(root, "start_opened_appearance", AppearanceState.StartOpened, taskbar, warnings);
        ReadPlain(root, "search_opened_appearance", AppearanceState.SearchOpened, taskbar, warnings);
        ReadPlain(root, "task_view_opened_appearance", AppearanceState.TaskViewOpened, taskbar, warnings);
        ReadPlain(root, "battery_saver_appearance", AppearanceState.BatterySaver, taskbar, warnings);

        if (root.TryGetProperty("ignored_windows", out var ignored) && ignored.ValueKind == JsonValueKind.Object)
        {
            taskbar.IgnoredWindows.ProcessName = ReadStringList(ignored, "process_name");
            taskbar.IgnoredWindows.WindowTitle = ReadStringList(ignored, "window_title");
            taskbar.IgnoredWindows.WindowClass = ReadStringList(ignored, "window_class");
        }

        if (root.TryGetProperty("verbosity", out var verbosity) && verbosity.ValueKind == JsonValueKind.String)
        {
            settings.Global.LogLevel = verbosity.GetString() ?? settings.Global.LogLevel;
        }

        if (root.TryGetProperty("hide_tray", out var hideTray) && hideTray.ValueKind is JsonValueKind.True or JsonValueKind.False)
        {
            settings.Global.ShowTrayIcon = !hideTray.GetBoolean();
        }

        SettingsStore.Normalize(settings);
        return new TtbImportResult(settings, warnings);
    }

    private static void ReadPlain(
        JsonElement root,
        string property,
        AppearanceState state,
        SurfaceConfig target,
        List<string> warnings)
    {
        if (!root.TryGetProperty(property, out var element) || element.ValueKind != JsonValueKind.Object)
        {
            return;
        }

        var entry = target.State(state);
        if (element.TryGetProperty("enabled", out var enabled) && enabled.ValueKind is JsonValueKind.True or JsonValueKind.False)
        {
            entry.Enabled = enabled.GetBoolean();
        }

        entry.Active = ReadAppearance(element, warnings);
        if (element.TryGetProperty("inactive", out var inactive) && inactive.ValueKind == JsonValueKind.Object)
            entry.Inactive = ReadAppearance(inactive, warnings);
    }

    private static void ReadRuled(
        JsonElement root,
        string property,
        AppearanceState state,
        SurfaceConfig target,
        List<string> warnings)
    {
        if (!root.TryGetProperty(property, out var element) || element.ValueKind != JsonValueKind.Object)
        {
            return;
        }

        var entry = target.State(state);
        if (element.TryGetProperty("enabled", out var enabled) && enabled.ValueKind is JsonValueKind.True or JsonValueKind.False)
        {
            entry.Enabled = enabled.GetBoolean();
        }

        entry.Active = ReadAppearance(element, warnings);
        if (element.TryGetProperty("inactive", out var inactive) && inactive.ValueKind == JsonValueKind.Object)
            entry.Inactive = ReadAppearance(inactive, warnings);

        if (!element.TryGetProperty("rules", out var rules) || rules.ValueKind != JsonValueKind.Object)
        {
            return;
        }

        ReadRules(rules, "process_name", entry.Rules.ProcessName, warnings);
        ReadRules(rules, "window_title", entry.Rules.WindowTitle, warnings);
        ReadRules(rules, "window_class", entry.Rules.WindowClass, warnings);
    }

    private static void ReadRules(
        JsonElement rules,
        string property,
        Dictionary<string, RuleTarget> sink,
        List<string> warnings)
    {
        if (!rules.TryGetProperty(property, out var map) || map.ValueKind != JsonValueKind.Object)
        {
            return;
        }

        foreach (var rule in map.EnumerateObject())
        {
            if (rule.Value.ValueKind != JsonValueKind.Object)
            {
                warnings.Add($"规则 '{rule.Name}' 不是对象，已跳过");
                continue;
            }

            var target = new RuleTarget { Active = ReadAppearance(rule.Value, warnings) };

            if (rule.Value.TryGetProperty("inactive", out var inactive) && inactive.ValueKind == JsonValueKind.Object)
            {
                target.Inactive = ReadAppearance(inactive, warnings);
            }

            sink[rule.Name] = target;
        }
    }

    private static SurfaceAppearance ReadAppearance(JsonElement element, List<string> warnings)
    {
        var appearance = new SurfaceAppearance();

        if (element.TryGetProperty("accent", out var accent) && accent.ValueKind == JsonValueKind.String)
        {
            var text = accent.GetString() ?? string.Empty;
            if (AccentMap.TryGetValue(text, out var kind))
            {
                appearance.Accent = kind;
            }
            else
            {
                warnings.Add($"未知 accent '{text}'，按 clear 处理");
                appearance.Accent = AccentKind.Clear;
            }
        }

        if (element.TryGetProperty("color", out var color) && color.ValueKind == JsonValueKind.String)
        {
            var text = color.GetString();
            if (ArgbColor.TryParse(text, out var parsed))
            {
                appearance.Color = parsed;
            }
            else
            {
                warnings.Add($"无法解析颜色 '{text}'，已忽略");
            }
        }

        appearance.ShowPeek = ReadBool(element, "show_peek", appearance.ShowPeek);
        appearance.ShowLine = ReadBool(element, "show_line", appearance.ShowLine);

        if (element.TryGetProperty("blur_radius", out var radius) && radius.ValueKind == JsonValueKind.Number)
        {
            appearance.BlurRadius = radius.GetDouble();
        }

        appearance.Validate();
        return appearance;
    }

    private static bool ReadBool(JsonElement element, string property, bool fallback) =>
        element.TryGetProperty(property, out var value) && value.ValueKind is JsonValueKind.True or JsonValueKind.False
            ? value.GetBoolean()
            : fallback;

    private static List<string> ReadStringList(JsonElement parent, string property)
    {
        var list = new List<string>();
        if (!parent.TryGetProperty(property, out var array) || array.ValueKind != JsonValueKind.Array)
        {
            return list;
        }

        foreach (var item in array.EnumerateArray())
        {
            if (item.ValueKind == JsonValueKind.String && item.GetString() is { Length: > 0 } text)
            {
                list.Add(text);
            }
        }

        return list;
    }
}
