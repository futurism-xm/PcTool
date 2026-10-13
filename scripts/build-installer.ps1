param([string]$BuildDirectory, [switch]$SkipDependencyBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (!$BuildDirectory) { $BuildDirectory = Join-Path $root 'cmake-build-release' }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$cache = Join-Path $BuildDirectory 'CMakeCache.txt'
if (!(Test-Path -LiteralPath $cache -PathType Leaf)) { throw 'Configure a Release CMake build before packaging.' }
$cacheText = [IO.File]::ReadAllText($cache)
function Get-CacheValue([string]$Name) {
    $match = [regex]::Match($cacheText, '(?m)^' + [regex]::Escape($Name) + ':[^=]+=(.*)\r?$')
    if (!$match.Success) { throw "Missing CMake cache entry: $Name" }
    $match.Groups[1].Value.TrimEnd([char]13)
}
$cmakeRoot = [IO.Path]::GetFullPath((Get-CacheValue 'CMAKE_HOME_DIRECTORY')).TrimEnd('\')
if ($cmakeRoot -ne [IO.Path]::GetFullPath($root).TrimEnd('\')) { throw 'CMake cache belongs to another source checkout.' }
if ($cacheText -match '(?m)^CMAKE_CONFIGURATION_TYPES:[^=]+=(.*)\r?$') {
    if ('Release' -notin $Matches[1].Trim().Split(';')) { throw 'CMake configuration has no Release target.' }
} elseif ((Get-CacheValue 'CMAKE_BUILD_TYPE') -cne 'Release') { throw 'Installer must use a Release build.' }
$runtime = Get-CacheValue 'PCTOOL_ARCHIVE_RUNTIME'
if (![IO.Path]::IsPathRooted($runtime)) { throw 'Configure PCTOOL_ARCHIVE_RUNTIME with an absolute path before packaging.' }
$runtime = [IO.Path]::GetFullPath($runtime)
$version = Get-CacheValue 'CMAKE_PROJECT_VERSION'
if ($version -notmatch '^\d+\.\d+\.\d+(\.\d+)?$') { throw 'Invalid CMake project version.' }
New-Item -ItemType Directory -Path (Join-Path $root 'out') -Force | Out-Null

function Get-InventoryHash($Inventory) {
    $lines = @($Inventory | ForEach-Object { $_.Path + ':' + $_.SHA256 })
    $hash = [Security.Cryptography.SHA256]::Create()
    try { ([BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes(($lines -join [char]10))))).Replace('-','') }
    finally { $hash.Dispose() }
}
function Get-PackageSourceInventory {
    $inputs = @('CMakeLists.txt','README.md','src','cmake','scripts','packaging','Resources','third_party')
    @(foreach ($inputPath in $inputs) {
        $path = Join-Path $root $inputPath
        if (Test-Path -LiteralPath $path -PathType Container) { Get-ChildItem -LiteralPath $path -Recurse -File -Force }
        else { Get-Item -LiteralPath $path }
    }) | Where-Object {
        # A vendor's DOC and Asm/x86 are real licensed source, not project docs/tests.
        $_.FullName -notmatch '\\third_party\\7zip\\CPP\\7zip\\.*\\(x64|x86|arm64|arm|o)\\' -and
        !($_.FullName -match '\\third_party\\7zip\\' -and $_.Extension -in @('.obj','.dll','.exe','.lib','.exp','.pdb','.res','.ilk','.pch'))
    } | Sort-Object FullName | ForEach-Object {
        if ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Package input link is not supported: $($_.FullName)" }
        [pscustomobject]@{ Path=$_.FullName.Substring($root.Length+1).Replace('\','/'); SHA256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
    }
}
function Get-VerifiedDownload([string]$Url,[string]$Name,[string]$SHA256) {
    $directory = Join-Path $root 'out/downloads'
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $path = Join-Path $directory $Name
    if (!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $SHA256) {
        if ($SkipDependencyBuild) { throw "Missing verified packaging resource $Name. Run without -SkipDependencyBuild first." }
        $temporary = $path + '.' + [Guid]::NewGuid().ToString('N') + '.download'
        Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $temporary
        if ((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash -ne $SHA256) { throw "Checksum mismatch: $Name" }
        Move-Item -LiteralPath $temporary -Destination $path -Force
    }
    return $path
}
function Get-FileRecord([string]$Path,[string]$Relative) {
    [pscustomobject]@{ Path=$Relative.Replace('\','/'); Bytes=(Get-Item -LiteralPath $Path).Length; SHA256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
}
function Assert-FileInventory([string]$Directory,$Expected) {
    $actual = @(Get-ChildItem -LiteralPath $Directory -Recurse -File -Force)
    if ($actual.Count -ne @($Expected).Count) { throw "Unexpected or missing runtime files in $Directory" }
    foreach ($entry in $Expected) {
        $path = Join-Path $Directory $entry.Path
        if (!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $entry.SHA256) {
            throw "Runtime file does not match its verified source: $($entry.Path)"
        }
    }
}
function ConvertTo-NsisLiteral([string]$Value) {
    # Dollar signs are syntax even inside NSIS quotes. Quote before generating includes.
    $Value.Replace('$','$$').Replace('"','$\"')
}

$requiredScripts = @('manage-data.ps1','cleanup-jobs.ps1','retry-cleanup.ps1','shell-integration.ps1')
foreach ($name in $requiredScripts) {
    if (!(Test-Path -LiteralPath (Join-Path $root "packaging/$name") -PathType Leaf)) { throw "Missing required packaging script: $name" }
}
$sourceInventory = @(Get-PackageSourceInventory)
$sourceFingerprint = Get-InventoryHash $sourceInventory
$started = [DateTime]::UtcNow
$buildId = $started.ToString('yyyyMMddTHHmmssfffZ') + '-' + $sourceFingerprint.Substring(0,12).ToLowerInvariant()
$stage = Join-Path $root "out/package-stage/$buildId"
$manifest = Join-Path $root "out/package-manifest/$buildId"
$toolsStage = Join-Path $root "out/package-tools/$buildId"
$output = Join-Path $root "out/installer/PcTool-$version-x64-$buildId-Setup.exe"
foreach ($path in @($stage,$manifest,$toolsStage,$output)) {
    if (Test-Path -LiteralPath $path) { throw "Refusing to reuse a package build location: $path" }
}
New-Item -ItemType Directory -Path "$stage/licenses",$manifest,$toolsStage,(Split-Path -Parent $output) -Force | Out-Null
if (!$SkipDependencyBuild) {
    & "$PSScriptRoot/build-7zip.ps1" -OutputDirectory $runtime *> (Join-Path $manifest '7zip-build.log')
}
$dependency = & "$PSScriptRoot/build-7zip.ps1" -OutputDirectory $runtime -VerifyOnly
$sevenZip = Join-Path $runtime '7z.exe'
$nsisHash = '56581F90DB321581C5381193D796FFFCF2D24B2F8FED2160A6C6A3BAA67F2C4F'
$officialHash = '0859C524B8A63551848F0C246ABDDCB1D0B7B656B0FBFE879F8D85E61A9E6EDD'
$nsisArchive = Get-VerifiedDownload 'https://master.dl.sourceforge.net/project/nsis/NSIS%203/3.12/nsis-3.12.zip?viasf=1' 'nsis-3.12.zip' $nsisHash
$officialArchive = Get-VerifiedDownload 'https://7-zip.org/a/7z2603-x64.exe' '7z2603-x64.exe' $officialHash
Expand-Archive -LiteralPath $nsisArchive -DestinationPath "$toolsStage/nsis"
& $sevenZip x $officialArchive "-o$toolsStage/official" -y *> (Join-Path $manifest 'official-resource-extraction.log')
if ($LASTEXITCODE -ne 0) { throw 'Cannot extract verified 7-Zip translations/help.' }
$official = Join-Path $toolsStage 'official'
$resources = @(Get-ChildItem -LiteralPath "$official/Lang" -Recurse -File | ForEach-Object {
    Get-FileRecord $_.FullName $_.FullName.Substring($official.Length+1)
})
$resources += Get-FileRecord "$official/7-zip.chm" '7-zip.chm'
foreach ($file in $resources) {
    $target=Join-Path $runtime $file.Path
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $official $file.Path) -Destination $target -Force
}
$runtimeFiles = @($dependency.Files) + $resources + @(Get-FileRecord (Join-Path $runtime '.pctool-7zip-build.json') '.pctool-7zip-build.json')
Assert-FileInventory $runtime $runtimeFiles

# Rebuild the two shipped targets from scratch, including headers not tracked by
# third-party build rules. The source digest is rechecked before and after NSIS.
& cmake --build $BuildDirectory --config Release --clean-first --target PcTool PcToolUninstallHelper -j 4
if ($LASTEXITCODE -ne 0) { throw 'Release build failed.' }
$cacheText = [IO.File]::ReadAllText($cache)
$updatedRuntime = Get-CacheValue 'PCTOOL_ARCHIVE_RUNTIME'
if (![IO.Path]::IsPathRooted($updatedRuntime)) { throw 'Archive runtime path became relative while packaging.' }
if ([IO.Path]::GetFullPath($updatedRuntime) -ne $runtime) { throw 'Archive runtime path changed while packaging.' }
& cmake --install $BuildDirectory --config Release --prefix $stage
if ($LASTEXITCODE -ne 0) { throw 'Install staging failed.' }
foreach ($developmentDirectory in @('docs','tests')) {
    if (Test-Path -LiteralPath (Join-Path $stage $developmentDirectory)) { throw "Development-only directory must not be packaged: $developmentDirectory" }
}
Assert-FileInventory (Join-Path $stage 'modules/archive') $runtimeFiles
foreach ($name in @('PcTool.exe','PcToolUninstallHelper.exe')) {
    if (!(Test-Path -LiteralPath (Join-Path $stage $name) -PathType Leaf)) { throw "Missing newly built package target: $name" }
}
# Use the verified inventory rather than copying generated files from a build tree.
$sourceStage = Join-Path $root "out/source-package/$buildId/7zip"
foreach ($file in $dependency.SourceFiles) {
    $target = Join-Path $sourceStage $file.Path
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $root "third_party/7zip/$($file.Path)") -Destination $target
    if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $file.SHA256) { throw "7-Zip source changed while packaging: $($file.Path)" }
}
Push-Location (Split-Path -Parent $sourceStage)
try {
    & $sevenZip a -tzip "$stage/licenses/7zip-source.zip" '7zip' *> (Join-Path $manifest 'source-archive.log')
    if ($LASTEXITCODE -ne 0) { throw 'Source archive failed.' }
} finally { Pop-Location }
Copy-Item -LiteralPath "$root/packaging/license.txt" -Destination "$stage/licenses/COMPONENTS.txt"
foreach ($name in $requiredScripts) { Copy-Item -LiteralPath (Join-Path $root "packaging/$name") -Destination (Join-Path $stage $name) }
$identity = [ordered]@{
    Version=1; ProductVersion=$version; BuildId=$buildId; StartedUtc=$started.ToString('o'); SourceFingerprint=$sourceFingerprint
    ArchiveBuildId=$dependency.BuildId; ArchiveSourceFingerprint=$dependency.SourceFingerprint
}
$identity | ConvertTo-Json | Set-Content -LiteralPath "$stage/.pctool-build.json" -Encoding UTF8
$payload = @(Get-ChildItem -LiteralPath $stage -Recurse -File -Force | ForEach-Object { $_.FullName.Substring($stage.Length+1).Replace('\','/') })
$directories = @(Get-ChildItem -LiteralPath $stage -Recurse -Directory -Force | ForEach-Object { $_.FullName.Substring($stage.Length+1).Replace('\','/') })
$legacy = @(Get-Content -LiteralPath "$root/packaging/legacy-development-files.txt" | Where-Object { $_ })
[ordered]@{
    Version=1; BuildId=$buildId; Files=@($payload + '.pctool-package-files.json' + $legacy | Select-Object -Unique)
    Directories=@($directories + 'docs' + 'tests' + 'tests/fixtures' + 'tests/ocr-evaluation' + 'tests/tts_evaluation' + 'tests/__pycache__' | Select-Object -Unique)
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$stage/.pctool-package-files.json" -Encoding UTF8
$files = @(Get-ChildItem -LiteralPath $stage -Recurse -File -Force | Sort-Object FullName)
$install = [Collections.Generic.List[string]]::new()
foreach ($file in $files) {
    $relative = $file.FullName.Substring($stage.Length+1)
    $parent = ConvertTo-NsisLiteral (Split-Path -Parent $relative)
    $install.Add(('SetOutPath "$INSTDIR\{0}"' -f $parent))
    $install.Add(('File "{0}"' -f (ConvertTo-NsisLiteral $file.FullName)))
}
[IO.File]::WriteAllLines("$manifest/install-files.nsh",$install,[Text.UTF8Encoding]::new($true))
$packageFiles = @($files | ForEach-Object { Get-FileRecord $_.FullName $_.FullName.Substring($stage.Length+1) })
$packageFiles | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$manifest/files.json" -Encoding UTF8
$sourceInventory | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$manifest/source-files.json" -Encoding UTF8
if ((Get-InventoryHash (Get-PackageSourceInventory)) -ne $sourceFingerprint) { throw 'Package sources changed during compilation. Start a fresh package build.' }
$pendingOutput = Join-Path $manifest 'installer.unverified.exe'
& "$toolsStage/nsis/nsis-3.12/makensis.exe" /V3 "/DOUTPUT=$pendingOutput" "/DMANIFEST=$manifest" "/DSTAGE=$stage" "/DBUILD_ID=$buildId" "$root/packaging/PcTool.nsi"
if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $pendingOutput -PathType Leaf)) { throw 'NSIS compilation failed.' }
Assert-FileInventory $stage $packageFiles
if ((Get-InventoryHash (Get-PackageSourceInventory)) -ne $sourceFingerprint) { throw 'Package sources changed during NSIS compilation. This output is not verified.' }
Move-Item -LiteralPath $pendingOutput -Destination $output
$packageHash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash
$metadata = [ordered]@{
    Version=1; ProductVersion=$version; BuildId=$buildId; StartedUtc=$started.ToString('o'); CompletedUtc=[DateTime]::UtcNow.ToString('o')
    SourceFingerprint=$sourceFingerprint; BuildDirectory=$BuildDirectory; ArchiveRuntime=$runtime
    ArchiveBuildId=$dependency.BuildId; ArchiveSourceFingerprint=$dependency.SourceFingerprint
    OfficialResourcesSHA256=$officialHash; NsisArchiveSHA256=$nsisHash; Files=$packageFiles
    Installer=(Get-FileRecord $output (Split-Path -Leaf $output)); ManifestDirectory=$manifest
}
$metadata | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath ($output + '.metadata.json') -Encoding UTF8
($packageHash + '  ' + (Split-Path -Leaf $output)) | Set-Content -LiteralPath ($output + '.sha256') -Encoding ASCII
[pscustomobject]@{ BuildId=$buildId; Installer=$output; SHA256=$packageHash; Metadata=$output+'.metadata.json'; Stage=$stage; Manifest=$manifest }
