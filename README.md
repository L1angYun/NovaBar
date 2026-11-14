# NovaBar

NovaBar 是面向 Windows 11 x64 的外壳外观工具，提供 WinUI 3 配置中心、状态规则和任务栏、开始菜单、通知 / 日历、快捷设置的独立外观配置。

版本：`0.4.3`。LiquidGlass 使用目标下方真实窗口与桌面的画面。首次就绪前，以及来源明确不可用或过期时保持透明；等待新帧时可沿用有效期内的真实画布。不自动改用 Acrylic、DWM 模糊或静态壁纸。

## 安装与使用

1. 从 [GitHub Releases](https://github.com/L1angYun/NovaBar/releases) 或 [Gitee 发行版](https://gitee.com/liangyun_Liu/NovaBar/releases) 下载 `NovaBar-0.4.3-win-x64-Setup.exe`。安装程序默认安装到 `%LOCALAPPDATA%\Programs\NovaBar`，无需管理员权限。
2. 打开配置中心，选择界面、状态和材质，点击“应用到系统”。默认自动保存并自动应用；可在关于页调整。
3. 开始菜单与通知 / 快捷设置默认关闭，可在界面页分别开启。LiquidGlass 提供模糊、折射、色散、噪声、色调与高光参数。
4. 在规则页配置通用、有窗口、最大化规则，以及活动 / 非活动外观。支持按进程名、标题或类名匹配，也可设置忽略列表。

托盘可打开配置中心、恢复默认外观或退出配置中心。关闭窗口默认留在托盘；退出配置中心不会自动停止独立运行的引擎。支持 JSON 导入 / 导出和 TTB 配置导入。

## 更新与恢复

关于页可选“自动”“GitHub”或“Gitee”更新源，并提供手动检查和发行说明。自动模式查询两个官方源，选取可用的新版本；指定源时只访问该源。启动检查默认关闭。下载前、安装前分别确认；下载可以取消，安装前检查文件大小与 SHA-256。应用内更新保留配置，并恢复更新前的引擎运行或还原状态。

从便携目录迁移时，先退出原目录的配置中心和引擎，再安装新版。卸载会恢复系统外观，保留用户配置。

恢复默认外观也可在安装目录运行：

```powershell
.\NovaBar.Engine.exe --restore
```

重新应用配置后恢复外观规则。安装目录中的 `NovaBar.Engine.exe`、`NovaBar.Capture.exe` 与 `NovaBar.XamlBridge.v043.dll` 应来自同一版本，请勿单独混用。

## 构建与测试

开发环境需要 Windows x64、.NET 10 SDK、Visual Studio 2022 / 2026 的 C++ x64 工具链，以及 Windows SDK 10.0.26100 或更高版本。在仓库根目录运行：

```powershell
# 构建配置中心、Core 与托盘
dotnet build .\NovaBar.slnx -c Release -p:Platform=x64

# 在独立目录构建原生组件
.\build\native-build.ps1 -OutputDirectory artifacts\next

# 常规回归与解决方案构建
.\build\test.ps1

# 增加真实 WGC 场景与 GPU 验证，需要可交互桌面
.\build\test.ps1 -Capture

# 构建自包含安装负载
.\build\package.ps1 -StageOnly

# 使用 Inno Setup 7.1.0 构建安装程序和更新元数据
.\build\installer.ps1 -CompilerPath "<Inno Setup 目录>\ISCC.exe"
```

自包含负载位于 `dist\NovaBar`；安装程序、`update.json` 和 `SHA256SUMS.txt` 位于 `dist\installer`。`build-info.json` 记录负载的构建信息与文件校验值。正式 Release 提供安装程序，不提供便携应用 ZIP；GitHub 自动生成的源代码归档仍可下载。

维护者同步发布时，两边同名标签必须指向同一提交。从同一次构建取出安装程序、`SHA256SUMS.txt` 和 `update.json`，把相同的三个文件上传到 GitHub 和 Gitee。先上传安装程序与校验文件，校验下载后再上传 `update.json`；Gitee 的源码同步不会复制发行附件。清单中的 GitHub URL 是固定的文件标识，Gitee 下载路径由应用按版本和文件名构造。GitHub Actions 只构建和提供这些附件，发行时复用该产物，避免在两边独立构建出不同哈希的安装包。

## 能力与限制

- 状态包括桌面、有窗口、最大化、开始菜单、搜索、任务视图和省电；各界面有独立配置与规则回退。
- 默认路径由 GPU 合成真实场景并处理基础画布，随后通过 BGRA 回读传输，由客户端 CPU 计算面板光学并上传到 XAML 画刷。候选源码包含显式选择的共享 GPU 纹理路径，仅供受支持设备上的面板验证，安装器不会默认启用。
- 首次按需创建的通知宿主需要等待接入与真实来源就绪，不能保证首次可见帧就显示玻璃。
- 部分原生独立动画中的背景对齐仍有限制，不保证打开或收起过程逐帧无拖影。
- 捕获结果受来源程序、驱动、受保护内容与系统策略影响。来源本身暂停时，真实背景也会静止。
- Acrylic 是可显式选择的独立材质。`ShowPeek` 字段可保留在配置中，当前后端不提供其开关。
- 当前目标平台为 Windows 11 x64；不承诺 Windows 10、早期 Windows 11 或独立 LegacyComposition 后端兼容性。

详细范围见 [能力与验证边界](docs/PARITY.md)，进程与来源契约见 [架构](docs/ARCHITECTURE.md)，本版变化见 [发行说明](docs/RELEASE_NOTES.md)。

本项目采用 GPL-3.0-or-later。许可证见 [LICENSE](LICENSE)，参考项目与随包运行时说明见 [NOTICE.md](NOTICE.md)。
