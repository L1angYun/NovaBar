# Only this installation's executables are eligible for shutdown.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InstallDirectory,
    [string]$DataDirectory = (Join-Path $env:LOCALAPPDATA 'NovaBar'),
    [string]$StatePath
)
$ErrorActionPreference = 'Stop'
$installRoot = [IO.Path]::GetFullPath($InstallDirectory).TrimEnd('\','/')
$engine = Join-Path $installRoot 'NovaBar.Engine.exe'
$dataRoot = [IO.Path]::GetFullPath($DataDirectory).TrimEnd('\','/')
$restorePath = Join-Path $dataRoot 'restore.txt'
$owned = @()
foreach ($name in @('NovaBar','NovaBar.Engine','NovaBar.Capture')) {
    $expected = Join-Path $installRoot ($name + '.exe')
    foreach ($candidate in @(Get-Process -Name $name -ErrorAction SilentlyContinue)) {
        try {
            $null = $candidate.Handle
            if ([string]::Equals($candidate.MainModule.FileName, $expected, [StringComparison]::OrdinalIgnoreCase)) { $owned += $candidate }
            else { $candidate.Dispose() }
        } catch [System.ComponentModel.Win32Exception] { $candidate.Dispose() }
        catch [System.InvalidOperationException] { $candidate.Dispose() }
    }
}
$state = $null
if ($StatePath) {
    if (Test-Path -LiteralPath $StatePath -PathType Leaf) {
        $state = Get-Content -LiteralPath $StatePath -Raw | ConvertFrom-Json
        if ($state.schemaVersion -ne 1 -or $state.installDirectory -ine $installRoot -or $state.dataDirectory -ine $dataRoot) { throw 'Updater resume state does not belong to this installation.' }
    } else {
        $restoreExisted = Test-Path -LiteralPath $restorePath -PathType Leaf
        $state = [pscustomobject]@{
            schemaVersion = 1
            installDirectory = $installRoot
            dataDirectory = $dataRoot
            engineWasRunning = [bool]@($owned | Where-Object { $_.ProcessName -ieq 'NovaBar.Engine' }).Count
            restoreExisted = $restoreExisted
            originalRestore = $(if ($restoreExisted) { [Convert]::ToBase64String([IO.File]::ReadAllBytes($restorePath)) } else { $null })
            updateRestore = $null
        }
        [IO.File]::WriteAllText($StatePath, ($state | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
    }
}
if (Test-Path -LiteralPath $engine -PathType Leaf) {
    $restore = Start-Process -FilePath $engine -ArgumentList @('--restore','--data-dir',"`"$dataRoot`"") -WindowStyle Hidden -PassThru
    if (-not $restore.WaitForExit(10000)) { $restore.Kill(); throw 'NovaBar appearance restore timed out.' }
    if ($restore.ExitCode -ne 0) { throw "NovaBar appearance restore failed: $($restore.ExitCode)." }
}
if ($state) {
    $state.updateRestore = $(if (Test-Path -LiteralPath $restorePath -PathType Leaf) { [Convert]::ToBase64String([IO.File]::ReadAllBytes($restorePath)) } else { $null })
    [IO.File]::WriteAllText($StatePath, ($state | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
}
foreach ($candidate in $owned) {
        try {
            $null = $candidate.CloseMainWindow()
            if (-not $candidate.WaitForExit(1000)) { $candidate.Kill(); $candidate.WaitForExit() }
        } catch [System.InvalidOperationException] {
            if (-not $candidate.HasExited) { throw }
        } finally { $candidate.Dispose() }
}
