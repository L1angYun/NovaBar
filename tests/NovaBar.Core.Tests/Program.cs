using System.Globalization;
using System.Text.Json;
using NovaBar.Core;

var failed = 0;
var total = 0;
void Test(string name, Action check)
{
    total++;
    try { check(); Console.WriteLine($"PASS {name}"); }
    catch (Exception ex) { failed++; Console.WriteLine($"FAIL {name}: {ex.Message}"); }
}
void Assert(bool condition, string message) { if (!condition) throw new Exception(message); }

Test("shell surface opt-ins default off", () => {
    var s = NovaBarSettings.CreateDefault();
    Assert(!s.Surface(SurfaceKind.StartMenu).Enabled && !s.Surface(SurfaceKind.NotificationCenter).Enabled, "unsupported surfaces advertised enabled");
});
Test("null configuration graph normalized", () => {
    var s = JsonSerializer.Deserialize<NovaBarSettings>("{\"global\":null,\"surfaces\":{\"taskbar\":{\"states\":{\"desktop\":null},\"rules\":null,\"ignored_windows\":null},\"start_menu\":null}}", SettingsStore.JsonOptions)!;
    SettingsStore.Normalize(s);
    Assert(s.Global is not null && s.Surface(SurfaceKind.Taskbar).State(AppearanceState.Desktop).Active.Glass is not null, "null graph retained");
    _ = s.Clone();
});
Test("nonfinite parameters and unknown enum normalized", () => {
    var s = NovaBarSettings.CreateDefault(); var a = s.Surface(SurfaceKind.Taskbar).State(AppearanceState.Desktop).Active;
    a.BlurRadius = double.NaN; a.Glass.Refraction = double.PositiveInfinity; a.Glass.CornerRadius = double.NaN; a.Accent = (AccentKind)99;
    SettingsStore.Normalize(s);
    Assert(double.IsFinite(a.BlurRadius) && double.IsFinite(a.Glass.Refraction) && double.IsFinite(a.Glass.CornerRadius) && Enum.IsDefined(a.Accent), "invalid numeric values retained");
    _ = JsonSerializer.Serialize(s, SettingsStore.JsonOptions);
});
Test("case-insensitive duplicate rules normalized", () => {
    var s = JsonSerializer.Deserialize<NovaBarSettings>("{\"surfaces\":{\"taskbar\":{\"rules\":{\"process_name\":{\"Chrome.exe\":{},\"chrome.EXE\":{}}}}}}", SettingsStore.JsonOptions)!;
    SettingsStore.Normalize(s);
    var rules = s.Surface(SurfaceKind.Taskbar).Rules.ProcessName;
    Assert(rules.Count == 1 && rules.ContainsKey("CHROME.EXE"), "duplicate rules or case-sensitive dictionary");
});
Test("engine protocol preserves disabled fallthrough and inactive", () => {
    var s = NovaBarSettings.CreateDefault(); var taskbar = s.Surface(SurfaceKind.Taskbar);
    taskbar.Enabled = false; taskbar.State(AppearanceState.MaximizedWindow).Enabled = false;
    taskbar.State(AppearanceState.VisibleWindow).Inactive = new() { Accent = AccentKind.Blur, ShowPeek = true };
    var text = EngineConfigWriter.Build(s);
    Assert(text.Contains("enabled=0") && text.Contains("surface.start=0") && text.Contains("surface.notification=0"), "missing enable flags");
    Assert(!text.Split('\n').Any(l => l.StartsWith("maximized=")), "disabled state masks lower priority");
    Assert(text.Contains("visible.inactive=accent=blur;") && text.Contains("peek=1"), "inactive, blur or peek missing");
});
Test("rule protocol escapes delimiters and exports ignores", () => {
    var s = NovaBarSettings.CreateDefault(); var t = s.Surface(SurfaceKind.Taskbar);
    t.Rules.WindowTitle["a|b\nvisible=bad%"] = new() { Inactive = new() };
    t.IgnoredWindows.ProcessName.Add("ignored.exe");
    var text = EngineConfigWriter.Build(s);
    Assert(text.Contains("rule=title|a%7Cb%0Avisible=bad%25|") && text.Contains("rule.inactive=title|"), "rule delimiters not escaped or inactive missing");
    Assert(text.Contains("ignore=process|ignored.exe") && !text.Split('\n').Any(l => l.StartsWith("visible=bad")), "ignored windows missing or newline injection");
});
Test("engine protocol invariant culture and material fields", () => {
    var before = CultureInfo.CurrentCulture;
    try { CultureInfo.CurrentCulture = CultureInfo.GetCultureInfo("fr-FR"); var text = EngineConfigWriter.Build(NovaBarSettings.CreateDefault());
        Assert(text.Contains("lens=0.55") && text.Contains("dispersion=0.18") && text.Contains("noise=0.12"), "material numbers missing or culture-dependent"); }
    finally { CultureInfo.CurrentCulture = before; }
});
Test("every surface exports full liquid optics for states and scoped rules", () => {
    var settings = NovaBarSettings.CreateDefault();
    foreach (var surface in Enum.GetValues<SurfaceKind>())
    {
        var appearance = new SurfaceAppearance {
            Accent = AccentKind.LiquidGlass, Color = ArgbColor.Parse("#66123456"), BlurRadius = 27,
            Glass = new() { Refraction = .83, Specular = .67, EdgeDispersion = .42, Noise = .19, TintStrength = .37 }
        };
        var config = settings.Surface(surface);
        config.State(AppearanceState.Desktop).Active = appearance;
        config.State(AppearanceState.VisibleWindow).Inactive = appearance.Clone();
        config.State(AppearanceState.MaximizedWindow).Rules.WindowTitle["liquid-optics"] = new() { Active = appearance.Clone(), Inactive = appearance.Clone() };
        var protocol = EngineConfigWriter.Build(settings, surface);
        foreach (var key in new[] { "desktop=", "visible.inactive=", "rule.maximized=title|liquid-optics|", "rule.maximized.inactive=title|liquid-optics|" })
        {
            var line = protocol.Split('\n').Single(item => item.StartsWith(key, StringComparison.Ordinal));
            foreach (var field in new[] { "accent=liquid", "color=#66123456", "blur=27", "lens=0.83", "specular=0.67", "dispersion=0.42", "noise=0.19", "tint=0.37" })
                Assert(line.Contains(field, StringComparison.Ordinal), $"{surface} {key} lost {field}");
            Assert(!line.Contains("accent=acrylic", StringComparison.Ordinal), $"{surface} downgraded liquid optics");
        }
    }
});
Test("disabled maximized falls through to visible", () => {
    var config = NovaBarSettings.CreateDefault().Surface(SurfaceKind.Taskbar); config.State(AppearanceState.MaximizedWindow).Enabled = false;
    var result = AppearanceResolver.Resolve(config, new() { HasVisibleWindow = true, IsMaximized = true });
    Assert(result?.State == AppearanceState.VisibleWindow, "skips enabled visible state");
});
Test("disabled desktop gives system control", () => {
    var config = NovaBarSettings.CreateDefault().Surface(SurfaceKind.Taskbar); config.State(AppearanceState.Desktop).Enabled = false;
    Assert(AppearanceResolver.Resolve(config, new()) is null, "disabled desktop still applied");
});
Test("save propagates failure and roundtrips nested rules", () => {
    var dir = Path.Combine(Path.GetTempPath(), "NovaBar-tests-" + Guid.NewGuid().ToString("N")); Directory.CreateDirectory(dir);
    try {
        var s = NovaBarSettings.CreateDefault(); s.Surface(SurfaceKind.Taskbar).Rules.WindowClass["Editor"] = new() { Inactive = new() { Accent = AccentKind.Opaque } };
        var file = Path.Combine(dir, "settings.json"); SettingsStore.Save(s, file);
        Assert(SettingsStore.Load(file).Surface(SurfaceKind.Taskbar).Rules.WindowClass["editor"].Inactive?.Accent == AccentKind.Opaque, "nested rule roundtrip failed");
        var threw = false; try { SettingsStore.Save(s, dir); } catch (IOException) { threw = true; } catch (UnauthorizedAccessException) { threw = true; }
        Assert(threw, "save failure swallowed");
    } finally { Directory.Delete(dir, true); }
});
Test("invalid color import is rejected", () => {
    var rejected = false;
    try { _ = SettingsStore.Parse("{\"surfaces\":{\"taskbar\":{\"states\":{\"desktop\":{\"active\":{\"color\":\"not-a-color\"}}}}}}"); }
    catch (JsonException) { rejected = true; }
    Assert(rejected, "malformed color silently replaced");
});
Test("explicit export works while autosaving disabled", () => {
    var dir = Path.Combine(Path.GetTempPath(), "NovaBar-tests-" + Guid.NewGuid().ToString("N")); Directory.CreateDirectory(dir);
    try { var s = NovaBarSettings.CreateDefault(); s.Global.DisableSaving = true;
        var file = Path.Combine(dir, "export.json"); SettingsStore.Export(s, file);
        Assert(File.Exists(file) && SettingsStore.Load(file).Global.DisableSaving, "export suppressed by session preference");
    } finally { Directory.Delete(dir, true); }
});
Test("TTB state inactive appearance is preserved", () => {
    var imported = TtbImporter.Import("{\"visible_window_appearance\":{\"accent\":\"clear\",\"inactive\":{\"accent\":\"opaque\",\"color\":\"#FF123456\"}},\"desktop_appearance\":{\"accent\":\"blur\",\"inactive\":{\"accent\":\"normal\"}}}");
    var config = imported.Settings.Surface(SurfaceKind.Taskbar);
    Assert(config.State(AppearanceState.VisibleWindow).Inactive?.Accent == AccentKind.Opaque && config.State(AppearanceState.Desktop).Inactive?.Accent == AccentKind.Normal, "state inactive appearance dropped during TTB import");
});
Test("engine writes independent surface files", () => {
    var dir = Path.Combine(Path.GetTempPath(), "NovaBar-tests-" + Guid.NewGuid().ToString("N"));
    try {
        var s = NovaBarSettings.CreateDefault();
        s.Surface(SurfaceKind.Taskbar).State(AppearanceState.Desktop).Active.Accent = AccentKind.Clear;
        var start = s.Surface(SurfaceKind.StartMenu); start.Enabled = true;
        start.State(AppearanceState.Desktop).Active.Accent = AccentKind.Opaque;
        start.Rules.ProcessName["start-only.exe"] = new() { Active = new() { Accent = AccentKind.Acrylic } };
        var notification = s.Surface(SurfaceKind.NotificationCenter); notification.Enabled = true;
        notification.State(AppearanceState.Desktop).Active.Accent = AccentKind.Normal;
        EngineConfigWriter.Write(s, Path.Combine(dir, "apply.txt"));
        var taskbarText = File.ReadAllText(Path.Combine(dir, "apply.txt"));
        var startText = File.ReadAllText(Path.Combine(dir, "apply.start.txt"));
        var notificationText = File.ReadAllText(Path.Combine(dir, "apply.notification.txt"));
        Assert(taskbarText.Contains("desktop=accent=clear;") && startText.Contains("desktop=accent=color;") && notificationText.Contains("desktop=accent=normal;"), "surface appearance leaked into another file");
        Assert(startText.Contains("start-only.exe") && !taskbarText.Contains("start-only.exe") && !notificationText.Contains("start-only.exe"), "surface rules leaked");
        Assert(taskbarText.Contains("surface.start=1") && taskbarText.Contains("surface.notification=1"), "missing injection opt-ins");
    } finally { if (Directory.Exists(dir)) Directory.Delete(dir, true); }
});
Test("desktop uses active appearance without a window", () => {
    var config = NovaBarSettings.CreateDefault().Surface(SurfaceKind.Taskbar);
    config.State(AppearanceState.Desktop).Active.Accent = AccentKind.Clear;
    config.State(AppearanceState.Desktop).Inactive = new() { Accent = AccentKind.Opaque };
    Assert(AppearanceResolver.Resolve(config, new())?.Appearance.Accent == AccentKind.Clear, "desktop applies inactive window appearance");
});
Test("nonwindow inactive settings stay out of engine protocol", () => {
    var s = NovaBarSettings.CreateDefault(); var config = s.Surface(SurfaceKind.Taskbar);
    config.State(AppearanceState.Desktop).Inactive = new(); config.State(AppearanceState.StartOpened).Inactive = new();
    var text = EngineConfigWriter.Build(s);
    Assert(!text.Contains("desktop.inactive=") && !text.Contains("start.inactive="), "unsupported inactive states exported");
});
Test("TTB conflicting window-state rules remain independent", () => {
    var imported = TtbImporter.Import("""
    {
      "visible_window_appearance": {"accent":"clear","rules":{"process_name":{"notepad.exe":{"accent":"opaque","color":"#FFFF0000","inactive":{"accent":"opaque","color":"#FFFFFF00"}}}}},
      "maximized_window_appearance": {"accent":"clear","rules":{"process_name":{"notepad.exe":{"accent":"opaque","color":"#FF0000FF","inactive":{"accent":"opaque","color":"#FF00FF00"}}}}}
    }
    """);
    var config = imported.Settings.Surface(SurfaceKind.Taskbar);
    var visible = AppearanceResolver.Resolve(config, new() { HasVisibleWindow=true, WindowIsForeground=true, ProcessName="NOTEPAD.EXE" });
    var maximized = AppearanceResolver.Resolve(config, new() { HasVisibleWindow=true, IsMaximized=true, WindowIsForeground=true, ProcessName="notepad.exe" });
    var inactive = AppearanceResolver.Resolve(config, new() { HasVisibleWindow=true, IsMaximized=true, WindowIsForeground=false, ProcessName="notepad.exe" });
    Assert(visible?.Appearance.Color.ToString()=="#FFFF0000", "maximized import overwrote visible rule");
    Assert(maximized?.Appearance.Color.ToString()=="#FF0000FF" && inactive?.Appearance.Color.ToString()=="#FF00FF00", "maximized active/inactive scope lost");
    Assert(config.Rules.IsEmpty, "state rules leaked into common fallback");
});
Test("engine protocol exports scoped rules and inactive without merging", () => {
    var imported = TtbImporter.Import("""
    {"visible_window_appearance":{"rules":{"window_title":{"a|b%":{"accent":"clear","inactive":{"accent":"blur"}}}}},
     "maximized_window_appearance":{"rules":{"window_title":{"a|b%":{"accent":"opaque","inactive":{"accent":"normal"}}}}}}
    """);
    var text=EngineConfigWriter.Build(imported.Settings);
    Assert(text.Contains("rule.visible=title|a%7Cb%25|accent=clear;") && text.Contains("rule.visible.inactive=title|a%7Cb%25|accent=blur;"), "visible scoped protocol missing");
    Assert(text.Contains("rule.maximized=title|a%7Cb%25|accent=color;") && text.Contains("rule.maximized.inactive=title|a%7Cb%25|accent=normal;"), "maximized scoped protocol missing");
    Assert(!text.Split('\n').Any(line=>line.StartsWith("rule=")), "state rules emitted as common fallback");
});
Test("scoped rules precede common matching and unrelated scope stays isolated", () => {
    var s=SettingsStore.Parse("""
    {"surfaces":{"taskbar":{"rules":{"process_name":{"editor.exe":{"active":{"accent":"clear"}}}},"states":{
      "visible_window":{"rules":{"window_title":{"Editor":{"active":{"accent":"opaque","color":"#FF123456"}}}}},
      "maximized_window":{"rules":null}
    }}}}
    """);
    var config=s.Surface(SurfaceKind.Taskbar);
    var visible=AppearanceResolver.Resolve(config,new(){HasVisibleWindow=true,WindowIsForeground=true,ProcessName="editor.exe",WindowTitle="Editor document"});
    var maximized=AppearanceResolver.Resolve(config,new(){HasVisibleWindow=true,IsMaximized=true,WindowIsForeground=true,ProcessName="editor.exe",WindowTitle="Editor document"});
    Assert(visible?.Appearance.Color.ToString()=="#FF123456", "common process rule hid scoped title rule");
    Assert(maximized?.Appearance.Accent==AccentKind.Clear, "visible-only rule leaked into maximized state");
});
Test("scoped rules normalize null graphs and survive cloning", () => {
    var s=SettingsStore.Parse("""
    {"surfaces":{"taskbar":{"states":{
      "visible_window":{"rules":null},
      "maximized_window":{"rules":{"process_name":{"Editor.exe":{"active":null,"inactive":{"accent":"opaque","glass":null}}},"window_title":null,"window_class":null}}
    }}}}
    """);
    var clone=s.Clone();
    var resolved=AppearanceResolver.Resolve(clone.Surface(SurfaceKind.Taskbar),new(){HasVisibleWindow=true,IsMaximized=true,ProcessName="EDITOR.EXE",WindowIsForeground=false});
    Assert(resolved?.Appearance.Accent==AccentKind.Opaque, "scoped inactive or null normalization lost on clone");
    clone.Surface(SurfaceKind.Taskbar).State(AppearanceState.MaximizedWindow).Rules.ProcessName["editor.exe"].Inactive!.Accent=AccentKind.Normal;
    Assert(AppearanceResolver.Resolve(s.Surface(SurfaceKind.Taskbar),new(){HasVisibleWindow=true,IsMaximized=true,ProcessName="editor.exe",WindowIsForeground=false})?.Appearance.Accent==AccentKind.Opaque, "editing cloned scoped rules changed original");
    using var json=JsonDocument.Parse(JsonSerializer.Serialize(clone,SettingsStore.JsonOptions));
    Assert(json.RootElement.GetProperty("surfaces").GetProperty("taskbar").GetProperty("states").GetProperty("visible_window").TryGetProperty("rules",out var rules) && rules.ValueKind==JsonValueKind.Object, "null state rules not normalized");
});
Test("capture readiness parses fresh broker keylines without exposing errors", () => {
    var now = new DateTimeOffset(2026, 11, 4, 12, 0, 0, TimeSpan.Zero);
    var status = CaptureStatus.Parse("\uFEFFstate=ready\r\nactive=2\r\nready=2\r\nerror=0\r\n", now.AddSeconds(-1), now);
    Assert(status.State == CaptureReadiness.Ready && status.ActiveTargets == 2 && status.ReadyTargets == 2, "fresh ready targets were lost");
    Assert(status.DisplayText == "玻璃：实时画面就绪", "ready label does not describe usable glass");
    var failed = CaptureStatus.Parse("state=unavailable\nactive=1\nready=0\nerror=-2147024891", now, now);
    Assert(failed.State == CaptureReadiness.Unavailable && failed.ErrorCode == -2147024891 && failed.DisplayText == "玻璃：画面暂不可用", "source failure not shown cleanly");
    Assert(!failed.DisplayText.Contains("2147024891"), "raw failure code leaked into product text");
});
Test("capture idle and warmup labels remain distinct", () => {
    var now = DateTimeOffset.UtcNow;
    Assert(CaptureStatus.Parse("state=idle\nactive=0\nready=0\nerror=0", now, now).DisplayText == "玻璃：空闲", "closed panels reported as disconnected");
    Assert(CaptureStatus.Parse("state=waiting\nactive=3\nready=1\nerror=0", now, now).DisplayText == "玻璃：正在读取背后画面", "warmup reported as ready");
});
Test("capture stale malformed and inconsistent readiness is disconnected", () => {
    var now = DateTimeOffset.UtcNow;
    const string ready = "state=ready\nactive=1\nready=1\nerror=0";
    Assert(CaptureStatus.Parse(ready, now.AddSeconds(-3), now).State == CaptureReadiness.Ready, "three-second fresh boundary rejected");
    Assert(CaptureStatus.Parse(ready, now.AddMilliseconds(-3001), now).DisplayText == "捕获：未连接", "stale heartbeat still displayed ready");
    foreach (var text in new string?[] { null, "", "state=unknown\nactive=1\nready=1\nerror=0", "state=ready\nactive=2\nready=1\nerror=0", "state=ready\nactive=-1\nready=0\nerror=0", "state=ready\nstate=idle\nactive=1\nready=1\nerror=0", "state=ready\nactive=1\nready=1" })
        Assert(CaptureStatus.Parse(text, now, now).State == CaptureReadiness.Disconnected, "malformed status reported as usable");
});
Test("capture status file heartbeat expires and missing files stay disconnected", () => {
    var directory = Path.Combine(Path.GetTempPath(), "NovaBar-status-" + Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(directory);
    try {
        var path = Path.Combine(directory, "capture-status.txt");
        var now = DateTimeOffset.UtcNow;
        Assert(CaptureStatus.Read(path, now).State == CaptureReadiness.Disconnected, "missing status should stay disconnected");
        File.WriteAllText(path, "state=ready\nactive=1\nready=1\nerror=0\n");
        File.SetLastWriteTimeUtc(path, now.AddSeconds(-1).UtcDateTime);
        Assert(CaptureStatus.Read(path, now).State == CaptureReadiness.Ready, "fresh producer file not read");
        File.SetLastWriteTimeUtc(path, now.AddSeconds(-4).UtcDateTime);
        Assert(CaptureStatus.Read(path, now).State == CaptureReadiness.Disconnected, "dead producer file remained ready");
    } finally { Directory.Delete(directory, true); }
});
Test("legacy settings default to automatic update source without enabling startup checks", () => {
    var settings = SettingsStore.Parse("{\"global\":{\"auto_apply\":false}}");
    var global = JsonSerializer.SerializeToElement(settings, SettingsStore.JsonOptions).GetProperty("global");
    Assert(global.TryGetProperty("update_source", out var source) && source.GetString() == "auto", "old settings must use automatic source");
    Assert(!settings.Global.CheckUpdatesOnStartup && !settings.Global.AutoApply, "unrelated startup and apply settings changed");
});
Test("update source survives settings clone and export", () => {
    var settings = SettingsStore.Parse("{\"global\":{\"update_source\":\"gitee\",\"check_updates_on_startup\":true}}").Clone();
    var parsed = SettingsStore.Parse(JsonSerializer.Serialize(settings, SettingsStore.JsonOptions));
    var global = JsonSerializer.SerializeToElement(parsed, SettingsStore.JsonOptions).GetProperty("global");
    Assert(global.TryGetProperty("update_source", out var source) && source.GetString() == "gitee", "Gitee preference lost in round trip");
    Assert(parsed.Global.CheckUpdatesOnStartup, "update source reset the startup check option");
});
Test("unrecognized imported update source falls back to automatic", () => {
    foreach (var value in new[] { "\"unknown\"", "99", "null" })
    {
        var settings = SettingsStore.Parse("{\"global\":{\"update_source\":" + value + "}}");
        var global = JsonSerializer.SerializeToElement(settings, SettingsStore.JsonOptions).GetProperty("global");
        Assert(global.TryGetProperty("update_source", out var source) && source.GetString() == "auto", "unexpected update source did not normalize");
    }
});
UpdateTests.Run(Test, Assert);
Environment.ExitCode = failed == 0 ? 0 : 1;
Console.WriteLine($"{total - failed}/{total} passed");
