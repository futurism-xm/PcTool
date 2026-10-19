param([ValidateSet('LockName','Record','Install','Uninstall','Finalize')][string]$Action,[Parameter(Mandatory=$true)][string]$Root,[string]$Manifest,[string]$CleanupNamespace='PcTool')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'cleanup-jobs.ps1')
Test-CleanupNamespace $CleanupNamespace
$Root=Get-CleanupRoot $Root
if($Action -eq 'LockName'){[Console]::Write((Get-CleanupLockName $Root $CleanupNamespace));exit 0}
# NSIS owns the named mutex for this entire install/uninstall transaction.
# Do not reacquire it here: this script runs in an installer child process.
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
function Clear-PreviousUninstallDeletes($inventory){
 # Match the new package, not arbitrary files from an old inventory. Windows
 # processes these pairs by pathname, even if a new file replaced the old one.
 $owned=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
 [void]$owned.Add($Root)
 foreach($relative in @($inventory.Files)+@($inventory.Directories)+@('Uninstall.exe','.pctool-managed-files.json','.pctool-uninstalling','Data','Cache')){
  [void]$owned.Add((Resolve-Owned $relative))
 }
 function Pending-Path([string]$path){
  if($path.StartsWith('!')){$path=$path.Substring(1)}
  if($path.StartsWith('\??\') -or $path.StartsWith('\\?\')){$path=$path.Substring(4)}
  if($path -notmatch '^[A-Za-z]:\\'){return ''}
  try{return [IO.Path]::GetFullPath($path).TrimEnd('\')}catch{return ''}
 }
 function Is-ReinstalledPath([string]$path){
  if(!$path){return $false}
  if($owned.Contains($path)){return $true}
  foreach($directory in @('Data','Cache')){
   if($path.StartsWith((Join-Path $Root $directory)+'\',[StringComparison]::OrdinalIgnoreCase)){
    # Reject directory junctions instead of modifying operations outside our root.
    [void](Resolve-Owned $path.Substring($Root.Length+1));return $true
   }
  }
  return $false
 }
 $queue=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Control\Session Manager',$true)
 if(!$queue){throw 'Cannot inspect pending uninstall operations'}
 try{
  $name='PendingFileRenameOperations'
  for($attempt=0;$attempt -lt 4;$attempt++){
   if($queue.GetValueNames() -notcontains $name){return}
   if($queue.GetValueKind($name) -ne [Microsoft.Win32.RegistryValueKind]::MultiString){throw 'Invalid pending-operation registry type'}
   $entries=@($queue.GetValue($name,[string[]]@()))
   if($entries.Count % 2){throw 'Invalid pending-operation pairs; installation stopped'}
   $kept=[Collections.Generic.List[string]]::new();$removed=0
   for($i=0;$i -lt $entries.Count;$i+=2){
    $source=Pending-Path $entries[$i]
    if($entries[$i+1] -eq '' -and (Is-ReinstalledPath $source)){$removed++;continue}
    if($entries[$i+1] -ne '' -and ((Is-ReinstalledPath $source) -or (Is-ReinstalledPath (Pending-Path $entries[$i+1])))){
     throw 'A pending rename affects this installation; restart Windows before installing'
    }
    $kept.Add($entries[$i]);$kept.Add($entries[$i+1])
   }
   if(!$removed){return}
   # Re-read before writing so operations registered while we inspected the list
   # are not silently overwritten. No unrelated entries are removed or reordered.
   $latest=@($queue.GetValue($name,[string[]]@()))
   if(($latest -join "`0") -cne ($entries -join "`0")){continue}
   if($kept.Count){$queue.SetValue($name,$kept.ToArray(),[Microsoft.Win32.RegistryValueKind]::MultiString)}else{$queue.DeleteValue($name,$false)}
   Write-Output "Cancelled $removed previous uninstall deletion(s) for this installation"
   # Recheck for new matching operations before allowing installation to proceed.
  }
  throw 'Pending operations kept changing; installation stopped for retry'
 }finally{$queue.Dispose()}
}
if($Action -eq 'Record'){
 $current=Read-Inventory $Manifest
 Clear-PreviousUninstallDeletes $current
 Stop-CleanupJobs $Root $CleanupNamespace
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
 Write-CleanupJson (Join-Path $Root '.pctool-install-instance.json') ([ordered]@{Version=1;Id=[guid]::NewGuid().ToString('N');Root=$Root;Entries=@()})
 exit 0
}
if(!(Test-Path -LiteralPath (Join-Path $Root 'PcTool.exe')) -and !(Test-Path -LiteralPath $inventoryPath) -and !(Test-Path -LiteralPath (Join-Path $Root '.pctool-install-instance.json'))){
 if($Action -in @('Uninstall','Finalize')){exit 0}
 throw 'Application root is not valid'
}
if($Action -eq 'Install'){
 # The executable, DLLs and their directories must not inherit writable ACEs
 # from custom locations such as D:\install. Only managed payload ACLs change.
 function Protect-Payload([string]$path,[bool]$directory){
  if((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Payload cannot be a reparse point'}
  if($directory){$acl=New-Object Security.AccessControl.DirectorySecurity}else{$acl=New-Object Security.AccessControl.FileSecurity}
  $acl.SetAccessRuleProtection($true,$false)
  $acl.SetOwner([Security.Principal.SecurityIdentifier]::new('S-1-5-32-544'))
  foreach($entry in @(@('S-1-5-18','FullControl'),@('S-1-5-32-544','FullControl'),@('S-1-5-32-545','ReadAndExecute'))){
   $identity=[Security.Principal.SecurityIdentifier]::new($entry[0])
   if($directory){$rule=[Security.AccessControl.FileSystemAccessRule]::new($identity,$entry[1],'ContainerInherit,ObjectInherit','None','Allow')}
   else{$rule=[Security.AccessControl.FileSystemAccessRule]::new($identity,$entry[1],'Allow')}
   [void]$acl.AddAccessRule($rule)
  }
  Set-Acl -LiteralPath $path -AclObject $acl
 }
 Protect-Payload $Root $true
 if(Test-Path -LiteralPath $inventoryPath){
  $payload=Read-Inventory $inventoryPath
  foreach($relative in @($payload.Directories)+@($payload.Files)){
   if($relative -match '^(Data|Cache)([\\/]|$)'){continue}
   $path=Resolve-Owned $relative
   if(Test-Path -LiteralPath $path){Protect-Payload $path ([bool]((Get-Item -LiteralPath $path -Force).PSIsContainer))}
  }
 }else{Protect-Payload (Join-Path $Root 'PcTool.exe') $false}
 & (Join-Path $PSScriptRoot 'lifecycle.ps1') -Action MigrateData -Root $Root -Namespace $CleanupNamespace
 if($LASTEXITCODE){throw 'Per-user data migration failed'}
 Remove-Item -LiteralPath (Join-Path $Root '.pctool-uninstalling') -Force -ErrorAction SilentlyContinue
 $archive=Resolve-Owned 'modules/archive'
 $old=@()
 if(Test-Path -LiteralPath $archive){
  if((Get-Item -LiteralPath $archive -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Archive directory cannot be a reparse point'}
  foreach($file in @(Get-ChildItem -LiteralPath $archive -File -Force|Where-Object {$_.Name -match '^7-zip(32)?\.dll\.\d+\.old$'})){
   $entry=New-CleanupEntry $Root $file.FullName.Substring($Root.Length+1)
   if($entry -and !(Remove-CleanupEntry $Root $entry)){$old+=@($entry)}
  }
 }
 if($old.Count){[void](Register-CleanupJob $Root $CleanupNamespace 'upgrade' $old);exit 2}
 exit 0
}
if($Action -eq 'Finalize'){
 $instance=Read-CleanupInstance $Root
 $entries=@($instance.Entries)
 $runtime=@{}
 foreach($name in @('cleanup-jobs.ps1','retry-cleanup.ps1')){$runtime[$name]=[IO.File]::ReadAllText((Join-Path $PSScriptRoot $name))}
 $bootstrap=@()
 foreach($relative in @('manage-data.ps1','cleanup-jobs.ps1','retry-cleanup.ps1','shell-integration.ps1','lifecycle.ps1','PcToolUninstallHelper.exe','Uninstall.exe','.pctool-package-files.json','.pctool-managed-files.json')){
  $entry=New-CleanupEntry $Root $relative;if($entry){$bootstrap+=@($entry);$entries+=@($entry)}
 }
 $pending=$false
 foreach($entry in @($instance.Entries|Where-Object {!$_.Directory})){if(!(Remove-CleanupEntry $Root $entry)){$pending=$true}}
 foreach($entry in @($instance.Entries|Where-Object {$_.Directory}|Sort-Object {$_.Path.Length} -Descending)){if(!(Remove-CleanupEntry $Root $entry)){$pending=$true}}
 $store=Get-CleanupStore $CleanupNamespace;$existing=@()
 if(Test-Path -LiteralPath $store){$hash=Get-CleanupRootHash $Root;$existing=@(Get-ChildItem -LiteralPath $store -Directory -Force|Where-Object {$_.Name.StartsWith($hash+'-',[StringComparison]::Ordinal)})}
 if(!$pending -and !$existing.Count){
  $retryUninstaller=$null
  $uninstallerPath=Join-Path $Root 'Uninstall.exe'
  if(Test-Path -LiteralPath $uninstallerPath){
   try{$retryUninstaller=[IO.File]::ReadAllBytes($uninstallerPath)}catch{
    # An occupied retry executable cannot be deleted either, so its original
    # remains available; let the normal identity-bound retry path handle it.
    if(($_.Exception.GetBaseException().HResult -band 65535) -notin @(32,33,1224)){throw}
   }
  }
  # Normal, unlocked uninstall needs no Task Scheduler service. The embedded
  # uninstaller is removed last so a newly discovered bootstrap lock/error can
  # still be retried even after the other bootstrap scripts have been removed.
  foreach($entry in @($bootstrap|Where-Object {$_.Path -ne 'Uninstall.exe'})){if(!(Remove-CleanupEntry $Root $entry)){$pending=$true}}
  if(!$pending){
   $finalizationFailed=$true
   try{
    foreach($name in @('.pctool-uninstalling','.pctool-install-instance.json')){
     $entry=New-CleanupEntry $Root $name;if($entry -and !(Remove-CleanupEntry $Root $entry)){$pending=$true;break}
    }
    if(!$pending){foreach($entry in @($bootstrap|Where-Object {$_.Path -eq 'Uninstall.exe'})){if(!(Remove-CleanupEntry $Root $entry)){$pending=$true}}}
    if(!$pending){$pending=!(Remove-CleanupRoot $Root $instance $retryUninstaller)}
    $finalizationFailed=$false
   }finally{
    # If final deletion was blocked, restore authoritative state before a job
    # is registered; the installation mutex prevents a reinstall in this gap.
    if($pending -or $finalizationFailed){
     if(!(Test-Path -LiteralPath (Join-Path $Root '.pctool-install-instance.json'))){Write-CleanupJson (Join-Path $Root '.pctool-install-instance.json') $instance}
     if(!(Test-Path -LiteralPath (Join-Path $Root '.pctool-uninstalling'))){[IO.File]::WriteAllText((Join-Path $Root '.pctool-uninstalling'),'')}
    }
   }
  }
  if(!$pending){exit 0}
  # An empty-root lock may have required restoring the embedded retry tool.
  # Register its current identity, rather than the deleted original's identity.
  $entries=@($entries|Where-Object {$_.Path -ne 'Uninstall.exe'})
  $retryEntry=New-CleanupEntry $Root 'Uninstall.exe';if($retryEntry){$entries+=@($retryEntry)}
 }
 # Known occupied content is registered before any bootstrap is removed.
 # RuntimeSource also survives a new bootstrap lock discovered on the fast path.
 $registered=Register-CleanupJob $Root $CleanupNamespace 'uninstall' $entries $runtime
 if($script:PendingMappedImage){[void](Retire-CleanupRoot $registered.Directory $registered.Job)}
 if(Invoke-CleanupJob $registered.Directory $registered.Job){exit 0}
 Write-Output 'Cleanup registered for immediate, five-minute and logon retries; Windows restart is not required.'
 exit 2
}
# Native helper has already stopped owned processes. Continue independent cleanup on error.
$script:failures=[Collections.Generic.List[string]]::new()
$classesPath='Software\Classes';$runPath='Software\Microsoft\Windows\CurrentVersion\Run'
if($CleanupNamespace -ne 'PcTool'){$classesPath='Software\'+$CleanupNamespace+'\Classes';$runPath='Software\'+$CleanupNamespace+'\Run'}
try{& (Join-Path $PSScriptRoot 'lifecycle.ps1') -Action RemoveStartup -Root $Root -Namespace $CleanupNamespace;if($LASTEXITCODE){throw 'Startup cleanup failed'}}catch{$script:failures.Add($_.Exception.Message)}
try {
# Remove associations created through the bundled manager only while their
# command still points to this installation. Never remove another 7-zip's keys.
foreach($hive in @([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryHive]::LocalMachine)){
 foreach($view in @([Microsoft.Win32.RegistryView]::Registry64,[Microsoft.Win32.RegistryView]::Registry32)){
  $base=[Microsoft.Win32.RegistryKey]::OpenBaseKey($hive,$view)
  $classes=$base.OpenSubKey($classesPath,$false)
  if($null -eq $classes){$base.Dispose();continue}
  try{
   foreach($name in @($classes.GetSubKeyNames() | Where-Object {$_.StartsWith('7-Zip.',[StringComparison]::OrdinalIgnoreCase)})){
    $command=$classes.OpenSubKey($name+'\shell\open\command')
    if($null -eq $command){continue}
    $value=[string]$command.GetValue('');$command.Dispose()
    $expected='"'+$Root+'\modules\archive\7zFM.exe"'
    if(!$value.StartsWith($expected,[StringComparison]::OrdinalIgnoreCase)){continue}
    $writable=$base.OpenSubKey($classesPath,$true)
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
# Capture an exact identity-bound plan before deleting anything. Deferred workers
# never recursively traverse a directory supplied by a later installation.
$instance=Read-CleanupInstance $Root
$script:entries=[Collections.Generic.List[object]]::new()
$seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach($entry in @($instance.Entries)){
 $full=Resolve-Owned ([string]$entry.Path)
 if($seen.Add($full)){$script:entries.Add($entry)}
}
function Add-Owned([string]$relative,[bool]$recurse){
 $full=Resolve-Owned $relative
 if(!(Test-Path -LiteralPath $full) -or !$seen.Add($full)){return}
 $item=Get-Item -LiteralPath $full -Force
 if($recurse -and $item.PSIsContainer -and !($item.Attributes -band [IO.FileAttributes]::ReparsePoint)){
  foreach($child in @(Get-ChildItem -LiteralPath $full -Force)){Add-Owned $child.FullName.Substring($Root.Length+1) $true}
 }
 $entry=New-CleanupEntry $Root $relative;if($entry){$script:entries.Add($entry)}
}
try{
 Add-Owned 'Cache' $true;Add-Owned 'Data' $true
 if(Test-Path -LiteralPath $inventoryPath){
  $inventory=Read-Inventory $inventoryPath
  $bootstrap=@('manage-data.ps1','cleanup-jobs.ps1','retry-cleanup.ps1','shell-integration.ps1','lifecycle.ps1','PcToolUninstallHelper.exe','Uninstall.exe','.pctool-package-files.json','.pctool-managed-files.json','.pctool-uninstalling','.pctool-install-instance.json')
  foreach($relative in $inventory.Files){if($relative -notin $bootstrap){
   $file=Resolve-Owned $relative
   if((Test-Path -LiteralPath $file) -and (Get-Item -LiteralPath $file -Force).PSIsContainer){throw "Expected managed file: $relative"}
   Add-Owned $relative $false
  }}
  foreach($relative in $inventory.Directories){Add-Owned $relative $false}
 }
 $archive=Resolve-Owned 'modules/archive'
 if((Test-Path -LiteralPath $archive) -and !((Get-Item -LiteralPath $archive -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)){
  foreach($file in @(Get-ChildItem -LiteralPath $archive -File -Force|Where-Object {$_.Name -match '^7-zip(32)?\.dll\.\d+\.old$'})){Add-Owned $file.FullName.Substring($Root.Length+1) $false}
 }
 $instance.Entries=@($script:entries.ToArray())
 Write-CleanupJson (Join-Path $Root '.pctool-install-instance.json') $instance
}catch{$script:failures.Add($_.Exception.Message)}
if($failures.Count){$failures|ForEach-Object {Write-Output "Cleanup failed: $_"};exit 1}
$pending=$false
foreach($entry in @($instance.Entries|Where-Object {!$_.Directory})){
 if(!(Remove-CleanupEntry $Root $entry)){$pending=$true}
}
foreach($entry in @($instance.Entries|Where-Object {$_.Directory}|Sort-Object {$_.Path.Length} -Descending)){
 if(!(Remove-CleanupEntry $Root $entry)){$pending=$true}
}
# Finalize follows menu/shortcut removal, registers reliable retries if needed,
# and only then removes bootstrap tools. Keep the uninstall guard until then.
if($pending){exit 2}
exit 0
