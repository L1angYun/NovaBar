# NOTICE

NovaBar 按 GPL-3.0-or-later 发布。以下保留项目来源与运行时说明；具体授权文本以项目及对应发行文件中的许可证为准。

## 参考项目

- [TranslucentTB](https://github.com/TranslucentTB/TranslucentTB)（GPL-3.0）：参考 ExplorerTAP 的 `InitializeXamlDiagnosticsEx` / `IXamlDiagnostics` 接入、可视树订阅、任务栏背景定位，以及状态化外观、规则和配置字段语义。NovaBar 使用自己的配置中心、文件协议与渲染管线，并按祖先链定位任务栏。
- [m417z 的 Windows 11 Styler 模组](https://github.com/ramensoftware/windhawk-mods/tree/main/mods)（相关模组源码声明 GPLv3）：参考任务栏、开始菜单、通知中心的类型 / 元素名选择器及 XAML 诊断接入做法。NovaBar 不随包分发 Windhawk，也不使用其符号钩子基础设施。
- [ZenDesktop](https://github.com/Liset999/ZenDesktop)（GPL-3.0）：参考让 XAML 背景让开、使用窗口材质及高光的外观组织思路。当前 WGC 捕获、GPU / CPU 像素处理、配置和宿主接入实现保存在本仓库。

上述说明区分实现参考与随包组件，不表示这些项目或其作者为 NovaBar 提供背书。

## 默认发行包的运行时

| 组件 | 用途与许可来源 |
|---|---|
| [Windows App SDK / WinUI 3](https://github.com/microsoft/WindowsAppSDK) | 配置中心；主项目采用 MIT 许可，随包组件的附带许可与第三方声明仍需保留 |
| [.NET 运行时](https://github.com/dotnet/runtime) | 自包含托管应用与 WinForms 托盘；保留发布输出中的许可及 ThirdPartyNotices |
| Microsoft C/C++ 运行时 | 原生 Engine / Bridge 使用 `/MT` 静态链接，分发遵循所用 Visual Studio 工具链的运行时条款 |

WGC、Direct3D 11、WIC、XAML 与 DWM 是调用的 Windows 系统 API。默认玻璃管线使用仓库内的 WGC 捕获、D3D11 合成 / 滤镜着色器与客户端边缘处理实现。安装程序由 Inno Setup 7.1.0 构建；编译器从官方发行下载并校验固定 SHA-256，不随应用分发。

## 商标

Windows、WinUI、Fluent Design 是 Microsoft Corporation 的商标。NovaBar 与 Microsoft 无关联，亦未获其背书。
