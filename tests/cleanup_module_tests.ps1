param([string]$Output,[string]$Library)
$ErrorActionPreference = 'Stop'
if (-not $Output) { $Output = Join-Path $PSScriptRoot '../out/cleanup-module-tests' }
$fixture = Join-Path ([IO.Path]::GetFullPath($Output)) ([guid]::NewGuid().ToString('N'))
$modules = Join-Path $fixture 'modules'
$fakeSecurity = Join-Path $modules 'Microsoft.PowerShell.Security'
New-Item -ItemType Directory -Path $fakeSecurity -Force | Out-Null
# Simulate a Core-only module inherited by a Windows PowerShell child process.
[IO.File]::WriteAllText((Join-Path $fakeSecurity 'Microsoft.PowerShell.Security.psd1'), "@{RootModule='Security.psm1';ModuleVersion='1.0';FunctionsToExport=@('Set-Acl','Get-Acl')}")
[IO.File]::WriteAllText((Join-Path $fakeSecurity 'Security.psm1'), "throw 'Simulated incompatible inherited security module'")
$probe = Join-Path $fixture 'probe.ps1'
$probeSource = @'
param([string]$Library,[string]$Modules,[string]$Output)
$ErrorActionPreference='Stop'
$env:PSModulePath=$Modules
. $Library
$file=Join-Path $Output 'acl-fixture.txt'
[IO.File]::WriteAllText($file,'module isolation fixture')
$acl=Get-Acl -LiteralPath $file
Set-Acl -LiteralPath $file -AclObject $acl
$command=Get-Command Set-Acl -ErrorAction Stop
$expected=Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Security\Microsoft.PowerShell.Security.psd1'
if([IO.Path]::GetFullPath($command.Module.Path) -ne [IO.Path]::GetFullPath($expected)){throw 'ACL cmdlets did not come from the Windows PowerShell system module'}
'PASS Windows PowerShell ACL cmdlets with an incompatible inherited module path'
'@
[IO.File]::WriteAllText($probe, $probeSource, [Text.UTF8Encoding]::new($true))
if (-not $Library) { $Library = Join-Path $PSScriptRoot '../packaging/cleanup-jobs.ps1' }
$Library = [IO.Path]::GetFullPath($Library)
$start=[Diagnostics.ProcessStartInfo]::new()
$start.FileName="$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe"
$start.Arguments='-NoProfile -ExecutionPolicy Bypass -File "'+$probe+'" -Library "'+$Library+'" -Modules "'+$modules+'" -Output "'+$fixture+'"'
$start.UseShellExecute=$false
$start.CreateNoWindow=$true
$start.EnvironmentVariables['PSModulePath']=$modules
$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
$process=[Diagnostics.Process]::Start($start)
try {
    $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    $stdout.GetAwaiter().GetResult()
    $errors=$stderr.GetAwaiter().GetResult();if($errors){Write-Output $errors}
    if ($process.ExitCode -ne 0) { throw "Cleanup module isolation regression failed: $fixture" }
} finally { $process.Dispose() }
