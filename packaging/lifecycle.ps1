param(
 [Parameter(Mandatory=$true)][ValidateSet('MigrateData','RemoveStartup')][string]$Action,
 [Parameter(Mandatory=$true)][string]$Root,
 [string]$Namespace='PcTool',
 [string]$TargetUserSid,
 [string[]]$TestOfflineHives=@(),
 [switch]$LegacyOnly
)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'cleanup-jobs.ps1')
Test-CleanupNamespace $Namespace
$Root=Get-CleanupRoot $Root
if($Namespace -eq 'PcTool' -and ($TargetUserSid -or $TestOfflineHives.Count)){throw 'Test overrides require an isolated namespace'}

function Assert-NoLink([string]$path){
 for($item=$path;$item;$item=Split-Path -Parent $item){
  if((Test-Path -LiteralPath $item) -and ((Get-Item -LiteralPath $item -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)){throw "Reparse point is not supported: $item"}
 }
}
function Set-PrivateTree([string]$path,[string]$sid){
 Assert-NoLink $path
 $item=Get-Item -LiteralPath $path -Force
 $acl=if($item.PSIsContainer){[Security.AccessControl.DirectorySecurity]::new()}else{[Security.AccessControl.FileSecurity]::new()}
 $acl.SetAccessRuleProtection($true,$false)
 $acl.SetOwner([Security.Principal.SecurityIdentifier]::new($sid))
 foreach($identity in @('S-1-5-18','S-1-5-32-544',$sid)){
  $id=[Security.Principal.SecurityIdentifier]::new($identity)
  $rule=if($item.PSIsContainer){[Security.AccessControl.FileSystemAccessRule]::new($id,'FullControl','ContainerInherit,ObjectInherit','None','Allow')}else{[Security.AccessControl.FileSystemAccessRule]::new($id,'FullControl','Allow')}
  $acl.AddAccessRule($rule)
 }
 Set-Acl -LiteralPath $path -AclObject $acl
 if($item.PSIsContainer){foreach($child in @(Get-ChildItem -LiteralPath $path -Force)){Set-PrivateTree $child.FullName $sid}}
}
function Merge-Verified([string]$source,[string]$target){
 Assert-NoLink $source;Assert-NoLink $target
 $item=Get-Item -LiteralPath $source -Force
 if($item.PSIsContainer){
  New-Item -ItemType Directory -Path $target -Force | Out-Null
  foreach($child in @(Get-ChildItem -LiteralPath $source -Force)){Merge-Verified $child.FullName (Join-Path $target $child.Name)}
  [IO.Directory]::Delete($source,$false)
 }else{
  $hash=(Get-FileHash -LiteralPath $source).Hash
  if(Test-Path -LiteralPath $target){if((Get-FileHash -LiteralPath $target).Hash -ne $hash){throw "Migration conflict; original retained: $source"}}
  else{Copy-Item -LiteralPath $source -Destination $target}
  if((Get-FileHash -LiteralPath $source).Hash -ne $hash -or (Get-FileHash -LiteralPath $target).Hash -ne $hash){throw "Migration verification failed: $source"}
  Remove-Item -LiteralPath $source -Force
 }
}
if($Action -eq 'MigrateData'){
 if(!$TargetUserSid -and $Namespace -ne 'PcTool'){$TargetUserSid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value}
 if(!$TargetUserSid){
  $helper=Join-Path $Root 'PcToolUninstallHelper.exe'
  $TargetUserSid=(& $helper interactive-user $Root | Out-String).Trim()
  if($LASTEXITCODE -or $TargetUserSid -notmatch '^S-1-\d+(?:-\d+)+$'){throw 'Cannot identify the original interactive user; legacy data was not assigned to the elevation account'}
 }
 foreach($category in @('Data','Cache')){
  $base=Join-Path $Root $category
  New-Item -ItemType Directory -Path $base -Force | Out-Null
  Assert-NoLink $base
  $target=Join-Path $base $TargetUserSid
  New-Item -ItemType Directory -Path $target -Force | Out-Null
  foreach($child in @(Get-ChildItem -LiteralPath $base -Force|Where-Object {$_.Name -notmatch '^S-1-\d+(?:-\d+)+$'})){Merge-Verified $child.FullName (Join-Path $target $child.Name)}
  # Existing SID layouts are already per-user; repair only product data ACLs.
  foreach($child in @(Get-ChildItem -LiteralPath $base -Directory -Force)){
   if($child.Name -notmatch '^S-1-\d+(?:-\d+)+$'){throw 'Unexpected data directory after migration'}
   Set-PrivateTree $child.FullName $child.Name
  }
  $acl=[Security.AccessControl.DirectorySecurity]::new();$acl.SetAccessRuleProtection($true,$false)
  $acl.SetOwner([Security.Principal.SecurityIdentifier]::new('S-1-5-32-544'))
  foreach($sid in @('S-1-5-18','S-1-5-32-544')){$acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.SecurityIdentifier]::new($sid),'FullControl','None','None','Allow'))}
  # Users may create their own private child; no inherited read/write grant.
  $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.SecurityIdentifier]::new('S-1-5-32-545'),([Security.AccessControl.FileSystemRights]::ReadAndExecute -bor [Security.AccessControl.FileSystemRights]::CreateDirectories),'None','None','Allow'))
  Set-Acl -LiteralPath $base -AclObject $acl
 }
 Write-Output 'Per-user installation data migrated and verified.'
 exit 0
}

$command='"'+$Root+'\PcTool.exe" --background'
$hash=Get-CleanupRootHash $Root
$valueName='PcTool-'+$hash
$runPath=if($Namespace -eq 'PcTool'){'Software\Microsoft\Windows\CurrentVersion\Run'}else{'Software\'+$Namespace+'\Run'}
function Remove-Run($hive){
 $run=$hive.OpenSubKey($runPath,$true);if(!$run){return}
 try{
  $names=if($LegacyOnly){@('PcTool')}else{@('PcTool',$valueName)}
  foreach($name in $names){
   $value=$run.GetValue($name,$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
   if($value -is [string] -and [string]::Equals($value,$command,[StringComparison]::OrdinalIgnoreCase)){
    $run.DeleteValue($name,$false)
    if($null -ne $run.GetValue($name)){throw 'Startup registration survived removal'}
   }
  }
 }finally{$run.Dispose()}
}
# Remove obsolete elevated tasks only after every ownership field matches.
$scheduler=New-Object -ComObject 'Schedule.Service';$scheduler.Connect()
$folder=$null
try{$folder=$scheduler.GetFolder('\'+$Namespace+'\Startup')}catch{
 if($_.Exception.GetBaseException().HResult -notin @(-2147024894,-2147024893)){throw}
}
if($folder){
 foreach($task in @($folder.GetTasks(1))){
  $d=$task.Definition;$sid=[string]$d.Principal.UserId
  if($sid -notlike 'S-1-*'){$sid=([Security.Principal.NTAccount]::new($sid)).Translate([Security.Principal.SecurityIdentifier]).Value}
  if($task.Name -ne ($sid+'-'+$hash) -or $d.RegistrationInfo.Description -ine ('PcTool Startup v1|'+$sid+'|'+$Root+'\PcTool.exe')){continue}
  if($d.Principal.LogonType -ne 3 -or $d.Principal.RunLevel -ne 1 -or $d.Actions.Count -ne 1){continue}
  $a=$d.Actions.Item(1)
  if($a.Type -eq 0 -and $a.Path -ieq ($Root+'\PcTool.exe') -and $a.Arguments -ceq '--background'){$folder.DeleteTask($task.Name,0)}
 }
 if(!@($folder.GetTasks(1)).Count){$scheduler.GetFolder('\'+$Namespace).DeleteFolder('Startup',0)}
}
foreach($sid in [Microsoft.Win32.Registry]::Users.GetSubKeyNames()|Where-Object {$_ -match '^S-1-\d+(?:-\d+)+$'}){
 $hive=[Microsoft.Win32.Registry]::Users.OpenSubKey($sid,$true)
 try{Remove-Run $hive}finally{$hive.Dispose()}
}
Add-Type @'
using System; using System.Runtime.InteropServices; using Microsoft.Win32; using Microsoft.Win32.SafeHandles;
public static class PcToolOfflineStartup {
 [DllImport("advapi32.dll",CharSet=CharSet.Unicode)] static extern int RegLoadAppKey(string file,out SafeRegistryHandle key,int access,int options,int reserved);
 public static RegistryKey Open(string file) {SafeRegistryHandle key;int code=RegLoadAppKey(file,out key,0x2001f,1,0);if(code!=0)throw new System.ComponentModel.Win32Exception(code);return RegistryKey.FromHandle(key);}
}
'@
$offline=@($TestOfflineHives)
if($Namespace -eq 'PcTool'){
 $profiles=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('Software\Microsoft\Windows NT\CurrentVersion\ProfileList')
 try{foreach($sid in $profiles.GetSubKeyNames()|Where-Object {$_ -match '^S-1-\d+(?:-\d+)+$'}){
  $loaded=[Microsoft.Win32.Registry]::Users.OpenSubKey($sid);if($loaded){$loaded.Dispose();continue}
  $profile=$profiles.OpenSubKey($sid);try{$directory=[Environment]::ExpandEnvironmentVariables([string]$profile.GetValue('ProfileImagePath'))}finally{$profile.Dispose()}
  if($directory){$file=Join-Path $directory 'NTUSER.DAT';if(Test-Path -LiteralPath $file){$offline+=,$file}}
 }}finally{$profiles.Dispose()}
}
foreach($file in $offline){
 Assert-NoLink $file
 if($Namespace -ne 'PcTool'){
  $full=[IO.Path]::GetFullPath($file)
  if(!$full.StartsWith($Root+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Offline fixture escaped isolated root'}
 }
 $hive=[PcToolOfflineStartup]::Open($file)
 try{Remove-Run $hive}finally{$hive.Dispose()}
}
Write-Output 'Owned Run entries and obsolete elevated startup tasks removed.'
exit 0
