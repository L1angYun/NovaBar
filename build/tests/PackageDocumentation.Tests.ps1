#requires -Version 5.1
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$package = Join-Path $root 'build\package.ps1'
$tokens = $null; $parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($package, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw 'Packaging script contains parse errors.' }
$definition = $ast.Find({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Copy-PublicDocumentation'
}, $true)
if (-not $definition) { throw 'Production public-documentation function is missing.' }
# Load only the production function; never invoke native builds or dotnet publish.
. ([scriptblock]::Create($definition.Extent.Text))
$fixtureBase = Join-Path $root 'artifacts\package-doc-tests'
$fixture = Join-Path $fixtureBase ([guid]::NewGuid().ToString('N'))
$source = Join-Path $fixture 'source'
$stage = Join-Path $fixture 'stage'
$checks = 0; $failures = 0
function Check([bool]$Condition, [string]$Label) {
    $script:checks++
    if (-not $Condition) { $script:failures++; Write-Host "FAIL: $Label" }
}
try {
    New-Item -ItemType Directory -Path (Join-Path $source 'docs'), $stage -Force | Out-Null
    foreach ($name in @('ARCHITECTURE.md','PARITY.md','RELEASE_NOTES.md')) {
        [IO.File]::WriteAllText((Join-Path $source "docs\$name"), "PUBLIC FIXTURE: $name")
    }
    foreach ($name in @('COMPLETION.md','TRUE_GLASS_PLAN.md')) {
        [IO.File]::WriteAllText((Join-Path $source "docs\$name"), 'SYNTHETIC LOCAL EVIDENCE: DO NOT PACKAGE')
    }
    $privateSource = Join-Path $source 'docs\COMPLETION.md'
    $before = (Get-FileHash -LiteralPath $privateSource -Algorithm SHA256).Hash
    Copy-PublicDocumentation -RepositoryRoot $source -StageDirectory $stage
    $actual = @(Get-ChildItem -LiteralPath (Join-Path $stage 'docs') -File | Select-Object -ExpandProperty Name | Sort-Object)
    Check (($actual -join '|') -ceq 'ARCHITECTURE.md|PARITY.md|RELEASE_NOTES.md') 'only approved public documents enter the package'
    Check ((Get-FileHash -LiteralPath $privateSource -Algorithm SHA256).Hash -ceq $before) 'local evidence remains intact in its source directory'
    [IO.File]::WriteAllText((Join-Path $stage 'docs\UNAPPROVED.md'), 'STALE SYNTHETIC EVIDENCE')
    $rejected = $false
    try { Copy-PublicDocumentation -RepositoryRoot $source -StageDirectory $stage } catch { $rejected = $true }
    Check $rejected 'unexpected staged documentation stops packaging instead of leaking'
    Remove-Item -LiteralPath (Join-Path $stage 'docs\UNAPPROVED.md') -Force
    Remove-Item -LiteralPath (Join-Path $source 'docs\PARITY.md') -Force
    $rejected = $false
    try { Copy-PublicDocumentation -RepositoryRoot $source -StageDirectory $stage } catch { $rejected = $true }
    Check $rejected 'missing required public documentation stops packaging'
}
finally {
    if (Test-Path -LiteralPath $fixture) {
        $resolvedFixture = [IO.Path]::GetFullPath($fixture)
        $resolvedBase = [IO.Path]::GetFullPath($fixtureBase).TrimEnd('\') + '\'
        if (-not $resolvedFixture.StartsWith($resolvedBase, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Fixture cleanup escaped its dedicated artifact directory.'
        }
        Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
    }
}
Write-Host "Package documentation tests: $checks checks, $failures failures"
if ($failures) { exit 1 }
exit 0
