param([string]$BuildDirectory, [switch]$SkipDependencyBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
New-Item -ItemType Directory -Force "$root/out" | Out-Null
if (!$BuildDirectory) { $BuildDirectory = Join-Path $root 'cmake-build-release' }
$cache=Join-Path $BuildDirectory 'CMakeCache.txt'
if (!(Test-Path -LiteralPath $cache)) {throw 'Configure a Release CMake build before packaging.'}
$cacheText=[IO.File]::ReadAllText((Resolve-Path -LiteralPath $cache))
if($cacheText -match '(?m)^CMAKE_BUILD_TYPE:STRING=(?!Release\r?$)\S+') {throw 'Installer must use a Release build.'}
if (!$SkipDependencyBuild) {
    & "$PSScriptRoot/build-7zip.ps1" *> "$root/out/7zip-build.log"
    & "$PSScriptRoot/setup-package-tools.ps1" *> "$root/out/package-tools.log"
}
& cmake --build $BuildDirectory --config Release -j 4
if ($LASTEXITCODE -ne 0) { throw 'Release build failed' }
# Every package uses a fresh staging folder, never copies the build tree or user recordings.
$stage = Join-Path $root ('out/package-stage/' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force "$stage/licenses", "$root/out/installer", "$root/out/package-manifest" | Out-Null
& cmake --install $BuildDirectory --config Release --prefix $stage
if ($LASTEXITCODE -ne 0) { throw 'Install staging failed' }
foreach ($developmentDirectory in @('docs', 'tests')) {
    if (Test-Path -LiteralPath (Join-Path $stage $developmentDirectory)) {
        throw "Development-only directory must not be packaged: $developmentDirectory"
    }
}
# Preserve Asm/x86 (real source); exclude only generated compiler target directories.
$sourceRoot=Join-Path $root 'third_party/7zip'
$sourceStage=Join-Path $root ('out/source-package/' + [Guid]::NewGuid().ToString('N') + '/7zip')
Get-ChildItem -LiteralPath $sourceRoot -Recurse -File -Force | Where-Object { $_.FullName -notmatch '\\CPP\\7zip\\.*\\(x64|x86)\\' } | ForEach-Object {
    $target=Join-Path $sourceStage $_.FullName.Substring($sourceRoot.Length+1)
    New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
    Copy-Item -LiteralPath $_.FullName -Destination $target -Force
}
& "$root/out/archive-runtime/7z.exe" a -tzip "$stage/licenses/7zip-source.zip" $sourceStage
if ($LASTEXITCODE -ne 0) { throw 'Source archive failed' }
Copy-Item "$root/packaging/license.txt" "$stage/licenses/COMPONENTS.txt"
Copy-Item "$root/packaging/manage-data.ps1" "$stage/manage-data.ps1"
$payload = @(Get-ChildItem -LiteralPath $stage -Recurse -File | ForEach-Object { $_.FullName.Substring($stage.Length + 1).Replace('\','/') })
$directories = @(Get-ChildItem -LiteralPath $stage -Recurse -Directory | ForEach-Object { $_.FullName.Substring($stage.Length + 1).Replace('\','/') })
$legacy = @(Get-Content "$root/packaging/legacy-development-files.txt" | Where-Object { $_ })
@{ Version=1; Files=@($payload + '.pctool-package-files.json' + $legacy); Directories=@($directories + 'docs' + 'tests' + 'tests/fixtures' + 'tests/ocr-evaluation' + 'tests/tts_evaluation' + 'tests/__pycache__') } |
    ConvertTo-Json -Depth 4 | Set-Content "$stage/.pctool-package-files.json" -Encoding UTF8
$files = @(Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName)
$install = [Collections.Generic.List[string]]::new()
$uninstall = [Collections.Generic.List[string]]::new()
foreach ($file in $files) {
    $relative = $file.FullName.Substring($stage.Length + 1)
    $parent = Split-Path -Parent $relative
    $install.Add(('SetOutPath "$INSTDIR\{0}"' -f $parent))
    $install.Add(('File "{0}"' -f $file.FullName))
    $uninstall.Add(('Delete /REBOOTOK "$INSTDIR\{0}"' -f $relative))
}
Get-ChildItem -LiteralPath $stage -Recurse -Directory | Sort-Object { $_.FullName.Length } -Descending | ForEach-Object {
    $uninstall.Add(('RMDir "$INSTDIR\{0}"' -f $_.FullName.Substring($stage.Length + 1)))
}
$manifest = "$root/out/package-manifest"
[IO.File]::WriteAllLines("$manifest/install-files.nsh", $install, [Text.UTF8Encoding]::new($true))
[IO.File]::WriteAllLines("$manifest/uninstall-files.nsh", $uninstall, [Text.UTF8Encoding]::new($true))
$files | ForEach-Object { [pscustomobject]@{Path=$_.FullName.Substring($stage.Length + 1);Bytes=$_.Length;SHA256=(Get-FileHash -LiteralPath $_.FullName).Hash} } | ConvertTo-Json | Set-Content "$manifest/files.json" -Encoding UTF8
$output = "$root/out/installer/PcTool-0.1.2-x64-Setup.exe"
& "$root/out/nsis/nsis-3.12/makensis.exe" /V3 "/DOUTPUT=$output" "/DMANIFEST=$manifest" "/DSTAGE=$stage" "$root/packaging/PcTool.nsi"
if ($LASTEXITCODE -ne 0) { throw 'NSIS compilation failed' }
Get-FileHash -LiteralPath $output
