param([Parameter(Mandatory=$true)][string]$Executable,[string]$Output,[string]$ProductExecutable,[string]$FixtureBase=$env:ProgramFiles)
$ErrorActionPreference='Stop'
if(!([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Startup integration tests require an elevated session'}
$project=Split-Path -Parent $PSScriptRoot
$Executable=(Resolve-Path -LiteralPath $Executable).Path
$namespace='PcToolTest-'+[guid]::NewGuid().ToString('N')
if(!$Output){$Output=Join-Path $project ('out/startup-tests/'+$namespace)}
New-Item -ItemType Directory -Path $Output -Force | Out-Null
$FixtureBase=(Resolve-Path -LiteralPath $FixtureBase).Path
$root=Join-Path $FixtureBase $namespace
$runPath='Software\'+$namespace+'\Run'
$scheduler=New-Object -ComObject 'Schedule.Service';$scheduler.Connect()
$sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$folder=$null
function Check([bool]$condition,[string]$message){if(!$condition){throw $message}}
function Check-TaskSecurity($object,[string]$label){
 $descriptor=[Security.AccessControl.RawSecurityDescriptor]::new([string]$object.GetSecurityDescriptor(7))
 Check ($descriptor.Owner.Value -eq 'S-1-5-32-544') "$label must be owned by Administrators"
 Check ($null -ne $descriptor.DiscretionaryAcl) "$label has no DACL"
 foreach($ace in $descriptor.DiscretionaryAcl){
  if($ace.AceQualifier -eq [Security.AccessControl.AceQualifier]::AccessAllowed){
   Check ($ace.SecurityIdentifier.Value -in @('S-1-5-18','S-1-5-32-544')) "$label grants access to an untrusted identity"
  }
 }
}
function User-Sid([string]$account){if($account -like 'S-1-*'){return $account};return ([Security.Principal.NTAccount]::new($account)).Translate([Security.Principal.SecurityIdentifier]).Value}
function Apply([string]$operation,[bool]$success=$true,[string]$path=(Join-Path $root 'PcTool.exe')){
 & $Executable $namespace $path $operation
 Check (($LASTEXITCODE -eq 0) -eq $success) "Unexpected startup $operation result: $LASTEXITCODE"
}
try{
 New-Item -ItemType Directory -Path $root | Out-Null
 Copy-Item -LiteralPath $Executable -Destination (Join-Path $root 'PcTool.exe')
 $manifest=Join-Path $root '.pctool-package-files.json'
 [IO.File]::WriteAllText($manifest,(@{Version=1;Files=@('PcTool.exe','.pctool-package-files.json');Directories=@()}|ConvertTo-Json),[Text.UTF8Encoding]::new($false))
 & "$project/packaging/manage-data.ps1" -Action Record -Root $root -Manifest $manifest -CleanupNamespace $namespace
 Check ($LASTEXITCODE -eq 0) 'Fixture inventory setup failed'
 & "$project/packaging/manage-data.ps1" -Action Install -Root $root -CleanupNamespace $namespace
 Check ($LASTEXITCODE -eq 0) 'Fixture ACL setup failed'
 $legacy='"'+$root+'\PcTool.exe" --background'
 $registry=[Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($runPath)
 $registry.SetValue('PcTool',$legacy);$registry.Dispose()
 Apply enable
 $folder=$scheduler.GetFolder('\'+$namespace+'\Startup')
 $tasks=@($folder.GetTasks(1));Check ($tasks.Count -eq 1) 'Expected one startup task'
 $task=$tasks[0];$name=$task.Name;$definition=$task.Definition
 Check-TaskSecurity $task 'Startup task'
 Check-TaskSecurity $folder 'Startup folder'
 Check-TaskSecurity ($scheduler.GetFolder('\'+$namespace)) 'Namespace folder'
 Check ((User-Sid $definition.Principal.UserId) -eq $sid -and $definition.Principal.LogonType -eq 3 -and $definition.Principal.RunLevel -eq 1) 'Wrong task principal'
 Check ($definition.Triggers.Count -eq 1 -and $definition.Triggers.Item(1).Type -eq 9 -and (User-Sid $definition.Triggers.Item(1).UserId) -eq $sid) 'Wrong logon trigger'
 Check ($definition.Actions.Count -eq 1 -and $definition.Actions.Item(1).Path -eq (Join-Path $root 'PcTool.exe') -and $definition.Actions.Item(1).Arguments -ceq '--background') 'Wrong direct task action'
 $registry=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($runPath)
 Check ($null -eq $registry.GetValue('PcTool')) 'Owned legacy Run entry remains';$registry.Dispose()
 [void]$task.Run($null)
 $tokenPath=Join-Path $root 'Data/startup-token.txt'
 for($i=0;$i -lt 100 -and !(Test-Path -LiteralPath $tokenPath);$i++){Start-Sleep -Milliseconds 100}
 Check (Test-Path -LiteralPath $tokenPath) 'Scheduled executable did not run'
 $token=@(Get-Content -LiteralPath $tokenPath)
 Check ($token[0] -eq $sid -and [int]$token[1] -eq (Get-Process -Id $PID).SessionId -and $token[2] -eq '1') 'Task was not elevated in the current interactive user session'
 Copy-Item -LiteralPath $tokenPath -Destination (Join-Path $Output 'startup-token.txt')
 Apply enable;Apply enable $true ((Join-Path $root 'PcTool.exe').ToUpperInvariant());Check (@($folder.GetTasks(1)).Count -eq 1) 'Duplicate startup tasks for equivalent path casing'
 Apply disable;Apply disable;Check (@($folder.GetTasks(1)).Count -eq 0) 'Repeated disable left tasks'
 'PASS highest interactive task, readback, legacy migration, idempotence and actual elevated execution'

 # Preserve a Run value that belongs to a different installation.
 $registry=[Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($runPath);$registry.SetValue('PcTool','"C:\Other\PcTool.exe" --background');$registry.Dispose()
 Apply enable;Apply disable
 $registry=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($runPath);Check ($registry.GetValue('PcTool') -eq '"C:\Other\PcTool.exe" --background') 'Foreign Run entry changed';$registry.Dispose()
 Apply enable
 $task=$folder.GetTask($name);$definition=$task.Definition;$definition.Actions.Item(1).Path='C:\Other\PcTool.exe'
 [void]$folder.RegisterTaskDefinition($name,$definition,6,$sid,$null,3)
 Apply enable $false;Apply disable $false
 Check ($folder.GetTask($name).Definition.Actions.Item(1).Path -eq 'C:\Other\PcTool.exe') 'Foreign task changed'
 $folder.DeleteTask($name,0)
 'PASS conflicting task and unrelated Run value preserved'

 # A failed security preflight must retain the old task and legacy startup.
 $registry=[Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($runPath);$registry.SetValue('PcTool',$legacy);$registry.Dispose()
 Apply enable
 $payload=Join-Path $root 'PcTool.exe';$savedAcl=Get-Acl -LiteralPath $payload
 $unsafeAcl=Get-Acl -LiteralPath $payload
 $unsafeAcl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.SecurityIdentifier]::new('S-1-5-32-545'),'Modify','Allow'))
 Set-Acl -LiteralPath $payload -AclObject $unsafeAcl
 try{
  $registry=[Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($runPath);$registry.SetValue('PcTool',$legacy);$registry.Dispose()
  Apply enable $false
  $registry=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($runPath);Check ($registry.GetValue('PcTool') -eq $legacy) 'Registration failure removed legacy startup';$registry.Dispose()
  Check (@($folder.GetTasks(1)).Count -eq 1) 'Failed registration removed existing task'
 }finally{Set-Acl -LiteralPath $payload -AclObject $savedAcl}
 Apply disable
 'PASS failed security validation preserves existing startup'

 # Production rejects replacement-prone executable/ancestor paths.
 Apply enable $false $Executable
 Check (@($folder.GetTasks(1)).Count -eq 0) 'Unsafe path created a task'
 'PASS writable path rejected'

 # A protected executable is still unsafe when its ordinary parent can be
 # renamed. The volume root's DELETE right is tested by a D:\ fixture base.
 $replaceable=Join-Path $root 'replaceable';$child=Join-Path $replaceable 'app'
 New-Item -ItemType Directory -Path $child -Force | Out-Null
 Copy-Item -LiteralPath $Executable -Destination (Join-Path $child 'PcTool.exe')
 $parentAcl=Get-Acl -LiteralPath $replaceable
 $unsafeParentAcl=Get-Acl -LiteralPath $replaceable
 $unsafeParentAcl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.SecurityIdentifier]::new('S-1-5-32-545'),'Modify','Allow'))
 Set-Acl -LiteralPath $replaceable -AclObject $unsafeParentAcl
 try{Apply enable $false (Join-Path $child 'PcTool.exe');Check (@($folder.GetTasks(1)).Count -eq 0) 'Replaceable ancestor created a task'}
 finally{Set-Acl -LiteralPath $replaceable -AclObject $parentAcl}
 'PASS protected payload under replaceable ancestor rejected'

 if($ProductExecutable){
  $productRoot=Join-Path $root 'product';New-Item -ItemType Directory -Path $productRoot | Out-Null
  Copy-Item -LiteralPath (Resolve-Path -LiteralPath $ProductExecutable).Path -Destination (Join-Path $productRoot 'PcTool.exe')
  & "$project/packaging/manage-data.ps1" -Action Install -Root $productRoot -CleanupNamespace $namespace
  Check ($LASTEXITCODE -eq 0) 'Product fixture protection failed'
  $settings=Join-Path $productRoot 'Data/settings.ini'
  [IO.File]::WriteAllText($settings,"[PcTool]`r`nVersion=1`r`nAutoStart=1`r`n",[Text.UTF8Encoding]::new($false))
  $registry=[Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($runPath);$registry.SetValue('PcTool',('"'+$productRoot+'\PcTool.exe" --background'));$registry.Dispose()
  $configured=Start-Process -FilePath (Join-Path $productRoot 'PcTool.exe') -ArgumentList @('--configure-startup',$namespace) -WindowStyle Hidden -Wait -PassThru
  Check ($configured.ExitCode -eq 0 -and @($folder.GetTasks(1)).Count -eq 1) 'Shipped installer entry did not migrate startup'
  $registry=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($runPath);Check ($null -eq $registry.GetValue('PcTool')) 'Shipped entry retained old Run value';$registry.Dispose()
  [IO.File]::WriteAllText($settings,"[PcTool]`r`nVersion=1`r`nAutoStart=0`r`n",[Text.UTF8Encoding]::new($false))
  $configured=Start-Process -FilePath (Join-Path $productRoot 'PcTool.exe') -ArgumentList @('--configure-startup',$namespace) -WindowStyle Hidden -Wait -PassThru
  Check ($configured.ExitCode -eq 0 -and @($folder.GetTasks(1)).Count -eq 0) 'Shipped entry did not honor disabled startup'
  'PASS shipped installer entry migrates startup before first launch and honors user preference'
 }

 Apply enable
 & "$project/packaging/manage-data.ps1" -Action Uninstall -Root $root -CleanupNamespace $namespace
 Check ($LASTEXITCODE -eq 0) 'Isolated uninstall failed'
 Check (@($folder.GetTasks(1)).Count -eq 0) 'Uninstall left its startup task'
 'PASS isolated uninstall removes owned startup task'
}finally{
 if($folder){foreach($task in @($folder.GetTasks(1))){$folder.DeleteTask($task.Name,0)}}
 try{$scheduler.GetFolder('\'+$namespace).DeleteFolder('Startup',0);$scheduler.GetFolder('\').DeleteFolder($namespace,0)}catch{Write-Warning $_}
 [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree('Software\'+$namespace,$false)
 $resolved=[IO.Path]::GetFullPath($root)
 if($resolved -ne (Join-Path $FixtureBase $namespace) -or $namespace -notmatch '^PcToolTest-[a-f0-9]{32}$'){throw 'Unsafe fixture cleanup path'}
 if(Test-Path -LiteralPath $resolved){Remove-Item -LiteralPath $resolved -Recurse -Force}
}
