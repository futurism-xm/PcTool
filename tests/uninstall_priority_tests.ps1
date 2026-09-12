param([Parameter(Mandatory=$true)][string]$Binaries)
$ErrorActionPreference='Stop'
$project=Split-Path -Parent $PSScriptRoot
$Binaries=[IO.Path]::GetFullPath($Binaries)
$root=Join-Path $project ('out/uninstall-priority-tests/'+[guid]::NewGuid().ToString('N'))
$owned=Join-Path $root 'installed'
$other=Join-Path $root 'other'
New-Item -ItemType Directory -Path "$owned/modules/archive",$other -Force | Out-Null
$fixture=Join-Path $Binaries 'PcToolUninstallFixture.exe'
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
 $arguments=@('-NoProfile','-ExecutionPolicy','Bypass','-File',"$project/packaging/manage-data.ps1",'-Action',$action,'-Root',$directory)
 if($manifest){$arguments+=@('-Manifest',$manifest)}
 & $shell @arguments *> "$root/manage-$action.log"
 if($LASTEXITCODE -ne $expected){throw "Manage $action returned $LASTEXITCODE, expected $expected"}
}
try {
 $polite=Launch "$owned/PcTool.exe" $owned 'polite'
 $hung=Launch "$owned/modules/archive/7z.exe" $owned 'stubborn'
 $duplicate=Launch "$owned/PcTool.exe" $owned 'stubborn'
 $unrelated=Launch "$other/PcTool.exe" $other 'stubborn'
 foreach($process in @($polite,$hung,$duplicate,$unrelated)){if($process.HasExited){throw "Fixture exited before uninstall: $($process.Id) code $($process.ExitCode)"}}
 $timer=[Diagnostics.Stopwatch]::StartNew()
 & "$Binaries/PcToolUninstallHelper.exe" stop $owned *> "$root/stop.log"
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

 Copy-Item "$Binaries/PcTool.exe" "$owned/PcTool.exe" -Force
 foreach($name in @('7z.exe','7zG.exe','7zFM.exe')){Copy-Item "$project/out/archive-runtime/$name" "$owned/modules/archive/$name" -Force}
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
 # Failed cleanup retains retry metadata and still cleans independent cache data.
 $inventory=Join-Path $owned '.pctool-managed-files.json'
 $valid=[IO.File]::ReadAllText($inventory)
 Copy-Item "$root/bad.json" $inventory -Force
 New-Item -ItemType Directory -Path "$owned/Cache" -Force | Out-Null
 [IO.File]::WriteAllText("$owned/Cache/discard.tmp",'cache')
 Manage 'Uninstall' $owned '' 1
 if((Test-Path "$owned/Cache") -or !(Test-Path $inventory) -or !(Test-Path "$owned/.pctool-uninstalling")){throw 'Failure did not preserve retry state or continue independent cleanup'}
 [IO.File]::WriteAllText($inventory,$valid)
 Remove-Item -LiteralPath "$owned/PcTool.exe" -Force
 Manage 'Uninstall' $owned ''
 'PASS partial failure and retry after primary executable removal'
 Copy-Item "$Binaries/PcTool.exe" "$owned/PcTool.exe"
 Manage 'Install' $owned ''
 if(Test-Path "$owned/.pctool-uninstalling"){throw 'Reinstall failed to clear marker'}
 'PASS reinstall clears uninstall marker'
 "Evidence: $root"
} finally {
 foreach($process in $processes){if(!$process.HasExited){$process.Kill();$process.WaitForExit()};$process.Dispose()}
}
