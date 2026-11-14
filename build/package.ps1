#requires -Version 5.1
[CmdletBinding()]
param([string]$Configuration = 'Release', [switch]$StageOnly)
$ErrorActionPreference = 'Stop'
function Copy-PublicDocumentation {
    param([string]$RepositoryRoot, [string]$StageDirectory)
    # Local acceptance records, plans and screenshots are not release inputs.
    $publicDocuments = @('ARCHITECTURE.md', 'PARITY.md', 'RELEASE_NOTES.md')
    $documentation = Join-Path $StageDirectory 'docs'
    New-Item -ItemType Directory -Path $documentation -Force | Out-Null
    foreach ($name in $publicDocuments) {
        $source = Join-Path $RepositoryRoot "docs\$name"
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "Required public documentation is missing: $name"
        }
        Copy-Item -LiteralPath $source -Destination (Join-Path $documentation $name) -Force
    }
    $unexpected = @(Get-ChildItem -LiteralPath $documentation -Recurse -File -Force | Where-Object {
        $_.FullName.Substring($documentation.Length + 1) -notin $publicDocuments
    })
    if ($unexpected.Count) { throw 'Package documentation contains a file outside the public allowlist.' }
}
$root = Split-Path $PSScriptRoot -Parent
$dist = Join-Path $root 'dist'
$publish = Join-Path $root ('artifacts\package-publish\' + [guid]::NewGuid().ToString('N'))
$native = Join-Path $root 'artifacts\package-native'
$stage = Join-Path $dist 'NovaBar'
$identityHeader = [IO.File]::ReadAllText((Join-Path $root 'src\NovaBar.Shared\BridgeIdentity.h'))
$bridgeNameMatch = [regex]::Match($identityHeader, '\bFileName\[\]\s*=\s*L"([^"\r\n]+)"')
if (-not $bridgeNameMatch.Success -or $bridgeNameMatch.Groups[1].Value -notmatch '^NovaBar\.XamlBridge\.[A-Za-z0-9_-]+\.dll$') {
    throw '共享桥接标识中的 DLL 文件名无效。'
}
$bridgeName = $bridgeNameMatch.Groups[1].Value

Write-Host '── 构建原生组件（独立目录）──' -ForegroundColor Cyan
& (Join-Path $PSScriptRoot 'native-build.ps1') -OutputDirectory $native
if ($LASTEXITCODE -ne 0) { throw '原生构建失败' }
foreach ($name in @('NovaBar.Engine.exe', 'NovaBar.Capture.exe', $bridgeName)) {
    if (-not (Test-Path -LiteralPath (Join-Path $native $name) -PathType Leaf)) {
        throw "原生构建缺少发行组件：$name"
    }
}
Write-Host '── 发布自包含配置中心 ──' -ForegroundColor Cyan
& dotnet publish (Join-Path $root 'src\NovaBar.App\NovaBar.App.csproj') -c $Configuration -r win-x64 --self-contained true -p:Platform=x64 -p:PublishTrimmed=false -p:DebugType=None -p:DebugSymbols=false -o $publish --nologo -v minimal
if ($LASTEXITCODE -ne 0) { throw 'UI 发布失败' }
foreach ($resource in @('NovaBar.pri', 'App.xbf', 'MainWindow.xbf')) {
    if (-not (Test-Path -LiteralPath (Join-Path $publish $resource))) {
        throw "发布目录缺少应用 XAML 资源：$resource"
    }
}

if (Test-Path -LiteralPath $stage) {
    $resolvedStage = [IO.Path]::GetFullPath($stage)
    $resolvedDist = [IO.Path]::GetFullPath($dist) + [IO.Path]::DirectorySeparatorChar
    if (-not $resolvedStage.StartsWith($resolvedDist, [StringComparison]::OrdinalIgnoreCase) -or
        ((Get-Item -LiteralPath $stage).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "拒绝删除非预期的打包路径：$resolvedStage"
    }
    Remove-Item -LiteralPath $resolvedStage -Recurse -Force
}
New-Item -ItemType Directory -Path $stage -Force | Out-Null
Copy-Item -Path (Join-Path $publish '*') -Destination $stage -Recurse -Force
foreach ($name in @('NovaBar.Engine.exe', 'NovaBar.Capture.exe', $bridgeName)) {
    Copy-Item -LiteralPath (Join-Path $native $name) -Destination $stage -Force
}
foreach ($name in @('README.md', 'NOTICE.md', 'LICENSE')) {
    if (Test-Path -LiteralPath (Join-Path $root $name)) {
        Copy-Item -LiteralPath (Join-Path $root $name) -Destination $stage -Force
    }
}
Copy-PublicDocumentation -RepositoryRoot $root -StageDirectory $stage

$version = (Get-Item -LiteralPath (Join-Path $stage 'NovaBar.exe')).VersionInfo.FileVersion
if ([string]::IsNullOrWhiteSpace($version)) { throw '安装负载缺少文件版本。' }
$manifest = [ordered]@{
    version = $version
    built_at_utc = [DateTime]::UtcNow.ToString('o')
    commit = (& git -C $root rev-parse HEAD)
    working_tree_modified = [bool](& git -C $root status --porcelain)
    runtime = 'win-x64 self-contained'
    files = @(Get-ChildItem -LiteralPath $stage -File -Recurse | ForEach-Object {
        [ordered]@{ path = $_.FullName.Substring($stage.Length + 1); sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
    })
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $stage 'build-info.json') -Encoding UTF8
if ($StageOnly) { Write-Host "Self-contained installer payload: $stage"; return }
$zip = Join-Path $dist "NovaBar-$version.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
Write-Host "已打包：$zip" -ForegroundColor Green
Get-Item -LiteralPath $zip | Select-Object FullName, Length
