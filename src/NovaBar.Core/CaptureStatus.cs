using System.Globalization;
using System.Text;

namespace NovaBar.Core;

public enum CaptureReadiness { Disconnected, Idle, Waiting, Ready, Unavailable }

public readonly record struct CaptureStatus(CaptureReadiness State, int ActiveTargets = 0, int ReadyTargets = 0, long ErrorCode = 0)
{
    public static string DefaultPath { get; } = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "NovaBar", "capture-status.txt");

    public string DisplayText => State switch {
        CaptureReadiness.Idle => "玻璃：空闲",
        CaptureReadiness.Waiting => "玻璃：正在读取背后画面",
        CaptureReadiness.Ready => "玻璃：实时画面就绪",
        CaptureReadiness.Unavailable => "玻璃：画面暂不可用",
        _ => "捕获：未连接",
    };

    public static CaptureStatus Parse(string? text, DateTimeOffset modifiedUtc, DateTimeOffset nowUtc)
    {
        var disconnected = new CaptureStatus(CaptureReadiness.Disconnected);
        var age = nowUtc - modifiedUtc;
        if (text is null || text.Length > 4096 || age < TimeSpan.Zero || age > TimeSpan.FromSeconds(3)) return disconnected;
        var fields = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var rawLine in text.TrimStart('\uFEFF').Split('\n'))
        {
            var line = rawLine.Trim();
            if (line.Length == 0) continue;
            var separator = line.IndexOf('=');
            if (separator <= 0) return disconnected;
            var key = line[..separator].Trim();
            if (key is not ("state" or "active" or "ready" or "error")) continue;
            if (!fields.TryAdd(key, line[(separator + 1)..].Trim())) return disconnected;
        }
        if (!fields.TryGetValue("state", out var stateText)
            || !fields.TryGetValue("active", out var activeText) || !int.TryParse(activeText, NumberStyles.Integer, CultureInfo.InvariantCulture, out var active)
            || !fields.TryGetValue("ready", out var readyText) || !int.TryParse(readyText, NumberStyles.Integer, CultureInfo.InvariantCulture, out var ready)
            || !fields.TryGetValue("error", out var errorText) || !long.TryParse(errorText, NumberStyles.Integer, CultureInfo.InvariantCulture, out var error)
            || active < 0 || ready < 0 || ready > active || error < int.MinValue || error > uint.MaxValue) return disconnected;
        var state = stateText switch {
            "idle" when active == 0 && ready == 0 => CaptureReadiness.Idle,
            "waiting" => CaptureReadiness.Waiting,
            "ready" when active > 0 && ready == active => CaptureReadiness.Ready,
            "unavailable" => CaptureReadiness.Unavailable,
            _ => CaptureReadiness.Disconnected,
        };
        return new CaptureStatus(state, active, ready, error);
    }

    public static CaptureStatus Read(string? path = null, DateTimeOffset? nowUtc = null)
    {
        try
        {
            var file = path ?? DefaultPath;
            var modifiedUtc = new DateTimeOffset(File.GetLastWriteTimeUtc(file));
            using var stream = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            if (stream.Length > 4096) return new(CaptureReadiness.Disconnected);
            using var reader = new StreamReader(stream, Encoding.UTF8, detectEncodingFromByteOrderMarks: true);
            return Parse(reader.ReadToEnd(), modifiedUtc, nowUtc ?? DateTimeOffset.UtcNow);
        }
        catch (IOException) { return new(CaptureReadiness.Disconnected); }
        catch (UnauthorizedAccessException) { return new(CaptureReadiness.Disconnected); }
        catch (System.Security.SecurityException) { return new(CaptureReadiness.Disconnected); }
    }
}
