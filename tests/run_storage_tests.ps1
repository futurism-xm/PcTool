param([Parameter(Mandatory=$true)][string]$Binaries,[Parameter(Mandatory=$true)][string]$Output)
$ErrorActionPreference='Stop'
$fixture=Join-Path $Output ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture -Force | Out-Null
Copy-Item (Join-Path $Binaries 'PcToolStorageTests.exe'),(Join-Path $Binaries 'PcToolArchiveStorageTests.exe') -Destination $fixture
Copy-Item (Join-Path $Binaries 'PcToolStorageTests.exe') -Destination (Join-Path $fixture 'PcTool.exe')
& (Join-Path $fixture 'PcToolStorageTests.exe');if($LASTEXITCODE){throw 'Storage test failed'}
& (Join-Path $fixture 'PcToolArchiveStorageTests.exe');if($LASTEXITCODE){throw 'Private hive write failed'}
$resolvedFixture = [IO.Path]::GetFullPath($fixture).TrimEnd('\')
$flatHive = [IO.Path]::GetFullPath((Join-Path $fixture 'Data/SevenZip'))
$legacyHive = [IO.Path]::GetFullPath((Join-Path $fixture ('Data/'+[Security.Principal.WindowsIdentity]::GetCurrent().User.Value+'/SevenZip')))
foreach ($migrationPath in @($flatHive,$legacyHive)) {
    if (!$migrationPath.StartsWith($resolvedFixture+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Hive fixture escaped test root' }
}
New-Item -ItemType Directory -Path (Split-Path -Parent $legacyHive) -Force | Out-Null
Move-Item -LiteralPath $flatHive -Destination $legacyHive
& (Join-Path $fixture 'PcToolArchiveStorageTests.exe') read;if($LASTEXITCODE){throw 'Private hive reload failed'}
if (!(Test-Path -LiteralPath $flatHive) -or (Test-Path -LiteralPath (Split-Path -Parent $legacyHive))) { throw 'Archive-only legacy SID migration failed' }
& "$PSScriptRoot/../packaging/manage-data.ps1" -Action Install -Root $fixture
if($LASTEXITCODE){throw 'ACL setup failed'}
& "$PSScriptRoot/../packaging/manage-data.ps1" -Action Uninstall -Root $fixture
if($LASTEXITCODE){throw 'Managed cleanup failed'}
if((Test-Path -LiteralPath (Join-Path $fixture 'Data')) -or (Test-Path -LiteralPath (Join-Path $fixture 'Cache'))){throw 'Managed directories survived cleanup'}
if(!(Test-Path -LiteralPath (Join-Path $fixture 'external-fixture/keep.txt'))){throw 'Unrelated file removed'}
'PASS data ACL, managed removal, unrelated files preserved'
