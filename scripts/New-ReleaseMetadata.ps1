#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InstallerPath,
    [Parameter(Mandatory)][ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version,
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$fileName = "NovaBar-$Version-win-x64-Setup.exe"
if ([IO.Path]::GetFileName($InstallerPath) -cne $fileName -or -not (Test-Path -LiteralPath $InstallerPath -PathType Leaf)) {
    throw 'Installer filename must match the release version.'
}
$installer = Get-Item -LiteralPath $InstallerPath
if ($installer.Length -le 0) { throw 'Installer is empty.' }
$hash = (Get-FileHash -LiteralPath $InstallerPath -Algorithm SHA256).Hash.ToLowerInvariant()
$manifest = [ordered]@{
    schemaVersion = 1
    version = $Version
    installer = [ordered]@{
        fileName = $fileName
        url = "https://github.com/L1angYun/NovaBar/releases/download/v$Version/$fileName"
        sha256 = $hash
        size = $installer.Length
    }
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$manifestPath = Join-Path $OutputDirectory 'update.json'
[IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($false))
$manifestHash = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'SHA256SUMS.txt'),
    "$hash  $fileName`n$manifestHash  update.json`n", [Text.UTF8Encoding]::new($false))
[pscustomobject]@{ Installer = $installer.FullName; Manifest = $manifestPath; SHA256 = $hash; Version = $Version }
