param([string]$OutputDirectory, [switch]$VerifyOnly)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root 'third_party/7zip'
if (!$OutputDirectory) { $OutputDirectory = Join-Path $root 'out/archive-runtime' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$manifestPath = Join-Path $OutputDirectory '.pctool-7zip-build.json'
$scriptHash = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash

function Get-SourceInventory([string]$Directory) {
    $base = [IO.Path]::GetFullPath($Directory).TrimEnd('\') + '\'
    @(Get-ChildItem -LiteralPath $Directory -Recurse -File -Force | Where-Object {
        # Keep real assembler directories such as Asm/x86; omit compiler outputs.
        $_.FullName -notmatch '\\CPP\\7zip\\.*\\(x64|x86|arm64|arm|o)\\' -and
        $_.Extension -notin @('.obj','.dll','.exe','.lib','.exp','.pdb','.res','.ilk','.pch')
    } | Sort-Object FullName | ForEach-Object {
        if ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Source link is not supported: $($_.FullName)" }
        [pscustomobject]@{ Path=$_.FullName.Substring($base.Length).Replace('\','/'); SHA256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
    })
}
function Get-InventoryHash($Inventory) {
    $lines = @($Inventory | ForEach-Object { $_.Path + ':' + $_.SHA256 })
    $hash = [Security.Cryptography.SHA256]::Create()
    try { ([BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes(($lines -join [char]10))))).Replace('-','') }
    finally { $hash.Dispose() }
}
function Get-PeMachine([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5a4d) { throw "Not a PE image: $Path" }
        $stream.Position=0x3c; $offset=$reader.ReadInt32()
        if ($offset -lt 64 -or $offset -gt $stream.Length-6) { throw "Invalid PE header: $Path" }
        $stream.Position=$offset
        if ($reader.ReadUInt32() -ne 0x4550) { throw "Invalid PE signature: $Path" }
        $reader.ReadUInt16()
    } finally { $reader.Dispose(); $stream.Dispose() }
}
$targets = [ordered]@{
    'Bundles/Format7zF'='7z.dll'; 'UI/Console'='7z.exe'; 'UI/GUI'='7zG.exe'
    'UI/FileManager'='7zFM.exe'; 'UI/Explorer'='7-zip.dll'
    'Bundles/SFXWin'='7z.sfx'; 'Bundles/SFXCon'='7zCon.sfx'
}
$expected = @($targets.Values) + '7-zip32.dll' + 'License.txt' + 'copying.txt' + 'unRarLicense.txt' + 'SOURCE_VERSION.h'
$inventory = @(Get-SourceInventory $source)
if (!$inventory.Count) { throw '7-Zip source tree is empty.' }
$sourceHash = Get-InventoryHash $inventory

if ($VerifyOnly) {
    if (!(Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw 'Missing 7-Zip build provenance. Rebuild dependencies without -SkipDependencyBuild.' }
    $record = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($record.Version -ne 1 -or !$record.BuildId -or $record.SourceFingerprint -ne $sourceHash -or $record.BuildScriptSHA256 -ne $scriptHash) {
        throw '7-Zip source/build-script fingerprint changed. Rebuild dependencies without -SkipDependencyBuild.'
    }
    if (@($record.Files).Count -ne $expected.Count -or (Get-InventoryHash $record.SourceFiles) -ne $sourceHash) { throw 'Invalid 7-Zip provenance inventory.' }
    foreach ($name in $expected) {
        $entries = @($record.Files | Where-Object { $_.Path -ceq $name })
        $path = Join-Path $OutputDirectory $name
        if ($entries.Count -ne 1 -or !(Test-Path -LiteralPath $path -PathType Leaf) -or
            (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $entries[0].SHA256) { throw "7-Zip artifact mismatch: $name" }
        if ($name -in (@($targets.Values) + '7-zip32.dll')) {
            $machine = if ($name -eq '7-zip32.dll') { 0x014c } else { 0x8664 }
            if ((Get-PeMachine $path) -ne $machine -or $entries[0].Machine -ne $machine) { throw "Wrong 7-Zip artifact architecture: $name" }
        }
    }
    # Includes the exact licensed real-source inventory for source packaging.
    return $record
}

if (!$env:VSINSTALLDIR) { throw 'A Visual Studio developer shell is required to build 7-Zip.' }
$devShell = Join-Path $env:VSINSTALLDIR 'Common7/Tools/Launch-VsDevShell.ps1'
if (!(Test-Path -LiteralPath $devShell -PathType Leaf)) { throw 'Visual Studio developer-shell launcher is missing.' }
$started = [DateTime]::UtcNow
$buildId = $started.ToString('yyyyMMddTHHmmssfffZ') + '-' + $sourceHash.Substring(0,12).ToLowerInvariant()
$stage = Join-Path $root ('out/7zip-build-source/' + $buildId + '-' + [Guid]::NewGuid().ToString('N'))
$runtimeStage = Join-Path $stage 'runtime'
New-Item -ItemType Directory -Path $runtimeStage -Force | Out-Null
foreach ($file in $inventory) {
    $target = Join-Path $stage $file.Path
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $source $file.Path) -Destination $target
    if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $file.SHA256) { throw "Source changed while staging: $($file.Path)" }
}
# Fresh object directories avoid upstream nmake's incomplete header tracking.
$toolchains = [Collections.Generic.List[object]]::new()
try {
    & $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
    $compiler = (Get-Command cl.exe -ErrorAction Stop).Source
    $toolchains.Add([pscustomobject]@{ Architecture='x64'; Compiler=$compiler; Version=(Get-Item -LiteralPath $compiler).VersionInfo.FileVersion })
    foreach ($target in $targets.Keys) {
        $directory = Join-Path $stage "CPP/7zip/$target"
        Push-Location $directory
        try {
            & nmake /nologo /f makefile PLATFORM=x64
            if ($LASTEXITCODE -ne 0) { throw "7-Zip x64 build failed: $target" }
            $binary = Join-Path "$directory/x64" $targets[$target]
            if ((Get-PeMachine $binary) -ne 0x8664) { throw "Expected x64 output: $target" }
            Copy-Item -LiteralPath $binary -Destination $runtimeStage
        } finally { Pop-Location }
    }
    & $devShell -Arch x86 -HostArch amd64 -SkipAutomaticLocation | Out-Null
    $compiler = (Get-Command cl.exe -ErrorAction Stop).Source
    $toolchains.Add([pscustomobject]@{ Architecture='x86'; Compiler=$compiler; Version=(Get-Item -LiteralPath $compiler).VersionInfo.FileVersion })
    Push-Location (Join-Path $stage 'CPP/7zip/UI/Explorer')
    try {
        & nmake /nologo /f makefile PLATFORM=x86
        if ($LASTEXITCODE -ne 0) { throw '7-Zip x86 shell extension build failed.' }
        $binary = Join-Path $stage 'CPP/7zip/UI/Explorer/x86/7-zip.dll'
        if ((Get-PeMachine $binary) -ne 0x014c) { throw 'Expected x86 shell extension output.' }
        Copy-Item -LiteralPath $binary -Destination (Join-Path $runtimeStage '7-zip32.dll')
    } finally { Pop-Location }
} finally { & $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null }
foreach ($name in @('License.txt','copying.txt','unRarLicense.txt')) { Copy-Item -LiteralPath (Join-Path $stage "DOC/$name") -Destination $runtimeStage }
Copy-Item -LiteralPath (Join-Path $stage 'C/7zVersion.h') -Destination (Join-Path $runtimeStage 'SOURCE_VERSION.h')
if ((Get-InventoryHash (Get-SourceInventory $source)) -ne $sourceHash -or (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash -ne $scriptHash) {
    throw '7-Zip inputs changed during compilation; outputs were not published.'
}
$files = @($expected | ForEach-Object {
    $path=Join-Path $runtimeStage $_
    $machine=if ($_ -in (@($targets.Values) + '7-zip32.dll')) { Get-PeMachine $path } else { $null }
    [pscustomobject]@{ Path=$_; Bytes=(Get-Item -LiteralPath $path).Length; SHA256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash; Machine=$machine }
})
$record = [ordered]@{
    Version=1; BuildId=$buildId; StartedUtc=$started.ToString('o'); CompletedUtc=[DateTime]::UtcNow.ToString('o')
    SourceFingerprint=$sourceHash; BuildScriptSHA256=$scriptHash; Toolchains=$toolchains.ToArray(); SourceFiles=$inventory; Files=$files
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
foreach ($name in $expected) { Copy-Item -LiteralPath (Join-Path $runtimeStage $name) -Destination (Join-Path $OutputDirectory $name) -Force }
# Publish the record last. A partial or interrupted copy never validates.
$record | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
Write-Output "Built source-verified x64 components and x86 shell extension: $OutputDirectory ($buildId)"
