$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$failed = @()
foreach ($module in @('shared','archive','system_tools')) {
    Get-ChildItem -LiteralPath "$root/src/$module" -Recurse -File | Where-Object { $_.Extension -in '.h','.cpp' } | ForEach-Object {
        foreach ($line in [IO.File]::ReadAllLines($_.FullName)) {
            if ($line -match '^\s*#include "([^/]+)/') {
                if ($Matches[1] -notin @('shared', $module)) { $failed += "$($_.FullName): $line" }
            }
        }
    }
}
if ($failed.Count) {throw ($failed -join "`n")}
Write-Output 'Shared primitives and system/archive adapters have no reverse feature dependencies.'
