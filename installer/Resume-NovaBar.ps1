# Restores the state captured by this updater, then opens the installed UI.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InstallDirectory,
    [Parameter(Mandatory)][string]$DataDirectory,
    [Parameter(Mandatory)][string]$StatePath
)
$ErrorActionPreference = 'Stop'
$installRoot = [IO.Path]::GetFullPath($InstallDirectory).TrimEnd('\','/')
$dataRoot = [IO.Path]::GetFullPath($DataDirectory).TrimEnd('\','/')
if (Test-Path -LiteralPath $StatePath -PathType Leaf) {
    $state = Get-Content -LiteralPath $StatePath -Raw | ConvertFrom-Json
    if ($state.schemaVersion -ne 1 -or $state.installDirectory -ine $installRoot -or $state.dataDirectory -ine $dataRoot -or
        $state.engineWasRunning -isnot [bool] -or $state.restoreExisted -isnot [bool]) { throw 'Invalid updater resume state.' }
    $marker = Join-Path $dataRoot 'restore.txt'
    # A newer user restore request must survive the update handoff.
    if ($state.updateRestore -and (Test-Path -LiteralPath $marker -PathType Leaf) -and
        [Convert]::ToBase64String([IO.File]::ReadAllBytes($marker)) -ceq $state.updateRestore) {
        if ($state.restoreExisted) { [IO.File]::WriteAllBytes($marker, [Convert]::FromBase64String($state.originalRestore)) }
        elseif ($state.engineWasRunning) { Remove-Item -LiteralPath $marker }
    }
    if ($state.engineWasRunning) {
        $engine = Join-Path $installRoot 'NovaBar.Engine.exe'
        $startedEngine = Start-Process -FilePath $engine -ArgumentList @('--data-dir',"`"$dataRoot`"") -WindowStyle Hidden -PassThru
        $startedEngine.Dispose()
    }
}
$ui = [Diagnostics.ProcessStartInfo]::new((Join-Path $installRoot 'NovaBar.exe'))
$ui.UseShellExecute = $false
$ui.CreateNoWindow = $true
$ui.WorkingDirectory = $installRoot
$ui.Arguments = '--installer-update'
$startedUi = [Diagnostics.Process]::Start($ui)
if (-not $startedUi) { throw 'Updated NovaBar UI did not start.' }
$startedUi.Dispose()
