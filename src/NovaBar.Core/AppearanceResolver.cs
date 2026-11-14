namespace NovaBar.Core;

/// <summary>
/// 决策所需的 shell 现状快照。由 Engine 采集（前台窗口、开始菜单/搜索/任务视图是否打开、是否省电）。
/// 纯数据，便于单元测试。
/// </summary>
public readonly record struct ShellContext
{
    public bool StartOpened { get; init; }

    public bool SearchOpened { get; init; }

    public bool TaskViewOpened { get; init; }

    public bool BatterySaver { get; init; }

    /// <summary>当前显示器上是否有可见窗口。</summary>
    public bool HasVisibleWindow { get; init; }

    /// <summary>该可见窗口是否最大化。</summary>
    public bool IsMaximized { get; init; }

    /// <summary>该窗口是否是前台窗口（决定规则里 active / inactive 的取舍）。</summary>
    public bool WindowIsForeground { get; init; }

    public string? ProcessName { get; init; }

    public string? WindowTitle { get; init; }

    public string? WindowClass { get; init; }
}

/// <summary>一次决策的结果。</summary>
public readonly record struct AppearanceResolution(
    SurfaceAppearance Appearance,
    AppearanceState State,
    string? MatchedRule)
{
    public bool IsFromRule => MatchedRule is not null;
}

/// <summary>
/// 状态优先级 + 规则匹配的决策器。<see cref="AppearanceState"/> 的枚举值即优先级（越大越优先）。
/// </summary>
public static class AppearanceResolver
{
    /// <summary>按优先级解析当前应生效的外观。返回 <c>null</c> 表示这个界面被禁用、不该动它。</summary>
    public static AppearanceResolution? Resolve(SurfaceConfig config, ShellContext context)
    {
        ArgumentNullException.ThrowIfNull(config);

        if (!config.Enabled)
        {
            return null;
        }

        // 1) 覆盖型状态：省电 > 任务视图 > 搜索 > 开始菜单
        foreach (var state in new[]
                 {
                     AppearanceState.BatterySaver,
                     AppearanceState.TaskViewOpened,
                     AppearanceState.SearchOpened,
                     AppearanceState.StartOpened,
                 })
        {
            if (!IsActiveState(state, context))
            {
                continue;
            }

            if (config.States.TryGetValue(state, out var entry) && entry.Enabled)
            {
                return new AppearanceResolution(entry.Active, state, null);
            }
        }

        // 2) 窗口状态（可被规则覆盖）
        if (context.HasVisibleWindow && !IsIgnored(config, context))
        {
            var state = context.IsMaximized ? AppearanceState.MaximizedWindow : AppearanceState.VisibleWindow;

            var rule = MatchRule(config, context);
            if (rule is { } matched)
            {
                var key = $"{matched.Kind}:{matched.Pattern}";
                var appearance = Pick(matched.Target, context.WindowIsForeground);
                return new AppearanceResolution(appearance, state, key);
            }

            if (context.IsMaximized && config.States.TryGetValue(AppearanceState.MaximizedWindow, out var maximized) && maximized.Enabled)
            {
                return new AppearanceResolution(Pick(maximized, context.WindowIsForeground), AppearanceState.MaximizedWindow, null);
            }
            if (config.States.TryGetValue(AppearanceState.VisibleWindow, out var entry) && entry.Enabled)
            {
                return new AppearanceResolution(Pick(entry, context.WindowIsForeground), AppearanceState.VisibleWindow, null);
            }
        }

        // 3) 兜底：桌面
        var desktop = config.State(AppearanceState.Desktop);
        return desktop.Enabled ? new AppearanceResolution(desktop.Active, AppearanceState.Desktop, null) : null;
    }

    /// <summary>判断某个状态在当前快照下是否成立。</summary>
    public static bool IsActiveState(AppearanceState state, ShellContext context) => state switch
    {
        AppearanceState.BatterySaver => context.BatterySaver,
        AppearanceState.TaskViewOpened => context.TaskViewOpened,
        AppearanceState.SearchOpened => context.SearchOpened,
        AppearanceState.StartOpened => context.StartOpened,
        AppearanceState.MaximizedWindow => context.HasVisibleWindow && context.IsMaximized,
        AppearanceState.VisibleWindow => context.HasVisibleWindow,
        AppearanceState.Desktop => true,
        _ => false,
    };

    /// <summary>命中忽略列表的窗口按桌面处理。</summary>
    public static bool IsIgnored(SurfaceConfig config, ShellContext context)
    {
        var filter = config.IgnoredWindows;
        return Matches(filter.ProcessName, context.ProcessName)
            || Matches(filter.WindowTitle, context.WindowTitle)
            || Matches(filter.WindowClass, context.WindowClass);

        static bool Matches(List<string> patterns, string? value)
        {
            if (string.IsNullOrEmpty(value))
            {
                return false;
            }

            foreach (var pattern in patterns)
            {
                if (!string.IsNullOrEmpty(pattern)
                    && value.Contains(pattern, StringComparison.OrdinalIgnoreCase))
                {
                    return true;
                }
            }

            return false;
        }
    }

    /// <summary>命中规则时返回规则与匹配到的模式串。</summary>
    public static (RuleMatchKind Kind, string Pattern, RuleTarget Target)? MatchRule(
        SurfaceConfig config,
        ShellContext context)
    {
        var state = context.IsMaximized ? AppearanceState.MaximizedWindow : AppearanceState.VisibleWindow;
        if (config.States.TryGetValue(state, out var entry) && MatchRule(entry.Rules, context) is { } scoped)
            return scoped;
        return MatchRule(config.Rules, context);
    }

    private static (RuleMatchKind Kind, string Pattern, RuleTarget Target)? MatchRule(SurfaceRules? rules, ShellContext context)
    {
        if (rules is null || rules.IsEmpty)
        {
            return null;
        }

        if (Find(rules.ProcessName, context.ProcessName) is { } byProcess)
        {
            return (RuleMatchKind.ProcessName, byProcess.Key, byProcess.Value);
        }

        if (Find(rules.WindowTitle, context.WindowTitle) is { } byTitle)
        {
            return (RuleMatchKind.WindowTitle, byTitle.Key, byTitle.Value);
        }

        if (Find(rules.WindowClass, context.WindowClass) is { } byClass)
        {
            return (RuleMatchKind.WindowClass, byClass.Key, byClass.Value);
        }

        return null;

        static KeyValuePair<string, RuleTarget>? Find(Dictionary<string, RuleTarget> rules, string? value)
        {
            if (string.IsNullOrEmpty(value))
            {
                return null;
            }

            foreach (var pair in rules)
            {
                if (!string.IsNullOrEmpty(pair.Key)
                    && value.Contains(pair.Key, StringComparison.OrdinalIgnoreCase))
                {
                    return pair;
                }
            }

            return null;
        }
    }

    private static SurfaceAppearance Pick(StateAppearance state, bool foreground) =>
        foreground || state.Inactive is null ? state.Active : state.Inactive;

    private static SurfaceAppearance Pick(RuleTarget target, bool foreground) =>
        foreground || target.Inactive is null ? target.Active : target.Inactive;
}
