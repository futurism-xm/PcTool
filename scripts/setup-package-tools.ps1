$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$downloads = Join-Path $root 'out/downloads'
New-Item -ItemType Directory -Force $downloads | Out-Null
function Fetch([string]$url,[string]$file,[string]$hash) {
    if (!(Test-Path -LiteralPath $file) -or (Get-FileHash -LiteralPath $file).Hash -ne $hash) { Invoke-WebRequest $url -OutFile $file }
    if ((Get-FileHash -LiteralPath $file).Hash -ne $hash) {throw "Checksum mismatch: $file"}
}
Fetch 'https://master.dl.sourceforge.net/project/nsis/NSIS%203/3.12/nsis-3.12.zip?viasf=1' "$downloads/nsis-3.12.zip" '56581F90DB321581C5381193D796FFFCF2D24B2F8FED2160A6C6A3BAA67F2C4F'
Expand-Archive "$downloads/nsis-3.12.zip" "$root/out/nsis" -Force
# Extract only translations/help from the matching official distribution; never execute its installer.
Fetch 'https://7-zip.org/a/7z2603-x64.exe' "$downloads/7z2603-x64.exe" '0859C524B8A63551848F0C246ABDDCB1D0B7B656B0FBFE879F8D85E61A9E6EDD'
& "$root/out/archive-runtime/7z.exe" x "$downloads/7z2603-x64.exe" "-o$root/out/7zip-official-resources" -y
if ($LASTEXITCODE -ne 0) {throw 'Cannot extract 7-Zip translations/help'}
Copy-Item "$root/out/7zip-official-resources/Lang" "$root/out/archive-runtime/" -Recurse -Force
Copy-Item "$root/out/7zip-official-resources/7-zip.chm" "$root/out/archive-runtime/" -Force
