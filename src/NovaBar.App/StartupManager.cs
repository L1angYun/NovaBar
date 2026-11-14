using System.Diagnostics;
using Microsoft.Win32;

namespace NovaBar.App;

/// <summary>
/// 开机自启。注册的是**引擎**而不是界面 —— 登录后要的是外观立刻生效，
/// 配置中心用户想开再开。
/// </summary>
public static class StartupManager
{
    private const string RunKeyPath = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string ValueName = "NovaBar.Engine";

    public static bool IsEnabled
    {
        get
        {
            using var key = Registry.CurrentUser.OpenSubKey(RunKeyPath, writable: false);
            return key?.GetValue(ValueName) is string value && value.Length > 0;
        }
    }

    /// <summary>开启/关闭自启。返回实际写入的命令行；失败返回 null。</summary>
    public static string? SetEnabled(bool enabled)
    {
        var target = enabled ? FindEngine() : null;
        if (enabled && target is null) return null;

        using var key = Registry.CurrentUser.OpenSubKey(RunKeyPath, writable: true)
                        ?? Registry.CurrentUser.CreateSubKey(RunKeyPath, writable: true);

        if (!enabled)
        {
            key.DeleteValue(ValueName, throwOnMissingValue: false);
            return string.Empty;
        }

        var command = $"\"{target}\"";
        key.SetValue(ValueName, command, RegistryValueKind.String);
        return command;
    }

    private static string? FindEngine()
    {
        var local = Path.Combine(AppContext.BaseDirectory, "NovaBar.Engine.exe");
        if (File.Exists(local))
        {
            return local;
        }

        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir is not null)
        {
            var candidate = Path.Combine(dir.FullName, "artifacts", "native", "NovaBar.Engine.exe");
            if (File.Exists(candidate))
            {
                return candidate;
            }

            dir = dir.Parent;
        }

        return null;
    }
}
