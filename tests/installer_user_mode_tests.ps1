param([Parameter(Mandatory=$true)][string]$Binaries,[string]$Output)
$ErrorActionPreference='Stop'
$project=Split-Path -Parent $PSScriptRoot
$Binaries=[IO.Path]::GetFullPath($Binaries)
$namespace='PcToolTest-'+[guid]::NewGuid().ToString('N')
if(!$Output){$Output=Join-Path $project ('out/installer-user-tests/'+$namespace)}
$root=Join-Path ([IO.Path]::GetFullPath($Output)) '非系统盘 app'
New-Item -ItemType Directory -Path $root -Force|Out-Null
$sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
function Check([bool]$ok,[string]$message){if(!$ok){throw $message}}
foreach($binary in @('PcTool.exe','PcToolStartupTests.exe','PcToolArchiveStorageTests.exe','PcToolUninstallHelper.exe')){Copy-Item -LiteralPath (Join-Path $Binaries $binary) -Destination $root}
$manifest=Join-Path $Output 'files.json'
@{Version=1;Files=@('PcTool.exe','PcToolStartupTests.exe','PcToolArchiveStorageTests.exe','PcToolUninstallHelper.exe');Directories=@()}|ConvertTo-Json|Set-Content $manifest -Encoding UTF8
New-Item -ItemType Directory -Path "$root/Data/Translation","$root/Cache/2026-01-01" -Force|Out-Null
[IO.File]::WriteAllText("$root/Data/settings.ini","[PcTool]`r`nVersion=1`r`nAutoStart=0`r`nTranslationEnabled=0`r`n")
[IO.File]::WriteAllBytes("$root/Data/Translation/provider.bin",[byte[]]@(0,1,254,255,18))
[IO.File]::WriteAllText("$root/Cache/2026-01-01/fixture.tmp",'cache')
$originalHash=(Get-FileHash "$root/Data/Translation/provider.bin").Hash
$runner=Join-Path $Binaries 'PcToolStandardUserRunner.exe'
try{
 # A pre-existing different setting must never be silently overwritten or
 # removed, even when migration is retried by an elevated installer.
 $collision=Join-Path $root ('Data/'+$sid+'/settings.ini')
 New-Item -ItemType Directory -Path (Split-Path -Parent $collision) -Force|Out-Null
 [IO.File]::WriteAllText($collision,'existing private setting')
 $sourceHash=(Get-FileHash "$root/Data/settings.ini").Hash
 $targetHash=(Get-FileHash $collision).Hash
 $migrationLog=Join-Path $Output 'migration-conflict.log'
 $migration=Start-Process -FilePath "$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe" -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$project+'/packaging/lifecycle.ps1"'),'-Action','MigrateData','-Root',('"'+$root+'"'),'-Namespace',$namespace) -WindowStyle Hidden -Wait -PassThru -RedirectStandardError $migrationLog
 Check ($migration.ExitCode -ne 0) 'Conflicting migration incorrectly succeeded'
 Check ((Get-FileHash "$root/Data/settings.ini").Hash -eq $sourceHash -and (Get-FileHash $collision).Hash -eq $targetHash) 'Conflicting migration lost original or private settings'
 Remove-Item -LiteralPath $collision -Force
 'PASS migration conflict reports failure and preserves both original files'
 & "$project/packaging/manage-data.ps1" -Action Record -Root $root -Manifest $manifest -CleanupNamespace $namespace
 Check ($LASTEXITCODE -eq 0) 'Inventory failed'
 & "$project/packaging/manage-data.ps1" -Action Install -Root $root -CleanupNamespace $namespace
 Check ($LASTEXITCODE -eq 0) 'Data migration/ACL failed'
 Check (!(Test-Path "$root/Data/settings.ini") -and (Get-FileHash "$root/Data/Translation/provider.bin").Hash -eq $originalHash -and !(Test-Path "$root/Data/$sid/Translation") -and (Test-Path "$root/Cache/$sid/2026-01-01/fixture.tmp")) 'Legacy migration changed bytes or moved portable Translation into SID directory'
 & $runner "$root/PcToolStartupTests.exe" --background
 Check ($LASTEXITCODE -eq 0) "Medium token storage probe failed: $LASTEXITCODE"
 $identity=@(Get-Content "$root/Data/$sid/startup-token.txt")
 Check ($identity[0] -eq $sid -and $identity[2] -eq '0' -and $identity[3] -eq '8192' -and $identity[4] -eq '0') 'Probe was elevated, high integrity or an enabled administrator'
 'PASS actual medium-integrity non-administrator token writes protected install Data/Cache'
 & $runner "$root/PcToolStartupTests.exe" --exercise $namespace
 Check ($LASTEXITCODE -eq 0) 'Medium token one-time startup/settings regression failed'
 & $runner "$root/PcToolArchiveStorageTests.exe"
 Check ($LASTEXITCODE -eq 0) 'Medium token 7-Zip hive/cache failed'
 & $runner "$root/PcToolArchiveStorageTests.exe" read
 Check ($LASTEXITCODE -eq 0) 'Medium token private 7-Zip hive reload failed'
 'PASS medium token startup preferences and 7-Zip private hive/cache reload'
 & $runner "$root/PcTool.exe" --initialize-startup $namespace
 Check ($LASTEXITCODE -eq 0) 'Medium token actual product entry failed'
 'PASS actual product startup entry runs without elevation'
 # An offline user hive exercises cleanup without loading a system profile.
 Add-Type @'
using System;using System.Runtime.InteropServices;using Microsoft.Win32;using Microsoft.Win32.SafeHandles;
public static class PcToolTestHive { [DllImport("advapi32.dll",CharSet=CharSet.Unicode)] static extern int RegLoadAppKey(string file,out SafeRegistryHandle key,int access,int options,int reserved); public static RegistryKey Open(string file){SafeRegistryHandle key;int code=RegLoadAppKey(file,out key,0xf003f,1,0);if(code!=0)throw new System.ComponentModel.Win32Exception(code);return RegistryKey.FromHandle(key);}}
'@
 $offline=Join-Path $root 'offline-test.hiv';$hive=[PcToolTestHive]::Open($offline)
 $path='Software\'+$namespace+'\Run';$valueName=(& "$root/PcToolStartupTests.exe" --name "$root/PcTool.exe"|Out-String).Trim()
 $key=$hive.CreateSubKey($path);$key.SetValue($valueName,('"'+$root+'\PcTool.exe" --background'));$key.SetValue('Foreign','untouched');$key.Dispose();$hive.Flush();$hive.Dispose()
 & "$project/packaging/lifecycle.ps1" -Action RemoveStartup -Root $root -Namespace $namespace -TestOfflineHives $offline
 Check ($LASTEXITCODE -eq 0) 'Offline startup cleanup failed'
 $hive=[PcToolTestHive]::Open($offline);$key=$hive.OpenSubKey($path)
 try{Check ($null -eq $key.GetValue($valueName) -and $key.GetValue('Foreign') -eq 'untouched') 'Offline cleanup changed foreign data or retained owned Run'}finally{$key.Dispose();$hive.Dispose()}
 $handle=[IO.File]::Open($offline,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None);$handle.Dispose()
 'PASS offline startup cleanup preserves unrelated values and releases hive handles'
 & "$project/packaging/manage-data.ps1" -Action Uninstall -Root $root -CleanupNamespace $namespace
 Check ($LASTEXITCODE -eq 0) 'User data removal failed'
 Check (!(Test-Path "$root/Data") -and !(Test-Path "$root/Cache")) 'Per-user managed data survived uninstall'
 'PASS all per-user data/cache removed; no reboot required'
}finally{
 [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree('Software\'+$namespace,$false)
 "Evidence: $Output"
}
