param([string]$Output)
$ErrorActionPreference='Stop'
$project=Split-Path -Parent $PSScriptRoot
. (Join-Path $project 'packaging/cleanup-jobs.ps1')
$namespace='PcToolTest-'+[guid]::NewGuid().ToString('N')
if(!$Output){$Output=Join-Path $project ('out/cleanup-jobs-tests/'+$namespace)}
$fixture=[IO.Path]::GetFullPath($Output)
[void](New-Item -ItemType Directory -Path $fixture -Force)
& (Join-Path $PSScriptRoot 'cleanup_module_tests.ps1') -Output (Join-Path $fixture 'module-import')
$roots=[Collections.Generic.List[string]]::new()
function Check([bool]$Value,[string]$Message){if(!$Value){throw $Message}}
function Prepare([string]$Name){
 $root=Join-Path $fixture $Name;[void](New-Item -ItemType Directory -Path $root)
 [IO.File]::WriteAllText((Join-Path $root 'PcTool.exe'),'fixture')
 [IO.File]::WriteAllText((Join-Path $root '.pctool-uninstalling'),'fixture guard')
 Write-CleanupJson (Join-Path $root '.pctool-install-instance.json') ([ordered]@{Version=1;Id=[guid]::NewGuid().ToString('N');Root=$root;Entries=@()})
 $roots.Add($root);return $root
}
function Wait-For([scriptblock]$Condition,[string]$Message){
 $deadline=[DateTime]::UtcNow.AddSeconds(30)
 while(!( & $Condition) -and [DateTime]::UtcNow -lt $deadline){Start-Sleep -Milliseconds 100}
 Check (& $Condition) $Message
}
$held=$null
try{
 Initialize-CleanupNative
 $root=Prepare 'identity'
 $state=Join-Path $root '.pctool-install-instance.json';$before=[IO.File]::ReadAllText($state)
 $held=[IO.File]::Open($state,'Open','Read','None');$rejected=$false
 try{Write-CleanupJson $state ([ordered]@{Version=1;Id=[guid]::NewGuid().ToString('N');Root=$root;Entries=@()})}catch{$rejected=$true}
 $held.Dispose();$held=$null
 Check ($rejected -and [IO.File]::ReadAllText($state) -ceq $before) 'Failed atomic state update lost previous installation identity'
 Check (@(Get-ChildItem -LiteralPath $root -Filter '*.tmp' -Force).Count -eq 0) 'Failed update left temporary state'
 'PASS failed state replacement retains previous instance atomically'
 $file=Join-Path $root 'owned.dll';[IO.File]::WriteAllText($file,'old');$entry=New-CleanupEntry $root 'owned.dll'
 $renamed=Join-Path $root 'original.dll';[IO.File]::Move($file,$renamed);[IO.File]::WriteAllText($file,'replacement')
 [IO.File]::SetAttributes($file,[IO.FileAttributes]::ReadOnly)
 $rejected=$false;try{[void](Remove-CleanupEntry $root $entry)}catch{$rejected=$true}
 Check $rejected 'Replaced file did not produce a hard failure'
 Check ((Get-Content -LiteralPath $file -Raw) -eq 'replacement') 'Replacement contents changed'
 Check (([IO.File]::GetAttributes($file) -band [IO.FileAttributes]::ReadOnly) -ne 0) 'Replacement attributes changed before identity check'
 [IO.File]::SetAttributes($file,[IO.FileAttributes]::Normal)
 [IO.File]::Delete($file);[IO.File]::Move($renamed,$file)
 [IO.File]::SetAttributes($file,[IO.FileAttributes]::ReadOnly)
 Check (Remove-CleanupEntry $root $entry) 'Original identity was not removable'
 'PASS same-handle readonly cleanup; replaced file contents and attributes remain intact'

 $file=Join-Path $root 'denied.dll';[IO.File]::WriteAllText($file,'permissions fixture');$entry=New-CleanupEntry $root 'denied.dll'
 $originalAcl=Get-Acl -LiteralPath $file;$deniedAcl=Get-Acl -LiteralPath $file
 $deniedAcl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.WindowsIdentity]::GetCurrent().User,'WriteAttributes','Deny'))
 Set-Acl -LiteralPath $file -AclObject $deniedAcl
 $rejected=$false
 try{[void](Remove-CleanupEntry $root $entry)}catch{$rejected=$true}finally{Set-Acl -LiteralPath $file -AclObject $originalAcl}
 Check $rejected 'ACL denial was incorrectly treated as a temporary lock'
 Check (Remove-CleanupEntry $root $entry) 'Permission fixture did not recover'
 'PASS ACL denial is a hard failure rather than an endless deferred job'

 Add-Type @'
using System;using System.Runtime.InteropServices;
public static class CleanupMappedFixture {
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] public static extern IntPtr LoadLibrary(string path);
 [DllImport("kernel32.dll")] public static extern bool FreeLibrary(IntPtr module);
}
'@
 $file=Join-Path $root 'mapped.dll';Copy-Item -LiteralPath (Join-Path $env:SystemRoot 'System32/version.dll') -Destination $file
 $entry=New-CleanupEntry $root 'mapped.dll';$module=[CleanupMappedFixture]::LoadLibrary($file)
 Check ($module -ne [IntPtr]::Zero) 'Cannot load mapped-DLL fixture'
 try{Check (!(Remove-CleanupEntry $root $entry)) 'Mapped image did not defer deletion'}finally{[void][CleanupMappedFixture]::FreeLibrary($module)}
 Check (Remove-CleanupEntry $root $entry) 'Mapped fixture was not removable after release'
 'PASS real mapped image defers at disposition, then deletes after FreeLibrary'

 $outside=Join-Path $fixture 'outside';[void](New-Item -ItemType Directory -Path $outside);[IO.File]::WriteAllText((Join-Path $outside 'keep'),'outside')
 $link=Join-Path $root 'link';[void](New-Item -ItemType Junction -Path $link -Target $outside)
 $rejected=$false;try{[void](New-CleanupEntry $root 'link/keep')}catch{$rejected=$true}
 Check $rejected 'Reparse parent accepted';Check ((Get-Content -LiteralPath (Join-Path $outside 'keep') -Raw) -eq 'outside') 'External file changed'
 Check (Remove-CleanupEntry $root (New-CleanupEntry $root 'link')) 'Junction itself was not removable'
 'PASS reparse parents rejected; junction removed without traversal'

 $store=Get-CleanupStore $namespace -Create
 $orphan=Join-Path $store ((Get-CleanupRootHash $root)+'-'+[guid]::NewGuid().ToString('N'))
 [void](New-Item -ItemType Directory -Path $orphan)
 [IO.File]::WriteAllText((Join-Path $orphan 'cleanup-jobs.ps1'),'incomplete runtime')
 [IO.File]::WriteAllText((Join-Path $orphan ('job.json.'+[guid]::NewGuid().ToString('N')+'.tmp')),'incomplete manifest')
 Stop-CleanupJobs $root $namespace
 Check (!(Test-Path -LiteralPath $orphan)) 'Incomplete job without manifest blocked cancellation'
 'PASS interrupted job creation is recoverable without a manifest'

 $root=Prepare 'scheduled'
 $file=Join-Path $root 'locked.dll';[IO.File]::WriteAllText($file,'mapped fixture')
 $held=[IO.File]::Open($file,'Open','Read','None')
 $entry=New-CleanupEntry $root 'locked.dll'
 $registered=Register-CleanupJob $root $namespace 'uninstall' @($entry,(New-CleanupEntry $root 'PcTool.exe'))
 $scheduler=Open-CleanupScheduler $namespace;$task=$scheduler.Folder.GetTask($registered.Job.TaskName)
 Check ($task.Definition.Principal.UserId -in @('SYSTEM','S-1-5-18')) 'Task is not SYSTEM'
 Check ($task.Definition.Triggers.Count -eq 2) 'Missing retry or logon trigger'
 Check ($task.Definition.Triggers.Item(1).Repetition.Interval -eq 'PT5M') 'Wrong retry interval'
 Assert-CleanupSecurity $registered.Directory;Assert-CleanupSecurity (Join-Path $registered.Directory 'job.json')
 Wait-For {Test-Path -LiteralPath (Join-Path $registered.Directory 'last-result.txt')} 'Scheduled SYSTEM worker did not record locked file'
 Check (Test-Path -LiteralPath (Join-Path $root '.pctool-uninstalling')) 'Guard removed while DLL locked'
 Check (Test-Path -LiteralPath (Join-Path $root '.pctool-install-instance.json')) 'Instance removed while DLL locked'
 # Keep the DLL locked until the first attempt exits; otherwise that attempt
 # can finish cleanup and delete its task before the state query below.
 Wait-For {$scheduler.Folder.GetTask($registered.Job.TaskName).State -ne 4} 'Pending task attempt did not exit'
 Check ($task.LastTaskResult -eq 2) 'Locked-file task did not return pending'
 $held.Dispose();$held=$null
 [void]$task.Run($null)
 Wait-For {!(Test-Path -LiteralPath $registered.Directory)} 'Worker did not remove completed task record'
 Check (!(Test-Path -LiteralPath $root)) 'Successful uninstall did not clear exact fixture root'
 Check (@($scheduler.Folder.GetTasks(1)|Where-Object {$_.Name -eq $registered.Job.TaskName}).Count -eq 0) 'Completed task survived'
 'PASS real SYSTEM task retries locked file, retains guard, then removes files/task/record'

 $root=Prepare 'reinstall'
 $file=Join-Path $root 'old.dll';[IO.File]::WriteAllText($file,'keep new installation')
 $mutex=[Threading.Mutex]::new($false,(Get-CleanupLockName $root $namespace));[void]$mutex.WaitOne()
 try{
  $registered=Register-CleanupJob $root $namespace 'upgrade' @((New-CleanupEntry $root 'old.dll'))
  Write-CleanupJson (Join-Path $root '.pctool-install-instance.json') ([ordered]@{Version=1;Id=[guid]::NewGuid().ToString('N');Root=$root;Entries=@()})
 }finally{$mutex.ReleaseMutex();$mutex.Dispose()}
 Wait-For {!(Test-Path -LiteralPath $registered.Directory)} 'Obsolete job was not retired'
 Check ((Get-Content -LiteralPath $file -Raw) -eq 'keep new installation') 'Old job deleted reinstalled file'
 'PASS changed installation instance retires old job without deletion'
 "Evidence: $fixture; isolated namespace: $namespace"
}finally{
 if($held){$held.Dispose()}
 foreach($root in $roots){Stop-CleanupJobs $root $namespace}
 $scheduler=Open-CleanupScheduler $namespace
 if($scheduler){Check ($scheduler.Folder.GetTasks(1).Count -eq 0) 'Test cleanup left scheduled tasks';$scheduler.Service.GetFolder('\').DeleteFolder($namespace,0)}
 $store=Get-CleanupStore $namespace
 if(Test-Path -LiteralPath $store){[IO.Directory]::Delete($store,$false);[IO.Directory]::Delete((Split-Path -Parent $store),$false)}
}
