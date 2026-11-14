using System.Drawing;
using WinForms = System.Windows.Forms;

namespace NovaBar.Tray;

/// <summary>
/// 托盘图标。WinUI 3 没有托盘 API，这里用 WinForms 的 NotifyIcon 兜底。
/// 消息循环仍然跑在调用方的 UI 线程上（WinUI 的 DispatcherQueue 会给它泵消息）。
/// </summary>
public sealed class TrayIcon : IDisposable
{
    private readonly WinForms.NotifyIcon _icon;
    private readonly Icon _ownedIcon;
    private bool _disposed;

    public TrayIcon(string iconPath, string tooltip, Action onOpen, Action onRestore, Action onExit)
    {
        ArgumentNullException.ThrowIfNull(onOpen);
        ArgumentNullException.ThrowIfNull(onRestore);
        ArgumentNullException.ThrowIfNull(onExit);

        var menu = new WinForms.ContextMenuStrip();

        var openItem = new WinForms.ToolStripMenuItem("打开配置中心");
        openItem.Click += (_, _) => onOpen();

        var restoreItem = new WinForms.ToolStripMenuItem("恢复系统默认外观");
        restoreItem.Click += (_, _) => onRestore();

        var exitItem = new WinForms.ToolStripMenuItem("退出配置中心");
        exitItem.Click += (_, _) => onExit();

        menu.Items.Add(openItem);
        menu.Items.Add(new WinForms.ToolStripSeparator());
        menu.Items.Add(restoreItem);
        menu.Items.Add(exitItem);

        _ownedIcon = LoadIcon(iconPath);
        _icon = new WinForms.NotifyIcon
        {
            Icon = _ownedIcon,
            Text = tooltip,
            ContextMenuStrip = menu,
            Visible = true,
        };

        _icon.DoubleClick += (_, _) => onOpen();
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        try
        {
            _icon.Visible = false;
            _icon.ContextMenuStrip?.Dispose();
            _icon.Dispose();
            _ownedIcon.Dispose();
        }
        catch
        {
            // 退出阶段忽略
        }
    }

    private static Icon LoadIcon(string path)
    {
        try
        {
            if (File.Exists(path))
            {
                return new Icon(path);
            }
        }
        catch
        {
            // 落到系统图标
        }

        return (Icon)SystemIcons.Application.Clone();
    }
}
