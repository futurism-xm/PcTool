param([Parameter(Mandatory=$true)][string]$Executable,[string]$Output,[string]$ProductExecutable,[string]$FixtureBase,[string]$InstallerHelper)
$ErrorActionPreference='Stop'
$project=Split-Path -Parent $PSScriptRoot
$Executable=(Resolve-Path -LiteralPath $Executable).Path
$namespace='PcToolTest-'+[guid]::NewGuid().ToString('N')
if(!$FixtureBase){$FixtureBase=Join-Path $project 'out/startup-tests'}
if(!$Output){$Output=Join-Path $project ('out/startup-tests/'+$namespace+'-logs')}
New-Item -ItemType Directory -Path $FixtureBase,$Output -Force|Out-Null
$root=Join-Path ([IO.Path]::GetFullPath($FixtureBase)) ($namespace+' 中文 app')
New-Item -ItemType Directory -Path $root|Out-Null
$runPath='Software\'+$namespace+'\Run'
function Check([bool]$ok,[string]$message){if(!$ok){throw $message}}
function Apply([string]$operation,[bool]$success=$true,[string]$path=(Join-Path $root 'PcTool.exe')){
 & $Executable $namespace $path $operation
 Check (($LASTEXITCODE -eq 0) -eq $success) "Unexpected startup $operation result: $LASTEXITCODE"
}
try{
 Copy-Item -LiteralPath $Executable -Destination (Join-Path $root 'PcTool.exe')
 $name=(& $Executable --name (Join-Path $root 'PcTool.exe')|Out-String).Trim()
 Check ($LASTEXITCODE -eq 0 -and $name -match '^PcTool-[A-F0-9]{64}$') 'Invalid per-install startup value name'
 Apply enable;Apply enable
 $key=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($runPath,$true)
 $expected='"'+$root+'\PcTool.exe" --background'
 Check ($key.GetValue($name) -ceq $expected -and $key.GetValueNames().Count -eq 1) 'Run command/readback/idempotence failure'
 $upper=(& $Executable --name ((Join-Path $root 'PcTool.exe').ToUpperInvariant())|Out-String).Trim()
 Check ($upper -eq $name) 'Path casing created duplicate startup identity'
 $key.SetValue('PcTool',$expected);$key.Dispose();Apply enable
 $key=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($runPath,$true)
 Check ($null -eq $key.GetValue('PcTool')) 'Owned legacy Run entry remains'
 $key.SetValue('PcTool','"C:\Other\PcTool.exe" --background');$key.Dispose()
 Apply disable;Apply disable
 $key=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($runPath,$true)
 Check ($null -eq $key.GetValue($name) -and $key.GetValue('PcTool') -eq '"C:\Other\PcTool.exe" --background') 'Disable touched foreign startup'
 $key.SetValue($name,'"C:\Other\PcTool.exe" --background');$key.Dispose()
 Apply enable $false;Apply disable $false
 $key=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($runPath,$true)
 Check ($key.GetValue($name) -eq '"C:\Other\PcTool.exe" --background') 'Conflicting Run entry changed'
 $key.DeleteValue($name);$key.Dispose()
 Apply enable $false (Join-Path $root 'missing.exe')
 'PASS ordinary per-install Run, Unicode paths, readback, idempotence, legacy migration and foreign-value preservation'
 & (Join-Path $root 'PcTool.exe') --exercise $namespace
 Check ($LASTEXITCODE -eq 0) 'Settings startup migration regression failed'
 if($ProductExecutable){
  $product=Join-Path $root 'product';New-Item -ItemType Directory -Path $product|Out-Null
  Copy-Item -LiteralPath (Resolve-Path -LiteralPath $ProductExecutable).Path -Destination (Join-Path $product 'PcTool.exe')
  $p=Start-Process -FilePath (Join-Path $product 'PcTool.exe') -ArgumentList @('--initialize-startup',$namespace) -WindowStyle Hidden -PassThru -Wait
  Check ($p.ExitCode -eq 0) 'Product startup entry failed'
  $sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
  $settings=Join-Path $product ('Data/'+$sid+'/settings.ini')
  $text=[IO.File]::ReadAllText($settings)
  Check ($text -match '(?m)^AutoStart=1\r?$' -and $text -match '(?m)^StartupSchema=2\r?$') 'Product did not persist normal startup migration'
  & "$project/packaging/lifecycle.ps1" -Action RemoveStartup -Root $product -Namespace $namespace
  Check ($LASTEXITCODE -eq 0) 'Owned startup cleanup failed'
  'PASS actual product startup entry and per-user preference persistence'
 }
 & "$project/packaging/lifecycle.ps1" -Action RemoveStartup -Root $root -Namespace $namespace
 Check ($LASTEXITCODE -eq 0) 'Isolated startup cleanup failed'
 $key=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($runPath)
 try{Check ($key.GetValue('PcTool') -eq '"C:\Other\PcTool.exe" --background') 'Cleanup removed foreign Run'}finally{$key.Dispose()}
}finally{
 [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree('Software\'+$namespace,$false)
 # Retain isolated evidence rather than recursively deleting fixture directories.
 "Evidence: $root"
}
