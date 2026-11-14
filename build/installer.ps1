#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$PayloadDirectory,
    [string]$OutputDirectory,
    [string]$CompilerPath,
    [string]$Version
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $Version) {
    [xml]$project = Get-Content -LiteralPath (Join-Path $root 'src\NovaBar.App\NovaBar.App.csproj') -Raw
    $Version = [string]$project.Project.PropertyGroup.Version | Select-Object -First 1
}
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw 'Installer version must be a stable semantic version.' }
if (-not $CompilerPath) {
    $command = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($command) { $CompilerPath = $command.Source }
    foreach ($candidate in @((Join-Path $root 'artifacts\installer-tools\inno-7.1.0\ISCC.exe'),
        'C:\Program Files\Inno Setup 7\ISCC.exe', 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe')) {
        if (-not $CompilerPath -and (Test-Path -LiteralPath $candidate -PathType Leaf)) { $CompilerPath = $candidate }
    }
}
if (-not $CompilerPath -or -not (Test-Path -LiteralPath $CompilerPath -PathType Leaf)) { throw 'Pass -CompilerPath to the pinned Inno Setup 7.1.0 ISCC.exe.' }
$compilerVersion = (& $CompilerPath --version | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $compilerVersion -cne '7.1.0') { throw 'The installer requires pinned Inno Setup 7.1.0.' }
if (-not $PayloadDirectory) {
    & (Join-Path $PSScriptRoot 'package.ps1') -StageOnly
    if ($LASTEXITCODE -ne 0) { throw 'Self-contained payload build failed.' }
    $PayloadDirectory = Join-Path $root 'dist\NovaBar'
}
$PayloadDirectory = [IO.Path]::GetFullPath($PayloadDirectory)
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root 'dist\installer' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$identity = [IO.File]::ReadAllText((Join-Path $root 'src\NovaBar.Shared\BridgeIdentity.h'))
$bridgeName = [regex]::Match($identity, '\bFileName\[\]\s*=\s*L"([^"\r\n]+)"').Groups[1].Value
if ($bridgeName -notmatch '^NovaBar\.XamlBridge\.[A-Za-z0-9_-]+\.dll$') { throw 'Invalid versioned bridge identity.' }
$required = @('NovaBar.exe','NovaBar.dll','NovaBar.Engine.exe','NovaBar.Capture.exe',$bridgeName,
    'coreclr.dll','hostfxr.dll','Microsoft.UI.Xaml.dll','NovaBar.pri','App.xbf','MainWindow.xbf','Assets\NovaBar.ico','LICENSE')
foreach ($file in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $PayloadDirectory $file) -PathType Leaf)) { throw "Self-contained payload is missing $file" }
}
$productVersion = ((Get-Item -LiteralPath (Join-Path $PayloadDirectory 'NovaBar.dll')).VersionInfo.ProductVersion -split '\+',2)[0]
if ($productVersion -cne $Version) { throw "Payload version $productVersion does not match $Version." }
foreach ($entry in Get-ChildItem -LiteralPath $PayloadDirectory -Recurse -Force) {
    if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Payload must not contain reparse points.' }
    if (-not $entry.PSIsContainer -and $entry.Extension -in @('.cs','.cpp','.h','.pdb','.ps1','.iss','.nupkg')) { throw "Payload contains build material: $($entry.Name)" }
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$arguments = @('--quiet-progress','--no-ide-signtools',"--define=AppVersion=$Version", "--define=PublishDir=$PayloadDirectory",
    "--define=BridgeName=$bridgeName", "--output-dir=$OutputDirectory", (Join-Path $root 'installer\NovaBar.iss'))
& $CompilerPath @arguments
if ($LASTEXITCODE -ne 0) { throw "Inno Setup compilation failed: $LASTEXITCODE" }
$installer = Join-Path $OutputDirectory "NovaBar-$Version-win-x64-Setup.exe"
if (-not (Test-Path -LiteralPath $installer -PathType Leaf)) { throw 'Installer output is missing.' }
if ([version](Get-Item -LiteralPath $installer).VersionInfo.FileVersion -ne [version]"$Version.0") { throw 'Installer binary version is incorrect.' }
& (Join-Path $root 'scripts\New-ReleaseMetadata.ps1') -InstallerPath $installer -Version $Version -OutputDirectory $OutputDirectory
