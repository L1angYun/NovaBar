using System.Runtime.InteropServices;

namespace NovaBar.Core;

/// <summary>绘制任务栏/开始菜单的实现路线。</summary>
public enum BackendKind
{
    /// <summary>老路线：<c>SetWindowCompositionAttribute</c>（ACCENT_*）。Win10、Win11 21H2 有效。</summary>
    LegacyComposition = 0,

    /// <summary>新路线：XAML 可视化树注入。Win11 22H2+ 必须走这条。</summary>
    XamlTree,
}

/// <summary>
/// 系统识别与后端选择。NovaBar 要同时活在新旧两代 shell 上，这里集中处理差异。
/// </summary>
public static class PlatformSupport
{
    public sealed record WindowsVersion(int Major, int Minor, int Build, string DisplayName)
    {
        public bool IsWindows11 => Build >= 22000;

        public override string ToString() => $"{DisplayName} (Build {Build})";
    }

    public static WindowsVersion Current { get; } = Detect();

    /// <summary>Win11 22H2（Build 22000）起任务栏换成 XAML，老办法失效。</summary>
    public const int FirstXamlTaskbarBuild = 22000;

    /// <summary>26xxx 起任务栏外面多了 ScrollViewer 容器，祖先链不再固定。</summary>
    public const int FirstWrappedTaskbarBuild = 26200;

    public static BackendKind PickBackend(int build) =>
        build >= FirstXamlTaskbarBuild ? BackendKind.XamlTree : BackendKind.LegacyComposition;

    public static BackendKind PreferredBackend => PickBackend(Current.Build);

    /// <summary>该版本是否需要在认领根节点时沿祖先链向上找，而不是只比一层。</summary>
    public static bool RequiresAncestorWalk(int build) => build >= FirstWrappedTaskbarBuild;

    /// <summary>该版本的任务栏是否是 XAML 实现（决定是否要注入桥接 DLL）。</summary>
    public static bool HasXamlTaskbar(int build) => build >= FirstXamlTaskbarBuild;

    private static WindowsVersion Detect()
    {
        var info = new OsVersionInfo
        {
            Size = Marshal.SizeOf<OsVersionInfo>(),
        };

        string? display = null;
        int major = Environment.OSVersion.Version.Major;
        int minor = Environment.OSVersion.Version.Minor;
        int build = Environment.OSVersion.Version.Build;

        // Environment.OSVersion 在 Win11 上会撒谎（报 10.0.22000 以下），必须问 ntdll。
        if (OperatingSystem.IsWindows() && RtlGetVersion(ref info) == 0)
        {
            major = (int)info.MajorVersion;
            minor = (int)info.MinorVersion;
            build = (int)info.BuildNumber;
            display = info.CsdVersion;
        }

        var name = major >= 10 && build >= 22000
            ? "Windows 11"
            : major >= 10
                ? "Windows 10"
                : $"Windows {major}.{minor}";

        if (string.IsNullOrWhiteSpace(display) || display.Trim().Length == 0)
        {
            display = name;
        }

        return new WindowsVersion(major, minor, build, display);
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct OsVersionInfo
    {
        public int Size;
        public uint MajorVersion;
        public uint MinorVersion;
        public uint BuildNumber;
        public uint PlatformId;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string CsdVersion;

        public ushort SuiteMask;
        public byte ProductType;
        private readonly byte _reserved;
    }

    [DllImport("ntdll.dll", SetLastError = true)]
    private static extern int RtlGetVersion(ref OsVersionInfo versionInfo);
}
