#requires -Version 5.1
<#
.SYNOPSIS
    构建 NovaBar 的原生部分：版本化桥接 DLL、Engine.exe 与 Capture.exe。

.DESCRIPTION
    直接用 cl.exe 编译，不依赖 .vcxproj，避免首次构建还要跑一遍 VS 工程向导。
    需要：VS 2022/2026 的 C++ 工具集 + Windows SDK 10.0.26100。

.EXAMPLE
    .\build\native-build.ps1
    .\build\native-build.ps1 -Clean
#>
[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$Win2D,
    [ValidateSet('all', 'engine', 'bridge', 'capture')]
    [string]$Target = 'all',
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot -Parent
$artifactDir = Join-Path $root 'artifacts\native'
if ($OutputDirectory) {
    $artifactDir = if ([IO.Path]::IsPathRooted($OutputDirectory)) { [IO.Path]::GetFullPath($OutputDirectory) } else { [IO.Path]::GetFullPath((Join-Path $root $OutputDirectory)) }
}
$bridgeDir = Join-Path $root 'src\NovaBar.XamlBridge'
$engineDir = Join-Path $root 'src\NovaBar.Engine'
$captureDir = Join-Path $root 'src\NovaBar.Capture'
$identityHeader = [IO.File]::ReadAllText((Join-Path $root 'src\NovaBar.Shared\BridgeIdentity.h'))
$bridgeNameMatch = [regex]::Match($identityHeader, '\bFileName\[\]\s*=\s*L"([^"\r\n]+)"')
if (-not $bridgeNameMatch.Success -or $bridgeNameMatch.Groups[1].Value -notmatch '^NovaBar\.XamlBridge\.[A-Za-z0-9_-]+\.dll$') {
    throw '共享桥接标识中的 DLL 文件名无效。'
}
$bridgeName = $bridgeNameMatch.Groups[1].Value

# ── 找工具链 ────────────────────────────────────────────────
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw "找不到 vswhere.exe，请确认装了 Visual Studio（含 C++ 桌面开发工作负载）。"
}

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    throw "没有找到带 MSVC x64 工具集的 Visual Studio 实例。"
}

$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) {
    throw "找不到 vcvars64.bat：$vcvars"
}

$sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$sdkVersion = Get-ChildItem (Join-Path $sdkRoot 'Include') -Directory |
    Sort-Object Name -Descending |
    Select-Object -First 1 -ExpandProperty Name

if (-not $sdkVersion) {
    throw "没有找到 Windows SDK。"
}

$umInclude = Join-Path $sdkRoot "Include\$sdkVersion\um"
$sharedInclude = Join-Path $sdkRoot "Include\$sdkVersion\shared"
$ucrtInclude = Join-Path $sdkRoot "Include\$sdkVersion\ucrt"
$cppwinrtInclude = Join-Path $sdkRoot "Include\$sdkVersion\cppwinrt"

if (-not (Test-Path (Join-Path $cppwinrtInclude 'winrt\base.h'))) {
    throw "Windows SDK $sdkVersion 里没有 C++/WinRT 投影（缺 cppwinrt\winrt\base.h）。"
}

# ── 准备输出目录 ────────────────────────────────────────────
if ($Clean -and (Test-Path $artifactDir)) {
    $resolvedArtifacts = [IO.Path]::GetFullPath((Join-Path $root 'artifacts')) + [IO.Path]::DirectorySeparatorChar
    if (-not ([IO.Path]::GetFullPath($artifactDir) + [IO.Path]::DirectorySeparatorChar).StartsWith($resolvedArtifacts, [StringComparison]::OrdinalIgnoreCase)) {
        throw '-Clean may only delete an output directory within project artifacts.'
    }
    Remove-Item -LiteralPath $artifactDir -Recurse -Force
}

New-Item -ItemType Directory -Path $artifactDir -Force | Out-Null

# ── Win2D（GPU 合成需要）────────────────────────────────────
# 三样东西：winmd（生成 C++/WinRT 投影）、原生 DLL（运行时加载）、cppwinrt.exe（SDK 自带）。
# 失败不阻断构建 —— 桥接层用 __has_include 判断，没有投影就退回 CPU 路径。
$win2dDir = Join-Path $root 'artifacts\win2d'
$win2dGen = Join-Path $root 'artifacts\win2d-gen'
$win2dIncludeFlag = @()

try {
    if (-not $Win2D) {
        throw 'Win2D 未启用'
    }

    # Win2D 包里自带生成好的 C++/WinRT 投影（include\x64\Microsoft.Graphics.Canvas.h），
    # 直接拿来用，比跑 cppwinrt 可靠得多 —— 后者会因为缺 WinUI 类型而失败。
    $packaged = Join-Path $win2dDir 'include\x64\Microsoft.Graphics.Canvas.h'
    if ((Test-Path $packaged) -and -not (Test-Path (Join-Path $win2dGen 'Microsoft.Graphics.Canvas.h'))) {
        New-Item -ItemType Directory -Path $win2dGen -Force | Out-Null
        Copy-Item $packaged $win2dGen -Force
        Copy-Item (Join-Path $win2dDir 'include\Microsoft.Graphics.Canvas.native.h') $win2dGen -Force
        Write-Host '  Win2D: 使用包内预生成投影' -ForegroundColor Green
    }

    if (-not (Test-Path (Join-Path $win2dGen 'winrt\Microsoft.Graphics.Canvas.h'))) {
        Write-Host '── 准备 Win2D 投影 ──' -ForegroundColor Cyan

        if (-not (Test-Path $win2dDir)) {
            New-Item -ItemType Directory -Path $win2dDir -Force | Out-Null
            $nupkg = Join-Path $win2dDir 'win2d.zip'
            Invoke-WebRequest -Uri 'https://www.nuget.org/api/v2/package/Microsoft.Graphics.Win2D/1.3.2' -OutFile $nupkg -TimeoutSec 120
            Expand-Archive -Path $nupkg -DestinationPath $win2dDir -Force
        }

        New-Item -ItemType Directory -Path $win2dGen -Force | Out-Null
        $cppwinrtExe = Join-Path $sdkRoot "bin\$sdkVersion\x64\cppwinrt.exe"
        $canvasWinmd = Join-Path $win2dDir 'lib\uap10.0\Microsoft.Graphics.Canvas.winmd'
        $facade = Join-Path $sdkRoot 'UnionMetadata\Facade\Windows.winmd'

        & $cppwinrtExe -in $canvasWinmd -out $win2dGen -ref 'C:\Windows\System32\WinMetadata' -ref $facade 2>&1 |
            Select-Object -Last 2 | ForEach-Object { Write-Host "  $_" }

        # cppwinrt 会因为 WinUI 的 WindowId 缺参考而返回非 0，
        # 但需要的头已经写出来了，所以只看文件在不在。
    }

    if (Test-Path (Join-Path $win2dGen 'winrt\Microsoft.Graphics.Canvas.h')) {
        $win2dIncludeFlag = @("/I`"$win2dGen`"")
        Copy-Item (Join-Path $win2dDir 'runtimes\win-x64\native\Microsoft.Graphics.Canvas.dll') $artifactDir -Force
        Write-Host '  Win2D: 投影 + 原生 DLL 就绪' -ForegroundColor Green
    }
    else {
        Write-Host '  Win2D: 投影生成失败，桥接层将退回 CPU 路径' -ForegroundColor Yellow
    }
}
catch {
    if ($Win2D) {
        Write-Host ("  Win2D 准备失败（{0}），跳过" -f $_.Exception.Message) -ForegroundColor Yellow
    }
}

# ── 生成批处理（避免 PowerShell 里的引号地狱） ──────────────
$commonFlags = @(
    '/nologo'
    '/std:c++20'
    '/EHsc'
    '/permissive-'
    '/W3'
    '/utf-8'
    '/O2'
    '/MT'
    '/DUNICODE'
    '/D_UNICODE'
    '/DWIN32_LEAN_AND_MEAN'
    '/D_WIN32_WINNT=0x0A00'
    "/I`"$umInclude`""
    "/I`"$sharedInclude`""
    "/I`"$ucrtInclude`""
    "/I`"$cppwinrtInclude`""
) + $win2dIncludeFlag -join ' '

$bridgeCmd = @"
call "$vcvars" >nul
cl $commonFlags /LD /Fe:"$artifactDir\$bridgeName" /Fo:"$artifactDir\\" "$bridgeDir\bridge.cpp" /link /DEF:"$bridgeDir\exports.def" user32.lib ole32.lib oleaut32.lib runtimeobject.lib windowsapp.lib windowscodecs.lib d2d1.lib dxguid.lib gdi32.lib d3d11.lib dxgi.lib d3dcompiler.lib dwmapi.lib uuid.lib
"@

$engineCmd = @"
call "$vcvars" >nul
cl $commonFlags /Fe:"$artifactDir\NovaBar.Engine.exe" /Fo:"$artifactDir\\" "$engineDir\main.cpp" /link user32.lib ole32.lib oleaut32.lib advapi32.lib
"@

$captureCmd = @"
call "$vcvars" >nul
cl $commonFlags /Fe:"$artifactDir\NovaBar.Capture.exe" /Fo:"$artifactDir\capture-main.obj" "$captureDir\main.cpp" /link user32.lib ole32.lib runtimeobject.lib windowsapp.lib windowscodecs.lib gdi32.lib d3d11.lib dxgi.lib d3dcompiler.lib dwmapi.lib uuid.lib
"@

$batchPath = Join-Path $artifactDir ('native-build-' + [guid]::NewGuid().ToString('N') + '.cmd')

Write-Host "VS      : $vsPath"
Write-Host "SDK     : $sdkVersion"
Write-Host "输出    : $artifactDir"
Write-Host ''

$failed = $false

foreach ($step in @(
        @{ Name = $bridgeName; Build = 'bridge'; Script = $bridgeCmd }
        @{ Name = 'NovaBar.Engine.exe'; Build = 'engine'; Script = $engineCmd }
        @{ Name = 'NovaBar.Capture.exe'; Build = 'capture'; Script = $captureCmd }
    )) {
    if ($Target -ne 'all' -and $Target -ne $step.Build) {
        continue
    }

    Write-Host "── 构建 $($step.Name) ──" -ForegroundColor Cyan
    [IO.File]::WriteAllText($batchPath, "@echo off`r`n@chcp 65001 >nul`r`n" + $step.Script, [Text.UTF8Encoding]::new($false))

    & cmd.exe /c "`"$batchPath`"" 2>&1 | ForEach-Object { Write-Host $_ }

    if ($LASTEXITCODE -ne 0) {
        Write-Host "构建失败：$($step.Name)（exit $LASTEXITCODE）" -ForegroundColor Red
        $failed = $true
        break
    }
}

Remove-Item $batchPath -Force -ErrorAction SilentlyContinue

if ($failed) {
    exit 1
}

Write-Host ''
Write-Host '构建完成：' -ForegroundColor Green
Get-ChildItem $artifactDir -File | Select-Object Name, @{ n = 'Size'; e = { '{0:N0} B' -f $_.Length } } | Format-Table -AutoSize
