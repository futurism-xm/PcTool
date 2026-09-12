param([Parameter(Mandatory=$true)][string]$Binaries,[Parameter(Mandatory=$true)][string]$Output)
$ErrorActionPreference='Stop'
$fixture=Join-Path $Output ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture -Force | Out-Null
Copy-Item (Join-Path $Binaries 'PcToolStorageTests.exe'),(Join-Path $Binaries 'PcToolArchiveStorageTests.exe') -Destination $fixture
Copy-Item (Join-Path $Binaries 'PcToolStorageTests.exe') -Destination (Join-Path $fixture 'PcTool.exe')
& (Join-Path $fixture 'PcToolStorageTests.exe');if($LASTEXITCODE){throw 'Storage test failed'}
& (Join-Path $fixture 'PcToolArchiveStorageTests.exe');if($LASTEXITCODE){throw 'Private hive write failed'}
& (Join-Path $fixture 'PcToolArchiveStorageTests.exe') read;if($LASTEXITCODE){throw 'Private hive reload failed'}
& "$PSScriptRoot/../packaging/manage-data.ps1" -Action Install -Root $fixture
if($LASTEXITCODE){throw 'ACL setup failed'}
& "$PSScriptRoot/../packaging/manage-data.ps1" -Action Uninstall -Root $fixture
if($LASTEXITCODE){throw 'Managed cleanup failed'}
if((Test-Path -LiteralPath (Join-Path $fixture 'Data')) -or (Test-Path -LiteralPath (Join-Path $fixture 'Cache'))){throw 'Managed directories survived cleanup'}
if(!(Test-Path -LiteralPath (Join-Path $fixture 'external-fixture/keep.txt'))){throw 'Unrelated file removed'}
'PASS data ACL, managed removal, unrelated files preserved'
