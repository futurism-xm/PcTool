param(
 [Parameter(Mandatory=$true)][string]$Runtime,
 [Parameter(Mandatory=$true)][string]$Executable,
 [ValidateSet('7-zip.dll','7-zip32.dll')][string]$DllName='7-zip.dll'
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version 2
$project=Split-Path -Parent $PSScriptRoot
$source=(Get-Item -LiteralPath (Join-Path $Runtime $DllName)).FullName
$Executable=(Get-Item -LiteralPath $Executable).FullName
function File-Hash([string]$Path){
 $stream=[IO.File]::OpenRead($Path);$sha=[Security.Cryptography.SHA256]::Create()
 try{return [BitConverter]::ToString($sha.ComputeHash($stream))}
 finally{$sha.Dispose();$stream.Dispose()}
}
$sourceHash=File-Hash $source
# Never exercise destructive lifecycle modes against the supplied runtime.
# A unique fixture also permits concurrent test runs without shared state.
$fixture=Join-Path $project ('out/archive-shell-lifecycle/'+[guid]::NewGuid().ToString('N'))
$installed=Join-Path $fixture 'installed'
$installedRuntime=Join-Path $installed 'modules/archive'
# modules/archive is now the durable PcTool identity after its executable and
# marker disappear. A truly standalone 7-Zip layout has no such product path.
$standaloneRuntime=Join-Path $fixture 'standalone/runtime'
[void](New-Item -ItemType Directory -Path $installedRuntime,$standaloneRuntime)
[IO.File]::WriteAllText((Join-Path $installed 'PcTool.exe'),'Inert archive shell regression fixture')
Copy-Item -LiteralPath $source -Destination (Join-Path $installedRuntime $DllName)
# The normal probe defaults to this filename; the copied PE retains its bitness.
Copy-Item -LiteralPath $source -Destination (Join-Path $standaloneRuntime '7-zip.dll')
function Run-Probe([string]$Name,[string[]]$Arguments){
 $log=Join-Path $fixture ($Name+'.log')
 $preference=$ErrorActionPreference;$ErrorActionPreference='Continue'
 try{& $Executable @Arguments *> $log;$code=$LASTEXITCODE}
 finally{$ErrorActionPreference=$preference}
 Get-Content -LiteralPath $log
 if($code -ne 0){throw "Archive shell $Name failed ($code); evidence: $fixture"}
}
Run-Probe 'standalone' @($standaloneRuntime)
foreach($mode in @('uninstall-guard','uninstall-root-guard','uninstall-command-guard')){
 Run-Probe $mode @($installedRuntime,$mode,$DllName)
 if(!(Test-Path -LiteralPath (Join-Path $installed 'PcTool.exe')) -or
    (Test-Path -LiteralPath (Join-Path $installed '.pctool-uninstalling'))){throw 'Lifecycle test did not restore its fixture'}
}
if((File-Hash $source) -ne $sourceHash){throw 'Source runtime changed during lifecycle tests'}
Write-Output "PASS standalone and cached COM uninstall/reinstall lifecycle ($DllName). Evidence: $fixture"
