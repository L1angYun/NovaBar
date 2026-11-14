namespace NovaBar.Core;

/// <summary>强调类型。前五种与 TranslucentTB 完全一致，<see cref="LiquidGlass"/> 是 NovaBar 新增。</summary>
public enum AccentKind
{
    /// <summary>系统默认（不做任何改动）。</summary>
    Normal = 0,

    /// <summary>纯色不透明。</summary>
    Opaque,

    /// <summary>全透明。</summary>
    Clear,

    /// <summary>高斯模糊。</summary>
    Blur,

    /// <summary>亚克力（模糊 + 噪声 + 色调）。</summary>
    Acrylic,

    /// <summary>液态玻璃：折射 + 高光 + 边缘色散。</summary>
    LiquidGlass,
}

/// <summary>可被美化的 shell 界面。</summary>
public enum SurfaceKind
{
    Taskbar = 0,
    StartMenu,
    NotificationCenter,
}

/// <summary>外观状态。数字越小优先级越低，决策时取命中的最高优先级。</summary>
public enum AppearanceState
{
    Desktop = 0,
    VisibleWindow,
    MaximizedWindow,
    StartOpened,
    SearchOpened,
    TaskViewOpened,
    BatterySaver,
}

/// <summary>规则匹配维度。</summary>
public enum RuleMatchKind
{
    ProcessName = 0,
    WindowTitle,
    WindowClass,
}
