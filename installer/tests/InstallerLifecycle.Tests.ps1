#requires -Version 5.1
[CmdletBinding()]
param([Parameter(Mandatory)][string]$CompilerPath)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$id = [guid]::NewGuid().ToString('N')
$fixture = Join-Path $root ('artifacts\installer-fixture-' + $id)
$payload = Join-Path $fixture 'payload'
$installed = Join-Path $fixture 'installed'
$neighbor = Join-Path $fixture 'neighbor'
$dataRoot = Join-Path $fixture 'protected-data-root'
New-Item -ItemType Directory -Path $payload,$neighbor,$dataRoot -Force | Out-Null
$source = Join-Path $fixture 'StandIn.cs'
[IO.File]::WriteAllText($source, @'
using System;
using System.Diagnostics;
using System.IO;
using System.Threading;
class StandIn {
    static int Main(string[] args) {
        string executable = Path.GetFileName(Environment.GetCommandLineArgs()[0]);
        int dataIndex = Array.IndexOf(args,"--data-dir");
        string dataRoot = dataIndex >= 0 ? args[dataIndex + 1] : AppDomain.CurrentDomain.BaseDirectory;
        if (Array.IndexOf(args,"--restore") >= 0) {
            bool allAlive = true;
            foreach (string name in new[] { "NovaBar.exe", "NovaBar.Engine.exe", "NovaBar.Capture.exe" }) {
                try {
                    int pid = Int32.Parse(File.ReadAllText(Path.Combine(AppDomain.CurrentDomain.BaseDirectory,name + ".pid")));
                    if (Process.GetProcessById(pid).HasExited) allAlive = false;
                } catch { allAlive = false; }
            }
            File.AppendAllText(Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"restore.log"),allAlive ? "restored-before-close\n" : "restored\n");
            Directory.CreateDirectory(dataRoot);
            File.WriteAllText(Path.Combine(dataRoot,"restore.txt"),"fixture update restore marker");
            return 0;
        }
        File.WriteAllText(Path.Combine(AppDomain.CurrentDomain.BaseDirectory,executable + ".args"),String.Join(" ",args));
        if (executable == "NovaBar.Engine.exe") {
            Directory.CreateDirectory(dataRoot);
            File.WriteAllText(Path.Combine(dataRoot,"engine-effect-state.txt"),File.Exists(Path.Combine(dataRoot,"restore.txt")) ? "normal" : "active");
        }
        File.WriteAllText(Path.Combine(AppDomain.CurrentDomain.BaseDirectory,executable + ".pid"),Process.GetCurrentProcess().Id.ToString());
        Thread.Sleep(60000);
        return 0;
    }
}
'@)
$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
& $csc /nologo /target:winexe "/out:$payload\NovaBar.Engine.exe" $source
if ($LASTEXITCODE -ne 0) { throw 'Synthetic executable compilation failed.' }
Copy-Item -LiteralPath (Join-Path $payload 'NovaBar.Engine.exe') -Destination (Join-Path $payload 'NovaBar.exe')
Copy-Item -LiteralPath (Join-Path $payload 'NovaBar.Engine.exe') -Destination (Join-Path $payload 'NovaBar.Capture.exe')
Copy-Item -LiteralPath (Join-Path $payload 'NovaBar.Engine.exe') -Destination (Join-Path $neighbor 'NovaBar.Capture.exe')
Copy-Item -LiteralPath (Join-Path $payload 'NovaBar.Engine.exe') -Destination (Join-Path $neighbor 'NovaBar.Engine.exe')
[IO.File]::WriteAllText((Join-Path $payload 'NovaBar.XamlBridge.v042.dll'), 'synthetic versioned bridge')
foreach ($file in @('coreclr.dll','hostfxr.dll','Microsoft.UI.Xaml.dll','NovaBar.pri','App.xbf','MainWindow.xbf')) {
    [IO.File]::WriteAllText((Join-Path $payload $file), "synthetic $file")
}
& $CompilerPath --quiet-progress --no-ide-signtools "--define=FixtureId=$id" "--define=FixtureDataDir=$dataRoot" '--define=AppVersion=0.4.2' "--define=PublishDir=$payload" '--define=BridgeName=NovaBar.XamlBridge.v042.dll' "--output-dir=$fixture" (Join-Path $root 'installer\NovaBar.iss')
if ($LASTEXITCODE -ne 0) { throw 'Synthetic installer compilation failed.' }
$setup = Join-Path $fixture 'NovaBar-0.4.2-win-x64-Setup.exe'
function Run-Setup([string]$Executable, [string]$LogName, [string[]]$Extra) {
    $arguments = @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/SP-',"/LOG=`"$fixture\$LogName`"") + $Extra
    $process = Start-Process -FilePath $Executable -ArgumentList $arguments -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(60000)) { $process.Kill(); throw 'Synthetic installer exceeded one minute.' }
    if ($process.ExitCode -ne 0) { throw "Synthetic installer failed: $($process.ExitCode); see $fixture\$LogName" }
}
Run-Setup $setup 'install.log' @("/DIR=`"$installed`"")
foreach ($file in @('NovaBar.exe','NovaBar.Engine.exe','NovaBar.Capture.exe','NovaBar.XamlBridge.v042.dll','coreclr.dll','hostfxr.dll','Microsoft.UI.Xaml.dll','NovaBar.pri','App.xbf','MainWindow.xbf','unins000.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $installed $file))) { throw "Installer is missing $file" }
}
$settings = Join-Path $installed 'user-settings.json'
[IO.File]::WriteAllText($settings, '{"preserve":true}')
$outsideSettings = Join-Path $fixture 'profile\NovaBar\settings.json'
New-Item -ItemType Directory -Path (Split-Path $outsideSettings -Parent) -Force | Out-Null
[IO.File]::WriteAllText($outsideSettings, '{"outside":true}')
$children = @()
$bridgeLock = $null
$uninstalled = $false
function Wait-NewOwnedProcess([string]$File, [int]$PreviousPid) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($timer.ElapsedMilliseconds -lt 5000) {
        $pidPath = Join-Path $installed ($File + '.pid')
        if (Test-Path -LiteralPath $pidPath) {
            $childPid = 0
            if ([int]::TryParse([IO.File]::ReadAllText($pidPath), [ref]$childPid) -and $childPid -ne $PreviousPid) {
                $candidate = Get-Process -Id $childPid -ErrorAction SilentlyContinue
                if ($candidate -and -not $candidate.HasExited -and $candidate.MainModule.FileName -ieq (Join-Path $installed $File)) { return $candidate }
            }
        }
        Start-Sleep -Milliseconds 25
    }
    throw "Updater did not restart owned $File"
}
try {
    foreach ($file in @('NovaBar.exe','NovaBar.Engine.exe','NovaBar.Capture.exe')) {
        $children += Start-Process -FilePath (Join-Path $installed $file) -ArgumentList @('--data-dir',"`"$dataRoot`"") -WindowStyle Hidden -PassThru
    }
    $ready = [Diagnostics.Stopwatch]::StartNew()
    while (@(Get-ChildItem -LiteralPath $installed -Filter '*.pid').Count -lt 3 -and $ready.ElapsedMilliseconds -lt 3000) { Start-Sleep -Milliseconds 25 }
    if (@(Get-ChildItem -LiteralPath $installed -Filter '*.pid').Count -ne 3) { throw 'Synthetic installed processes did not start.' }
    $neighborProcess = Start-Process -FilePath (Join-Path $neighbor 'NovaBar.Capture.exe') -WindowStyle Hidden -PassThru
    $neighborEngine = Start-Process -FilePath (Join-Path $neighbor 'NovaBar.Engine.exe') -WindowStyle Hidden -PassThru
    $children += $neighborProcess,$neighborEngine
    [IO.File]::WriteAllText((Join-Path $installed 'NovaBar.XamlBridge.v042.dll'), 'resident bridge bytes must stay unchanged')
    $bridgeLock = [IO.File]::Open((Join-Path $installed 'NovaBar.XamlBridge.v042.dll'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    Run-Setup $setup 'repair.log' @("/DIR=`"$installed`"",'/CLOSEAPPLICATIONS','/NOVABARUPDATE=1')
    foreach ($child in $children[0..2]) { if (-not $child.HasExited) { throw 'Installed fixture process was not closed.' } }
    if ($neighborProcess.HasExited -or $neighborEngine.HasExited) { throw 'Neighbor installation was incorrectly closed.' }
    if (-not [IO.File]::ReadAllText((Join-Path $installed 'restore.log')).Contains('restored-before-close')) { throw 'Appearance was not restored while all installed processes were still alive.' }
    if ([IO.File]::ReadAllText((Join-Path $installed 'NovaBar.XamlBridge.v042.dll')) -cne 'resident bridge bytes must stay unchanged') { throw 'Repair replaced a resident versioned bridge.' }
    if ([IO.File]::ReadAllText($settings) -cne '{"preserve":true}') { throw 'Repair changed custom settings.' }
    $activeEngine = Wait-NewOwnedProcess 'NovaBar.Engine.exe' $children[1].Id
    $activeUi = Wait-NewOwnedProcess 'NovaBar.exe' $children[0].Id
    $children += $activeEngine,$activeUi
    if ((Test-Path -LiteralPath (Join-Path $dataRoot 'restore.txt')) -or [IO.File]::ReadAllText((Join-Path $dataRoot 'engine-effect-state.txt')) -cne 'active') { throw 'Active appearance was not resumed after update.' }
    if ([IO.File]::ReadAllText((Join-Path $installed 'NovaBar.exe.args')) -cne '--installer-update') { throw 'Updater UI did not receive the state-preserving startup flag.' }
    [IO.File]::WriteAllText((Join-Path $dataRoot 'restore.txt'), 'prior user Normal marker')
    Run-Setup $setup 'restored-normal-update.log' @("/DIR=`"$installed`"",'/CLOSEAPPLICATIONS','/NOVABARUPDATE=1')
    $normalEngine = Wait-NewOwnedProcess 'NovaBar.Engine.exe' $activeEngine.Id
    $normalUi = Wait-NewOwnedProcess 'NovaBar.exe' $activeUi.Id
    $children += $normalEngine,$normalUi
    if ([IO.File]::ReadAllText((Join-Path $dataRoot 'restore.txt')) -cne 'prior user Normal marker' -or [IO.File]::ReadAllText((Join-Path $dataRoot 'engine-effect-state.txt')) -cne 'normal') { throw 'Previously restored Normal appearance was reactivated.' }
    $normalEngine.Kill(); $normalEngine.WaitForExit()
    Remove-Item -LiteralPath (Join-Path $dataRoot 'restore.txt')
    [IO.File]::WriteAllText($outsideSettings, '{"global":{"auto_apply":false},"outside":true}')
    Run-Setup $setup 'stopped-engine-update.log' @("/DIR=`"$installed`"",'/CLOSEAPPLICATIONS','/NOVABARUPDATE=1')
    $stoppedUi = Wait-NewOwnedProcess 'NovaBar.exe' $normalUi.Id
    $children += $stoppedUi
    foreach ($candidate in @(Get-Process -Name 'NovaBar.Engine' -ErrorAction SilentlyContinue)) {
        if ($candidate.MainModule.FileName -ieq (Join-Path $installed 'NovaBar.Engine.exe')) { throw 'Previously stopped engine was activated by update.' }
    }
    if ([IO.File]::ReadAllText($outsideSettings) -cne '{"global":{"auto_apply":false},"outside":true}') { throw 'Updater modified AutoApply=false settings.' }
    if (-not (Test-Path -LiteralPath (Join-Path $dataRoot 'restore.txt'))) { throw 'Stopped Normal state lost its restore marker.' }
    $bridgeLock.Dispose(); $bridgeLock = $null
    Run-Setup (Join-Path $installed 'unins000.exe') 'uninstall.log' @()
    $uninstalled = $true
    if (Test-Path -LiteralPath (Join-Path $installed 'NovaBar.exe')) { throw 'Uninstaller left an owned executable.' }
    if ([IO.File]::ReadAllText($settings) -cne '{"preserve":true}' -or [IO.File]::ReadAllText($outsideSettings) -cne '{"global":{"auto_apply":false},"outside":true}') { throw 'Uninstaller changed user settings.' }
    Write-Host 'PASS: synthetic install, locked bridge repair, restore-before-close, scoped shutdown, active engine/UI relaunch, prior Normal preservation, stopped engine preservation, AutoApply=false settings, uninstall'
    Write-Host "Evidence: $fixture"
} finally {
    if ($bridgeLock) { $bridgeLock.Dispose() }
    foreach ($child in $children) { if (-not $child.HasExited) { $child.Kill(); $child.WaitForExit() }; $child.Dispose() }
    foreach ($name in @('NovaBar','NovaBar.Engine','NovaBar.Capture')) {
        foreach ($candidate in @(Get-Process -Name $name -ErrorAction SilentlyContinue)) {
            if ($candidate.MainModule.FileName -ieq (Join-Path $installed ($name + '.exe'))) { $candidate.Kill(); $candidate.WaitForExit() }
            $candidate.Dispose()
        }
    }
    $fixtureUninstaller = Join-Path $installed 'unins000.exe'
    if (-not $uninstalled -and (Test-Path -LiteralPath $fixtureUninstaller -PathType Leaf)) { Run-Setup $fixtureUninstaller 'cleanup-uninstall.log' @() }
}
