param(
 [Parameter(Mandatory=$true)][ValidateSet('Register','Unregister')][string]$Action,
 [Parameter(Mandatory=$true)][string]$Root,
 [string]$Namespace='PcTool',
 [int]$TestFailAfter=0
)
$ErrorActionPreference='Stop'
if($Namespace -ne 'PcTool' -and $Namespace -notmatch '^PcToolTest-[a-f0-9]{32}$'){throw 'Invalid integration namespace'}
if($TestFailAfter -and $Namespace -eq 'PcTool'){throw 'Failure injection is restricted to isolated tests'}
$Root=[IO.Path]::GetFullPath($Root).TrimEnd('\')
if($Root -eq [IO.Path]::GetPathRoot($Root).TrimEnd('\')){throw 'Invalid installation root'}
$test=$Namespace -ne 'PcTool'
$prefix='Software\'+$Namespace
$classes=if($test){$prefix+'\Classes'}else{'Software\Classes'}
$installer=if($test){$prefix+'\Installer'}else{'Software\PcToolInstaller'}
$approved=if($test){$prefix+'\Approved'}else{'Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved'}
$sevenZip=if($test){$prefix+'\SevenZip'}else{'Software\7-Zip'}
$appPath=if($test){$prefix+'\AppPath'}else{'Software\Microsoft\Windows\CurrentVersion\App Paths\7zFM.exe'}
$clsid='{23170F69-40C1-278A-1000-000100020000}'
$archive=$Root+'\modules\archive\'
$changes=[Collections.Generic.List[object]]::new()
$bases=@{}
$writes=0
function Read-Value($Base,[string]$Path,[string]$Name){
 $key=$Base.OpenSubKey($Path,$false)
 if(!$key){return @{Key=$false;Exists=$false}}
 try{
  if($key.GetValueNames() -notcontains $Name){return @{Key=$true;Exists=$false}}
  return @{Key=$true;Exists=$true;Kind=$key.GetValueKind($Name);Data=$key.GetValue($Name,$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)}
 }finally{$key.Dispose()}
}
function Same-Data($A,$B){return (ConvertTo-Json -InputObject $A -Compress) -ceq (ConvertTo-Json -InputObject $B -Compress)}
function Write-Value($Base,[string]$Path,[string]$Name,$Value){
 if($Value.Exists){
  $key=$Base.CreateSubKey($Path,$true)
  try{$key.SetValue($Name,$Value.Data,$Value.Kind)}finally{$key.Dispose()}
 }else{
  $key=$Base.OpenSubKey($Path,$true)
  if($key){
   try{$key.DeleteValue($Name,$false);$empty=$key.ValueCount -eq 0 -and $key.SubKeyCount -eq 0}finally{$key.Dispose()}
   if($empty){$Base.DeleteSubKey($Path,$false)}
  }
 }
 $actual=Read-Value $Base $Path $Name
 if($actual.Exists -ne $Value.Exists -or ($Value.Exists -and ($actual.Kind -ne $Value.Kind -or !(Same-Data $actual.Data $Value.Data)))){
  throw "Registry readback failed: $Path [$Name]"
 }
}
function Change-Value($Base,[string]$Path,[string]$Name,$Value){
 $old=Read-Value $Base $Path $Name
 $changes.Add(@{Base=$Base;Path=$Path;Name=$Name;Old=$old})
 Write-Value $Base $Path $Name $Value
 $script:writes++
 if($TestFailAfter -gt 0 -and $script:writes -eq $TestFailAfter){throw 'Injected isolated registration failure'}
}
function String-Value([string]$Data){return @{Exists=$true;Data=$Data;Kind=[Microsoft.Win32.RegistryValueKind]::String}}
function Entries([int]$View){
 $dll=if($View -eq 64){'7-zip.dll'}else{'7-zip32.dll'}
 $class=$classes+'\CLSID\'+$clsid
 @(
  @{Id='Class';Path=$class;Name='';Data='7-Zip Shell Extension';Shell=$true},
  @{Id='Thread';Path=$class+'\InprocServer32';Name='ThreadingModel';Data='Apartment';Shell=$true},
  @{Id='Inproc';Path=$class+'\InprocServer32';Name='';Data=$archive+$dll;Shell=$true},
  @{Id='Approved';Path=$approved;Name=$clsid;Data='7-Zip Shell Extension';Shell=$true},
  @{Id='Files';Path=$classes+'\*\shellex\ContextMenuHandlers\7-Zip';Name='';Data=$clsid;Shell=$true},
  @{Id='Directory';Path=$classes+'\Directory\shellex\ContextMenuHandlers\7-Zip';Name='';Data=$clsid;Shell=$true},
  @{Id='Folder';Path=$classes+'\Folder\shellex\ContextMenuHandlers\7-Zip';Name='';Data=$clsid;Shell=$true},
  @{Id='DirectoryDrag';Path=$classes+'\Directory\shellex\DragDropHandlers\7-Zip';Name='';Data=$clsid;Shell=$true},
  @{Id='DriveDrag';Path=$classes+'\Drive\shellex\DragDropHandlers\7-Zip';Name='';Data=$clsid;Shell=$true},
  @{Id='Path';Path=$sevenZip;Name='Path';Data=$archive},
  @{Id='Path64';Path=$sevenZip;Name='Path64';Data=$archive}
 )
 if($View -eq 64){
  @{Id='AppPath';Path=$appPath;Name='';Data=$archive+'7zFM.exe'}
  @{Id='AppDirectory';Path=$appPath;Name='Path';Data=$archive}
 }
}
try{
 foreach($view in @(64,32)){
  $registryView=if($view -eq 64){[Microsoft.Win32.RegistryView]::Registry64}else{[Microsoft.Win32.RegistryView]::Registry32}
  $base=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,$registryView)
  $bases[$view]=$base
  $entries=@(Entries $view)
  $inproc=$entries|Where-Object Id -eq 'Inproc'
  $owner=Read-Value $base $inproc.Path ''
  $ownsShell=$owner.Exists -and [string]::Equals([string]$owner.Data,$inproc.Data,[StringComparison]::OrdinalIgnoreCase)
  foreach($entry in $entries){
   $backup=$installer+'\SevenZip\'+$entry.Id
   if($Action -eq 'Register'){
    $saved=Read-Value $base $backup 'Saved'
    if(!$saved.Exists -or $saved.Data -ne '1'){
     $prior=Read-Value $base $entry.Path $entry.Name
     Change-Value $base $backup 'Exists' (String-Value $(if($prior.Exists){'1'}else{'0'}))
     if($prior.Exists){Change-Value $base $backup 'Data' $prior}
     Change-Value $base $backup 'Saved' (String-Value '1')
    }
    Change-Value $base $entry.Path $entry.Name (String-Value $entry.Data)
   }else{
    if($entry.Shell -and !$ownsShell){continue}
    $actual=Read-Value $base $entry.Path $entry.Name
    if(!$actual.Exists -or ![string]::Equals([string]$actual.Data,$entry.Data,[StringComparison]::OrdinalIgnoreCase)){continue}
    $exists=Read-Value $base $backup 'Exists'
    if($exists.Exists -and $exists.Data -eq '1'){
     $prior=Read-Value $base $backup 'Data'
     if(!$prior.Exists){throw "Missing integration recovery data: $backup"}
    }else{$prior=@{Exists=$false}}
    Change-Value $base $entry.Path $entry.Name $prior
   }
  }
  if($Action -eq 'Unregister'){
   $remaining=Read-Value $base $inproc.Path ''
   if($remaining.Exists -and [string]::Equals([string]$remaining.Data,$inproc.Data,[StringComparison]::OrdinalIgnoreCase)){
    throw "The $view-bit shell registration still belongs to this installation"
   }
  }else{
   # HKCU overrides HKLM in HKCR. Preserve a separately installed provider;
   # report the effective choice instead of mistaking HKLM for the merged view.
   $user=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,$registryView)
   try{$override=Read-Value $user $inproc.Path ''}finally{$user.Dispose()}
   if($override.Exists -and ![string]::Equals([string]$override.Data,$inproc.Data,[StringComparison]::OrdinalIgnoreCase)){
    Write-Output "Preserved the existing user-level $view-bit 7-Zip provider; it takes precedence over this machine registration."
   }
  }
 }
 Write-Output "7-Zip $Action succeeded; both registry views verified."
}catch{
 $reason=$_.Exception.Message
 $rollbackErrors=[Collections.Generic.List[string]]::new()
 for($i=$changes.Count-1;$i -ge 0;$i--){
  $change=$changes[$i]
  try{Write-Value $change.Base $change.Path $change.Name $change.Old}catch{$rollbackErrors.Add($_.Exception.Message)}
 }
 Write-Output "7-Zip $Action failed: $reason"
 foreach($errorText in $rollbackErrors){Write-Output "Registry rollback failed: $errorText"}
 exit 1
}finally{foreach($base in $bases.Values){$base.Dispose()}}
exit 0
