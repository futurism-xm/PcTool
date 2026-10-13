param()
$ErrorActionPreference='Stop'
$project=Split-Path -Parent $PSScriptRoot
$namespace='PcToolTest-'+[guid]::NewGuid().ToString('N')
$prefix='Software\'+$namespace
$root=Join-Path $project ('out/shell-registration-tests/'+$namespace)
New-Item -ItemType Directory -Path $root -Force | Out-Null
$clsid='{23170F69-40C1-278A-1000-000100020000}'
$bases=@();$users=@()
function Read-Value($base,$path,$name=''){
 $key=$base.OpenSubKey($path)
 if(!$key){return $null}
 try{return $key.GetValue($name,$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)}finally{$key.Dispose()}
}
function Write-Value($base,$path,$name,$value){
 $key=$base.CreateSubKey($path);try{$key.SetValue($name,$value)}finally{$key.Dispose()}
}
function Run-Integration($action,[int]$failure=0){
 & "$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$project/packaging/shell-integration.ps1" -Action $action -Root $root -Namespace $namespace -TestFailAfter $failure *> "$root/$action-$failure.log"
 if(($LASTEXITCODE -eq 0) -ne ($failure -eq 0)){throw "Unexpected $action result; see $root/$action-$failure.log"}
}
function Check([bool]$condition,[string]$message){if(!$condition){throw $message}}
function Live-Snapshot {
 $values=@()
 foreach($base in $bases){
  foreach($pair in @(@("Software\Classes\CLSID\$clsid\InprocServer32",''),@('Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved',$clsid),@('Software\7-Zip','Path'),@('Software\PcToolInstaller','InstallDir'))){
   $values+=,@($pair[0],$pair[1],(Read-Value $base $pair[0] $pair[1]))
  }
 }
 return ConvertTo-Json -InputObject $values -Compress -Depth 5
}
try{
 foreach($view in @([Microsoft.Win32.RegistryView]::Registry64,[Microsoft.Win32.RegistryView]::Registry32)){
  $bases+=,[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,$view)
  $users+=,[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,$view)
 }
 $before=Live-Snapshot
 foreach($failure in @(1,9,45)){
  Run-Integration Register $failure
  foreach($base in $bases){
   Check ($null -eq (Read-Value $base "$prefix\Classes\CLSID\$clsid\InprocServer32")) 'Rollback left an Inproc value'
   Check ($null -eq (Read-Value $base "$prefix\Installer\SevenZip\Class" 'Saved')) 'Rollback left a committed backup'
   $base.DeleteSubKeyTree($prefix,$false)
  }
 }
 'PASS transactional rollback at three write positions'
 foreach($prior in @($false,$true)){
  foreach($base in $bases){
   if($prior){
    Write-Value $base "$prefix\Classes\CLSID\$clsid\InprocServer32" '' 'C:\Independent7Zip\7-zip.dll'
    Write-Value $base "$prefix\Approved" $clsid 'Independent approved value'
    Write-Value $base "$prefix\SevenZip" 'Path' 'C:\Independent7Zip\'
   }
  }
  Run-Integration Register
  Run-Integration Register
  for($i=0;$i -lt 2;$i++){
   $base=$bases[$i];$dll=if($i -eq 0){'7-zip.dll'}else{'7-zip32.dll'}
   Check ((Read-Value $base "$prefix\Classes\CLSID\$clsid\InprocServer32") -eq "$root\modules\archive\$dll") 'Wrong shell module registration'
   Check ((Read-Value $base "$prefix\Approved" $clsid) -eq '7-Zip Shell Extension') 'Approved value missing'
   foreach($path in @('*\shellex\ContextMenuHandlers','Directory\shellex\ContextMenuHandlers','Folder\shellex\ContextMenuHandlers','Directory\shellex\DragDropHandlers','Drive\shellex\DragDropHandlers')){
    Check ((Read-Value $base "$prefix\Classes\$path\7-Zip") -eq $clsid) "Handler missing: $path"
   }
  }
  Run-Integration Unregister
  Run-Integration Unregister
  foreach($base in $bases){
   $expected=if($prior){'C:\Independent7Zip\7-zip.dll'}else{$null}
   Check ((Read-Value $base "$prefix\Classes\CLSID\$clsid\InprocServer32") -eq $expected) 'Previous provider was not restored'
   $expected=if($prior){'Independent approved value'}else{$null}
   Check ((Read-Value $base "$prefix\Approved" $clsid) -eq $expected) 'Previous Approved value was not restored'
   $base.DeleteSubKeyTree($prefix,$false)
  }
 }
 'PASS first install, upgrade, repeated unregister and independent 7-Zip restoration in both views'
 foreach($user in $users){Write-Value $user "$prefix\Classes\CLSID\$clsid\InprocServer32" '' 'C:\User7Zip\7-zip.dll'}
 Run-Integration Register
 foreach($user in $users){Check ((Read-Value $user "$prefix\Classes\CLSID\$clsid\InprocServer32") -eq 'C:\User7Zip\7-zip.dll') 'HKCU provider overwritten'}
 foreach($base in $bases){
  Write-Value $base "$prefix\Classes\CLSID\$clsid\InprocServer32" '' 'C:\NewProvider\7-zip.dll'
  Write-Value $base "$prefix\Approved" $clsid 'New provider'
 }
 Run-Integration Unregister
 foreach($base in $bases){
  Check ((Read-Value $base "$prefix\Classes\CLSID\$clsid\InprocServer32") -eq 'C:\NewProvider\7-zip.dll') 'Later provider overwritten'
  Check ((Read-Value $base "$prefix\Approved" $clsid) -eq 'New provider') 'Later provider Approved value overwritten'
 }
 Check ((Live-Snapshot) -ceq $before) 'Production registrations changed during isolated tests'
 'PASS HKCU override, later owner and production Approved/registration isolation'
 "Evidence: $root"
}finally{
 foreach($base in $bases+$users){$base.DeleteSubKeyTree($prefix,$false);$base.Dispose()}
}
