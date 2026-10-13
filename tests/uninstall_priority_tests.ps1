param([Parameter(Mandatory=$true)][string]$Binaries)
$ErrorActionPreference='Stop'
$project=Split-Path -Parent $PSScriptRoot
. (Join-Path $project 'packaging/cleanup-jobs.ps1')
$namespace='PcToolTest-'+[guid]::NewGuid().ToString('N')
$Binaries=[IO.Path]::GetFullPath($Binaries)
$root=Join-Path $project ('out/uninstall-priority-tests/'+$namespace)
$owned=Join-Path $root 'installed'
$other=Join-Path $root 'other'
$inputs=Join-Path $root 'inputs'
New-Item -ItemType Directory -Path "$owned/modules/archive",$other,$inputs -Force | Out-Null
# Build directories can be cleaned by a concurrent package build. Use this
# run's immutable copy for every process fixture and startup guard check.
foreach($name in @('PcTool.exe','PcToolUninstallHelper.exe','PcToolUninstallFixture.exe')){Copy-Item -LiteralPath (Join-Path $Binaries $name) -Destination $inputs}
foreach($name in @('7z.exe','7zG.exe','7zFM.exe')){Copy-Item -LiteralPath (Join-Path $project ('out/archive-runtime/'+$name)) -Destination $inputs}
$fixture=Join-Path $inputs 'PcToolUninstallFixture.exe'
Copy-Item $fixture "$owned/PcTool.exe"
Copy-Item $fixture "$owned/modules/archive/7z.exe"
Copy-Item $fixture "$other/PcTool.exe"
$processes=[Collections.Generic.List[object]]::new()
function Launch([string]$file,[string]$data,[string]$mode){
 $process=Start-Process -FilePath $file -ArgumentList @(('"'+$data+'"'),$mode) -WindowStyle Hidden -PassThru
 $processes.Add($process)
 for($i=0;$i -lt 60 -and !(Test-Path "$data/$($process.Id).ready");$i++){Start-Sleep -Milliseconds 50}
 if(!(Test-Path "$data/$($process.Id).ready")){throw 'Fixture did not become ready'}
 return $process
}
$shell="$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe"
function Manage([string]$action,[string]$directory,[string]$manifest,[int]$expected=0){
 $arguments=@('-NoProfile','-ExecutionPolicy','Bypass','-File',"$project/packaging/manage-data.ps1",'-Action',$action,'-Root',$directory,'-CleanupNamespace',$namespace)
 if($manifest){$arguments+=@('-Manifest',$manifest)}
 $preference=$ErrorActionPreference;$ErrorActionPreference='Continue'
 try{& $shell @arguments *> "$root/manage-$action.log";$code=$LASTEXITCODE}finally{$ErrorActionPreference=$preference}
 if($code -ne $expected){throw "Manage $action returned $code, expected $expected; see $root/manage-$action.log"}
}
$mutex=[Threading.Mutex]::new($false,(Get-CleanupLockName $owned $namespace))
$locked=$false
try {
 $locked=$mutex.WaitOne(30000)
 if(!$locked){throw 'Could not acquire isolated installation mutex'}
 # Record establishes the installation instance before any uninstall phase.
 @{Version=1;Files=@();Directories=@()}|ConvertTo-Json|Set-Content "$root/initial.json" -Encoding UTF8
 Manage 'Record' $owned "$root/initial.json"
 $polite=Launch "$owned/PcTool.exe" $owned 'polite'
 $hung=Launch "$owned/modules/archive/7z.exe" $owned 'stubborn'
 $duplicate=Launch "$owned/PcTool.exe" $owned 'stubborn'
 $unrelated=Launch "$other/PcTool.exe" $other 'stubborn'
 foreach($process in @($polite,$hung,$duplicate,$unrelated)){if($process.HasExited){throw "Fixture exited before uninstall: $($process.Id) code $($process.ExitCode)"}}
 $timer=[Diagnostics.Stopwatch]::StartNew()
 & "$inputs/PcToolUninstallHelper.exe" stop $owned *> "$root/stop.log"
 if($LASTEXITCODE){throw "Stop failed: $LASTEXITCODE"}
 if($timer.Elapsed.TotalSeconds -gt 8){throw 'Stop exceeded shared timeout'}
 foreach($process in @($polite,$hung,$duplicate)){if(!$process.WaitForExit(1000)){throw 'Owned process survived'}}
 if($polite.ExitCode -ne 0 -or $hung.ExitCode -ne 1602 -or $duplicate.ExitCode -ne 1602){throw "Graceful/forced exit evidence mismatch: $($polite.ExitCode), $($hung.ExitCode), $($duplicate.ExitCode)"}
 if($unrelated.HasExited){throw 'Other installation was terminated'}
 if(!(Test-Path "$owned/.pctool-uninstalling")){throw 'Uninstall marker missing'}
 "PASS graceful + forced multi-process termination in $([math]::Round($timer.Elapsed.TotalSeconds,2))s; unrelated same-name process preserved"
 # No more live hive handles: data can now be removed without a reboot.
 Manage 'Uninstall' $owned ''
 if(Test-Path "$owned/Data"){throw 'Hive survived process termination'}
 'PASS private hive released and removed immediately'

 Copy-Item "$inputs/PcTool.exe" "$owned/PcTool.exe" -Force
 foreach($name in @('7z.exe','7zG.exe','7zFM.exe')){Copy-Item "$inputs/$name" "$owned/modules/archive/$name" -Force}
 foreach($file in @('PcTool.exe','modules/archive/7z.exe','modules/archive/7zG.exe','modules/archive/7zFM.exe')){
  $process=Start-Process -FilePath (Join-Path $owned $file) -WindowStyle Hidden -PassThru
  if(!$process.WaitForExit(5000) -or $process.ExitCode -ne 1618){throw "Uninstall startup guard failed: $file"}
 }
 'PASS real PcTool and three 7-Zip executables reject startup during uninstall'

 # Upgrade retains removed payload names and legacy development documents.
 New-Item -ItemType Directory -Path "$owned/docs","$owned/tests" -Force | Out-Null
 foreach($file in @('old.dll','new.dll','docs/old.md','tests/old.md','keep.txt','docs/user.md')){[IO.File]::WriteAllText((Join-Path $owned $file),'fixture')}
 @{Version=1;Files=@('old.dll','docs/old.md','tests/old.md');Directories=@('docs','tests')}|ConvertTo-Json|Set-Content "$root/old.json"
 @{Version=1;Files=@('new.dll');Directories=@()}|ConvertTo-Json|Set-Content "$root/new.json"
 Manage 'Record' $owned "$root/old.json"
 Manage 'Record' $owned "$root/new.json"
 Manage 'Uninstall' $owned ''
 foreach($file in @('old.dll','new.dll','docs/old.md','tests/old.md')){if(Test-Path (Join-Path $owned $file)){throw "Managed upgrade file survived: $file"}}
 foreach($file in @('keep.txt','docs/user.md')){if(!(Test-Path (Join-Path $owned $file))){throw "Unrelated file removed: $file"}}
 if(Test-Path "$owned/tests"){throw 'Empty legacy directory survived'}
 'PASS merged upgrade inventory, legacy files, and unrelated file protection'

 @{Version=1;Files=@('../other/keep.txt');Directories=@()}|ConvertTo-Json|Set-Content "$root/bad.json"
 Manage 'Record' $owned "$root/bad.json" 1
 'PASS traversal manifest rejected'
 # Invalid inventory must fail before deleting captured data or replacing the
 # authoritative retry plan. The next valid attempt can remove that data.
 $inventory=Join-Path $owned '.pctool-managed-files.json'
 $valid=[IO.File]::ReadAllText($inventory)
 $instance=Join-Path $owned '.pctool-install-instance.json'
 $instanceBefore=[IO.File]::ReadAllText($instance)
 Copy-Item "$root/bad.json" $inventory -Force
 New-Item -ItemType Directory -Path "$owned/Cache" -Force | Out-Null
 [IO.File]::WriteAllText("$owned/Cache/discard.tmp",'cache')
 Manage 'Uninstall' $owned '' 1
 if(!(Test-Path "$owned/Cache/discard.tmp") -or !(Test-Path $inventory) -or !(Test-Path "$owned/.pctool-uninstalling") -or [IO.File]::ReadAllText($instance) -cne $instanceBefore){throw 'Invalid inventory changed data or authoritative retry state'}
 [IO.File]::WriteAllText($inventory,$valid)
 Remove-Item -LiteralPath "$owned/PcTool.exe" -Force
 Manage 'Uninstall' $owned ''
 if(Test-Path "$owned/Cache"){throw 'Corrected inventory retry left cache data'}
 'PASS invalid inventory preserves data/state; corrected retry succeeds without primary executable'
 Copy-Item "$inputs/PcTool.exe" "$owned/PcTool.exe"
 Manage 'Record' $owned "$root/new.json"
 Manage 'Install' $owned ''
 if(Test-Path "$owned/.pctool-uninstalling"){throw 'Reinstall failed to clear marker'}
 'PASS reinstall clears uninstall marker'
 "Evidence: $root; isolated namespace: $namespace"
} finally {
 foreach($process in $processes){if(!$process.HasExited){$process.Kill();$process.WaitForExit()};$process.Dispose()}
 try{
  Stop-CleanupJobs $owned $namespace
  $scheduler=Open-CleanupScheduler $namespace
  if($scheduler){if($scheduler.Folder.GetTasks(1).Count){throw 'Test tasks survived cleanup'};$scheduler.Service.GetFolder('\').DeleteFolder($namespace,0)}
  $store=Get-CleanupStore $namespace
  if(Test-Path -LiteralPath $store){[IO.Directory]::Delete($store,$false);[IO.Directory]::Delete((Split-Path -Parent $store),$false)}
 }finally{if($locked){$mutex.ReleaseMutex()};$mutex.Dispose()}
}
