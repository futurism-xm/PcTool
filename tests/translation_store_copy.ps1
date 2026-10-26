param([Parameter(Mandatory=$true)][string]$Executable,[Parameter(Mandatory=$true)][string]$Output,[string]$Runner)
$ErrorActionPreference='Stop'
$root=Join-Path ([IO.Path]::GetFullPath($Output)) ([guid]::NewGuid().ToString('N'))
$first=Join-Path $root '原电脑 app'
$second=Join-Path $root '新电脑 app'
New-Item -ItemType Directory -Path $first,$second -Force|Out-Null
foreach($directory in @($first,$second)){Copy-Item -LiteralPath $Executable -Destination (Join-Path $directory 'PcTool.exe')}
& (Join-Path $first 'PcTool.exe') --default-write
if($LASTEXITCODE){throw 'Default portable configuration write failed'}
New-Item -ItemType Directory -Path (Join-Path $second 'Data') -Force|Out-Null
Copy-Item -LiteralPath (Join-Path $first 'Data/Translation') -Destination (Join-Path $second 'Data/Translation') -Recurse
if($Runner){
 & "$PSScriptRoot/../packaging/lifecycle.ps1" -Action MigrateData -Root $second -Namespace ('PcToolTest-'+[guid]::NewGuid().ToString('N'))
 if($LASTEXITCODE){throw 'Installer moved or rejected portable Translation'}
 & $Runner (Join-Path $second 'PcTool.exe') --default-read
}else{& (Join-Path $second 'PcTool.exe') --default-read}
if($LASTEXITCODE){throw 'Copied Translation unusable in another installation'}
$original=[Convert]::ToBase64String([IO.File]::ReadAllBytes((Join-Path $first 'Data/Translation/sources.json')))
$imported=[Convert]::ToBase64String([IO.File]::ReadAllBytes((Join-Path $second 'Data/Translation/sources.json')))
if($original -ne $imported){throw 'Copy/read/save changed portable data'}
'PASS independent processes, only Translation copied, default path, Chinese/spaces, installer/medium token when supplied'
