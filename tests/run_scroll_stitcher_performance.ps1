param(
    [Parameter(Mandatory=$true)][string]$BaselineDirectory,
    [string]$OutputDirectory,
    [int]$Samples = 30,
    [int]$Rounds = 3,
    [string]$Case = 'all',
    [switch]$BuildOnly,
    [switch]$VerifyOnly
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root 'out/scroll-stitcher-performance' }
$output = [IO.Path]::GetFullPath($OutputDirectory)
$allowed = [IO.Path]::GetFullPath((Join-Path $root 'out')) + [IO.Path]::DirectorySeparatorChar
if (-not $output.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Benchmark output must be inside the project out directory.' }
New-Item -ItemType Directory -Path $output -Force | Out-Null
$baseline = (Resolve-Path -LiteralPath $BaselineDirectory).Path
foreach ($extension in @('h','cpp')) {
    $source = [IO.File]::ReadAllText((Join-Path $baseline "scroll_stitcher.$extension"))
    foreach ($name in @('ScrollStitcher','ScrollEndDetector','ScrollAnalysis','StitchResult','FrameDifference')) {
        $source = [regex]::Replace($source, "\b$name\b", "Baseline$name")
    }
    $source = $source.Replace('"long_capture/scroll_stitcher.h"', '"baseline_stitcher.h"')
    [IO.File]::WriteAllText((Join-Path $output "baseline_stitcher.$extension"), $source, [Text.UTF8Encoding]::new($false))
}
$arguments = @('/nologo','/O2','/DNDEBUG','/std:c++17','/EHsc','/utf-8','/W4',
    '/DUNICODE','/D_UNICODE','/DNOMINMAX','/DWIN32_LEAN_AND_MEAN','/DPCTOOL_SCROLL_BASELINE',
    "/I$root/src", "/I$output", "/Fo$output\", "/Fe$output/scroll_stitcher_performance.exe",
    "$root/tests/scroll_stitcher_performance.cpp", "$root/src/shared/image/image_document.cpp",
    "$root/src/long_capture/scroll_stitcher.cpp", "$output/baseline_stitcher.cpp", '/link','user32.lib','gdi32.lib')
& cl @arguments *> (Join-Path $output 'build.log')
if ($LASTEXITCODE -ne 0) { Get-Content (Join-Path $output 'build.log') -Tail 25; throw 'Paired benchmark build failed.' }
if ($BuildOnly) { Write-Output "Built $output/scroll_stitcher_performance.exe"; return }
$mode = if ($VerifyOnly) { '--verify' } else { '--benchmark' }
& "$output/scroll_stitcher_performance.exe" $mode --samples $Samples --rounds $Rounds --case $Case
if ($LASTEXITCODE -ne 0) { throw 'Scroll stitcher verification or benchmark failed.' }
