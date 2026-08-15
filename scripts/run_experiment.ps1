<#
.SYNOPSIS
    Build CacheScope and produce a report, in one command.

.DESCRIPTION
    Configures, builds, tests and runs the headless benchmark, then opens the
    HTML report. Requires only CMake and a C++ compiler; no vcpkg, no Ninja and
    no Developer Command Prompt.

.PARAMETER Preset
    quick, standard (default) or deep.

.PARAMETER Label
    Name for this machine in the report. Defaults to the computer name.

.PARAMETER Repeat
    Number of runs. Three is recommended when comparing machines: it shows how
    much of any difference is just run-to-run variation.

.PARAMETER Out
    Report directory. Defaults to .\results

.PARAMETER NoOpen
    Do not open the HTML report when finished.

.EXAMPLE
    .\scripts\run_experiment.ps1

.EXAMPLE
    .\scripts\run_experiment.ps1 -Preset deep -Repeat 3 -Label "old laptop"
#>
[CmdletBinding()]
param(
    [ValidateSet('quick', 'standard', 'deep')]
    [string]$Preset = 'standard',
    [string]$Label = $env:COMPUTERNAME,
    [int]$Repeat = 1,
    [string]$Out = 'results',
    [switch]$NoOpen
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "cmake was not found on PATH. Install CMake 3.22 or newer from https://cmake.org/download/"
}

Write-Host "==> Configuring" -ForegroundColor Cyan
cmake --preset headless-release
if ($LASTEXITCODE -ne 0) { throw "configure failed" }

Write-Host "==> Building" -ForegroundColor Cyan
cmake --build --preset headless-release --parallel
if ($LASTEXITCODE -ne 0) { throw "build failed" }

Write-Host "==> Testing" -ForegroundColor Cyan
ctest --preset headless-release
if ($LASTEXITCODE -ne 0) { throw "tests failed" }

$exe = Join-Path $root 'build\headless-release\bin\cachescope_cli.exe'
if (-not (Test-Path $exe)) { throw "built binary not found at $exe" }

Write-Host "==> Measuring ($Preset preset, $Repeat run(s))" -ForegroundColor Cyan
Write-Host "    Close heavy applications and leave the machine idle for the best results." -ForegroundColor DarkGray

$arguments = @('--preset', $Preset, '--label', $Label, '--repeat', $Repeat, '--out', $Out)
if (-not $NoOpen) { $arguments += '--open' }
& $exe @arguments
if ($LASTEXITCODE -ne 0) { throw "benchmark failed" }

Write-Host ""
Write-Host "Reports are in $(Resolve-Path $Out)" -ForegroundColor Green
Write-Host "Share the .csv files to compare machines:" -ForegroundColor Green
Write-Host "  build\headless-release\bin\cachescope_compare.exe --out comparison.html $Out other-machine.csv"
