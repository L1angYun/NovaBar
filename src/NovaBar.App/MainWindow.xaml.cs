using System.Collections.ObjectModel;
using System.Diagnostics;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;
using NovaBar.Core;
using Windows.UI;
using Windows.Storage.Pickers;

namespace NovaBar.App;

public sealed partial class MainWindow : Window
{
    private readonly ObservableCollection<string> _ruleItems = new();
    private readonly List<(RuleMatchKind Kind, string Pattern)> _rules = new();
    private readonly ObservableCollection<string> _ignoredItems = new();
    private readonly List<(RuleMatchKind Kind, string Pattern)> _ignored = new();
    private readonly DispatcherTimer _applyTimer = new() { Interval = TimeSpan.FromMilliseconds(700) };
    private readonly DispatcherTimer _statusTimer = new() { Interval = TimeSpan.FromSeconds(2) };
    private readonly SemaphoreSlim _dialogGate = new(1, 1);

    private bool _dirty;
    private bool _appearanceDirty;
    private bool _suppressStartupToggle;
    private bool _exitRequested;
    private bool _closed;
    private bool _editingInactive;
    private (AppearanceState? Scope, RuleMatchKind Kind, string Pattern)? _editingRule;
    private AppearanceState? _ruleScope;
    private string? _loadError;

    private NovaBarSettings _settings = NovaBarSettings.CreateDefault();
    private bool _loading = true;
    private SurfaceKind _surface = SurfaceKind.Taskbar;
    private AppearanceState _state = AppearanceState.Desktop;

    public MainWindow(bool installerUpdate = false)
    {
        InitializeComponent();

        Title = "NovaBar";
        TrySetWindowIcon();
        RootGrid.Loaded += OnRootLoaded;

        LoadSettings();
        BuildSurfaceToggles();
        BuildPresetButtons();

        RuleList.ItemsSource = _ruleItems;
        IgnoredList.ItemsSource = _ignoredItems;

        _loading = false;
        RefreshFromModel();
        ReloadRulesFromModel();
        UpdateAbout();

        // 更新重启由安装助手恢复原先的引擎/还原状态，避免覆盖用户主动关闭的效果。
        try
        {
            if (_settings.Global.AutoApply && !installerUpdate)
            {
                EngineConfigWriter.Write(_settings);
                var engine = FindEngine();
                if (engine is null || !EnsureEngineRunning(engine))
                    SaveStatusText.Text = "配置已下发，但引擎未能启动。请检查安装文件后重试。";
            }
        }
        catch (Exception ex)
        {
            SaveStatusText.Text = $"启动时未能应用配置：{ex.Message}";
        }

        UpdateEngineStatus();
        StartStatusTimer();

        // 改一下就自动下发，不用每次点「应用到系统」
        _applyTimer.Tick += (_, _) => FlushIfDirty();
        _applyTimer.Start();

        SetupTrayIcon();
        AppWindow.Closing += OnWindowClosing;
        Closed += OnWindowClosed;
    }

    private NovaBar.Tray.TrayIcon? _tray;

    private void SetupTrayIcon()
    {
        if (!_settings.Global.ShowTrayIcon)
        {
            _tray?.Dispose();
            _tray = null;
            return;
        }
        if (_tray is not null) return;
        try
        {
            var iconPath = Path.Combine(AppContext.BaseDirectory, "Assets", "NovaBar.ico");
            _tray = new NovaBar.Tray.TrayIcon(iconPath, "NovaBar — 液态玻璃", ActivateFromTray, RestoreFromTray, CloseFromTray);
        }
        catch (Exception ex)
        {
            SaveStatusText.Text = $"托盘不可用，关闭窗口将退出配置中心：{ex.Message}";
        }
    }

    private void ActivateFromTray()
    {
        AppWindow.Show();
        if (AppWindow.Presenter is OverlappedPresenter presenter
            && presenter.State == OverlappedPresenterState.Minimized)
        {
            presenter.Restore();
        }

        Activate();
    }

    private void RestoreFromTray()
    {
        try
        {
            var engine = FindEngine();
            if (engine is null)
            {
                return;
            }

            using var process = Process.Start(new ProcessStartInfo(engine, "--restore")
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                WorkingDirectory = Path.GetDirectoryName(engine)!,
            });

            UpdateEngineStatus();
        }
        catch (Exception ex)
        {
            _ = ShowDialogAsync("恢复失败", ex.Message);
        }
    }

    private void CloseFromTray() { _exitRequested = true; Close(); }
    private void OnExitClick(object sender, RoutedEventArgs e) => CloseFromTray();

    private void OnWindowClosing(AppWindow sender, AppWindowClosingEventArgs args)
    {
        if (!FlushIfDirty())
        {
            args.Cancel = true;
            _exitRequested = false;
            ActivateFromTray();
            _ = ShowDialogAsync("配置尚未保存", SaveStatusText.Text);
            return;
        }
        if (!_exitRequested && _tray is not null && _settings.Global.CloseToTray)
        {
            args.Cancel = true;
            AppWindow.Hide();
        }
    }

    private void OnWindowClosed(object sender, WindowEventArgs args)
    {
        _closed = true;
        StopUpdateRequests();
        _applyTimer.Stop();
        _statusTimer.Stop();
        AppWindow.Closing -= OnWindowClosing;
        _tray?.Dispose();
        _tray = null;
    }

    private RuleTarget? EditedRuleTarget => _editingRule is { } rule ? RulesFor(rule.Kind, rule.Scope).GetValueOrDefault(rule.Pattern) : null;
    private SurfaceAppearance CurrentAppearance => _editingInactive
        ? EditedRuleTarget is { } target ? target.Inactive ?? target.Active : CurrentState.Inactive ?? CurrentState.Active
        : EditedRuleTarget?.Active ?? CurrentState.Active;
    private StateAppearance CurrentState => _settings.Surface(_surface).State(_state);
    private SurfaceAppearance? CurrentInactive => EditedRuleTarget is { } target ? target.Inactive : CurrentState.Inactive;

    private void SetCurrentAppearance(SurfaceAppearance appearance)
    {
        if (EditedRuleTarget is { } target)
        {
            if (_editingInactive) target.Inactive = appearance; else target.Active = appearance;
        }
        else if (_editingInactive) CurrentState.Inactive = appearance;
        else CurrentState.Active = appearance;
    }

    private void TrySetWindowIcon()
    {
        try
        {
            var path = Path.Combine(AppContext.BaseDirectory, "Assets", "NovaBar.ico");
            if (File.Exists(path))
            {
                AppWindow.SetIcon(path);
            }
        }
        catch
        {
            // 图标设不上不影响主流程
        }
    }

    /// <summary>
    /// 按工作区比例定尺寸并居中。必须先等 XamlRoot 就绪才能拿到 RasterizationScale ——
    /// DisplayArea 给的是 DIP，而 AppWindow.Resize 吃的是物理像素，混用会得到一半大小的窗口。
    /// </summary>
    private void OnRootLoaded(object sender, RoutedEventArgs e)
    {
        RootGrid.Loaded -= OnRootLoaded;
        if (_loadError is { } error) { _loadError = null; _ = ShowDialogAsync("配置读取失败", error); }

        var area = DisplayArea.GetFromWindowId(AppWindow.Id, DisplayAreaFallback.Primary);
        var scale = RootGrid.XamlRoot?.RasterizationScale ?? 1.0;

        // WorkArea 与 AppWindow.Resize 都是物理像素；只有 RasterizationScale 用来把 DIP 目标换算过去。
        var work = area.WorkArea;

        var width = (int)Math.Min(1400 * scale, work.Width * 0.82);
        var height = (int)Math.Min(900 * scale, work.Height * 0.86);

        width = Math.Max(width, (int)(1000 * scale));
        height = Math.Max(height, (int)(700 * scale));

        AppWindow.Resize(new Windows.Graphics.SizeInt32(width, height));
        AppWindow.Move(new Windows.Graphics.PointInt32(
            work.X + ((work.Width - width) / 2),
            work.Y + ((work.Height - height) / 2)));
        if (_settings.Global.CheckUpdatesOnStartup) _ = CheckUpdatesAsync(startup: true);
    }

    private void LoadSettings()
    {
        try
        {
            _settings = SettingsStore.Load();
        }
        catch (Exception ex)
        {
            _settings = NovaBarSettings.CreateDefault();
            _loadError = $"已回退到默认配置。原文件未修改。\n\n{ex.Message}";
        }
    }

    // ───────────────────────── 界面构建 ─────────────────────────

    private void BuildSurfaceToggles()
    {
        SurfaceToggleHost.Children.Clear();

        foreach (var kind in Enum.GetValues<SurfaceKind>())
        {
            var config = _settings.Surface(kind);
            var toggle = new ToggleSwitch
            {
                Header = Describe(kind),
                IsOn = config.Enabled,
                Tag = kind,
                Margin = new Thickness(0, 0, 0, 4),
            };

            toggle.Toggled += (sender, _) =>
            {
                if (_loading) return;
                var cfg = _settings.Surface((SurfaceKind)((ToggleSwitch)sender).Tag);
                cfg.Enabled = ((ToggleSwitch)sender).IsOn;
                MarkDirty();
            };

            SurfaceToggleHost.Children.Add(toggle);
        }
    }

    private void BuildPresetButtons()
    {
        PresetHost.Children.Clear();

        foreach (var preset in PresetLibrary.All)
        {
            var button = new Button
            {
                Content = preset.Name,
                Tag = preset,
                IsEnabled = _surface == SurfaceKind.Taskbar || preset.Appearance.Accent != AccentKind.Blur,
                Padding = new Thickness(12, 6, 12, 6),
            };

            ToolTipService.SetToolTip(button, preset.Description);
            button.Click += OnPresetClick;
            PresetHost.Children.Add(button);
        }
    }

    private void RefreshFromModel()
    {
        _loading = true;

        SurfaceBox.Items.Clear();
        foreach (var kind in Enum.GetValues<SurfaceKind>())
        {
            SurfaceBox.Items.Add(Describe(kind));
        }

        SurfaceBox.SelectedIndex = (int)_surface;

        StateBox.Items.Clear();
        foreach (var state in Enum.GetValues<AppearanceState>())
        {
            StateBox.Items.Add(Describe(state));
        }

        StateBox.SelectedIndex = Array.IndexOf(Enum.GetValues<AppearanceState>(), _state);

        var inactiveSupported = _editingRule is not null || _state is AppearanceState.VisibleWindow or AppearanceState.MaximizedWindow;
        if (!inactiveSupported) _editingInactive = false;
        var appearance = CurrentAppearance;
        StateEnabledToggle.IsOn = CurrentState.Enabled;
        StateEnabledToggle.Visibility = _editingRule is null ? Visibility.Visible : Visibility.Collapsed;
        AppearanceRoleBox.SelectedIndex = _editingInactive ? 1 : 0;
        AppearanceRoleBox.IsEnabled = inactiveSupported;
        InactiveToggle.IsOn = CurrentInactive is not null;
        InactiveToggle.IsEnabled = inactiveSupported;
        AccentBox.SelectedIndex = (int)appearance.Accent;
        ShowLineToggle.IsOn = appearance.ShowLine;
        ShowPeekToggle.IsOn = appearance.ShowPeek;
        UpdateAppearanceCapabilities(appearance);
        AppearanceEditor.IsEnabled = !_editingInactive || CurrentInactive is not null;
        EditorContextText.Text = _editingRule is { } rule ? $"正在编辑规则：{DescribeRuleScope(rule.Scope)} · {Describe(rule.Kind)} · {rule.Pattern}" : $"正在编辑：{Describe(_surface)} · {Describe(_state)}";
        ReturnToStateButton.Visibility = _editingRule is null ? Visibility.Collapsed : Visibility.Visible;
        TrayToggle.IsOn = _settings.Global.ShowTrayIcon;
        CloseToTrayToggle.IsOn = _settings.Global.CloseToTray;
        CloseToTrayToggle.IsEnabled = _settings.Global.ShowTrayIcon;
        AutoApplyToggle.IsOn = _settings.Global.AutoApply;
        DisableSavingToggle.IsOn = _settings.Global.DisableSaving;
        CheckUpdatesOnStartupToggle.IsOn = _settings.Global.CheckUpdatesOnStartup;
        RefreshUpdateSourceSelection();
        SldBlur.Value = appearance.BlurRadius;
        SldRefraction.Value = appearance.Glass.Refraction;
        SldSpecular.Value = appearance.Glass.Specular;
        SldDispersion.Value = appearance.Glass.EdgeDispersion;
        SldNoise.Value = appearance.Glass.Noise;
        SldTint.Value = appearance.Glass.TintStrength;
        ColorBox.Text = appearance.Color.ToString();
        RuleScopeBox.SelectedIndex = _ruleScope switch { AppearanceState.VisibleWindow => 1, AppearanceState.MaximizedWindow => 2, _ => 0 };
        RuleSurfaceText.Text = $"当前规则界面：{Describe(_surface)} · {DescribeRuleScope(_ruleScope)}（界面可在外观页切换）";
        BuildPresetButtons();

        _loading = false;
        UpdatePreview();
    }

    private void UpdateAppearanceCapabilities(SurfaceAppearance appearance)
    {
        var taskbar = _surface == SurfaceKind.Taskbar;
        var liquid = appearance.Accent == AccentKind.LiquidGlass;
        ShowPeekToggle.IsEnabled = false;
        ((ComboBoxItem)AccentBox.Items[(int)AccentKind.Blur]).IsEnabled = taskbar;
        ((ComboBoxItem)AccentBox.Items[(int)AccentKind.Blur]).Content = taskbar ? "模糊 / Blur" : "模糊 / Blur（导入值回退亚克力）";
        ((ComboBoxItem)AccentBox.Items[(int)AccentKind.LiquidGlass]).Content = "液态玻璃 / LiquidGlass";
        ShowLineToggle.Header = taskbar ? "显示任务栏线条" : "保留系统边框";
        SldBlur.IsEnabled = liquid;
        ColorBox.IsEnabled = appearance.Accent is not AccentKind.Normal and not AccentKind.Clear;
        ShowLineToggle.IsEnabled = appearance.Accent != AccentKind.Normal;
        SldRefraction.IsEnabled = liquid;
        SldDispersion.IsEnabled = liquid;
        SldNoise.IsEnabled = liquid;
        SldSpecular.IsEnabled = liquid;
        SldTint.IsEnabled = liquid;
        CapabilityText.Text = taskbar
            ? "液态玻璃支持模糊、折射、高光、色散、噪声与色调调节；Blur / Acrylic 的模糊由系统管理。桌面预览按钮与圆角由系统管理；非活动外观用于窗口状态与规则。"
            : "此界面独立保存完整液态玻璃参数，支持模糊、折射、高光、色散、噪声与色调调节。Acrylic 与导入的 Blur 使用系统模糊；圆角由系统管理，非活动外观用于窗口状态与规则。";
    }

    // ───────────────────────── 事件 ─────────────────────────

    private void OnNavSelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        var tag = (args.SelectedItemContainer as NavigationViewItem)?.Tag as string ?? "appearance";

        PageAppearance.Visibility = tag == "appearance" ? Visibility.Visible : Visibility.Collapsed;
        PageSurfaces.Visibility = tag == "surfaces" ? Visibility.Visible : Visibility.Collapsed;
        PageRules.Visibility = tag == "rules" ? Visibility.Visible : Visibility.Collapsed;
        PageAbout.Visibility = tag == "about" ? Visibility.Visible : Visibility.Collapsed;
    }

    private void OnSurfaceChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading)
        {
            return;
        }

        _surface = (SurfaceKind)Math.Max(0, SurfaceBox.SelectedIndex);
        _editingRule = null;
        _loading = true;
        RefreshFromModel();
        _loading = false;
        ReloadRulesFromModel();
    }

    private void OnStateChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading)
        {
            return;
        }

        var values = Enum.GetValues<AppearanceState>();
        _state = values[Math.Clamp(StateBox.SelectedIndex, 0, values.Length - 1)];
        _editingRule = null;
        _loading = true;
        RefreshFromModel();
        _loading = false;
    }

    private void OnStateEnabledToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentState.Enabled = StateEnabledToggle.IsOn;
        MarkDirty();
    }

    private void OnAppearanceRoleChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading) return;
        _editingInactive = AppearanceRoleBox.SelectedIndex == 1;
        RefreshFromModel();
    }

    private void OnInactiveToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        var target = EditedRuleTarget;
        if (target is not null) target.Inactive = InactiveToggle.IsOn ? target.Active.Clone() : null;
        else CurrentState.Inactive = InactiveToggle.IsOn ? CurrentState.Active.Clone() : null;
        MarkDirty();
        RefreshFromModel();
    }

    private void OnAccentChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading || AccentBox.SelectedIndex < 0) return;
        CurrentAppearance.Accent = (AccentKind)AccentBox.SelectedIndex;
        UpdateAppearanceCapabilities(CurrentAppearance);
        MarkDirty();
        UpdatePreview();
    }

    private void OnAppearanceToggleChanged(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentAppearance.ShowLine = ShowLineToggle.IsOn;
        if (ShowPeekToggle.IsEnabled) CurrentAppearance.ShowPeek = ShowPeekToggle.IsOn;
        MarkDirty();
        UpdatePreview();
    }

    private void OnReturnToStateClick(object sender, RoutedEventArgs e)
    {
        _editingRule = null;
        RefreshFromModel();
    }

    private void OnGlobalToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _settings.Global.ShowTrayIcon = TrayToggle.IsOn;
        _settings.Global.CloseToTray = CloseToTrayToggle.IsOn;
        _settings.Global.AutoApply = AutoApplyToggle.IsOn;
        _settings.Global.DisableSaving = DisableSavingToggle.IsOn;
        _settings.Global.CheckUpdatesOnStartup = CheckUpdatesOnStartupToggle.IsOn;
        if (!_settings.Global.CheckUpdatesOnStartup) StartupUpdateNotice.IsOpen = false;
        CloseToTrayToggle.IsEnabled = TrayToggle.IsOn;
        SetupTrayIcon();
        MarkDirty(applyAppearance: !ReferenceEquals(sender, CheckUpdatesOnStartupToggle));
    }

    private void OnSliderChanged(object sender, RangeBaseValueChangedEventArgs e)
    {
        if (_loading)
        {
            return;
        }

        var appearance = CurrentAppearance;
        appearance.BlurRadius = SldBlur.Value;
        appearance.Glass.Refraction = SldRefraction.Value;
        appearance.Glass.Specular = SldSpecular.Value;
        appearance.Glass.EdgeDispersion = SldDispersion.Value;
        appearance.Glass.Noise = SldNoise.Value;
        appearance.Glass.TintStrength = SldTint.Value;

        MarkDirty();
        UpdatePreview();
    }

    private void OnColorChanged(object sender, TextChangedEventArgs e)
    {
        if (_loading)
        {
            return;
        }

        if (ArgbColor.TryParse(ColorBox.Text, out var color))
        {
            CurrentAppearance.Color = color;
            MarkDirty();
            UpdatePreview();
        }
    }

    private void OnPresetClick(object sender, RoutedEventArgs e)
    {
        if (sender is Button { Tag: AppearancePreset preset })
        {
            SetCurrentAppearance(preset.Appearance.Clone());
            RefreshFromModel();
            MarkDirty();
        }
    }

    private async void OnApplyClick(object sender, RoutedEventArgs e)
    {
        try
        {
            if (!ValidateColor()) return;
            MarkDirty();
            if (!FlushIfDirty()) { await ShowDialogAsync("保存失败", SaveStatusText.Text); return; }
            EngineConfigWriter.Write(_settings);

            var engine = FindEngine();
            if (engine is null)
            {
                await ShowDialogAsync(
                    "找不到引擎",
                    "没有找到 NovaBar.Engine.exe。\n\n先运行 build\\native-build.ps1 构建原生部分。");
                return;
            }

            if (!EnsureEngineRunning(engine))
            {
                await ShowDialogAsync("引擎启动失败", "NovaBar.Engine.exe 没能启动，请检查是否被拦截。");
                return;
            }

            UpdateEngineStatus();

            await ShowDialogAsync(
                "已应用",
                "配置已下发。引擎会按当前窗口状态自动切换外观。\n\n若外观没有变化，请查看引擎日志确认桥接连接状态。");
        }
        catch (Exception ex)
        {
            await ShowDialogAsync("应用失败", ex.Message);
        }
    }

    // ───────────────────────── 引擎对接 ─────────────────────────

    private static string? FindEngine()
    {
        var local = Path.Combine(AppContext.BaseDirectory, "NovaBar.Engine.exe");
        if (File.Exists(local))
        {
            return local;
        }

        // 开发时从 bin 目录向上找 artifacts\native
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

    private static bool EnsureEngineRunning(string enginePath)
    {
        if (IsEngineRunning(enginePath))
        {
            return true;
        }

        var startInfo = new ProcessStartInfo(enginePath)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            WorkingDirectory = Path.GetDirectoryName(enginePath)!,
        };

        using var process = Process.Start(startInfo);
        return process is not null;
    }

    private static bool IsEngineRunning(string? expectedPath = null)
    {
        var processes = Process.GetProcessesByName("NovaBar.Engine");
        try
        {
            if (expectedPath is null) return processes.Length > 0;
            expectedPath = Path.GetFullPath(expectedPath);
            foreach (var process in processes)
            {
                try
                {
                    if (string.Equals(process.MainModule?.FileName, expectedPath, StringComparison.OrdinalIgnoreCase))
                        return true;
                }
                catch (System.ComponentModel.Win32Exception) { }
                catch (InvalidOperationException) { }
            }
            return false;
        }
        finally { foreach (var process in processes) process.Dispose(); }
    }

    private void MarkDirty(bool applyAppearance = true)
    {
        _dirty = true;
        _appearanceDirty |= applyAppearance;
        SaveStatusText.Text = _settings.Global.DisableSaving ? "修改仅在本次会话中保留。" : "修改待保存…";
    }

    private bool FlushIfDirty()
    {
        if (!_dirty || _loading || _closed)
        {
            return true;
        }
        try
        {
            if (_appearanceDirty && !ValidateColor()) return false;
            SettingsStore.Save(_settings);
            if (_appearanceDirty && _settings.Global.AutoApply) EngineConfigWriter.Write(_settings);
            var appearanceChanged = _appearanceDirty;
            _dirty = false;
            _appearanceDirty = false;
            SaveStatusText.Text = _settings.Global.DisableSaving
                ? "本次会话已更新；配置文件未保存。"
                : !appearanceChanged ? "已保存。" : _settings.Global.AutoApply ? "已保存并下发到引擎。" : "已保存；点击应用到系统后生效。";
            return true;
        }
        catch (Exception ex)
        {
            SaveStatusText.Text = $"保存或下发失败，修改仍保留，稍后重试：{ex.Message}";
            return false;
        }
    }

    private bool ValidateColor()
    {
        if (ArgbColor.TryParse(ColorBox.Text, out _)) return true;
        SaveStatusText.Text = "颜色格式无效，请输入 #AARRGGBB 或 #RRGGBB。";
        return false;
    }

    private void UpdateEngineStatus()
    {
        var running = IsEngineRunning();

        EngineStatusText.Text = running ? "引擎：运行中" : "引擎：未运行";
        EngineStatusText.Foreground = running
            ? new SolidColorBrush(Color.FromArgb(255, 16, 124, 16))
            : new SolidColorBrush(Color.FromArgb(255, 157, 93, 0));
        var capture = CaptureStatus.Read();
        CaptureStatusText.Text = capture.DisplayText;
        CaptureStatusText.Foreground = new SolidColorBrush(capture.State switch {
            CaptureReadiness.Ready => Color.FromArgb(255, 16, 124, 16),
            CaptureReadiness.Waiting or CaptureReadiness.Unavailable => Color.FromArgb(255, 157, 93, 0),
            _ => Color.FromArgb(255, 96, 96, 96),
        });
    }

    private void StartStatusTimer()
    {
        _statusTimer.Tick += (_, _) => UpdateEngineStatus();
        _statusTimer.Start();
    }

    private async void OnSaveClick(object sender, RoutedEventArgs e)
    {
        MarkDirty();
        if (!FlushIfDirty()) { await ShowDialogAsync("保存失败", SaveStatusText.Text); return; }
        await ShowDialogAsync(_settings.Global.DisableSaving ? "仅本次会话" : "已保存", _settings.Global.DisableSaving ? "当前已关闭配置保存，修改只在本次会话中保留。可在关于中重新启用保存。" : $"配置已写入：\n{SettingsStore.DefaultPath}");
    }

    private async void OnImportClick(object sender, RoutedEventArgs e)
    {
        try
        {
            var path = await PickJsonAsync();
            if (path is null) return;
            var result = TtbImporter.ImportFile(path);
            _settings.Surfaces[SurfaceKind.Taskbar] = result.Settings.Surface(SurfaceKind.Taskbar);
            SettingsStore.Normalize(_settings);
            _editingRule = null;
            RefreshFromModel();
            BuildSurfaceToggles();
            ReloadRulesFromModel();
            MarkDirty();

            var extra = result.Warnings.Count == 0
                ? string.Empty
                : $"\n\n{result.Warnings.Count} 条提示：\n" + string.Join('\n', result.Warnings.Take(5));

            await ShowDialogAsync("导入完成", $"来自：\n{path}{extra}");
        }
        catch (Exception ex)
        {
            await ShowDialogAsync("导入失败", ex.Message);
        }
    }

    private async Task<string?> PickJsonAsync()
    {
        var picker = new FileOpenPicker();
        picker.FileTypeFilter.Add(".json");
        WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(this));
        return (await picker.PickSingleFileAsync())?.Path;
    }

    private async void OnImportJsonClick(object sender, RoutedEventArgs e)
    {
        try
        {
            var path = await PickJsonAsync();
            if (path is null) return;
            var imported = SettingsStore.Parse(File.ReadAllText(path));
            _settings = imported;
            _editingRule = null;
            RefreshFromModel();
            ReloadRulesFromModel();
            BuildSurfaceToggles();
            SetupTrayIcon();
            MarkDirty();
            await ShowDialogAsync("导入完成", $"已载入并排队保存 / 应用：\n{path}");
        }
        catch (Exception ex) { await ShowDialogAsync("导入失败", ex.Message); }
    }

    private async void OnExportClick(object sender, RoutedEventArgs e)
    {
        try
        {
            if (!ValidateColor()) { await ShowDialogAsync("导出失败", SaveStatusText.Text); return; }
            var picker = new FileSavePicker { SuggestedFileName = "NovaBar-settings" };
            picker.FileTypeChoices.Add("JSON 配置", new List<string> { ".json" });
            WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(this));
            var file = await picker.PickSaveFileAsync();
            if (file is null) return;
            SettingsStore.Export(_settings, file.Path);
            await ShowDialogAsync("已导出", file.Path);
        }
        catch (Exception ex) { await ShowDialogAsync("导出失败", ex.Message); }
    }

    private void OnAddRuleClick(object sender, RoutedEventArgs e)
    {
        var pattern = RulePatternBox.Text?.Trim();
        if (string.IsNullOrEmpty(pattern))
        {
            RuleHint.Text = "先填一个匹配串，例如 chrome.exe。";
            return;
        }

        var kind = (RuleMatchKind)Math.Max(0, RuleKindBox.SelectedIndex);
        var target = new RuleTarget { Active = CurrentAppearance.Clone() };

        var rules = RuleSetFor(_ruleScope);
        var sink = kind switch
        {
            RuleMatchKind.ProcessName => rules.ProcessName,
            RuleMatchKind.WindowTitle => rules.WindowTitle,
            _ => rules.WindowClass,
        };

        if (sink.ContainsKey(pattern))
        {
            var index = _rules.FindIndex(rule => rule.Kind == kind && string.Equals(rule.Pattern, pattern, StringComparison.OrdinalIgnoreCase));
            RuleList.SelectedIndex = index;
            RuleHint.Text = "此规则已存在，已选中；可更新匹配条件或编辑外观。";
            return;
        }
        sink[pattern] = target;
        ReloadRulesFromModel();
        MarkDirty();

        RulePatternBox.Text = string.Empty;
        RuleHint.Text = $"已保存 {Describe(kind)} 规则：{pattern}";
    }

    private void OnRemoveRuleClick(object sender, RoutedEventArgs e)
    {
        var index = RuleList.SelectedIndex;
        if (index < 0 || index >= _rules.Count)
        {
            RuleHint.Text = "先在列表里选一条规则。";
            return;
        }

        var (kind, pattern) = _rules[index];
        var rules = RuleSetFor(_ruleScope);
        var sink = kind switch
        {
            RuleMatchKind.ProcessName => rules.ProcessName,
            RuleMatchKind.WindowTitle => rules.WindowTitle,
            _ => rules.WindowClass,
        };

        sink.Remove(pattern);
        if (_editingRule is { } editing && editing.Scope == _ruleScope && editing.Kind == kind && string.Equals(editing.Pattern, pattern, StringComparison.OrdinalIgnoreCase))
        {
            _editingRule = null;
            RefreshFromModel();
        }
        ReloadRulesFromModel();
        MarkDirty();
        RuleHint.Text = $"已删除：{pattern}";
    }

    private SurfaceRules RuleSetFor(AppearanceState? scope) => scope is { } state
        ? _settings.Surface(_surface).State(state).Rules
        : _settings.Surface(_surface).Rules;

    private Dictionary<string, RuleTarget> RulesFor(RuleMatchKind kind) => RulesFor(kind, _ruleScope);

    private Dictionary<string, RuleTarget> RulesFor(RuleMatchKind kind, AppearanceState? scope)
    {
        var rules = RuleSetFor(scope);
        return kind switch { RuleMatchKind.ProcessName => rules.ProcessName, RuleMatchKind.WindowTitle => rules.WindowTitle, _ => rules.WindowClass };
    }

    private static string DescribeRuleScope(AppearanceState? scope) => scope switch
    {
        AppearanceState.VisibleWindow => "有窗口规则",
        AppearanceState.MaximizedWindow => "最大化规则",
        _ => "通用规则",
    };

    private void OnRuleScopeChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading) return;
        _ruleScope = RuleScopeBox.SelectedIndex switch
        {
            1 => AppearanceState.VisibleWindow,
            2 => AppearanceState.MaximizedWindow,
            _ => null,
        };
        _editingRule = null;
        _editingInactive = false;
        RefreshFromModel();
        ReloadRulesFromModel();
    }

    private List<string> IgnoredFor(RuleMatchKind kind)
    {
        var filter = _settings.Surface(_surface).IgnoredWindows;
        return kind switch { RuleMatchKind.ProcessName => filter.ProcessName, RuleMatchKind.WindowTitle => filter.WindowTitle, _ => filter.WindowClass };
    }

    private void OnRuleSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        var index = RuleList.SelectedIndex;
        if (index < 0 || index >= _rules.Count) return;
        var rule = _rules[index];
        RuleKindBox.SelectedIndex = (int)rule.Kind;
        RulePatternBox.Text = rule.Pattern;
    }

    private void OnUpdateRuleClick(object sender, RoutedEventArgs e)
    {
        var index = RuleList.SelectedIndex;
        if (index < 0 || index >= _rules.Count) { RuleHint.Text = "先选一条规则。"; return; }
        var pattern = RulePatternBox.Text.Trim();
        if (string.IsNullOrWhiteSpace(pattern)) { RuleHint.Text = "匹配串不能为空。"; return; }
        var old = _rules[index]; var kind = (RuleMatchKind)Math.Max(0, RuleKindBox.SelectedIndex);
        var target = RulesFor(old.Kind)[old.Pattern];
        if (RulesFor(kind).ContainsKey(pattern) && (old.Kind != kind || !string.Equals(old.Pattern, pattern, StringComparison.OrdinalIgnoreCase)))
        { RuleHint.Text = "此匹配条件已有规则，请选择已有规则编辑。"; return; }
        RulesFor(old.Kind).Remove(old.Pattern);
        RulesFor(kind)[pattern] = target;
        if (_editingRule is { } editing && editing.Scope == _ruleScope && editing.Kind == old.Kind
            && string.Equals(editing.Pattern, old.Pattern, StringComparison.OrdinalIgnoreCase))
            _editingRule = (_ruleScope, kind, pattern);
        ReloadRulesFromModel();
        RefreshFromModel();
        MarkDirty();
        RuleHint.Text = "匹配条件已更新；活动和非活动外观已保留。";
    }

    private void OnEditRuleClick(object sender, RoutedEventArgs e)
    {
        var index = RuleList.SelectedIndex;
        if (index < 0 || index >= _rules.Count) { RuleHint.Text = "先选一条规则。"; return; }
        var selected = _rules[index];
        _editingRule = (_ruleScope, selected.Kind, selected.Pattern);
        _editingInactive = false;
        RefreshFromModel();
        Nav.SelectedItem = Nav.MenuItems[0];
    }

    private void OnIgnoredSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        var index = IgnoredList.SelectedIndex;
        if (index < 0 || index >= _ignored.Count) return;
        var ignored = _ignored[index];
        IgnoreKindBox.SelectedIndex = (int)ignored.Kind;
        IgnorePatternBox.Text = ignored.Pattern;
    }

    private void OnAddIgnoredClick(object sender, RoutedEventArgs e)
    {
        var pattern = IgnorePatternBox.Text.Trim();
        if (string.IsNullOrWhiteSpace(pattern)) { RuleHint.Text = "请填写忽略匹配串。"; return; }
        var list = IgnoredFor((RuleMatchKind)Math.Max(0, IgnoreKindBox.SelectedIndex));
        if (list.Contains(pattern, StringComparer.OrdinalIgnoreCase)) { RuleHint.Text = "此忽略条件已经存在。"; return; }
        list.Add(pattern);
        ReloadRulesFromModel();
        MarkDirty();
        RuleHint.Text = "已添加忽略窗口。";
    }

    private void OnUpdateIgnoredClick(object sender, RoutedEventArgs e)
    {
        var index = IgnoredList.SelectedIndex;
        if (index < 0 || index >= _ignored.Count) { RuleHint.Text = "先选择一条忽略条件。"; return; }
        var pattern = IgnorePatternBox.Text.Trim();
        if (string.IsNullOrWhiteSpace(pattern)) { RuleHint.Text = "忽略匹配串不能为空。"; return; }
        var old = _ignored[index]; var kind = (RuleMatchKind)Math.Max(0, IgnoreKindBox.SelectedIndex);
        if (IgnoredFor(kind).Contains(pattern, StringComparer.OrdinalIgnoreCase) && (old.Kind != kind || !string.Equals(old.Pattern, pattern, StringComparison.OrdinalIgnoreCase)))
        { RuleHint.Text = "此忽略条件已经存在。"; return; }
        IgnoredFor(old.Kind).Remove(old.Pattern);
        IgnoredFor(kind).Add(pattern);
        ReloadRulesFromModel();
        MarkDirty();
    }

    private void OnRemoveIgnoredClick(object sender, RoutedEventArgs e)
    {
        var index = IgnoredList.SelectedIndex;
        if (index < 0 || index >= _ignored.Count) { RuleHint.Text = "先选择一条忽略条件。"; return; }
        var old = _ignored[index];
        IgnoredFor(old.Kind).Remove(old.Pattern);
        ReloadRulesFromModel();
        MarkDirty();
    }

    // ───────────────────────── 渲染与杂项 ─────────────────────────

    private void RebuildRuleList()
    {
        _ruleItems.Clear();
        foreach (var (kind, pattern) in _rules)
        {
            _ruleItems.Add($"{Describe(kind)} · {pattern}");
        }
    }

    /// <summary>从配置里重建规则列表。之前只在"新增"时填，重启后界面是空的。</summary>
    private void ReloadRulesFromModel()
    {
        _rules.Clear();

        var rules = RuleSetFor(_ruleScope);
        foreach (var key in rules.ProcessName.Keys)
        {
            _rules.Add((RuleMatchKind.ProcessName, key));
        }

        foreach (var key in rules.WindowTitle.Keys)
        {
            _rules.Add((RuleMatchKind.WindowTitle, key));
        }

        foreach (var key in rules.WindowClass.Keys)
        {
            _rules.Add((RuleMatchKind.WindowClass, key));
        }

        RebuildRuleList();
        _ignored.Clear();
        _ignoredItems.Clear();
        foreach (var kind in Enum.GetValues<RuleMatchKind>())
            foreach (var pattern in IgnoredFor(kind))
            {
                _ignored.Add((kind, pattern));
                _ignoredItems.Add($"{Describe(kind)} · {pattern}");
            }
        RuleHint.Text = _rules.Count == 0 ? $"{DescribeRuleScope(_ruleScope)}还没有规则。" : $"{DescribeRuleScope(_ruleScope)}共 {_rules.Count} 条规则";
    }

    private void UpdatePreview()
    {
        var appearance = CurrentAppearance;
        var tintAlpha = (byte)Math.Clamp(appearance.Color.A * appearance.Glass.TintStrength, 0, 255);
        var tint = Color.FromArgb(tintAlpha, appearance.Color.R, appearance.Color.G, appearance.Color.B);

        // 真实玻璃 = DWM 窗口模糊 + 这层色调 + 下面那层高光；
        // 预览没有"背后内容"可模糊，所以只还原色调与高光。
        PreviewCard.Background = new SolidColorBrush(tint);

        var highlight = new LinearGradientBrush
        {
            StartPoint = new Windows.Foundation.Point(0, 0),
            EndPoint = new Windows.Foundation.Point(0.7, 1),
        };
        highlight.GradientStops.Add(new GradientStop
        {
            Color = Color.FromArgb(190, 255, 255, 255),
            Offset = 0,
        });
        highlight.GradientStops.Add(new GradientStop
        {
            Color = Color.FromArgb(0, 255, 255, 255),
            Offset = 0.55,
        });

        PreviewHighlight.Background = highlight;
        PreviewHighlight.Opacity = Math.Clamp(appearance.Glass.Specular, 0, 1) * 0.9;

        var rim = new LinearGradientBrush
        {
            StartPoint = new Windows.Foundation.Point(0, 0),
            EndPoint = new Windows.Foundation.Point(1, 1),
        };
        rim.GradientStops.Add(new GradientStop
        {
            Color = Color.FromArgb(220, 255, 255, 255),
            Offset = 0,
        });
        rim.GradientStops.Add(new GradientStop
        {
            Color = Color.FromArgb(40, 180, 220, 255),
            Offset = 0.5,
        });
        rim.GradientStops.Add(new GradientStop
        {
            Color = Color.FromArgb(200, 255, 255, 255),
            Offset = 1,
        });

        PreviewRim.BorderBrush = rim;
        PreviewRim.Opacity = 0.25 + (Math.Clamp(appearance.Glass.EdgeDispersion, 0, 1) * 0.75);

        var radius = appearance.Glass.CornerRadius > 0 ? appearance.Glass.CornerRadius : 14;
        PreviewCard.CornerRadius = new CornerRadius(radius);
        PreviewHighlight.CornerRadius = new CornerRadius(radius);
        PreviewRim.CornerRadius = new CornerRadius(radius);
    }

    private void UpdateAbout()
    {
        var version = typeof(MainWindow).Assembly.GetName().Version?.ToString(3) ?? "0.1.0";
        var platform = PlatformSupport.Current;
        var backend = PlatformSupport.PreferredBackend;
        var ancestorNote = PlatformSupport.RequiresAncestorWalk(platform.Build)
            ? " · 需要祖先链匹配"
            : string.Empty;

        AboutVersionText.Text = $"NovaBar {version}";
        AboutPlatformText.Text = $"系统：{platform.DisplayName} · {platform.Major}.{platform.Minor} · Build {platform.Build}";
        AboutBackendText.Text = backend == BackendKind.XamlTree
            ? $"后端：XamlTree（注入 explorer）{ancestorNote}"
            : "后端：LegacyComposition（SetWindowCompositionAttribute）";
        AboutConfigPathText.Text = $"配置：{SettingsStore.DefaultPath}";

        _suppressStartupToggle = true;
        try { StartupToggle.IsOn = StartupManager.IsEnabled; }
        catch (Exception ex) { StartupToggle.IsEnabled = false; SaveStatusText.Text = $"无法读取开机启动状态：{ex.Message}"; }
        _suppressStartupToggle = false;
    }

    private void OnStartupToggled(object sender, RoutedEventArgs e)
    {
        if (_suppressStartupToggle)
        {
            return;
        }

        try
        {
            var command = StartupManager.SetEnabled(StartupToggle.IsOn);
            if (command is null)
            {
                _ = ShowDialogAsync("设置失败", "没有找到 NovaBar.Engine.exe，无法注册开机启动。\n先构建原生部分。");
                _suppressStartupToggle = true;
                StartupToggle.IsOn = StartupManager.IsEnabled;
                _suppressStartupToggle = false;
                return;
            }
            _settings.Global.StartWithWindows = StartupToggle.IsOn;
            MarkDirty();
        }
        catch (Exception ex)
        {
            _suppressStartupToggle = true;
            try { StartupToggle.IsOn = StartupManager.IsEnabled; } catch { StartupToggle.IsOn = false; }
            _suppressStartupToggle = false;
            _ = ShowDialogAsync("设置失败", ex.Message);
        }
    }

    private async Task ShowDialogAsync(string title, string message)
    {
        await _dialogGate.WaitAsync();
        try
        {
        if (_closed || Content.XamlRoot is null) return;
        var dialog = new ContentDialog
        {
            Title = title,
            Content = message,
            CloseButtonText = "好",
            XamlRoot = Content.XamlRoot,
        };

        await dialog.ShowAsync();
        }
        finally { _dialogGate.Release(); }
    }

    private static Color ToWindowsColor(ArgbColor color) => Color.FromArgb(color.A, color.R, color.G, color.B);

    private static string Describe(SurfaceKind kind) => kind switch
    {
        SurfaceKind.Taskbar => "任务栏",
        SurfaceKind.StartMenu => "开始菜单",
        SurfaceKind.NotificationCenter => "通知中心 / 快速设置",
        _ => kind.ToString(),
    };

    private static string Describe(AppearanceState state) => state switch
    {
        AppearanceState.Desktop => "桌面（无窗口）",
        AppearanceState.VisibleWindow => "有窗口",
        AppearanceState.MaximizedWindow => "最大化窗口",
        AppearanceState.StartOpened => "开始菜单打开",
        AppearanceState.SearchOpened => "搜索打开",
        AppearanceState.TaskViewOpened => "任务视图打开",
        AppearanceState.BatterySaver => "省电模式",
        _ => state.ToString(),
    };

    private static string Describe(RuleMatchKind kind) => kind switch
    {
        RuleMatchKind.ProcessName => "进程名",
        RuleMatchKind.WindowTitle => "窗口标题",
        RuleMatchKind.WindowClass => "窗口类名",
        _ => kind.ToString(),
    };
}
