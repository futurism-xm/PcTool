param([ValidateSet('Record','Install','Uninstall')][string]$Action,[Parameter(Mandatory=$true)][string]$Root,[string]$Manifest)
$ErrorActionPreference='Stop'
$Root=[IO.Path]::GetFullPath($Root).TrimEnd('\')
if($Root -eq [IO.Path]::GetPathRoot($Root).TrimEnd('\')){throw 'Refusing drive root'}
$ancestor=$Root
while($ancestor){
 if(Test-Path -LiteralPath $ancestor){if((Get-Item -LiteralPath $ancestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Installation path cannot traverse a reparse point'}}
 $ancestor=Split-Path -Parent $ancestor
}
$inventoryPath=Join-Path $Root '.pctool-managed-files.json'
function Resolve-Owned([string]$relative){
 if([string]::IsNullOrWhiteSpace($relative) -or [IO.Path]::IsPathRooted($relative) -or $relative.Contains(':')){throw 'Invalid managed relative path'}
 $full=[IO.Path]::GetFullPath((Join-Path $Root $relative))
 if(!$full.StartsWith($Root+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Managed path escaped installation root'}
 $parent=Split-Path -Parent $full
 while($parent -and $parent -ne $Root){
  if(Test-Path -LiteralPath $parent){if((Get-Item -LiteralPath $parent -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw "Managed path traverses a reparse point: $relative"}}
  $parent=Split-Path -Parent $parent
 }
 return $full
}
function Read-Inventory([string]$path){
 $value=Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
 if($value.Version -ne 1){throw 'Unsupported managed file inventory'}
 foreach($relative in @($value.Files)+@($value.Directories)){[void](Resolve-Owned $relative)}
 return $value
}
if($Action -eq 'Record'){
 $current=Read-Inventory $Manifest
 $files=@($current.Files);$directories=@($current.Directories)
 if(Test-Path -LiteralPath $inventoryPath){$old=Read-Inventory $inventoryPath;$files+=@($old.Files);$directories+=@($old.Directories)}
 New-Item -ItemType Directory -Path $Root -Force | Out-Null
 $text=@{Version=1;Files=@($files|Sort-Object -Unique);Directories=@($directories|Sort-Object -Unique)}|ConvertTo-Json -Depth 4
 $temporary=$inventoryPath+'.tmp'
 [IO.File]::WriteAllText($temporary,$text,[Text.UTF8Encoding]::new($false))
 if(Test-Path -LiteralPath $inventoryPath){
  # Keep the native null inside C#; PowerShell string binding turns $null into "".
  Add-Type 'public static class InventoryFile { public static void Replace(string source, string target) { System.IO.File.Replace(source, target, null); } }'
  [InventoryFile]::Replace($temporary,$inventoryPath)
 }else{[IO.File]::Move($temporary,$inventoryPath)}
 exit 0
}
if(!(Test-Path -LiteralPath (Join-Path $Root 'PcTool.exe')) -and !(Test-Path -LiteralPath $inventoryPath)){throw 'Application root is not valid'}
if($Action -eq 'Install'){
 $sid=[Security.Principal.WindowsIdentity]::GetCurrent().User
 foreach($name in @('Data','Cache')){
  $path=Join-Path $Root $name
  if(Test-Path -LiteralPath $path){if((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Data root cannot be a reparse point'}}
  New-Item -ItemType Directory -Path $path -Force | Out-Null
  $acl=New-Object Security.AccessControl.DirectorySecurity
  $acl.SetAccessRuleProtection($true,$false)
  foreach($entry in @(@('S-1-5-18','FullControl'),@('S-1-5-32-544','FullControl'),@($sid.Value,'Modify'))){
   $identity=New-Object Security.Principal.SecurityIdentifier($entry[0])
   $rule=New-Object Security.AccessControl.FileSystemAccessRule($identity,$entry[1],'ContainerInherit,ObjectInherit','None','Allow')
   $acl.AddAccessRule($rule)
  }
  $directory=New-Object IO.DirectoryInfo($path)
  if($PSVersionTable.PSEdition -eq 'Core'){[IO.FileSystemAclExtensions]::SetAccessControl($directory,$acl)}else{$directory.SetAccessControl($acl)}
 }
 Remove-Item -LiteralPath (Join-Path $Root '.pctool-uninstalling') -Force -ErrorAction SilentlyContinue
 exit 0
}
# Native helper has already stopped owned processes. Continue independent cleanup on error.
$script:failures=[Collections.Generic.List[string]]::new()
# Remove this installation's startup entry from every currently loaded user hive.
try {
 foreach($sid in [Microsoft.Win32.Registry]::Users.GetSubKeyNames() | Where-Object {$_ -match '^S-1-5-\d+(?:-\d+)+$'}){
  $run=[Microsoft.Win32.Registry]::Users.OpenSubKey($sid+'\Software\Microsoft\Windows\CurrentVersion\Run',$true)
  if($null -eq $run){continue}
  try{
   if([string]$run.GetValue('PcTool') -eq ('"'+$Root+'\PcTool.exe" --background')){
    $run.DeleteValue('PcTool',$false)
    if($null -ne $run.GetValue('PcTool')){throw 'Startup registration survived removal'}
   }
  }finally{$run.Dispose()}
 }
}catch{$script:failures.Add($_.Exception.Message)}
try {
# Remove associations created through the bundled manager only while their
# command still points to this installation. Never remove another 7-zip's keys.
foreach($hive in @([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryHive]::LocalMachine)){
 foreach($view in @([Microsoft.Win32.RegistryView]::Registry64,[Microsoft.Win32.RegistryView]::Registry32)){
  $base=[Microsoft.Win32.RegistryKey]::OpenBaseKey($hive,$view)
  $classes=$base.OpenSubKey('Software\Classes',$false)
  if($null -eq $classes){$base.Dispose();continue}
  try{
   foreach($name in @($classes.GetSubKeyNames() | Where-Object {$_.StartsWith('7-Zip.',[StringComparison]::OrdinalIgnoreCase)})){
    $command=$classes.OpenSubKey($name+'\shell\open\command')
    if($null -eq $command){continue}
    $value=[string]$command.GetValue('');$command.Dispose()
    $expected='"'+$Root+'\modules\archive\7zFM.exe"'
    if(!$value.StartsWith($expected,[StringComparison]::OrdinalIgnoreCase)){continue}
    $writable=$base.OpenSubKey('Software\Classes',$true)
    foreach($extension in @($classes.GetSubKeyNames() | Where-Object {$_.StartsWith('.')})){
     $key=$classes.OpenSubKey($extension,$false)
     $owned=[string]$key.GetValue('') -eq $name
     if($owned){$key.Dispose();$key=$writable.OpenSubKey($extension,$true);$key.DeleteValue('',$false)}
     $empty=$owned -and $key.ValueCount -eq 0 -and $key.SubKeyCount -eq 0;$key.Dispose()
     if($empty){$writable.DeleteSubKey($extension,$false)}
    }
    $writable.DeleteSubKeyTree($name,$false);$writable.Dispose()
   }
  }finally{$classes.Dispose();$base.Dispose()}
 }
}
}catch{$script:failures.Add($_.Exception.Message)}
Add-Type @'
using System;using System.Runtime.InteropServices;
public static class DeferredDelete {
 // PowerShell converts $null to an empty string for a string parameter.
 // MOVEFILE_DELAY_UNTIL_REBOOT requires a native NULL destination to delete.
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] public static extern bool MoveFileEx(string path,IntPtr target,int flags);
}
'@
$script:pending=$false
function Remove-One([string]$full,[bool]$directory){
 try{if($directory){[IO.Directory]::Delete($full,$false)}else{Remove-Item -LiteralPath $full -Force -ErrorAction Stop}}
 catch{
  if(![DeferredDelete]::MoveFileEx($full,[IntPtr]::Zero,4)){
   $code=[Runtime.InteropServices.Marshal]::GetLastWin32Error()
   throw "Cannot remove or schedule '$full' (Win32 $code). $($_.Exception.Message)"
  }
  Write-Output "Scheduled for deletion after restart: $full"
  $script:pending=$true
 }
}
function Remove-Owned([string]$relative){
 try{
 $full=Resolve-Owned $relative
 if(!(Test-Path -LiteralPath $full)){return}
 $item=Get-Item -LiteralPath $full -Force
 if($item.PSIsContainer -and !($item.Attributes -band [IO.FileAttributes]::ReparsePoint)){
  Get-ChildItem -LiteralPath $full -Force | ForEach-Object {Remove-Owned $_.FullName.Substring($Root.Length+1)}
 }
 Remove-One $full $item.PSIsContainer
 }catch{$script:failures.Add($_.Exception.Message)}
}
Remove-Owned 'Cache'
Remove-Owned 'Data'
if(Test-Path -LiteralPath $inventoryPath){
 try{
  $inventory=Read-Inventory $inventoryPath
  # Keep the retry machinery until all independent uninstall phases succeed.
  $bootstrap=@('manage-data.ps1','PcToolUninstallHelper.exe','Uninstall.exe','.pctool-managed-files.json','.pctool-uninstalling')
  foreach($relative in $inventory.Files){
   if($relative -in $bootstrap){continue}
   try{
    $full=Resolve-Owned $relative
    if(Test-Path -LiteralPath $full){
     if((Get-Item -LiteralPath $full -Force).PSIsContainer){throw "Expected managed file: $relative"}
     Remove-One $full $false
    }
   }catch{$script:failures.Add($_.Exception.Message)}
  }
  $archive=Resolve-Owned 'modules/archive'
  if((Test-Path -LiteralPath $archive) -and !((Get-Item -LiteralPath $archive -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)){
   Get-ChildItem -LiteralPath $archive -File -Force | Where-Object {$_.Name -match '^7-zip(32)?\.dll\.\d+\.old$'} | ForEach-Object {
    try{Remove-One $_.FullName $false}catch{$script:failures.Add($_.Exception.Message)}
   }
  }
  foreach($relative in @($inventory.Directories|Sort-Object {$_.Length} -Descending)){
   try{
    $full=Resolve-Owned $relative
    if(!(Test-Path -LiteralPath $full)){continue}
    $item=Get-Item -LiteralPath $full -Force
    if(!$item.PSIsContainer){continue}
    if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){Remove-One $full $true;continue}
    $children=@(Get-ChildItem -LiteralPath $full -Force)
    if(!$children.Count){Remove-One $full $true}
    elseif($pending){
     # Reboot only removes empty directories; unrelated contents prevent deletion.
     if(![DeferredDelete]::MoveFileEx($full,[IntPtr]::Zero,4)){throw "Cannot schedule managed directory: $full"}
    }
   }catch{$script:failures.Add($_.Exception.Message)}
  }
 }catch{$script:failures.Add($_.Exception.Message)}
}
if($failures.Count){$failures|ForEach-Object {Write-Output "Cleanup failed: $_"};exit 1}
if($pending){exit 3010}
exit 0
