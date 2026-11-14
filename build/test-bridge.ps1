[CmdletBinding()]
param([switch]$Capture)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$testOut = Join-Path $root 'artifacts\bridge-tests'
New-Item -ItemType Directory -Path $testOut -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC x64 tools are required.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$source = Join-Path $root 'src\NovaBar.XamlBridge\tests\bridge-tests.cpp'
$exe = Join-Path $testOut 'bridge-tests.exe'
$batch = Join-Path $testOut 'build.cmd'
# Device modes use only offscreen resources; keep hardware/Composition optional for CI.
$gpuTests = @(
    @{ Name = 'gpu-input-client'; RequiresDevice = $true; DeviceArguments = @(); Libraries = 'd3d11.lib dxgi.lib windowsapp.lib' },
    @{ Name = 'gpu-texture-presenter'; RequiresDevice = $false; DeviceArguments = @('--device'); Libraries = 'user32.lib ole32.lib runtimeobject.lib windowsapp.lib d3d11.lib dxgi.lib CoreMessaging.lib' },
    @{ Name = 'gpu-liquid-surface'; RequiresDevice = $false; DeviceArguments = @('--device'); Libraries = 'd3d11.lib dxgi.lib d3dcompiler.lib windowsapp.lib CoreMessaging.lib' }
)
$gpuBuild = foreach ($test in $gpuTests) {
    $testSource = Join-Path $root ('src\NovaBar.XamlBridge\tests\' + $test.Name + '-tests.cpp')
    $testExe = Join-Path $testOut ($test.Name + '-tests.exe')
    $testObj = Join-Path $testOut ($test.Name + '-tests.obj')
    'cl /nologo /std:c++20 /EHsc /permissive- /W4 /utf-8 /O2 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Fo:"{0}" /Fe:"{1}" "{2}" /link {3}' -f $testObj, $testExe, $testSource, $test.Libraries
    'if errorlevel 1 exit /b 1'
}
@"
@call "$vcvars" >nul
@cl /nologo /std:c++20 /EHsc /permissive- /W3 /utf-8 /O2 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Fo:"$testOut\\" /Fe:"$exe" "$source" /link user32.lib ole32.lib oleaut32.lib runtimeobject.lib windowsapp.lib windowscodecs.lib d2d1.lib dxguid.lib gdi32.lib d3d11.lib dxgi.lib d3dcompiler.lib dwmapi.lib
if errorlevel 1 exit /b 1
$($gpuBuild -join "`r`n")
"@ | Set-Content -LiteralPath $batch -Encoding ascii
& cmd.exe /c "`"$batch`""
if ($LASTEXITCODE -ne 0) { throw 'Bridge test compilation failed.' }
if ($Capture) { & $exe --capture } else { & $exe }
if ($LASTEXITCODE -ne 0) { throw 'Bridge tests failed.' }
foreach ($test in $gpuTests) {
    $testExe = Join-Path $testOut ($test.Name + '-tests.exe')
    if ($test.RequiresDevice -and -not $Capture) {
        Write-Host ('SKIP: ' + $test.Name + ' requires an offscreen hardware device; run -Capture for coverage.')
        continue
    }
    $testArguments = @()
    if ($Capture) { $testArguments = @($test.DeviceArguments) }
    & $testExe @testArguments
    if (-not $Capture -and $test.DeviceArguments.Count) {
        Write-Host ('SKIP: ' + $test.Name + ' offscreen device mode; pure checks only, run -Capture for device coverage.')
    }
    if ($LASTEXITCODE -ne 0) { throw ('Bridge ' + $test.Name + ' tests failed.') }
}
