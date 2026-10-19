param([Parameter(Mandatory=$true)][string]$Binaries,[string]$Stage,[string]$Manifest,[switch]$WaitForAutomaticRetry)
$ErrorActionPreference='Stop'
$project=Split-Path -Parent $PSScriptRoot
$Binaries=[IO.Path]::GetFullPath($Binaries)
$testId=[guid]::NewGuid().ToString('N')
$base=Join-Path $project "out/uninstall-installer-tests/$testId"
New-Item -ItemType Directory -Path $base -Force | Out-Null
if(!$Stage){$Stage=(Get-ChildItem "$project/out/package-stage" -Directory|Sort-Object LastWriteTime -Descending|Select-Object -First 1).FullName}
if(!$Manifest){$Manifest=Join-Path $project ('out/package-manifest/'+(Split-Path -Leaf $Stage))}
$Stage=[IO.Path]::GetFullPath($Stage)
$Manifest=[IO.Path]::GetFullPath($Manifest)
$cleanupNamespace="PcToolTest-$testId";$namespace="Software\$cleanupNamespace"
. "$project/packaging/cleanup-jobs.ps1"
# All external registration, startup, shortcut and task destinations are isolated.
$script=[IO.File]::ReadAllText("$project/packaging/PcTool.nsi")
if($script -match 'release-shell|repair-shell|REBOOTOK|SetRebootFlag|SetErrorLevel 3010'){throw 'Installer must not restart Explorer or queue reboot deletion'}
$script=$script.Replace('Software\PcToolInstaller',"$namespace\Installer")
$script=$script.Replace('Software\Microsoft\Windows\CurrentVersion\Uninstall\PcTool',"$namespace\Uninstall")
$script=$script.Replace('$SMPROGRAMS\PcTool','$INSTDIR\test-shortcuts').Replace('$DESKTOP\PcTool.lnk','$INSTDIR\test-desktop.lnk')
$script=$script.Replace('..\Resources\PcTool.ico',"$project\Resources\PcTool.ico")
$script=$script.Replace('MUI_PAGE_LICENSE "license.txt"',('MUI_PAGE_LICENSE "'+$project+'\packaging\license.txt"'))
foreach($name in @('manage-data.ps1','cleanup-jobs.ps1','retry-cleanup.ps1','shell-integration.ps1','lifecycle.ps1')){$script=$script.Replace(('"'+$name+'"'),('"'+$project+'\packaging\'+$name+'"'))}
[IO.File]::WriteAllText("$base/test.nsi",$script,[Text.UTF8Encoding]::new($true))
$compiler="$project/out/nsis/nsis-3.12/makensis.exe"
if(!(Test-Path -LiteralPath $compiler)){$compiler=(Get-ChildItem "$project/out/package-tools" -Filter makensis.exe -Recurse|Select-Object -First 1).FullName}
& $compiler /V2 "/DOUTPUT=$base/Setup.exe" "/DMANIFEST=$Manifest" "/DSTAGE=$Stage" "/DPCTOOL_CLEANUP_NAMESPACE=$cleanupNamespace" "$base/test.nsi" *> "$base/compile.log"
if($LASTEXITCODE){throw "Isolated NSIS compilation failed: $base/compile.log"}
$clsid='{23170F69-40C1-278A-1000-000100020000}'
$keys=@();$processes=@();$hosts=@();$destinations=@()
$startupService=New-Object -ComObject 'Schedule.Service';$startupService.Connect();$startupFolder=$null
$startupSid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
function Startup-Folder {
 if(!$script:startupFolder){
  try{$parent=$startupService.GetFolder('\'+$cleanupNamespace)}catch{
   if($_.Exception.GetBaseException().HResult -notin @(-2147024894,-2147024893)){throw}
   $parent=$startupService.GetFolder('\').CreateFolder($cleanupNamespace,'O:BAG:SYD:P(A;;GA;;;SY)(A;;GA;;;BA)')
  }
  try{$script:startupFolder=$parent.GetFolder('Startup')}catch{
   if($_.Exception.GetBaseException().HResult -notin @(-2147024894,-2147024893)){throw}
   $script:startupFolder=$parent.CreateFolder('Startup','O:BAG:SYD:P(A;;GA;;;SY)(A;;GA;;;BA)')
  }
 }
 return $script:startupFolder
}
function Register-StartupFixture([string]$destination){
 $folder=Startup-Folder;$definition=$startupService.NewTask(0)
 $definition.Principal.UserId=$startupSid;$definition.Principal.LogonType=3;$definition.Principal.RunLevel=1
 # This action is only an uninstall ownership fixture, never an executable task.
 $definition.Settings.Enabled=$false
 $definition.RegistrationInfo.Description='PcTool Startup v1|'+$startupSid+'|'+$destination+'\PcTool.exe'
 $entry=$definition.Actions.Create(0);$entry.Path=Join-Path $destination 'PcTool.exe';$entry.Arguments='--background'
 [void]$folder.RegisterTaskDefinition(($startupSid+'-'+(Get-CleanupRootHash $destination)),$definition,22,$startupSid,$null,3,'O:BAG:SYD:P(A;;GA;;;SY)(A;;GA;;;BA)')
}
function Check([bool]$condition,[string]$message){if(!$condition){throw $message}}
function Read-Value($registry,$path,$name=''){
 $key=$registry.OpenSubKey($path);if(!$key){return $null};try{return $key.GetValue($name)}finally{$key.Dispose()}
}
function Production-Snapshot {
 $values=@();foreach($registry in $keys){
  $values+=Read-Value $registry "Software\Classes\CLSID\$clsid\InprocServer32"
  $values+=Read-Value $registry 'Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved' $clsid
  $values+=Read-Value $registry 'Software\PcToolInstaller' 'InstallDir'
 }
 $userRun=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Microsoft\Windows\CurrentVersion\Run')
 if($userRun){try{$values+=$userRun.GetValue('PcTool')}finally{$userRun.Dispose()}}
 try{$productionStartup=$startupService.GetFolder('\PcTool\Startup');$values+=@($productionStartup.GetTasks(1)|ForEach-Object {$_.Xml})}catch{if($_.Exception.GetBaseException().HResult -notin @(-2147024894,-2147024893)){throw}}
 return ConvertTo-Json -InputObject $values -Compress
}
function Queue-Snapshot {
 $key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Control\Session Manager')
 try{return @($key.GetValue('PendingFileRenameOperations',[string[]]@())) -join "`0"}finally{$key.Dispose()}
}
function Install-App($destination){
 # The installer does not write the elevation account's startup preference;
 # the first ordinary product launch initializes the interactive user's Run.
 $setup=Start-Process -FilePath "$base/Setup.exe" -ArgumentList @('/S',"/D=$destination") -WindowStyle Hidden -PassThru -Wait
 Check ($setup.ExitCode -eq 0) "Install failed: $($setup.ExitCode)"
 Check (!(Test-Path (Join-Path $destination 'Data/settings.ini'))) 'Installer created legacy flat settings'
 Check (Test-Path "$destination/.pctool-managed-files.json") 'Installed inventory missing'
 foreach($dll in @('7-zip.dll','7-zip32.dll')){Check ((Get-FileHash "$destination/modules/archive/$dll").Hash -eq (Get-FileHash "$Stage/modules/archive/$dll").Hash) 'Installed extension hash mismatch'}
 foreach($binary in @('PcTool.exe','PcToolUninstallHelper.exe')){Check ((Get-FileHash (Join-Path $destination $binary)).Hash -eq (Get-FileHash (Join-Path $Stage $binary)).Hash) 'Installed executable hash mismatch'}
 Check (!(Test-Path "$destination/.pctool-uninstalling")) 'Installation retained uninstall guard'
 Register-StartupFixture $destination
}
function Uninstall-App($destination,$label){
 $runner=Join-Path $base "Uninstall-$label.exe"
 if(Test-Path "$destination/Uninstall.exe"){Copy-Item -LiteralPath "$destination/Uninstall.exe" -Destination $runner -Force}
 $elapsed=[Diagnostics.Stopwatch]::StartNew()
 $uninstall=Start-Process -FilePath $runner -ArgumentList @('/S',"_?=$destination") -WindowStyle Hidden -PassThru -Wait
 Check ($uninstall.ExitCode -eq 0) "Uninstall failed: $($uninstall.ExitCode)"
 $startupName=$startupSid+'-'+(Get-CleanupRootHash $destination)
 $script:startupFolder=$null
 Check (@((Startup-Folder).GetTasks(1)|Where-Object {$_.Name -eq $startupName}).Count -eq 0) 'Uninstall left its administrator startup task'
 foreach($registry in $keys){Check ($null -eq (Read-Value $registry "$namespace\Uninstall" 'DisplayName')) 'Uninstall registration remains'}
 Write-Host "Uninstall $label returned 0 in $([math]::Round($elapsed.Elapsed.TotalSeconds,2)) s"
}
function Start-Host($destination,$label,$arch){
 $prefix=Join-Path $base "$label-$arch"
 $exe=Join-Path $project "out/installer-menu-20261013/$arch/PcToolArchiveShellTests.exe"
 $args=@(('"'+$destination+'/modules/archive"'),'--lifecycle-host',('"'+$prefix+'"'))
 if($arch -eq 'x86'){$args+='7-zip32.dll'}
 $process=Start-Process -FilePath $exe -ArgumentList $args -WindowStyle Hidden -PassThru
 $script:processes+=,$process
 for($i=0;$i -lt 100 -and !(Test-Path "$prefix.ready") -and !$process.HasExited;$i++){Start-Sleep -Milliseconds 100}
 Check (Test-Path "$prefix.ready") "Lifecycle host did not become ready: $prefix"
 $hostState=[pscustomobject]@{Process=$process;Prefix=$prefix}
 $script:hosts+=,$hostState;return $hostState
}
function Check-Host($hostState){
 Check (!$hostState.Process.HasExited) 'Uninstaller terminated an unrelated DLL host'
 [IO.File]::WriteAllText(($hostState.Prefix+'.check'),'check')
 for($i=0;$i -lt 100 -and !(Test-Path ($hostState.Prefix+'.checked')) -and !$hostState.Process.HasExited;$i++){Start-Sleep -Milliseconds 100}
 Check (Test-Path ($hostState.Prefix+'.checked')) "Cached COM object still active: $($hostState.Prefix)"
}
function Release-Host($hostState){
 [IO.File]::WriteAllText(($hostState.Prefix+'.release'),'release')
 Check ($hostState.Process.WaitForExit(10000)) 'Host did not release DLL'
 Check ($hostState.Process.ExitCode -eq 0) "Lifecycle host failed: $($hostState.Process.ExitCode)"
}
function Wait-Cleanup($destination,[bool]$automatic){
 $store=Get-CleanupStore $cleanupNamespace
 $watch=[Diagnostics.Stopwatch]::StartNew()
 for($i=0;$i -lt 360;$i++){
  $jobs=@();if(Test-Path $store){$jobs=@(Get-ChildItem -LiteralPath $store -Filter job.json -Recurse|ForEach-Object {Get-Content -LiteralPath $_.FullName -Raw|ConvertFrom-Json}|Where-Object {$_.Root -eq $destination})}
  if(!$jobs.Count){return $watch.Elapsed.TotalSeconds}
  if(!$automatic -and $i % 10 -eq 0){$scheduler=Open-CleanupScheduler $cleanupNamespace;foreach($job in $jobs){try{[void]$scheduler.Folder.GetTask($job.TaskName).Run($null)}catch{}}}
  Start-Sleep -Seconds 1
 }
 throw 'Scheduled cleanup did not finish within six minutes'
}
function Wait-FirstDeferredAttempt($destination){
 # Ensure the immediate SYSTEM attempt has observed the occupied DLL. Only
 # then release it, so completion must come from the real five-minute trigger.
 $store=Get-CleanupStore $cleanupNamespace
 for($i=0;$i -lt 40;$i++){
  $jobs=@(Get-ChildItem -LiteralPath $store -Filter job.json -Recurse|ForEach-Object {Get-Content -LiteralPath $_.FullName -Raw|ConvertFrom-Json}|Where-Object {$_.Root -eq $destination})
  if($jobs.Count -eq 1){
   $scheduler=Open-CleanupScheduler $cleanupNamespace
   $task=$scheduler.Folder.GetTask($jobs[0].TaskName)
   if($task.State -ne 4 -and $task.LastTaskResult -eq 2){
    Check ($task.Xml -match '<Interval>PT5M</Interval>' -and $task.Xml -match '<LogonTrigger') 'Retry triggers missing'
    Write-Host 'Immediate SYSTEM attempt returned pending; waiting for the automatic five-minute retry after release.'
    return
   }
  }
  Start-Sleep -Seconds 1
 }
 throw 'Immediate SYSTEM task did not observe the occupied DLL'
}
try{
 foreach($view in @([Microsoft.Win32.RegistryView]::Registry64,[Microsoft.Win32.RegistryView]::Registry32)){$keys+=,[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,$view)}
 $before=Production-Snapshot;$queueBefore=Queue-Snapshot
 foreach($previous in @($false,$true)){
  $label=if($previous){'previous-7zip'}else{'no-previous'}
  $destination=[IO.Path]::GetFullPath((Join-Path $base ($label+' app')));$destinations+=,$destination
  foreach($registry in $keys){if($previous){$key=$registry.CreateSubKey("$namespace\Classes\CLSID\$clsid\InprocServer32");$key.SetValue('','C:\Independent7Zip\7-zip.dll');$key.Dispose()}}
  Install-App $destination
  & "$Binaries/PcToolArchiveShellTests.exe" "$destination/modules/archive"
  Check ($LASTEXITCODE -eq 0) 'Installed menu generation failed'
  $held64=Start-Host $destination "$label-upgrade" 'x64'
  Install-App $destination
  Check (@(Get-ChildItem "$destination/modules/archive" -Filter '*.old').Count -gt 0) 'Occupied upgrade DLL was not retired'
  Release-Host $held64
  [void](Wait-Cleanup $destination $false)
  [IO.File]::WriteAllText("$destination/keep.txt",'unrelated file')
  $host64=Start-Host $destination "$label-uninstall" 'x64'
  $host32=Start-Host $destination "$label-uninstall" 'x86'
  Uninstall-App $destination $label
  Check-Host $host64;Check-Host $host32
  Check (Test-Path "$destination/.pctool-uninstalling") 'Uninstall guard removed while DLLs remain'
  foreach($registry in $keys){
   $expected=if($previous){'C:\Independent7Zip\7-zip.dll'}else{$null}
   Check ((Read-Value $registry "$namespace\Classes\CLSID\$clsid\InprocServer32") -eq $expected) 'Previous provider not restored'
  }
  if(!$previous){
   # Reinstall before the old mapping is released; old cleanup must not remove new files.
   Install-App $destination
   $hash=(Get-FileHash "$destination/modules/archive/7-zip.dll").Hash
   Release-Host $host64;Release-Host $host32
   [void](Wait-Cleanup $destination $false)
   Check ((Get-FileHash "$destination/modules/archive/7-zip.dll").Hash -eq $hash) 'Old cleanup deleted reinstalled DLL'
   Check (Test-Path "$destination/PcTool.exe") 'Old cleanup deleted reinstalled application'
   $host64=Start-Host $destination "$label-second" 'x64'
   $host32=Start-Host $destination "$label-second" 'x86'
   Uninstall-App $destination $label
   Check-Host $host64;Check-Host $host32
  }
  # Repeat with only instance/guard/occupied DLLs left, using the copied uninstaller.
  Uninstall-App $destination $label
  if($previous -and $WaitForAutomaticRetry){Wait-FirstDeferredAttempt $destination}
  Release-Host $host64;Release-Host $host32
  $seconds=Wait-Cleanup $destination ($previous -and $WaitForAutomaticRetry)
  foreach($path in @('PcTool.exe','Uninstall.exe','modules','Data','Cache','.pctool-uninstalling','.pctool-install-instance.json','.pctool-managed-files.json','test-shortcuts','test-desktop.lnk')){Check (!(Test-Path (Join-Path $destination $path))) "Residual after cleanup: $path"}
  Check (Test-Path "$destination/keep.txt") 'Unrelated file removed'
  Uninstall-App $destination $label
  Check (!(Test-Path "$destination/.pctool-uninstalling")) 'Repeated uninstall recreated guard'
  foreach($registry in $keys){$registry.DeleteSubKeyTree($namespace,$false)}
  "PASS isolated install, mapped-DLL upgrade, cached x64/x86 COM uninstall, repeat uninstall, scheduled cleanup and unrelated file preservation ($label); cleanup wait $([math]::Round($seconds,2)) s"
 }
 # No unrelated user files: a real mapped x64/x86 image must no longer leave
 # the installation directory behind, even while both external hosts survive.
 $destination=[IO.Path]::GetFullPath((Join-Path $base 'retired-root app 中文'));$destinations+=,$destination
 Install-App $destination
 $mapped64=Start-Host $destination 'retired-root' 'x64'
 $mapped32=Start-Host $destination 'retired-root' 'x86'
 Uninstall-App $destination 'retired-root'
 Check (!(Test-Path -LiteralPath $destination)) 'Mapped DLL left original installation directory behind'
 Check-Host $mapped64;Check-Host $mapped32
 $jobs=@(Get-ChildItem -LiteralPath (Get-CleanupStore $cleanupNamespace) -Filter job.json -Recurse|ForEach-Object {Get-Content -LiteralPath $_.FullName -Raw|ConvertFrom-Json}|Where-Object {$_.Root -eq $destination})
 Check ($jobs.Count -eq 1) 'Retired cleanup record missing'
 $retired=Get-JobPhysicalRoot $jobs[0]
 Check ($retired -ne $destination -and (Test-Path "$retired/.pctool-uninstalling")) 'Retired uninstall state missing'
 Install-App $destination
 Release-Host $mapped64;Release-Host $mapped32
 [void](Wait-Cleanup $destination $false)
 Check (!(Test-Path $retired) -and (Test-Path "$destination/PcTool.exe")) 'Retired cleanup left files or damaged reinstall'
 Uninstall-App $destination 'retired-root-new'
 Check (!(Test-Path -LiteralPath $destination)) 'Reinstalled application did not uninstall cleanly'
 'PASS mapped x64/x86 DLLs retire out of original directory; hosts survive; detached cleanup preserves reinstall'
 $destination=[IO.Path]::GetFullPath((Join-Path $base 'clean-root app 中文'));$destinations+=,$destination
 Install-App $destination
 Uninstall-App $destination 'clean-root'
 [void](Wait-Cleanup $destination $false)
 Check (!(Test-Path -LiteralPath $destination)) 'Empty installation root survived normal uninstall'
 'PASS unlocked uninstall removes every generated file and the installation root'
 Check ((Production-Snapshot) -ceq $before) 'Production integration changed'
 Check ((Queue-Snapshot) -ceq $queueBefore) 'Installer wrote a reboot-delete operation'
 'PASS production registry isolation and no new reboot-delete operations'
 "Evidence: $base"
}catch{
 $_ | Format-List * -Force | Out-File (Join-Path $base 'failure.log')
 throw
}finally{
 foreach($hostState in $hosts){if(!$hostState.Process.HasExited){[IO.File]::WriteAllText(($hostState.Prefix+'.release'),'release');[void]$hostState.Process.WaitForExit(5000)}}
 foreach($process in $processes){if(!$process.HasExited){$process.Kill();$process.WaitForExit()};$process.Dispose()}
 foreach($destination in $destinations){Stop-CleanupJobs $destination $cleanupNamespace}
 foreach($registry in $keys){$registry.DeleteSubKeyTree($namespace,$false);$registry.Dispose()}
 $store=Get-CleanupStore $cleanupNamespace
 if(Test-Path $store){Assert-CleanupSecurity $store;if(!(Get-ChildItem -LiteralPath $store -Force)){[IO.Directory]::Delete($store,$false);[IO.Directory]::Delete((Split-Path -Parent $store),$false)}}
 # Resolve the folder again: install migration can invalidate cached COM handles.
 $startupCleanup=$null
 try{$startupCleanup=$startupService.GetFolder('\'+$cleanupNamespace+'\Startup')}catch{if($_.Exception.GetBaseException().HResult -notin @(-2147024894,-2147024893)){throw}}
 if($startupCleanup){foreach($task in @($startupCleanup.GetTasks(1))){$startupCleanup.DeleteTask($task.Name,0)};$startupService.GetFolder('\'+$cleanupNamespace).DeleteFolder('Startup',0)}
 try{$startupService.GetFolder('\').DeleteFolder($cleanupNamespace,0)}catch{}
}
