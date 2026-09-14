param([Parameter(Mandatory=$true)][string]$Binaries)
$ErrorActionPreference='Stop'
$project=Split-Path -Parent $PSScriptRoot
$Binaries=[IO.Path]::GetFullPath($Binaries)
$testId=[guid]::NewGuid().ToString('N')
$base=Join-Path $project "out/uninstall-installer-tests/$testId"
New-Item -ItemType Directory -Path $base -Force | Out-Null
$stage=(Get-ChildItem "$project/out/package-stage" -Directory|Sort-Object LastWriteTime -Descending|Select-Object -First 1).FullName
$namespace="Software\PcToolUninstallTest-$testId"
# Compile the production script with only external destinations isolated.
# This exercises real NSIS install/upgrade/uninstall without replacing live integration.
$script=[IO.File]::ReadAllText("$project/packaging/PcTool.nsi")
$script=$script.Replace('Software\PcToolInstaller',"$namespace\Installer")
$script=$script.Replace('Software\Microsoft\Windows\CurrentVersion\Uninstall\PcTool',"$namespace\Uninstall")
$script=$script.Replace('Software\Microsoft\Windows\CurrentVersion\App Paths\7zFM.exe',"$namespace\AppPath")
$script=$script.Replace('Software\Microsoft\Windows\CurrentVersion\Run',"$namespace\Run")
$script=$script.Replace('Software\Classes',"$namespace\Classes").Replace('Software\7-Zip',"$namespace\SevenZip")
$script=$script.Replace('$SMPROGRAMS\PcTool','$INSTDIR\test-shortcuts').Replace('$DESKTOP\PcTool.lnk','$INSTDIR\test-desktop.lnk')
$script=$script.Replace('..\Resources\PcTool.ico',"$project\Resources\PcTool.ico")
$script=$script.Replace('MUI_PAGE_LICENSE "license.txt"',('MUI_PAGE_LICENSE "'+$project+'\packaging\license.txt"'))
$script=$script.Replace('"manage-data.ps1"',('"'+$project+'\packaging\manage-data.ps1"'))
$script=$script.Replace('  Call ClosePcTool','  ; Isolated test must not close the live developer application.')
[IO.File]::WriteAllText("$base/test.nsi",$script,[Text.UTF8Encoding]::new($true))
& "$project/out/nsis/nsis-3.12/makensis.exe" /V2 "/DOUTPUT=$base/Setup.exe" "/DMANIFEST=$project/out/package-manifest" "/DSTAGE=$stage" "$base/test.nsi" *> "$base/compile.log"
if($LASTEXITCODE){throw "Isolated NSIS compilation failed: $base/compile.log"}
$clsid='{23170F69-40C1-278A-1000-000100020000}'
$keys=@()
$processes=@()
$lockedFile=$null
function Remove-TestPending {
 $queue=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Control\Session Manager',$true)
 try {
  $name='PendingFileRenameOperations';$entries=@($queue.GetValue($name,[string[]]@()))
  if($entries.Count % 2){throw 'Unexpected pending operation list'}
  $kept=[Collections.Generic.List[string]]::new()
  $prefix='\??\'+[IO.Path]::GetFullPath($base)+'\'
  for($i=0;$i -lt $entries.Count;$i+=2){if(!$entries[$i].StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){$kept.Add($entries[$i]);$kept.Add($entries[$i+1])}}
  if($kept.Count){$queue.SetValue($name,$kept.ToArray(),[Microsoft.Win32.RegistryValueKind]::MultiString)}else{$queue.DeleteValue($name,$false)}
 } finally {$queue.Dispose()}
}
try {
 foreach($previous in @($false,$true)){
  $label=if($previous){'previous-7zip'}else{'no-previous'}
  $destination=[IO.Path]::GetFullPath((Join-Path $base $label))
  foreach($view in @([Microsoft.Win32.RegistryView]::Registry64,[Microsoft.Win32.RegistryView]::Registry32)){
   $registry=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,$view)
   $keys+=,$registry
   if($previous){
    $key=$registry.CreateSubKey("$namespace\Classes\CLSID\$clsid\InprocServer32")
    $key.SetValue('','C:\Independent7Zip\7-zip.dll');$key.Dispose()
   }
  }
  foreach($iteration in 1,2){
   $setup=Start-Process -FilePath "$base/Setup.exe" -ArgumentList @('/S',"/D=$destination") -WindowStyle Hidden -PassThru -Wait
   if($setup.ExitCode -ne 0){throw "Install/upgrade failed: $($setup.ExitCode)"}
   if(!(Test-Path "$destination/.pctool-managed-files.json")){throw 'Installed inventory missing'}
   if((Test-Path "$destination/docs") -or (Test-Path "$destination/tests")){throw 'Development directories installed'}
  }
  [IO.File]::WriteAllText("$destination/keep.txt",'unrelated file')
  # A stubborn task holds a real hive, exercising forced stop from the actual uninstaller.
  Copy-Item "$Binaries/PcToolUninstallFixture.exe" "$destination/PcTool.exe" -Force
  $task=Start-Process -FilePath "$destination/PcTool.exe" -ArgumentList @(('"'+$destination+'"'),'stubborn') -WindowStyle Hidden -PassThru
  $processes+=,$task
  for($i=0;$i -lt 60 -and !(Test-Path "$destination/$($task.Id).ready");$i++){Start-Sleep -Milliseconds 50}
  if(!(Test-Path "$destination/$($task.Id).ready")){throw 'Task fixture failed'}
  $timer=[Diagnostics.Stopwatch]::StartNew()
  # Run a copy outside the installation with _?= to wait for the actual cleanup
  # process. NSIS's normal self-copy launcher can return before its child finishes.
  $uninstallRunner=Join-Path $base ("Uninstall-$label.exe")
  Copy-Item -LiteralPath "$destination/Uninstall.exe" -Destination $uninstallRunner
  $lockedFile=[IO.File]::Open("$destination/Data/external-lock.tmp",'Create','ReadWrite','None')
  $uninstall=Start-Process -FilePath $uninstallRunner -ArgumentList @('/S',"_?=$destination") -WindowStyle Hidden -PassThru -Wait
  if($uninstall.ExitCode -ne 3010){throw "Locked uninstall returned $($uninstall.ExitCode)"}
  foreach($retryFile in @('Uninstall.exe','PcToolUninstallHelper.exe','manage-data.ps1','.pctool-managed-files.json')){
   if(!(Test-Path (Join-Path $destination $retryFile))){throw "Retry file removed on incomplete cleanup: $retryFile"}
  }
  $lockedFile.Dispose();$lockedFile=$null;Remove-TestPending
  $uninstall=Start-Process -FilePath $uninstallRunner -ArgumentList @('/S',"_?=$destination") -WindowStyle Hidden -PassThru -Wait
  if($uninstall.ExitCode -ne 0){throw "Uninstall returned $($uninstall.ExitCode)"}
  for($i=0;$i -lt 100 -and (Test-Path "$destination/Uninstall.exe");$i++){Start-Sleep -Milliseconds 100}
  if(!$task.WaitForExit(1000) -or $task.ExitCode -ne 1602){throw 'Uninstaller did not force-stop the task'}
  foreach($path in @('PcTool.exe','PcToolUninstallHelper.exe','Uninstall.exe','Data','Cache','modules','ocr_models','ocr_licenses','licenses','.pctool-uninstalling','.pctool-managed-files.json','test-shortcuts','test-desktop.lnk')){
   if(Test-Path (Join-Path $destination $path)){throw "Uninstall residual: $path"}
  }
  if(!(Test-Path "$destination/keep.txt")){throw 'Unrelated file removed'}
  foreach($registry in $keys){
   $key=$registry.OpenSubKey("$namespace\Classes\CLSID\$clsid\InprocServer32")
   $actual=if($key){$value=$key.GetValue('');$key.Dispose();$value}else{''}
   if($previous -and $actual -ne 'C:\Independent7Zip\7-zip.dll'){throw 'Previous registration not restored'}
   if(!$previous -and $actual){throw 'Extension registration remains'}
   foreach($kind in @('Uninstall','Installer')){
    $key=$registry.OpenSubKey("$namespace\$kind")
    if($key){$key.Dispose();throw "Registration remains: $kind"}
   }
   $registry.DeleteSubKeyTree($namespace,$false)
  }
  "PASS isolated NSIS install, upgrade, forced uninstall, files and 32/64-bit registration restoration ($label), $([math]::Round($timer.Elapsed.TotalSeconds,2))s"
  foreach($key in $keys){$key.Dispose()};$keys=@()
 }
 "Evidence: $base"
} finally {
 if($lockedFile){$lockedFile.Dispose()}
 Remove-TestPending
 foreach($process in $processes){if(!$process.HasExited){$process.Kill();$process.WaitForExit()};$process.Dispose()}
 foreach($registry in $keys){$registry.DeleteSubKeyTree($namespace,$false);$registry.Dispose()}
}
