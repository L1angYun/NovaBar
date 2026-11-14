[CmdletBinding()]
param([switch]$SkipCapture)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\..')).Path
$outDir = Join-Path $root 'artifacts\capture-tests'
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
$source = Join-Path $PSScriptRoot 'broker-tests.cpp'
$supervisionSource = Join-Path $PSScriptRoot 'supervision-tests.cpp'
$statusSource = Join-Path $PSScriptRoot 'status-tests.cpp'
$sceneSource = Join-Path $PSScriptRoot 'scene-tests.cpp'
$panelSource = Join-Path $PSScriptRoot 'panel-tests.cpp'
$integrationSource = Join-Path $PSScriptRoot 'scene-integration.cpp'
$gpuSource = Join-Path $PSScriptRoot 'gpu-tests.cpp'
$schedulerSource = Join-Path $PSScriptRoot 'scheduler-tests.cpp'
$gpuSceneSource = Join-Path $PSScriptRoot 'gpu-scene-tests.cpp'
$gpuTextureSource = Join-Path $PSScriptRoot 'gpu-texture-tests.cpp'
$gpuStripSource = Join-Path $PSScriptRoot 'gpu-strip-tests.cpp'
$gpuAllocationSource = Join-Path $PSScriptRoot 'gpu-allocation-tests.cpp'
$gpuClientOpticsSource = Join-Path $PSScriptRoot 'gpu-client-optics-tests.cpp'
$gpuTransferSource = Join-Path $PSScriptRoot 'gpu-transfer-tests.cpp'
$gpuMappedReaderSource = Join-Path $PSScriptRoot 'gpu-mapped-reader-tests.cpp'
$captureWakeSource = Join-Path $PSScriptRoot 'capture-wake-tests.cpp'
$capturePoolWakeSource = Join-Path $PSScriptRoot 'capture-pool-wake-tests.cpp'
$gpuOutputSource = Join-Path $PSScriptRoot 'gpu-output-tests.cpp'
$gpuInputBrokerSource = Join-Path $PSScriptRoot 'gpu-input-broker-tests.cpp'
$gpuBrokerIntegrationSource = Join-Path $PSScriptRoot 'gpu-broker-integration-tests.cpp'
$exe = Join-Path $outDir 'NovaBar.Capture.Tests.exe'
$supervisionExe = Join-Path $outDir 'NovaBar.Capture.Supervision.Tests.exe'
$statusExe = Join-Path $outDir 'NovaBar.Capture.Status.Tests.exe'
$sceneExe = Join-Path $outDir 'NovaBar.Capture.Scene.Tests.exe'
$panelExe = Join-Path $outDir 'NovaBar.Capture.Panel.Tests.exe'
$integrationExe = Join-Path $outDir 'NovaBar.Capture.Scene.Integration.exe'
$gpuExe = Join-Path $outDir 'NovaBar.Capture.Gpu.Tests.exe'
$schedulerExe = Join-Path $outDir 'NovaBar.Capture.Scheduler.Tests.exe'
$gpuSceneExe = Join-Path $outDir 'NovaBar.Capture.Gpu.Scene.Tests.exe'
$gpuTextureExe = Join-Path $outDir 'NovaBar.Capture.Gpu.Texture.Tests.exe'
$gpuStripExe = Join-Path $outDir 'NovaBar.Capture.Gpu.Strip.Tests.exe'
$gpuAllocationExe = Join-Path $outDir 'NovaBar.Capture.Gpu.Allocation.Tests.exe'
$gpuClientOpticsExe = Join-Path $outDir 'NovaBar.Capture.Gpu.ClientOptics.Tests.exe'
$gpuTransferExe = Join-Path $outDir 'NovaBar.Capture.Gpu.Transfer.Tests.exe'
$gpuMappedReaderExe = Join-Path $outDir 'NovaBar.Capture.Gpu.MappedReader.Tests.exe'
$captureWakeExe = Join-Path $outDir 'NovaBar.Capture.Wake.Tests.exe'
$capturePoolWakeExe = Join-Path $outDir 'NovaBar.Capture.PoolWake.Tests.exe'
$gpuOutputExe = Join-Path $outDir 'NovaBar.Capture.Gpu.Output.Tests.exe'
$gpuInputBrokerExe = Join-Path $outDir 'NovaBar.Capture.Gpu.InputBroker.Tests.exe'
$gpuBrokerIntegrationExe = Join-Path $outDir 'NovaBar.Capture.Gpu.BrokerIntegration.Tests.exe'
$sceneLibs = 'user32.lib gdi32.lib dwmapi.lib windowsapp.lib runtimeobject.lib ole32.lib d2d1.lib dxguid.lib d3d11.lib dxgi.lib d3dcompiler.lib'
$batch = Join-Path $outDir 'capture-tests-build.cmd'
$build = @"
@echo off
call "$vcvars" >nul
cl /nologo /std:c++20 /EHsc /W4 /O2 /utf-8 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Fe:"$exe" /Fo:"$outDir\broker-tests.obj" "$source" /link user32.lib gdi32.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /O2 /utf-8 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Fe:"$supervisionExe" /Fo:"$outDir\supervision-tests.obj" "$supervisionSource" /link user32.lib ole32.lib oleaut32.lib advapi32.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /O2 /utf-8 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Fe:"$statusExe" /Fo:"$outDir\status-tests.obj" "$statusSource" /link $sceneLibs
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /O2 /utf-8 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Fe:"$sceneExe" /Fo:"$outDir\scene-tests.obj" "$sceneSource" /link $sceneLibs
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /O2 /utf-8 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Fe:"$panelExe" /Fo:"$outDir\panel-tests.obj" "$panelSource" /link $sceneLibs
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /O2 /utf-8 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Fe:"$integrationExe" /Fo:"$outDir\scene-integration.obj" "$integrationSource" /link $sceneLibs
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /O2 /utf-8 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Fe:"$gpuExe" /Fo:"$outDir\gpu-tests.obj" "$gpuSource" /link d3d11.lib dxgi.lib d3dcompiler.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /O2 /utf-8 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Fe:"$schedulerExe" /Fo:"$outDir\scheduler-tests.obj" "$schedulerSource"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /O2 /utf-8 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Fe:"$gpuSceneExe" /Fo:"$outDir\gpu-scene-tests.obj" "$gpuSceneSource" /link $sceneLibs
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /O2 /utf-8 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Fe:"$gpuTextureExe" /Fo:"$outDir\gpu-texture-tests.obj" "$gpuTextureSource" /link d3d11.lib dxgi.lib d3dcompiler.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /O2 /utf-8 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Fe:"$gpuStripExe" /Fo:"$outDir\gpu-strip-tests.obj" "$gpuStripSource" /link d3d11.lib dxgi.lib d3dcompiler.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /O2 /utf-8 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Fe:"$gpuAllocationExe" /Fo:"$outDir\gpu-allocation-tests.obj" "$gpuAllocationSource" /link $sceneLibs
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /O2 /utf-8 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Fe:"$gpuClientOpticsExe" /Fo:"$outDir\gpu-client-optics-tests.obj" "$gpuClientOpticsSource" /link d3d11.lib dxgi.lib d3dcompiler.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /O2 /utf-8 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Fe:"$gpuTransferExe" /Fo:"$outDir\gpu-transfer-tests.obj" "$gpuTransferSource"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /O2 /utf-8 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Fe:"$gpuMappedReaderExe" /Fo:"$outDir\gpu-mapped-reader-tests.obj" "$gpuMappedReaderSource"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /O2 /utf-8 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Fe:"$captureWakeExe" /Fo:"$outDir\capture-wake-tests.obj" "$captureWakeSource"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /O2 /utf-8 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Fe:"$capturePoolWakeExe" /Fo:"$outDir\capture-pool-wake-tests.obj" "$capturePoolWakeSource" /link $sceneLibs
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /O2 /utf-8 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Fe:"$gpuOutputExe" /Fo:"$outDir\gpu-output-tests.obj" "$gpuOutputSource" /link d3d11.lib dxgi.lib d3dcompiler.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /O2 /utf-8 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Fe:"$gpuInputBrokerExe" /Fo:"$outDir\gpu-input-broker-tests.obj" "$gpuInputBrokerSource" /link d3d11.lib dxgi.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /O2 /utf-8 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Fe:"$gpuBrokerIntegrationExe" /Fo:"$outDir\gpu-broker-integration-tests.obj" "$gpuBrokerIntegrationSource" /link $sceneLibs
if errorlevel 1 exit /b 1
"@
[IO.File]::WriteAllText($batch, $build, [Text.UTF8Encoding]::new($false))
& cmd.exe /d /c "`"$batch`""
if ($LASTEXITCODE -ne 0) { throw 'Capture broker test compilation failed.' }
$fixture = Join-Path $outDir ('fixture-' + [guid]::NewGuid().ToString('N'))
try {
    if ($SkipCapture) { & $exe $fixture }
    else {
        & (Join-Path $root 'build\native-build.ps1') -Target capture -OutputDirectory $outDir
        if ($LASTEXITCODE -ne 0) { throw 'Capture fixture broker compilation failed.' }
        & $exe $fixture (Join-Path $outDir 'NovaBar.Capture.exe')
    }
    if ($LASTEXITCODE -ne 0) { throw 'Capture broker tests failed.' }
    & $supervisionExe (Join-Path $fixture 'supervision')
    if ($LASTEXITCODE -ne 0) { throw 'Capture broker supervision tests failed.' }
    & $statusExe (Join-Path $fixture 'status')
    if ($LASTEXITCODE -ne 0) { throw 'Capture broker status tests failed.' }
    & $sceneExe
    if ($LASTEXITCODE -ne 0) { throw 'Capture scene tests failed.' }
    & $panelExe
    if ($LASTEXITCODE -ne 0) { throw 'Capture panel renderer tests failed.' }
    & $schedulerExe
    if ($LASTEXITCODE -ne 0) { throw 'Capture scheduler tests failed.' }
    # Wake policies use fake capture/window boundaries and run on headless CI.
    & $captureWakeExe
    if ($LASTEXITCODE -ne 0) { throw 'Capture event wake tests failed.' }
    & $capturePoolWakeExe
    if ($LASTEXITCODE -ne 0) { throw 'Capture pool wake tests failed.' }
    # Transport/owner policies are pure; hardware suites remain optional on CI.
    & $gpuTransferExe
    if ($LASTEXITCODE -ne 0) { throw 'Capture GPU transfer and owner tests failed.' }
    & $gpuMappedReaderExe
    if ($LASTEXITCODE -ne 0) { throw 'Capture mapped GPU reader tests failed.' }
    if ($SkipCapture) {
        Write-Host 'SKIP: offscreen GPU optics/output/input-broker/integration and capture fixtures; run without -SkipCapture (build/test.ps1 -Capture) for coverage.'
    }
    if (-not $SkipCapture) {
        & $gpuClientOpticsExe
        if ($LASTEXITCODE -ne 0) { throw 'Capture GPU client optics tests failed.' }
        & $gpuOutputExe
        if ($LASTEXITCODE -ne 0) { throw 'Capture GPU texture output tests failed.' }
        & $gpuInputBrokerExe
        if ($LASTEXITCODE -ne 0) { throw 'Capture GPU input broker tests failed.' }
        & $gpuBrokerIntegrationExe
        if ($LASTEXITCODE -ne 0) { throw 'Capture GPU broker integration tests failed.' }
        & $gpuExe
        if ($LASTEXITCODE -ne 0) { throw 'Capture GPU preprocessor tests failed.' }
        & $gpuSceneExe
        if ($LASTEXITCODE -ne 0) { throw 'Capture GPU scene tests failed.' }
        & $gpuTextureExe
        if ($LASTEXITCODE -ne 0) { throw 'Capture GPU texture tests failed.' }
        & $gpuStripExe
        if ($LASTEXITCODE -ne 0) { throw 'Capture GPU strip tests failed.' }
        & $gpuAllocationExe
        if ($LASTEXITCODE -ne 0) { throw 'Capture GPU allocation recovery tests failed.' }
        & $integrationExe
        if ($LASTEXITCODE -ne 0) { throw 'Capture scene window integration failed.' }
    }
} finally {
    if (Test-Path -LiteralPath $fixture) {
        $resolvedFixture = [IO.Path]::GetFullPath($fixture)
        $resolvedRoot = [IO.Path]::GetFullPath($outDir).TrimEnd('\') + '\'
        if (-not $resolvedFixture.StartsWith($resolvedRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture escaped test output directory.' }
        Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
    }
}
