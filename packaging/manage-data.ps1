param([ValidateSet('Install','Uninstall')][string]$Action,[Parameter(Mandatory=$true)][string]$Root)
$ErrorActionPreference='Stop'
$Root=[IO.Path]::GetFullPath($Root).TrimEnd('\')
if($Root -eq [IO.Path]::GetPathRoot($Root).TrimEnd('\')){throw 'Refusing drive root'}
if(!(Test-Path -LiteralPath (Join-Path $Root 'PcTool.exe'))){throw 'Application root is not valid'}
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
 exit 0
}
# Do not kill archive jobs. Ask their windows to close; fail if work is still active.
Get-Process -ErrorAction SilentlyContinue | ForEach-Object {
 $p=$_;try{$file=$p.Path}catch{return}
 if($file -and $file.StartsWith($Root+'\modules\archive\',[StringComparison]::OrdinalIgnoreCase)){
  if(!$p.HasExited){[void]$p.CloseMainWindow();if(!$p.WaitForExit(10000)){throw 'Please finish and close the running 7-zip operation, then retry uninstall'}}
 }
}
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
Add-Type @'
using System;using System.Runtime.InteropServices;
public static class DeferredDelete {
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] public static extern bool MoveFileEx(string path,string target,int flags);
}
'@
$script:pending=$false
function Remove-Owned([string]$path){
 $full=[IO.Path]::GetFullPath($path)
 $within=$false
 foreach($name in @('Data','Cache')){$base=Join-Path $Root $name;if($full.Equals($base,[StringComparison]::OrdinalIgnoreCase) -or $full.StartsWith($base+'\',[StringComparison]::OrdinalIgnoreCase)){$within=$true}}
 if(!$within){throw 'Cleanup escaped data root'}
 if(!(Test-Path -LiteralPath $full)){return}
 $item=Get-Item -LiteralPath $full -Force
 if($item.PSIsContainer -and !($item.Attributes -band [IO.FileAttributes]::ReparsePoint)){
  Get-ChildItem -LiteralPath $full -Force | ForEach-Object {Remove-Owned $_.FullName}
 }
 try{if($item.PSIsContainer){[IO.Directory]::Delete($full,$false)}else{Remove-Item -LiteralPath $full -Force -ErrorAction Stop}}
 catch{if(![DeferredDelete]::MoveFileEx($full,$null,4)){throw};$script:pending=$true}
}
Remove-Owned (Join-Path $Root 'Cache')
Remove-Owned (Join-Path $Root 'Data')
if($pending){exit 3010}
