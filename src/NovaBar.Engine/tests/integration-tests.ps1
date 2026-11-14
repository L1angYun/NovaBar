param([Parameter(Mandatory)][string]$EngineExe, [Parameter(Mandatory)][string]$FixtureDir)
$ErrorActionPreference = 'Stop'
$dataDir = Join-Path $FixtureDir 'cli'
New-Item -ItemType Directory -Path $dataDir | Out-Null
$apply = Join-Path $dataDir 'apply.txt'
$identityHeader = [IO.File]::ReadAllText((Join-Path $PSScriptRoot '..\..\NovaBar.Shared\BridgeIdentity.h'))
function Read-Identity([string]$Name) {
    $match = [regex]::Match($identityHeader, '\b' + [regex]::Escape($Name) + '\[\]\s*=\s*L"([^"\r\n]+)"')
    if (-not $match.Success) { throw "Shared bridge identity is missing $Name" }
    return $match.Groups[1].Value
}
$state = Join-Path $dataDir (Read-Identity 'TaskbarState')
$startState = Join-Path $dataDir (Read-Identity 'StartState')
$notificationState = Join-Path $dataDir (Read-Identity 'NotificationState')
$legacyStates = @('state.txt','state.start.txt','state.notification.txt','state.search.txt',
    'state.v04.txt','state.start.v04.txt','state.notification.v04.txt','state.search.v04.txt',
    'state.v041.txt','state.start.v041.txt','state.notification.v041.txt','state.search.v041.txt',
    'state.v042.txt','state.start.v042.txt','state.notification.v042.txt','state.search.v042.txt')
if (-not ('NovaBarTestThread' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class NovaBarTestThread {
    [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr OpenThread(uint access, bool inherit, uint id);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern uint SuspendThread(IntPtr thread);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern uint ResumeThread(IntPtr thread);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
}
'@
}

function Assert-Result([bool]$Condition, [string]$Name) {
    if (-not $Condition) { throw "FAIL: $Name" }
    Write-Host "PASS: $Name"
}
function Set-Appearance([string]$Spec) {
    # All states use one fixture spec so tests do not depend on the user's foreground window.
    $lines = @('enabled=1')
    $lines += @('desktop','visible','maximized','start','search','taskview','battery') | ForEach-Object { "$_=$Spec" }
    [IO.File]::WriteAllText($apply, ($lines -join "`n"))
}
function Invoke-Engine([string[]]$Options, [int]$ExpectedExit = 0) {
    & $EngineExe @Options --data-dir $dataDir
    Assert-Result ($LASTEXITCODE -eq $ExpectedExit) "CLI exits $ExpectedExit for $($Options -join ' ')"
}
function Wait-State([string]$Spec) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($timer.ElapsedMilliseconds -lt 5000) {
        if ((Test-Path -LiteralPath $state) -and [IO.File]::ReadAllText($state).TrimStart([char]0xfeff) -eq $Spec) { return }
        Start-Sleep -Milliseconds 25
    }
    throw "$([IO.Path]::GetFileName($state)) did not become $Spec"
}
function Assert-LegacyRetired {
    foreach ($legacy in $legacyStates) {
        $path = Join-Path $dataDir $legacy
        Assert-Result ((Test-Path -LiteralPath $path) -and [IO.File]::ReadAllText($path).TrimStart([char]0xfeff) -eq $normal) "legacy $legacy remains Normal beside the current bridge channel"
    }
}

$first = 'accent=liquid;color=#FF123456;line=0;specular=1;tint=0.5'
$second = 'accent=color;color=#FF654321'
$normal = 'accent=normal;color=#00000000;line=0;specular=0;tint=0'
foreach ($legacy in $legacyStates) {
    [IO.File]::WriteAllText((Join-Path $dataDir $legacy), $first)
}
Invoke-Engine @('--once','--no-inject') 3
Assert-LegacyRetired
Assert-Result (-not (Test-Path -LiteralPath $state)) 'missing config does not publish a fabricated appearance'
Set-Appearance $first
$timer = [Diagnostics.Stopwatch]::StartNew()
Invoke-Engine @('--once','--no-inject')
Assert-Result ($timer.ElapsedMilliseconds -lt 2500) 'isolated once calculation never waits on shell injection'
Wait-State $first
Assert-LegacyRetired
Assert-Result (-not (Test-Path -LiteralPath (Join-Path $dataDir 'bridge.log'))) 'no-inject performs no bridge load'
Invoke-Engine @('--restore','--no-inject')
Wait-State $normal
Invoke-Engine @('--once','--no-inject')
Wait-State $normal
Assert-Result (Test-Path -LiteralPath (Join-Path $dataDir 'restore.txt')) 'restore survives later engine invocations'
$priorStamp = [IO.File]::GetLastWriteTimeUtc($apply)
Set-Appearance $second
[IO.File]::SetLastWriteTimeUtc($apply, $priorStamp.AddSeconds(1))
Invoke-Engine @('--once','--no-inject')
Wait-State $second
Assert-Result (-not (Test-Path -LiteralPath (Join-Path $dataDir 'restore.txt'))) 'new apply timestamp resumes configured appearance'

$start = [Diagnostics.ProcessStartInfo]::new($EngineExe)
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.ArgumentList.Add('--no-inject')
$start.ArgumentList.Add('--data-dir')
$start.ArgumentList.Add($dataDir)
$daemon = [Diagnostics.Process]::Start($start)
try {
    Wait-State $second
    Start-Sleep -Milliseconds 250
    $daemon.Refresh()
    $loaded = @($daemon.Modules | ForEach-Object { $_.ModuleName })
    Assert-Result (-not @($loaded | Where-Object { $_ -like 'NovaBar.XamlBridge*.dll' }).Count -and -not ($loaded -contains 'Windows.UI.Xaml.dll')) 'resident no-inject engine loads neither current nor legacy bridge nor XAML diagnostics'
    # Pause only this test host's main thread to make the restore/reapply race deterministic.
    # It resumes with the same desired spec it cached before --restore wrote Normal.
    $mainThread = $daemon.Threads | Sort-Object StartTime | Select-Object -First 1
    $threadHandle = [NovaBarTestThread]::OpenThread(2, $false, $mainThread.Id)
    Assert-Result ($threadHandle -ne [IntPtr]::Zero) 'isolated main thread is available for deterministic race test'
    $suspended = $false
    try {
        $suspended = [NovaBarTestThread]::SuspendThread($threadHandle) -ne [uint32]::MaxValue
        Assert-Result $suspended 'isolated main thread paused for restore/reapply'
        Invoke-Engine @('--restore','--no-inject')
        $priorStamp = [IO.File]::GetLastWriteTimeUtc($apply)
        Set-Appearance $second
        [IO.File]::SetLastWriteTimeUtc($apply, $priorStamp.AddSeconds(1))
    } finally {
        if ($suspended) { [void][NovaBarTestThread]::ResumeThread($threadHandle) }
        [void][NovaBarTestThread]::CloseHandle($threadHandle)
    }
    Wait-State $second
    Assert-Result $true 'same appearance reapply repairs state changed by concurrent restore'
    Invoke-Engine @('--restore','--no-inject')
    Wait-State $normal
    Start-Sleep -Milliseconds 500
    Assert-Result ([IO.File]::ReadAllText($state).TrimStart([char]0xfeff) -eq $normal) 'running engine honors persistent restore without being stopped'
    $priorStamp = [IO.File]::GetLastWriteTimeUtc($apply)
    Set-Appearance $first
    [IO.File]::SetLastWriteTimeUtc($apply, $priorStamp.AddSeconds(1))
    Wait-State $first
    Assert-Result (-not $daemon.HasExited) 'running engine resumes immediately when configuration changes'

    $duplicateStart = [Diagnostics.ProcessStartInfo]::new($EngineExe)
    $duplicateStart.UseShellExecute = $false
    $duplicateStart.CreateNoWindow = $true
    $duplicateStart.ArgumentList.Add('--no-inject')
    $duplicateStart.ArgumentList.Add('--data-dir')
    $duplicateStart.ArgumentList.Add($dataDir + '\.')
    $duplicate = [Diagnostics.Process]::Start($duplicateStart)
    try {
        Assert-Result ($duplicate.WaitForExit(2000)) 'resident duplicate exits for canonical alias of same data directory'
        Assert-Result ($duplicate.ExitCode -eq 0 -and -not $daemon.HasExited) 'resident owner survives duplicate startup'
        Assert-Result ([IO.File]::ReadAllText($state).TrimStart([char]0xfeff) -eq $first) 'duplicate startup does not restore over owner appearance'
    } finally {
        if (-not $duplicate.HasExited) { $duplicate.Kill(); $duplicate.WaitForExit() }
        $duplicate.Dispose()
    }

    $independentDir = Join-Path $FixtureDir 'independent'
    New-Item -ItemType Directory -Path $independentDir | Out-Null
    [IO.File]::WriteAllText((Join-Path $independentDir 'apply.txt'), "enabled=0`n")
    $independentStart = [Diagnostics.ProcessStartInfo]::new($EngineExe)
    $independentStart.UseShellExecute = $false
    $independentStart.CreateNoWindow = $true
    $independentStart.ArgumentList.Add('--no-inject')
    $independentStart.ArgumentList.Add('--data-dir')
    $independentStart.ArgumentList.Add($independentDir)
    $independent = [Diagnostics.Process]::Start($independentStart)
    try {
        Assert-Result (-not $independent.WaitForExit(500) -and -not $daemon.HasExited) 'different data directories can own independent resident engines'
    } finally {
        if (-not $independent.HasExited) { $independent.Kill(); $independent.WaitForExit() }
        $independent.Dispose()
    }
} finally {
    # Only the isolated --no-inject fixture process is stopped.
    if (-not $daemon.HasExited) { $daemon.Kill(); $daemon.WaitForExit() }
    $daemon.Dispose()
}

$startSpec = 'accent=color;color=#FF334455'
$notificationSpec = 'accent=color;color=#FF778899'
$startApply = Join-Path $dataDir 'apply.start.txt'
$notificationApply = Join-Path $dataDir 'apply.notification.txt'
$startLines = @('enabled=1')
$notificationLines = @('enabled=1')
foreach ($key in @('desktop','visible','maximized','start','search','taskview','battery')) {
    $startLines += "$key=$startSpec"
    $notificationLines += "$key=$notificationSpec"
}
[IO.File]::WriteAllText($startApply, ($startLines -join "`n"))
[IO.File]::WriteAllText($notificationApply, ($notificationLines -join "`n"))
[IO.File]::WriteAllText($apply, "enabled=0`nsurface.start=1`nsurface.notification=1`n")
Invoke-Engine @('--once','--no-inject')
Wait-State $normal
Assert-Result ([IO.File]::ReadAllText($startState).TrimStart([char]0xfeff) -eq $startSpec) 'Start appearance resolves independently with taskbar disabled'
Assert-Result ([IO.File]::ReadAllText($notificationState).TrimStart([char]0xfeff) -eq $notificationSpec) 'notification appearance resolves independently with taskbar disabled'
Assert-LegacyRetired
[IO.File]::WriteAllText($startApply, "enabled=0`ndesktop=$startSpec`n")
Invoke-Engine @('--once','--no-inject')
Assert-Result ([IO.File]::ReadAllText($startState).TrimStart([char]0xfeff) -eq $normal) 'disabled Start restores only Start appearance'
Assert-Result ([IO.File]::ReadAllText($notificationState).TrimStart([char]0xfeff) -eq $notificationSpec) 'disabled Start preserves notification appearance'
Invoke-Engine @('--restore','--no-inject')
Invoke-Engine @('--once','--no-inject')
Assert-Result ([IO.File]::ReadAllText($notificationState).TrimStart([char]0xfeff) -eq $normal) 'restore covers independent shell surface channels'
Assert-LegacyRetired
