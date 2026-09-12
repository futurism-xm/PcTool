param([string]$Output)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (!$Output) { $Output = Join-Path $root 'out/uninstall-cleanup-tests' }
if (!( [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Deferred deletion tests require an elevated test session'
}
$fixture = [IO.Path]::GetFullPath((Join-Path $Output ([guid]::NewGuid().ToString('N'))))
New-Item -ItemType Directory -Path $fixture -Force | Out-Null
$scriptPath = Join-Path $root 'packaging/manage-data.ps1'
$shell = "$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe"
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class UninstallHiveFixture {
 [DllImport("advapi32.dll", CharSet=CharSet.Unicode)] public static extern int RegLoadAppKey(string path, out IntPtr key, int access, int options, int reserved);
 [DllImport("advapi32.dll")] public static extern int RegCloseKey(IntPtr key);
}
'@
$queue = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Control\Session Manager', $true)
$queueName = 'PendingFileRenameOperations'
function Read-Pending { return @($queue.GetValue($queueName, [string[]]@())) }
function Is-FixturePath([string]$path) {
    return $path.StartsWith('\??\' + $fixture + '\', [StringComparison]::OrdinalIgnoreCase)
}
function Remove-FixturePending {
    # Remove only this test's unique paths; preserve all other pending operations.
    $entries = @(Read-Pending)
    if ($entries.Count % 2) { throw 'Unexpected pending-operation list; refusing to change it' }
    $kept = [Collections.Generic.List[string]]::new()
    for ($i = 0; $i -lt $entries.Count; $i += 2) {
        if (!(Is-FixturePath $entries[$i])) { $kept.Add($entries[$i]); $kept.Add($entries[$i+1]) }
    }
    if ($kept.Count) { $queue.SetValue($queueName, $kept.ToArray(), [Microsoft.Win32.RegistryValueKind]::MultiString) }
    else { $queue.DeleteValue($queueName, $false) }
}
function Run-Cleanup([string]$directory, [int]$expected) {
    & $shell -NoProfile -ExecutionPolicy Bypass -File $scriptPath -Action Uninstall -Root $directory *> "$directory/cleanup.log"
    if ($LASTEXITCODE -ne $expected) { throw "Cleanup returned $LASTEXITCODE, expected $expected; see $directory/cleanup.log" }
}
function Prepare([string]$name) {
    $directory = Join-Path $fixture $name
    New-Item -ItemType Directory -Path "$directory/Data/test/SevenZip", "$directory/Cache" -Force | Out-Null
    [IO.File]::WriteAllText("$directory/PcTool.exe", 'test root marker')
    [IO.File]::WriteAllText("$directory/keep.txt", 'unrelated user file')
    [IO.File]::WriteAllText("$directory/Cache/ordinary.png", 'cache fixture')
    return $directory
}
try {
    $normal = Prepare 'normal'
    Run-Cleanup $normal 0
    if ((Test-Path "$normal/Data") -or (Test-Path "$normal/Cache") -or !(Test-Path "$normal/keep.txt")) { throw 'Normal cleanup or unrelated file protection failed' }
    'PASS normal cleanup and unrelated files'

    foreach ($kind in @('file-lock', 'loaded-hive')) {
        $directory = Prepare $kind
        $file = "$directory/Data/test/SevenZip/settings.hiv"
        $stream = $null; $hive = [IntPtr]::Zero
        try {
            if ($kind -eq 'file-lock') {
                $stream = [IO.File]::Open($file, 'Create', 'ReadWrite', 'None')
            } else {
                $result = [UninstallHiveFixture]::RegLoadAppKey($file, [ref]$hive, 0xF003F, 0, 0)
                if ($result) { throw "Cannot load fixture hive: $result" }
            }
            Run-Cleanup $directory 3010
            $entries = @(Read-Pending)
            $expectedPaths = @($file, "$directory/Data/test/SevenZip", "$directory/Data/test", "$directory/Data")
            $previous = -1
            foreach ($path in $expectedPaths) {
                $native = '\??\' + [IO.Path]::GetFullPath($path)
                $index = [Array]::IndexOf($entries, $native)
                if ($index -lt 0 -or $index % 2 -or $index -le $previous -or $entries[$index+1] -ne '') { throw "Missing ordered delete operation for $path" }
                $previous = $index
            }
            if (!(Test-Path "$directory/keep.txt") -or (Test-Path "$directory/Cache")) { throw 'Pending cleanup did not continue safely' }
            "PASS $kind returns 3010 and queues file before parents with native NULL destination"
        } finally {
            if ($stream) { $stream.Dispose() }
            if ($hive -ne [IntPtr]::Zero) { [UninstallHiveFixture]::RegCloseKey($hive) | Out-Null }
            Remove-FixturePending
        }
        Run-Cleanup $directory 0
        if (Test-Path "$directory/Data") { throw 'Cleanup after releasing lock failed' }
    }

    $linked = Prepare 'junction'
    $external = Join-Path $fixture 'external'
    New-Item -ItemType Directory -Path $external -Force | Out-Null
    [IO.File]::WriteAllText("$external/keep.txt", 'must not be traversed')
    New-Item -ItemType Junction -Path "$linked/Cache/outside" -Target $external | Out-Null
    Run-Cleanup $linked 0
    if (!(Test-Path "$external/keep.txt") -or (Test-Path "$linked/Cache")) { throw 'Reparse-point protection failed' }
    'PASS junction removed without traversing outside managed root'
    "Evidence: $fixture"
} finally {
    try { Remove-FixturePending } finally { $queue.Dispose() }
}
