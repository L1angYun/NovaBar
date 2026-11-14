#requires -Version 5.1
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$fixture = Join-Path $root ('artifacts\release-assets-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture -Force | Out-Null
$installer = Join-Path $fixture 'NovaBar-0.4.2-win-x64-Setup.exe'
[IO.File]::WriteAllBytes($installer, [byte[]](77,90,1,2,3,4))
& (Join-Path $root 'scripts\New-ReleaseMetadata.ps1') -InstallerPath $installer -Version '0.4.2' -OutputDirectory $fixture
$manifest = Get-Content -LiteralPath (Join-Path $fixture 'update.json') -Raw | ConvertFrom-Json
$hash = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
if ($manifest.schemaVersion -ne 1 -or $manifest.version -cne '0.4.2' -or
    $manifest.installer.fileName -cne 'NovaBar-0.4.2-win-x64-Setup.exe' -or
    $manifest.installer.url -cne 'https://github.com/L1angYun/NovaBar/releases/download/v0.4.2/NovaBar-0.4.2-win-x64-Setup.exe' -or
    $manifest.installer.sha256 -cne $hash -or $manifest.installer.size -ne 6) { throw 'Release manifest contract failed.' }
$sums = [IO.File]::ReadAllText((Join-Path $fixture 'SHA256SUMS.txt'))
if (-not $sums.Contains("$hash  NovaBar-0.4.2-win-x64-Setup.exe")) { throw 'Installer checksum is missing.' }
$rejected = $false
try { & (Join-Path $root 'scripts\New-ReleaseMetadata.ps1') -InstallerPath $installer -Version '0.4.3' -OutputDirectory $fixture } catch { $rejected = $true }
if (-not $rejected) { throw 'Mismatched installer version must be rejected.' }
Write-Host 'PASS: installer release filename, fixed GitHub URL, SHA256, size, schema and version rejection'
