param()
$ErrorActionPreference='Stop'
$project=Split-Path -Parent $PSScriptRoot
$fixture=Join-Path $project ('out/install-pending-tests/'+[guid]::NewGuid().ToString('N'))
$root=Join-Path $fixture 'app'
$namespace='PcToolTest-'+[guid]::NewGuid().ToString('N')
New-Item -ItemType Directory -Path $root -Force | Out-Null
$manifest=Join-Path $fixture 'manifest.json'
@{Version=1;Files=@('PcTool.exe','modules/archive/7-zip.dll');Directories=@('modules','modules/archive')} | ConvertTo-Json | Set-Content -LiteralPath $manifest -Encoding UTF8
$queue=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Control\Session Manager',$true)
$name='PendingFileRenameOperations'
function Read-Queue {return @($queue.GetValue($name,[string[]]@()))}
function Write-Queue($entries){
 if($entries.Count){$queue.SetValue($name,[string[]]$entries,[Microsoft.Win32.RegistryValueKind]::MultiString)}else{$queue.DeleteValue($name,$false)}
}
function Run-Record([bool]$success){
 $ErrorActionPreference='Continue'
 & "$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$project/packaging/manage-data.ps1" -Action Record -Root $root -Manifest $manifest -CleanupNamespace $namespace *> "$fixture/record.log"
 $result=$LASTEXITCODE
 $ErrorActionPreference='Stop'
 if(($result -eq 0) -ne $success){throw "Unexpected Record result: see $fixture/record.log"}
}
try{
 $original=@(Read-Queue)
 if($original.Count % 2){throw 'Invalid existing pending list'}
 $preserved=@(("\??\$root\personal.txt"),'',("\??\$fixture\app-other\PcTool.exe"),'',("\??\$fixture\other.tmp"),("\??\$fixture\other.new"),("\??\$root\modules\archive\7-zip.dll.123.old"),'')
 $removed=@(("\??\$root\modules\archive\7-zip.dll"),'',("\\?\$root\PCTOOL.EXE"),'',("\??\$root\Data\SevenZip\settings.hiv"),'',("\??\$root\Cache\2026-09-01\test.png"),'',("\??\$root"),'')
 Write-Queue ($original+$preserved+$removed)
 Run-Record $true
 $actual=@(Read-Queue)
 if(($actual -join "`0") -cne (($original+$preserved) -join "`0")){throw 'Removal scope or preserved pair order incorrect'}
 Run-Record $true
 if((@(Read-Queue) -join "`0") -cne ($actual -join "`0")){throw 'Not idempotent'}
 'PASS exact owned deletes, Data/Cache, root, native path forms; unrelated files, sibling installs, rename pairs and old DLL cleanup preserved; repeated install'
 $conflict=@(("\??\$fixture\replacement.dll"),("!\??\$root\modules\archive\7-zip.dll"))
 Write-Queue ($actual+$conflict)
 Run-Record $false
 if((@(Read-Queue) -join "`0") -cne (($actual+$conflict) -join "`0")){throw 'Conflicting rename modified'}
 'PASS conflicting replacement stops installation without changing queue'
}finally{
 $entries=@(Read-Queue);$kept=[Collections.Generic.List[string]]::new()
 if($entries.Count % 2){throw 'Invalid pending pairs; refusing cleanup'}
 for($i=0;$i -lt $entries.Count;$i+=2){
  if($entries[$i].IndexOf($fixture,[StringComparison]::OrdinalIgnoreCase) -lt 0){$kept.Add($entries[$i]);$kept.Add($entries[$i+1])}
 }
 Write-Queue $kept.ToArray();$queue.Dispose()
}
