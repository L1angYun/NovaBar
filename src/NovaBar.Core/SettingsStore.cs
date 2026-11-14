using System.Text.Json;
using System.Text.Json.Serialization;

namespace NovaBar.Core;

/// <summary>配置文件的读写、校验与旧配置迁移。</summary>
public static class SettingsStore
{
    public static JsonSerializerOptions JsonOptions { get; } = CreateOptions();

    /// <summary>默认配置路径：<c>%APPDATA%\NovaBar\settings.json</c>。</summary>
    public static string DefaultPath { get; } = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "NovaBar",
        "settings.json");

    public static NovaBarSettings Load(string? path = null)
    {
        var file = path ?? DefaultPath;
        if (!File.Exists(file))
        {
            return NovaBarSettings.CreateDefault();
        }

        var json = File.ReadAllText(file);
        return Parse(json);
    }

    public static NovaBarSettings Parse(string json)
    {
        var settings = JsonSerializer.Deserialize<NovaBarSettings>(json, JsonOptions)
            ?? throw new JsonException("配置必须是 JSON 对象，不能是 null。");
        Normalize(settings);
        return settings;
    }

    public static void Export(NovaBarSettings settings, string path)
    {
        ArgumentNullException.ThrowIfNull(settings);
        Normalize(settings);
        WriteAtomic(path, JsonSerializer.Serialize(settings, JsonOptions));
    }

    public static void Save(NovaBarSettings settings, string? path = null)
    {
        ArgumentNullException.ThrowIfNull(settings);

        Normalize(settings);
        if (settings.Global.DisableSaving)
        {
            return;
        }

        WriteAtomic(path ?? DefaultPath, JsonSerializer.Serialize(settings, JsonOptions));
    }

    private static void WriteAtomic(string file, string content)
    {
        var dir = Path.GetDirectoryName(file);
        if (!string.IsNullOrEmpty(dir))
        {
            Directory.CreateDirectory(dir);
        }

        // 原子写：先写临时文件再替换，避免掉电写坏配置
        var temp = file + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.WriteAllText(temp, content);
            File.Move(temp, file, overwrite: true);
        }
        finally
        {
            if (File.Exists(temp)) File.Delete(temp);
        }
    }

    /// <summary>补全缺失项、裁剪越界值、保证每个界面都有完整状态表。</summary>
    public static void Normalize(NovaBarSettings settings)
    {
        ArgumentNullException.ThrowIfNull(settings);
        settings.Global ??= new();
        settings.Global.LogLevel ??= "warn";
        settings.Global.Language ??= "zh-CN";
        settings.Surfaces ??= new();
        foreach (var key in settings.Surfaces.Keys.Where(key => !Enum.IsDefined(key)).ToArray()) settings.Surfaces.Remove(key);
        foreach (var kind in Enum.GetValues<SurfaceKind>())
        {
            if (!settings.Surfaces.TryGetValue(kind, out var found) || found is null)
                settings.Surfaces[kind] = new() { Enabled = kind == SurfaceKind.Taskbar };
            var cfg = settings.Surface(kind);
            cfg.States ??= new();
            cfg.Rules ??= new();
            cfg.IgnoredWindows ??= new();
            foreach (var key in cfg.States.Keys.Where(key => !Enum.IsDefined(key)).ToArray()) cfg.States.Remove(key);
            foreach (var state in Enum.GetValues<AppearanceState>())
            {
                if (!cfg.States.TryGetValue(state, out var entry) || entry is null)
                {
                    cfg.States[state] = new StateAppearance { Enabled = state != AppearanceState.BatterySaver };
                }
                cfg.States[state].Active ??= new();
                cfg.States[state].Active.Validate();
                cfg.States[state].Inactive?.Validate();
                cfg.States[state].Rules = NormalizeRuleSet(cfg.States[state].Rules);
            }

            cfg.Rules = NormalizeRuleSet(cfg.Rules);

            cfg.IgnoredWindows.ProcessName = NormalizePatterns(cfg.IgnoredWindows.ProcessName);
            cfg.IgnoredWindows.WindowTitle = NormalizePatterns(cfg.IgnoredWindows.WindowTitle);
            cfg.IgnoredWindows.WindowClass = NormalizePatterns(cfg.IgnoredWindows.WindowClass);
        }
    }

    private static SurfaceRules NormalizeRuleSet(SurfaceRules? rules)
    {
        rules ??= new();
        rules.ProcessName = NormalizeRules(rules.ProcessName);
        rules.WindowTitle = NormalizeRules(rules.WindowTitle);
        rules.WindowClass = NormalizeRules(rules.WindowClass);
        foreach (var target in rules.ProcessName.Values.Concat(rules.WindowTitle.Values).Concat(rules.WindowClass.Values))
        {
            target.Active ??= new();
            target.Active.Validate();
            target.Inactive?.Validate();
        }
        return rules;
    }

    private static Dictionary<string, RuleTarget> NormalizeRules(Dictionary<string, RuleTarget>? rules)
    {
        var result = new Dictionary<string, RuleTarget>(StringComparer.OrdinalIgnoreCase);
        if (rules is not null)
            foreach (var (pattern, target) in rules)
                if (!string.IsNullOrWhiteSpace(pattern)) result[pattern.Trim()] = target ?? new();
        return result;
    }

    private static List<string> NormalizePatterns(List<string>? patterns) =>
        patterns?.Where(p => !string.IsNullOrWhiteSpace(p)).Select(p => p.Trim()).Distinct(StringComparer.OrdinalIgnoreCase).ToList() ?? new();

    private static JsonSerializerOptions CreateOptions()
    {
        var options = new JsonSerializerOptions
        {
            WriteIndented = true,
            PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
            ReadCommentHandling = JsonCommentHandling.Skip,
            AllowTrailingCommas = true,
            DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        };

        options.Converters.Add(new JsonStringEnumConverter(JsonNamingPolicy.SnakeCaseLower));
        options.Converters.Add(new ArgbColorJsonConverter());
        return options;
    }
}

/// <summary>把 <see cref="ArgbColor"/> 读写为 <c>#AARRGGBB</c>，与 TTB 的格式一致。</summary>
public sealed class ArgbColorJsonConverter : JsonConverter<ArgbColor>
{
    public override ArgbColor Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options)
    {
        if (reader.TokenType != JsonTokenType.String) throw new JsonException("颜色必须是 #AARRGGBB 或 #RRGGBB 字符串。");
        var text = reader.GetString();
        return ArgbColor.TryParse(text, out var color) ? color : throw new JsonException($"颜色格式无效：{text}");
    }

    public override void Write(Utf8JsonWriter writer, ArgbColor value, JsonSerializerOptions options) =>
        writer.WriteStringValue(value.ToString());
}
