param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (!$OutputDirectory) { $OutputDirectory = Join-Path $root 'out/archive-runtime' }
$source = Join-Path $root 'third_party/7zip'
$stage = Join-Path $root 'out/7zip-build-source'
New-Item -ItemType Directory -Force $stage, $OutputDirectory | Out-Null
$storageHeader = 'CPP/Windows/PcToolStorage.h'
$storageChanged = !(Test-Path -LiteralPath (Join-Path $stage $storageHeader))
if (!$storageChanged) {
    $storageChanged = (Get-FileHash -LiteralPath (Join-Path $source $storageHeader)).Hash -ne (Get-FileHash -LiteralPath (Join-Path $stage $storageHeader)).Hash
}
Get-ChildItem -LiteralPath $source -Recurse -File -Force | Where-Object { $_.Extension -notin @('.obj','.dll','.exe','.lib','.exp','.pdb','.res') -and $_.FullName -notmatch '\\CPP\\7zip\\.*\\(x64|x86)\\' } | ForEach-Object {
    $relative = $_.FullName.Substring($source.Length + 1)
    $target = Join-Path $stage $relative
    if (!(Test-Path -LiteralPath $target) -or (Get-Item -LiteralPath $target).LastWriteTimeUtc -ne $_.LastWriteTimeUtc) {
        New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $target -Force
    }
}
# Upstream nmake does not track this local header. Recompile its direct consumers.
if ($storageChanged) {
    $stageRoot = [IO.Path]::GetFullPath($stage).TrimEnd('\') + '\'
    $storageObjects = @('Registry.obj','FileDir.obj','GUI.obj','DllExportsExplorer.obj','MainAr.obj','ContextMenu.obj','FM.obj')
    Get-ChildItem -LiteralPath $stage -Recurse -File -Filter '*.obj' | Where-Object { $_.Name -in $storageObjects } | ForEach-Object {
        if (!$_.FullName.StartsWith($stageRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid staged object path' }
        Remove-Item -LiteralPath $_.FullName -Force
    }
}
$targets = @{
    'Bundles/Format7zF' = '7z.dll'
    'UI/Console' = '7z.exe'
    'UI/GUI' = '7zG.exe'
    'UI/FileManager' = '7zFM.exe'
    'UI/Explorer' = '7-zip.dll'
    'Bundles/SFXWin' = '7z.sfx'
    'Bundles/SFXCon' = '7zCon.sfx'
}
foreach ($target in $targets.Keys) {
    $directory = Join-Path $stage "CPP/7zip/$target"
    Push-Location $directory
    try {
        & nmake /nologo /f makefile PLATFORM=x64
        if ($LASTEXITCODE -ne 0) { throw "7-Zip build failed: $target" }
        Copy-Item -LiteralPath (Join-Path "$directory/x64" $targets[$target]) -Destination $OutputDirectory -Force
    } finally { Pop-Location }
}
Copy-Item "$source/DOC/License.txt", "$source/DOC/copying.txt", "$source/DOC/unRarLicense.txt" $OutputDirectory -Force
Copy-Item "$source/C/7zVersion.h" "$OutputDirectory/SOURCE_VERSION.h" -Force
$devShell = Join-Path $env:VSINSTALLDIR 'Common7/Tools/Launch-VsDevShell.ps1'
if (!(Test-Path -LiteralPath $devShell)) {throw 'VS developer shell is required to build the 32-bit shell extension.'}
try {
    & $devShell -Arch x86 -HostArch amd64 -SkipAutomaticLocation | Out-Null
    Push-Location "$stage/CPP/7zip/UI/Explorer"
    try {
        & nmake /nologo /f makefile PLATFORM=x86
        if ($LASTEXITCODE -ne 0) {throw '32-bit shell extension build failed'}
        Copy-Item "$stage/CPP/7zip/UI/Explorer/x86/7-zip.dll" "$OutputDirectory/7-zip32.dll" -Force
    } finally {Pop-Location}
} finally { & $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null }
Write-Output "Built original 7-Zip 26.03 components: $OutputDirectory"
