param([Parameter(Mandatory=$true)][string]$Binaries,[Parameter(Mandatory=$true)][string]$Runtime,[string]$Output)
$ErrorActionPreference='Stop'
$project=Split-Path -Parent $PSScriptRoot
. (Join-Path $project 'packaging/cleanup-jobs.ps1')
$namespace='PcToolTest-'+[guid]::NewGuid().ToString('N')
if(!$Output){$Output=Join-Path $project ('out/uninstall-mapped-tests/'+$namespace)}
$base=[IO.Path]::GetFullPath($Output)
$root=Join-Path $base ($namespace+' app 中文')
New-Item -ItemType Directory -Path "$root/modules/archive" -Force|Out-Null
Copy-Item -LiteralPath "$Runtime/7-zip.dll" -Destination "$root/modules/archive/7-zip.dll"
[IO.File]::WriteAllText("$root/PcTool.exe",'fixture')
$manifest=Join-Path $base 'files.json'
@{Version=1;Files=@('PcTool.exe','modules/archive/7-zip.dll');Directories=@('modules','modules/archive')}|ConvertTo-Json|Set-Content $manifest -Encoding UTF8
function Check([bool]$value,[string]$message){if(!$value){throw $message}}
function Wait-For([scriptblock]$condition,[string]$message){
 $until=[DateTime]::UtcNow.AddSeconds(30)
 while(!(& $condition) -and [DateTime]::UtcNow -lt $until){Start-Sleep -Milliseconds 100}
 Check (& $condition) $message
}
$hostProcess=$null;$lateHost=$null;$job=$null
$mutex=[Threading.Mutex]::new($false,(Get-CleanupLockName $root $namespace));$held=$mutex.WaitOne(10000)
Check $held 'Test installation mutex unavailable'
try{
 & "$project/packaging/manage-data.ps1" -Action Record -Root $root -Manifest $manifest -CleanupNamespace $namespace
 Check ($LASTEXITCODE -eq 0) 'Record failed'
 [IO.File]::WriteAllText("$root/keep.txt",'unrelated user file')
 $instance=Read-CleanupInstance $root
 $entries=@();foreach($path in @('modules/archive/7-zip.dll','modules/archive','modules','PcTool.exe')){$entries+=@(New-CleanupEntry $root $path)}
 [IO.File]::WriteAllText("$root/.pctool-uninstalling",'')
 $registered=Register-CleanupJob $root $namespace 'uninstall' $entries
 Check (!(Retire-CleanupRoot $registered.Directory $registered.Job)) 'Retired unrelated files'
 'PASS unrelated data prevents whole-directory retirement'
 Stop-CleanupJobs $root $namespace
 Remove-Item -LiteralPath "$root/keep.txt" -Force
 Remove-Item -LiteralPath "$root/.pctool-uninstalling" -Force
 $prefix=Join-Path $base 'cached'
 $hostProcess=Start-Process -FilePath "$Binaries/PcToolArchiveShellTests.exe" -ArgumentList @(('"'+$root+'/modules/archive"'),'--lifecycle-host',('"'+$prefix+'"')) -WindowStyle Hidden -PassThru
 Wait-For {(Test-Path ($prefix+'.ready'))} 'Cached COM host did not start'
 $latePrefix=Join-Path $base 'late'
 $lateHost=Start-Process -FilePath "$Binaries/PcToolArchiveShellTests.exe" -ArgumentList @(('"'+$root+'/modules/archive"'),'--late-retirement-host',('"'+$latePrefix+'"')) -WindowStyle Hidden -PassThru
 Wait-For {(Test-Path ($latePrefix+'.ready'))} 'Late COM host did not start'
 $instance=Read-CleanupInstance $root
 [IO.File]::WriteAllText("$root/.pctool-uninstalling",'')
 $unknownImage=Join-Path $root 'modules/archive/7-zip.dll.999.old'
 [IO.File]::WriteAllText($unknownImage,'unknown legacy module')
 $entries=@(Get-ChildItem -LiteralPath $root -Recurse -Force|Where-Object {$_.Name -notin @('.pctool-uninstalling','.pctool-install-instance.json')}|ForEach-Object {New-CleanupEntry $root $_.FullName.Substring($root.Length+1)})
 $registered=Register-CleanupJob $root $namespace 'uninstall' $entries
 Check (!(Retire-CleanupRoot $registered.Directory $registered.Job)) 'Unknown legacy DLL lost original uninstall guard'
 Remove-Item -LiteralPath $unknownImage -Force
 'PASS unknown legacy extension preserves its original uninstall guard'
 Check (!(Invoke-CleanupJob $registered.Directory $registered.Job)) 'Mapped image unexpectedly deleted while host was alive'
 $job=$registered.Job;$retired=Get-JobPhysicalRoot $job
 Check (!(Test-Path $root) -and (Test-Path "$retired/modules/archive/7-zip.dll")) 'Original directory not cleaned while image is mapped'
 Check (!$hostProcess.HasExited) 'External host terminated'
 [IO.File]::WriteAllText(($prefix+'.check'),'check')
 Wait-For {(Test-Path ($prefix+'.checked'))} 'Cached COM objects did not stay blocked after retirement'
 [IO.File]::WriteAllText(($latePrefix+'.check'),'check')
 Wait-For {(Test-Path ($latePrefix+'.checked'))} 'Late COM request did not stay blocked after retirement'
 'PASS original directory disappears while external mapped DLL and cached COM objects stay alive and blocked'
 # Reinstall may not cancel a detached cleanup or delete the new installation.
 New-Item -ItemType Directory -Path $root -Force|Out-Null
 [IO.File]::WriteAllText("$root/PcTool.exe",'new installation')
 & "$project/packaging/manage-data.ps1" -Action Record -Root $root -Manifest $manifest -CleanupNamespace $namespace
 Check ($LASTEXITCODE -eq 0) 'Reinstall failed'
 Check (Test-Path (Join-Path $registered.Directory 'job.json')) 'Reinstall cancelled detached cleanup'
 [IO.File]::WriteAllText(($prefix+'.release'),'release')
 [IO.File]::WriteAllText(($latePrefix+'.release'),'release')
 Check ($hostProcess.WaitForExit(10000) -and $hostProcess.ExitCode -eq 0) 'Lifecycle host failed'
 Check ($lateHost.WaitForExit(10000) -and $lateHost.ExitCode -eq 0) 'Late COM host failed'
 $mutex.ReleaseMutex();$held=$false
 $scheduler=Open-CleanupScheduler $namespace
 [void]$scheduler.Folder.GetTask($job.TaskName).Run($null)
 Wait-For {!(Test-Path $registered.Directory)} 'Detached cleanup did not finish'
 Check (!(Test-Path $retired) -and [IO.File]::ReadAllText("$root/PcTool.exe") -eq 'new installation') 'Detached cleanup damaged reinstall or left files'
 'PASS detached cleanup removes its files and records without touching same-path reinstall'
}finally{
 if($hostProcess -and !$hostProcess.HasExited){[IO.File]::WriteAllText((Join-Path $base 'cached.release'),'release');[void]$hostProcess.WaitForExit(5000)}
 if($lateHost -and !$lateHost.HasExited){[IO.File]::WriteAllText((Join-Path $base 'late.release'),'release');[void]$lateHost.WaitForExit(5000)}
 Stop-CleanupJobs $root $namespace
 if($held){$mutex.ReleaseMutex()};$mutex.Dispose()
}
exit 0
