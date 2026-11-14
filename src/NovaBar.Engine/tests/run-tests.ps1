[CmdletBinding()]
param([switch]$SkipIntegration)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$outDir = Join-Path $root 'artifacts\native-tests'
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
$batch = Join-Path $outDir 'build-tests.cmd'
$testSource = Join-Path $PSScriptRoot 'engine-tests.cpp'
$testExe = Join-Path $outDir 'NovaBar.Engine.Tests.exe'
$engineSource = Join-Path $PSScriptRoot '..\main.cpp'
$engineExe = Join-Path $outDir 'NovaBar.Engine.TestHost.exe'
$testBuild = @"
@chcp 65001 >nul
@call "$vcvars" >nul
@cl /nologo /std:c++20 /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Fe:"$testExe" /Fo:"$outDir\\" "$testSource" /link user32.lib ole32.lib oleaut32.lib advapi32.lib
"@
[IO.File]::WriteAllText($batch, $testBuild, [Text.UTF8Encoding]::new($false))
& cmd.exe /d /c "`"$batch`""
if ($LASTEXITCODE -ne 0) { throw 'Engine test compilation failed.' }
$fixtureDir = Join-Path $outDir ('fixture-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureDir | Out-Null
try {
    & $testExe $fixtureDir
    if ($LASTEXITCODE -ne 0) { throw 'Engine regression tests failed.' }
    if (-not $SkipIntegration) {
        $engineBuild = @"
@chcp 65001 >nul
@call "$vcvars" >nul
@cl /nologo /std:c++20 /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Fe:"$engineExe" /Fo:"$outDir\\" "$engineSource" /link user32.lib ole32.lib oleaut32.lib advapi32.lib
"@
        [IO.File]::WriteAllText($batch, $engineBuild, [Text.UTF8Encoding]::new($false))
        & cmd.exe /d /c "`"$batch`""
        if ($LASTEXITCODE -ne 0) { throw 'Engine integration host compilation failed.' }
        & (Join-Path $PSScriptRoot 'integration-tests.ps1') -EngineExe $engineExe -FixtureDir $fixtureDir
    }
} finally {
    $resolvedOut = (Resolve-Path -LiteralPath $outDir).ProviderPath.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    $resolvedFixture = (Resolve-Path -LiteralPath $fixtureDir).ProviderPath
    if (-not [IO.Path]::IsPathRooted($resolvedFixture) -or -not $resolvedFixture.StartsWith($resolvedOut, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove test fixture outside $resolvedOut"
    }
    Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
}
