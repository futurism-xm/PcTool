param([string]$Output)
$ErrorActionPreference='Stop'
$project=Split-Path -Parent $PSScriptRoot
. (Join-Path $project 'packaging/cleanup-jobs.ps1')
$namespace='PcToolTest-'+[guid]::NewGuid().ToString('N')
if(!$Output){$Output=Join-Path $project ('out/uninstall-cleanup-tests/'+$namespace)}
$fixture=[IO.Path]::GetFullPath($Output)
[void](New-Item -ItemType Directory -Path $fixture -Force)
$shell=Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
$roots=[Collections.Generic.List[string]]::new()
function Check([bool]$value,[string]$message){if(!$value){throw $message}}
function Run-Action([string]$root,[string]$action,[int]$expected,[string]$manifest=''){
 $scriptPath=Join-Path $root 'manage-data.ps1'
 if($action -eq 'Record' -or !(Test-Path -LiteralPath $scriptPath)){$scriptPath=Join-Path $project 'packaging/manage-data.ps1'}
 $log=Join-Path $fixture ((Split-Path -Leaf $root)+'-'+$action+'.log')
 $arguments=@('-NoProfile','-ExecutionPolicy','Bypass','-File',$scriptPath,'-Action',$action,'-Root',$root,'-CleanupNamespace',$namespace)
 if($manifest){$arguments+=@('-Manifest',$manifest)}
 $preference=$ErrorActionPreference;$ErrorActionPreference='Continue'
 try{& $shell @arguments *> $log;$code=$LASTEXITCODE}finally{$ErrorActionPreference=$preference}
 if($code -ne $expected){throw "$action returned $code, expected $expected; see $log"}
}
function Prepare([string]$name){
 $root=Join-Path $fixture $name;$roots.Add($root)
 [void](New-Item -ItemType Directory -Path (Join-Path $root 'Data'),(Join-Path $root 'Cache'),(Join-Path $root 'modules/archive') -Force)
 foreach($name in @('PcTool.exe','Uninstall.exe','PcToolUninstallHelper.exe','keep.txt')){[IO.File]::WriteAllText((Join-Path $root $name),'fixture')}
 [IO.File]::WriteAllText((Join-Path $root 'Data/settings'),'data fixture')
 [IO.File]::WriteAllText((Join-Path $root 'Cache/cached.png'),'cache fixture')
 [IO.File]::WriteAllText((Join-Path $root 'modules/archive/7-zip.dll'),'extension fixture')
 foreach($name in @('manage-data.ps1','cleanup-jobs.ps1','retry-cleanup.ps1')){Copy-Item -LiteralPath (Join-Path $project ('packaging/'+$name)) -Destination $root}
 $manifest=Join-Path $fixture ((Split-Path -Leaf $root)+'-manifest.json')
 @{Version=1;Files=@('PcTool.exe','modules/archive/7-zip.dll','manage-data.ps1','cleanup-jobs.ps1','retry-cleanup.ps1','PcToolUninstallHelper.exe');Directories=@('modules/archive','modules')}|ConvertTo-Json|Set-Content -LiteralPath $manifest -Encoding UTF8
 Run-Action $root 'Record' 0 $manifest
 [IO.File]::WriteAllText((Join-Path $root '.pctool-uninstalling'),'guard')
 return $root
}
function Wait-For([scriptblock]$condition,[string]$message){
 $end=[DateTime]::UtcNow.AddSeconds(30)
 while(!(& $condition) -and [DateTime]::UtcNow -lt $end){Start-Sleep -Milliseconds 100}
 Check (& $condition) $message
}
function Inject-RegistrationFailure([string]$root){
 $library=Join-Path $root 'cleanup-jobs.ps1';$text=[IO.File]::ReadAllText($library)
 $needle="function Register-CleanupJob([string]`$Root,[string]`$Namespace,[string]`$Kind,[object[]]`$Entries,[hashtable]`$RuntimeSource=@{}) {"
 Check ($text.Contains($needle)) 'Failure-injection anchor missing'
 [IO.File]::WriteAllText($library,$text.Replace($needle,$needle+"`n throw 'Injected test-only task registration failure'"))
}
$held=$null
try{
 $root=Prepare 'normal'
 Inject-RegistrationFailure $root
 Run-Action $root 'Uninstall' 0
 Check (Test-Path -LiteralPath (Join-Path $root 'manage-data.ps1')) 'Bootstrap removed before finalization'
 Check (Test-Path -LiteralPath (Join-Path $root '.pctool-uninstalling')) 'Guard removed before finalization'
 Run-Action $root 'Finalize' 0
 Check (@(Get-ChildItem -LiteralPath $root -Force).Count -eq 1) 'Normal uninstall left managed files'
 Check (Test-Path -LiteralPath (Join-Path $root 'keep.txt')) 'Unrelated file removed'
 'PASS normal uninstall needs no task registration, preserves bootstrap until Finalize and keeps unrelated files'

 $root=Prepare 'locked'
 $held=[IO.File]::Open((Join-Path $root 'modules/archive/7-zip.dll'),'Open','Read','None')
 Run-Action $root 'Uninstall' 2
 Run-Action $root 'Finalize' 2
 Check (Test-Path -LiteralPath (Join-Path $root '.pctool-uninstalling')) 'Guard removed while extension remains'
 Run-Action $root 'Uninstall' 2
 Run-Action $root 'Finalize' 2
 $store=Get-CleanupStore $namespace
 $jobs=@(Get-ChildItem -LiteralPath $store -Directory)
 Check ($jobs.Count -eq 1) 'Pending cleanup job missing'
 $job=Get-Content -LiteralPath (Join-Path $jobs[0].FullName 'job.json') -Raw|ConvertFrom-Json
 $scheduler=Open-CleanupScheduler $namespace
 Wait-For {(Test-Path -LiteralPath (Join-Path $jobs[0].FullName 'last-result.txt')) -and $scheduler.Folder.GetTask($job.TaskName).State -ne 4} 'Worker did not complete pending attempt'
 $held.Dispose();$held=$null
 [void]$scheduler.Folder.GetTask($job.TaskName).Run($null)
 Wait-For {!(Test-Path -LiteralPath $jobs[0].FullName)} 'Deferred cleanup did not finish after lock release'
 Check (@(Get-ChildItem -LiteralPath $root -Force).Count -eq 1) 'Deferred cleanup left managed files'
 Check (Test-Path -LiteralPath (Join-Path $root 'keep.txt')) 'Deferred cleanup removed unrelated file'
 'PASS locked extension returns 2, independent SYSTEM worker cleans without reboot and removes guard last'
 Run-Action $root 'Uninstall' 0;Run-Action $root 'Finalize' 0
 Check (!(Test-Path -LiteralPath (Join-Path $root '.pctool-uninstalling'))) 'Repeated completed uninstall created a permanent marker'
 'PASS repeated pending uninstall retains exact entries with one job; completed uninstall is idempotent'

 $root=Prepare 'upgrade'
 $old=Join-Path $root 'modules/archive/7-zip.dll.123.old';[IO.File]::WriteAllText($old,'old mapped copy')
 $held=[IO.File]::Open($old,'Open','Read','None')
 Run-Action $root 'Install' 2
 Check (!(Test-Path -LiteralPath (Join-Path $root '.pctool-uninstalling'))) 'Install did not release guard'
 $previous=(Read-CleanupInstance $root).Id
 $manifest=Join-Path $fixture 'upgrade-manifest.json'
 Run-Action $root 'Record' 0 $manifest
 Check ((Read-CleanupInstance $root).Id -ne $previous) 'Record reused previous installation identity'
 Check (@(Get-ChildItem -LiteralPath (Get-CleanupStore $namespace) -Directory).Count -eq 0) 'Record did not cancel previous cleanup job'
 Check (Test-Path -LiteralPath $old) 'Record deleted the previous mapped copy'
 Run-Action $root 'Install' 2
 $jobDirectory=@(Get-ChildItem -LiteralPath (Get-CleanupStore $namespace) -Directory)[0].FullName
 $job=Get-Content -LiteralPath (Join-Path $jobDirectory 'job.json') -Raw|ConvertFrom-Json
 Check ($job.Kind -eq 'upgrade' -and $job.Entries.Count -eq 1) 'Upgrade job includes non-old files'
 $held.Dispose();$held=$null
 $scheduler=Open-CleanupScheduler $namespace
 Wait-For {!(Test-Path -LiteralPath $jobDirectory) -or $scheduler.Folder.GetTask($job.TaskName).State -ne 4} 'Upgrade worker stayed running'
 if(Test-Path -LiteralPath $jobDirectory){[void]$scheduler.Folder.GetTask($job.TaskName).Run($null)}
 Wait-For {!(Test-Path -LiteralPath $jobDirectory)} 'Upgrade cleanup did not finish'
 Check (!(Test-Path -LiteralPath $old)) 'Old DLL survived upgrade cleanup'
 Check ((Test-Path -LiteralPath (Join-Path $root 'PcTool.exe')) -and (Test-Path -LiteralPath (Join-Path $root 'modules/archive/7-zip.dll')) -and (Test-Path -LiteralPath (Join-Path $root '.pctool-install-instance.json'))) 'Upgrade cleanup touched active installation'
 'PASS Record cancels old jobs and changes instance; upgrade job removes only renamed old DLL'

 $root=Prepare 'late-bootstrap-lock'
 $held=[IO.File]::Open((Join-Path $root 'Uninstall.exe'),'Open','Read','None')
 Run-Action $root 'Uninstall' 0
 Inject-RegistrationFailure $root
 Run-Action $root 'Finalize' 1
 foreach($name in @('Uninstall.exe','.pctool-uninstalling','.pctool-install-instance.json')){Check (Test-Path -LiteralPath (Join-Path $root $name)) "Late bootstrap lock lost retry state: $name"}
 Check (!(Test-Path -LiteralPath (Join-Path $root 'manage-data.ps1'))) 'Late-lock fixture did not exercise removed-bootstrap recovery'
 $held.Dispose();$held=$null
 # The real uninstaller embeds these scripts. Source fallback simulates that
 # independent retry payload after the installed scripts were already removed.
 Run-Action $root 'Uninstall' 0;Run-Action $root 'Finalize' 0
 Check (@(Get-ChildItem -LiteralPath $root -Force).Count -eq 1) 'Late-lock retry left managed files'
 'PASS new bootstrap lock plus registration failure retains embedded uninstaller/state and can retry'

 $root=Prepare 'registration-failure'
 $held=[IO.File]::Open((Join-Path $root 'modules/archive/7-zip.dll'),'Open','Read','None')
 Run-Action $root 'Uninstall' 2
 Inject-RegistrationFailure $root
 Run-Action $root 'Finalize' 1
 foreach($name in @('manage-data.ps1','cleanup-jobs.ps1','retry-cleanup.ps1','PcToolUninstallHelper.exe','Uninstall.exe','.pctool-managed-files.json','.pctool-uninstalling','.pctool-install-instance.json')){
  Check (Test-Path -LiteralPath (Join-Path $root $name)) "Registration failure removed retry tool: $name"
 }
 'PASS task-registration failure preserves every retry tool and guard'
 $held.Dispose();$held=$null
 $queue=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Control\Session Manager')
 try{$pending=@($queue.GetValue('PendingFileRenameOperations',[string[]]@()));Check (@($pending|Where-Object {$_ -like ('*'+$namespace+'*')}).Count -eq 0) 'Cleanup registered a reboot delete'}finally{$queue.Dispose()}
 "Evidence: $fixture; isolated namespace: $namespace"
}finally{
 if($held){$held.Dispose()}
 foreach($root in $roots){Stop-CleanupJobs $root $namespace}
 $scheduler=Open-CleanupScheduler $namespace
 if($scheduler){Check ($scheduler.Folder.GetTasks(1).Count -eq 0) 'Test tasks survived';$scheduler.Service.GetFolder('\').DeleteFolder($namespace,0)}
 $store=Get-CleanupStore $namespace
 if(Test-Path -LiteralPath $store){[IO.Directory]::Delete($store,$false);[IO.Directory]::Delete((Split-Path -Parent $store),$false)}
}
