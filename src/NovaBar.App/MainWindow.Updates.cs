using System.Net.Http;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using NovaBar.Core;

namespace NovaBar.App;

public sealed partial class MainWindow
{
    private readonly HttpClient _updateHttp = new(new HttpClientHandler
    {
        AllowAutoRedirect = false,
        UseCookies = false,
        UseDefaultCredentials = false,
    }) { Timeout = Timeout.InfiniteTimeSpan };
    private readonly CancellationTokenSource _updateLifetime = new();
    private CancellationTokenSource? _updateOperation;
    private UpdateCheckResult? _updateResult;
    private bool _updateBusy;
    private bool _updateDownloading;

    private static string CurrentUpdateVersion
    {
        get
        {
            var version = typeof(MainWindow).Assembly.GetName().Version ?? new Version(0, 0, 0);
            return $"{version.Major}.{version.Minor}.{Math.Max(0, version.Build)}";
        }
    }

    private static string DescribeUpdateSource(UpdateSourcePreference source) => source switch
    {
        UpdateSourcePreference.GitHub => "GitHub",
        UpdateSourcePreference.Gitee => "Gitee",
        _ => "GitHub / Gitee",
    };

    private void OnUpdateSourceChanged(object sender, SelectionChangedEventArgs args)
    {
        if (_loading || _closed || _updateBusy || !UpdateSourceBox.IsEnabled || UpdateSourceBox.SelectedIndex < 0) return;
        var preference = (UpdateSourcePreference)UpdateSourceBox.SelectedIndex;
        if (_settings.Global.UpdateSource == preference) return;
        _settings.Global.UpdateSource = preference;
        ClearUpdateOffer("更新来源已切换，请重新检查。");
        MarkDirty(applyAppearance: false);
    }

    private void RefreshUpdateSourceSelection()
    {
        // RefreshFromModel suppresses SelectionChanged while loading or importing settings.
        var previous = UpdateSourceBox.SelectedIndex;
        var selected = (int)_settings.Global.UpdateSource;
        if (previous == selected) return;
        UpdateSourceBox.SelectedIndex = selected;
        if (previous >= 0) ClearUpdateOffer("更新来源已切换，请重新检查。");
    }

    private void ClearUpdateOffer(string status)
    {
        _updateResult = null;
        _updateOperation?.Cancel();
        _updateDownloading = false;
        StartupUpdateNotice.IsOpen = false;
        ReleaseNotesExpander.Visibility = Visibility.Collapsed;
        UpdateProgressBar.Visibility = Visibility.Collapsed;
        UpdateStatusText.Text = status;
        RefreshUpdateButtons();
    }

    private async void OnCheckUpdatesClick(object sender, RoutedEventArgs args) => await CheckUpdatesAsync(startup: false);

    private async Task CheckUpdatesAsync(bool startup)
    {
        if (_closed || _updateBusy) return;
        _updateBusy = true;
        var preference = _settings.Global.UpdateSource;
        ClearUpdateOffer($"正在检查 {DescribeUpdateSource(preference)} 稳定版本…");
        using var cancellation = CancellationTokenSource.CreateLinkedTokenSource(_updateLifetime.Token);
        _updateOperation = cancellation;
        try
        {
            var result = await new UpdateChecker(_updateHttp).CheckAsync(CurrentUpdateVersion, preference, cancellation.Token);
            if (_closed || cancellation.IsCancellationRequested) return;
            _updateResult = result;
            if (result is null) UpdateStatusText.Text = "所选来源尚未发布稳定版本。";
            else
            {
                var source = DescribeUpdateSource(result.Source);
                UpdateStatusText.Text = result.IsNewer
                    ? result.Installer is null ? $"{source} 上有 {result.Version}，尚未提供可验证的安装包。" : $"在 {source} 发现新版本 {result.Version}。"
                    : $"{source} 未发现比当前版本 {CurrentUpdateVersion} 更新的稳定版。";
                var notes = result.ReleaseNotes.Length > 12000 ? result.ReleaseNotes[..12000] + "\n…完整内容见发行说明。" : result.ReleaseNotes;
                ReleaseNotesText.Text = $"{result.ReleaseName}\n版本：{result.Version} · 来源：{source}\n\n{(string.IsNullOrWhiteSpace(notes) ? "此版本未提供更新说明。" : notes)}";
                ReleaseNotesExpander.Visibility = Visibility.Visible;
                if (startup && _settings.Global.CheckUpdatesOnStartup && result.IsNewer)
                {
                    StartupUpdateNotice.Title = $"发现 NovaBar {result.Version}";
                    StartupUpdateNotice.Message = UpdateStatusText.Text;
                    StartupUpdateNotice.IsOpen = true;
                }
            }
        }
        catch (OperationCanceledException) when (cancellation.IsCancellationRequested) { }
        catch (InvalidDataException)
        {
            if (!_closed && !cancellation.IsCancellationRequested) UpdateStatusText.Text = "官方更新信息校验失败，本次检查已停止。";
        }
        catch (Exception)
        {
            if (!_closed && !cancellation.IsCancellationRequested) UpdateStatusText.Text = startup
                ? "本次自动检查未完成，可稍后手动检查。"
                : "无法检查更新。请检查网络或切换来源后重试。";
        }
        finally
        {
            _updateOperation = null;
            _updateBusy = false;
            if (!_closed) RefreshUpdateButtons();
        }
    }

    private void OnReviewStartupUpdate(object sender, RoutedEventArgs args)
    {
        StartupUpdateNotice.IsOpen = false;
        Nav.SelectedItem = AboutNavItem;
        if (!DownloadUpdateButton.Focus(FocusState.Programmatic)) CheckUpdatesButton.Focus(FocusState.Programmatic);
    }

    private async void OnDownloadUpdateClick(object sender, RoutedEventArgs args)
    {
        if (_closed || _updateBusy || _updateResult is not { IsNewer: true, Installer: { } package } result) return;
        _updateBusy = true;
        using var cancellation = CancellationTokenSource.CreateLinkedTokenSource(_updateLifetime.Token);
        _updateOperation = cancellation;
        RefreshUpdateButtons();
        try
        {
            var source = DescribeUpdateSource(result.Source);
            var mirrorNote = _settings.Global.UpdateSource == UpdateSourcePreference.Auto
                ? "\n连接失败时可尝试另一官方来源。" : "";
            if (!await ConfirmUpdateAsync("下载更新", $"从 NovaBar 官方 {source} 发行页下载 {result.Version}（{package.Size / 1048576.0:F1} MB）。{mirrorNote}\n\n下载后将校验 SHA-256 与文件大小，安装前还会再次确认。", "下载", cancellation.Token)) return;
            if (_closed || cancellation.IsCancellationRequested) return;
            _updateDownloading = true;
            UpdateProgressBar.Value = 0;
            UpdateProgressBar.Visibility = Visibility.Visible;
            UpdateStatusText.Text = $"正在下载 {result.Version}…";
            RefreshUpdateButtons();
            var progress = new Progress<double>(value =>
            {
                if (_closed || !_updateDownloading || cancellation.IsCancellationRequested) return;
                UpdateProgressBar.Value = value * 100;
                UpdateStatusText.Text = $"正在下载 {result.Version}… {value:P0}";
            });
            using var staged = await new UpdateInstaller(_updateHttp).DownloadAsync(package, progress, cancellation.Token);
            cancellation.Token.ThrowIfCancellationRequested();
            if (_closed) return;
            _updateDownloading = false;
            UpdateProgressBar.Value = 100;
            var downloadSource = DescribeUpdateSource(staged.Source);
            UpdateStatusText.Text = $"已从 {downloadSource} 下载，SHA-256 和文件大小已验证。";
            RefreshUpdateButtons();
            var sessionNote = _settings.Global.DisableSaving ? "\n当前启用了“仅本次会话”，本次未保存的外观修改不会写入磁盘。" : "";
            if (!await ConfirmUpdateAsync("安装并重启 NovaBar", $"已验证来自 {downloadSource} 的版本 {result.Version}。\n\n安装程序将更新当前用户的 NovaBar，退出配置中心，并在完成后重新启动。现有配置文件将保留。{sessionNote}", "安装并重启", cancellation.Token)) return;
            if (_closed || cancellation.IsCancellationRequested) return;
            if (!FlushIfDirty())
            {
                UpdateStatusText.Text = "配置尚未保存，更新已停止。";
                await ShowDialogAsync("配置尚未保存", SaveStatusText.Text);
                return;
            }
            UpdateStatusText.Text = "正在启动已验证的安装程序…";
            // Start only after rehashing the same read-locked file; never invoke a shell or request elevation.
            using var installer = staged.StartInstaller();
            _exitRequested = true;
            Close();
        }
        catch (OperationCanceledException)
        {
            if (!_closed && ReferenceEquals(_updateResult, result)) UpdateStatusText.Text = "更新下载已取消或超时，可重新尝试。";
        }
        catch (Exception)
        {
            if (!_closed && ReferenceEquals(_updateResult, result)) UpdateStatusText.Text = "更新未安装：下载、完整性校验或安装程序启动失败。请重新检查更新后再试。";
        }
        finally
        {
            _updateOperation = null;
            _updateDownloading = false;
            _updateBusy = false;
            if (!_closed) RefreshUpdateButtons();
        }
    }

    private void OnCancelUpdateClick(object sender, RoutedEventArgs args)
    {
        if (_updateDownloading) _updateOperation?.Cancel();
    }

    private async void OnOpenReleaseClick(object sender, RoutedEventArgs args)
    {
        if (_closed || _updateBusy || _updateResult is not { } result) return;
        _updateBusy = true;
        RefreshUpdateButtons();
        try
        {
            if (!await Windows.System.Launcher.LaunchUriAsync(result.ReleaseUri) && !_closed && ReferenceEquals(_updateResult, result))
                UpdateStatusText.Text = "无法打开发行说明。";
        }
        catch { if (!_closed && ReferenceEquals(_updateResult, result)) UpdateStatusText.Text = "无法打开发行说明。"; }
        finally
        {
            _updateBusy = false;
            if (!_closed) RefreshUpdateButtons();
        }
    }

    private void RefreshUpdateButtons()
    {
        UpdateSourceBox.IsEnabled = !_updateBusy;
        CheckUpdatesButton.IsEnabled = !_updateBusy;
        DownloadUpdateButton.IsEnabled = !_updateBusy && _updateResult is { IsNewer: true, Installer: not null };
        OpenReleaseButton.IsEnabled = !_updateBusy && _updateResult is not null;
        CancelUpdateButton.Visibility = _updateDownloading ? Visibility.Visible : Visibility.Collapsed;
    }

    private async Task<bool> ConfirmUpdateAsync(string title, string message, string primaryButton, CancellationToken cancellationToken)
    {
        await _dialogGate.WaitAsync(cancellationToken);
        try
        {
            if (_closed || cancellationToken.IsCancellationRequested || RootGrid.XamlRoot is null) return false;
            var dialog = new ContentDialog
            {
                Title = title,
                Content = message,
                PrimaryButtonText = primaryButton,
                CloseButtonText = "取消",
                DefaultButton = ContentDialogButton.Close,
                XamlRoot = RootGrid.XamlRoot,
            };
            return await dialog.ShowAsync() == ContentDialogResult.Primary;
        }
        finally { _dialogGate.Release(); }
    }

    private void StopUpdateRequests()
    {
        _updateLifetime.Cancel();
        _updateHttp.Dispose();
    }
}
