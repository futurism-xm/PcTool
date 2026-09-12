param([Parameter(Mandatory=$true)][string]$Runtime)
$ErrorActionPreference = 'Stop'
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('PcTool-archive-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force "$testRoot/input/子目录" | Out-Null
[IO.File]::WriteAllText("$testRoot/input/子目录/中文 文本.txt", "PcTool 中文 English`r`n7-Zip 原文完整性", [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllBytes("$testRoot/input/random.bin", (1..16384 | ForEach-Object {[byte]($_ % 251)}))
$exe = Join-Path $Runtime '7z.exe'
function FileHash([string]$Path) {
    $stream=[IO.File]::OpenRead($Path)
    $sha=[Security.Cryptography.SHA256]::Create()
    try { [BitConverter]::ToString($sha.ComputeHash($stream)) } finally {$sha.Dispose();$stream.Dispose()}
}
function Run([string[]]$Arguments) {
    & $exe @Arguments | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "7-Zip failed ($LASTEXITCODE): $Arguments" }
}
foreach ($type in '7z','zip','tar','wim') {
    Run @('a', "-t$type", "$testRoot/sample.$type", "$testRoot/input/*")
    Run @('t', "$testRoot/sample.$type")
    Run @('x', "$testRoot/sample.$type", "-o$testRoot/extracted-$type", '-y')
    $expected = FileHash "$testRoot/input/子目录/中文 文本.txt"
    if ((FileHash "$testRoot/extracted-$type/子目录/中文 文本.txt") -ne $expected) {throw "$type content mismatch"}
}
Run @('a', '-t7z', "$testRoot/encrypted.7z", "$testRoot/input/*", '-pTest-only-密码', '-mhe=on')
Run @('t', "$testRoot/encrypted.7z", '-pTest-only-密码')
$ErrorActionPreference='Continue'
try { & $exe t "$testRoot/encrypted.7z" '-pWrong' *> "$testRoot/wrong-password.log"; $wrongPasswordExit=$LASTEXITCODE }
finally { $ErrorActionPreference='Stop' }
if ($wrongPasswordExit -eq 0) {throw 'Wrong password accepted'}
Run @('a', '-t7z', "$testRoot/split.7z", "$testRoot/input/*", '-v1k', '-mx=0')
Run @('t', "$testRoot/split.7z.001")
if (@(Get-ChildItem "$testRoot/split.7z.*").Count -lt 2) {throw 'Split volumes missing'}
[IO.File]::WriteAllText("$testRoot/added.txt", 'update')
Run @('u', "$testRoot/sample.zip", "$testRoot/added.txt")
Run @('t', "$testRoot/sample.zip")
Run @('d', "$testRoot/sample.zip", 'added.txt')
Run @('a', '-sfx7z.sfx', "$testRoot/self-extract.exe", "$testRoot/input/*")
Run @('t', "$testRoot/self-extract.exe")
Write-Output "PASS: formats, Unicode paths, original data, encryption/password rejection, split volumes, update/delete, SFX. Evidence: $testRoot"
