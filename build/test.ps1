[CmdletBinding()]
param([switch]$Capture)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
& (Join-Path $PSScriptRoot 'tests\PackageDocumentation.Tests.ps1')
if ($LASTEXITCODE -ne 0) { throw 'Package documentation tests failed' }
& dotnet run --project (Join-Path $root 'tests\NovaBar.Core.Tests\NovaBar.Core.Tests.csproj') -c Release
if ($LASTEXITCODE -ne 0) { throw 'Core tests failed' }
& (Join-Path $root 'src\NovaBar.Engine\tests\run-tests.ps1')
if ($LASTEXITCODE -ne 0) { throw 'Engine tests failed' }
& (Join-Path $root 'src\NovaBar.Capture\tests\run-tests.ps1') -SkipCapture:(-not $Capture)
if ($LASTEXITCODE -ne 0) { throw 'Capture broker tests failed' }
& (Join-Path $PSScriptRoot 'test-bridge.ps1') -Capture:$Capture
if ($LASTEXITCODE -ne 0) { throw 'Bridge tests failed' }
& dotnet build (Join-Path $root 'NovaBar.slnx') -c Release -p:Platform=x64 --nologo -v minimal
if ($LASTEXITCODE -ne 0) { throw 'Solution build failed' }
