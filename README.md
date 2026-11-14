# NovaBar

让 Windows 11 的任务栏、开始菜单、通知中心 / 日历和快捷设置各有自己的外观。你可以只改任务栏，也可以按桌面、窗口最大化等状态自动切换材质。液态玻璃会对界面背后的真实窗口和桌面做模糊、折射和色散。

**下载 0.4.3：[GitHub Releases](https://github.com/L1angYun/NovaBar/releases/tag/v0.4.3) · [Gitee 发行版（国内下载）](https://gitee.com/liangyun_Liu/NovaBar/releases/tag/v0.4.3)**

适用于 **Windows 11 x64**。下载并运行 `NovaBar-0.4.3-win-x64-Setup.exe`。安装包自带运行时，默认安装到当前用户目录，无需管理员权限。

## 三步上手

1. 打开 NovaBar，在「外观」中选中任务栏，选择「液态玻璃」或其他材质。
2. 想改开始菜单，先在「界面」开启「开始菜单」；想改通知中心或快捷设置，开启「通知中心 / 快速设置」。这两个开关默认关闭。
3. 点击「应用到系统」。如需让外观随窗口切换，到「规则」中按进程名、标题或窗口类名设置。

想恢复原状，右键托盘图标，选择「恢复系统默认外观」；也可以在安装目录运行 `.\NovaBar.Engine.exe --restore`。卸载会恢复系统外观并保留配置。

## 更新和登录自启

- **更新：** 在「关于 → 更新来源」选择自动、GitHub 或 Gitee，再点「检查更新」。自动模式在网络故障时可尝试另一官方来源。安装前会校验文件大小和 SHA-256，更新保留现有配置。「启动配置中心时检查更新」默认关闭。
- **登录自启：** 在「关于」启用「开机自动启动引擎」。Windows 登录当前用户后只启动后台引擎，不弹出配置中心。目前没有登录前启动、管理员模式或高优先级自启。

## 已知限制

- 首次打开通知中心时，玻璃可能会晚一点出现；开始菜单等面板在部分原生收起动画里仍可能出现背景拖影。
- 液态玻璃需要有效的画面来源。来源未就绪或捕获失败时暂时透明，不会自动换成亚克力或静态壁纸。
- 共享 GPU 纹理通道仍在试验中，安装后默认不开启。

## 开发与发布

Windows x64、.NET 10 SDK、Visual Studio C++ x64 工具链及 Windows SDK 10.0.26100 或更高版本。在仓库根目录运行：

```powershell
.\build\test.ps1                      # 常规回归与构建；-Capture 增加真实捕获与 GPU 测试
.\build\installer.ps1 -CompilerPath "<Inno Setup 7.1.0 目录>\ISCC.exe"
```

GitHub 与 Gitee 的同名标签指向同一提交，并上传同一次构建产出的 Setup、`SHA256SUMS.txt` 和 `update.json`。先上传并核验安装包与校验文件，再上传更新清单；同步源码不会同步发行附件。

[发行说明](docs/RELEASE_NOTES.md) · [能力与测试范围](docs/PARITY.md) · [架构](docs/ARCHITECTURE.md) · [GPL-3.0 许可证](LICENSE) · [第三方说明](NOTICE.md)
