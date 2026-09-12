param([Parameter(Mandatory=$true)][string]$Setup)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (!( [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {throw 'Installer integration test requires an elevated test session'}
if (Test-Path 'HKLM:/Software/PcToolInstaller') {throw 'Test requires no existing installed PcTool; development build is allowed'}
$destination = Join-Path $root ('out/installer-tests/自定义 安装目录-' + [Guid]::NewGuid().ToString('N'))
$runKey='HKCU:/Software/Microsoft/Windows/CurrentVersion/Run'
$oldRun = Get-ItemPropertyValue $runKey PcTool -ErrorAction SilentlyContinue
$registration=@(
    'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes\CLSID\{23170F69-40C1-278A-1000-000100020000}\InprocServer32',
    'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes\WOW6432Node\CLSID\{23170F69-40C1-278A-1000-000100020000}\InprocServer32')
$before=@($registration | ForEach-Object {if(Test-Path -LiteralPath $_){(Get-Item -LiteralPath $_).GetValue('')}else{''}})
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class InstallerTestWindow {
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr LoadLibrary(string path);
 [DllImport("kernel32.dll")] public static extern bool FreeLibrary(IntPtr h);
}
'@
function RunSetup {
    $process=Start-Process -FilePath $Setup -ArgumentList @('/S',"/D=$destination") -WindowStyle Hidden -PassThru -Wait
    if($process.ExitCode -notin @(0,3010)){throw "Installer returned $($process.ExitCode)"}
    if(!(Test-Path -LiteralPath "$destination/PcTool.exe")){throw 'Custom installation path ignored'}
}
RunSetup
$expected="$destination\modules\archive"
foreach($i in 0,1){
    $dll=if($i -eq 0){'7-zip.dll'}else{'7-zip32.dll'}
    if((Get-Item -LiteralPath $registration[$i]).GetValue('') -ne "$expected\$dll"){throw "Shell registration mismatch ($i)"}
}
if(!(Test-Path -LiteralPath "$destination/licenses/7zip-source.zip")){throw 'Source archive missing'}
$sourceCheck=Join-Path ([IO.Path]::GetTempPath()) ('PcTool-source-check-'+[Guid]::NewGuid().ToString('N'))
& "$destination/modules/archive/7z.exe" x "$destination/licenses/7zip-source.zip" "-o$sourceCheck" -y | Out-Null
if($LASTEXITCODE -ne 0){throw 'Source archive extraction failed'}
$sourceFiles=@(Get-ChildItem -LiteralPath "$root/third_party/7zip" -Recurse -File -Force | Where-Object {$_.FullName -notmatch '\\CPP\\7zip\\.*\\(x64|x86)\\'})
if(@(Get-ChildItem -LiteralPath "$sourceCheck/7zip" -Recurse -File -Force).Count -ne $sourceFiles.Count){throw 'Packaged source file count mismatch'}
$sourcePrefix=(Join-Path $root 'third_party/7zip').Length+1
$sha=[Security.Cryptography.SHA256]::Create()
try {
    foreach($file in $sourceFiles){
        $packaged=Join-Path "$sourceCheck/7zip" $file.FullName.Substring($sourcePrefix)
        $expected=[BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($file.FullName)))
        $actual=[BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($packaged)))
        if($actual -ne $expected){throw "Packaged source mismatch: $packaged"}
    }
} finally {$sha.Dispose()}
Write-Output "PASS: all $($sourceFiles.Count) original source files match the packaged source archive, including assembly sources."
if((Get-Content -LiteralPath "$destination/ocr_models/MODEL_VERSION.txt" -Raw) -notmatch 'PP-OCRv6'){throw 'Model changed during packaging'}
[IO.File]::WriteAllText("$destination/user-sentinel.txt",'must survive upgrade and uninstall')
New-Item -ItemType Directory -Force "$destination/user-recordings" | Out-Null
[IO.File]::WriteAllText("$destination/user-recordings/retained.mp4",'unrelated user data')
$mappedExtension=[InstallerTestWindow]::LoadLibrary("$destination/modules/archive/7-zip.dll")
if($mappedExtension -eq [IntPtr]::Zero){throw 'Shell extension failed to load'}
try { RunSetup } finally { [InstallerTestWindow]::FreeLibrary($mappedExtension) | Out-Null }
if((Get-Content -LiteralPath "$destination/user-sentinel.txt") -ne 'must survive upgrade and uninstall'){throw 'Upgrade altered user data'}
& "$PSScriptRoot/archive_integration.ps1" -Runtime "$destination/modules/archive"
if($LASTEXITCODE -ne 0){throw 'Installed archive runtime test failed'}
# The PcTool entry must launch the bundled original manager, with no external 7-Zip dependency.
$managerBefore=@(Get-Process 7zFM -ErrorAction SilentlyContinue | ForEach-Object Id)
$launcher=Start-Process -FilePath "$destination/PcTool.exe" -ArgumentList '--archive' -WindowStyle Hidden -PassThru
if(!$launcher.WaitForExit(10000)){throw 'PcTool archive launcher did not exit'}
$manager=$null
for($attempt=0;$attempt -lt 50;$attempt++){
    $manager=Get-Process 7zFM -ErrorAction SilentlyContinue | Where-Object { $_.Id -notin $managerBefore -and $_.Path -eq "$destination\modules\archive\7zFM.exe" } | Select-Object -First 1
    if($manager -and $manager.MainWindowHandle -ne 0){break}
    Start-Sleep -Milliseconds 100
}
if(!$manager -or !$manager.MainWindowHandle){throw 'Bundled file manager UI did not open'}
Write-Output "Original UI: $($manager.MainWindowTitle) [$($manager.Path)]"
[InstallerTestWindow]::PostMessage($manager.MainWindowHandle,0x10,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null
if(!$manager.WaitForExit(10000)){throw 'Manager did not close'}
$process=Start-Process -FilePath "$destination/Uninstall.exe" -ArgumentList '/S' -WindowStyle Hidden -PassThru -Wait
if($process.ExitCode -notin @(0,3010)){throw "Uninstall returned $($process.ExitCode)"}
if(Test-Path -LiteralPath "$destination/PcTool.exe"){throw 'Application not removed'}
if(!(Test-Path -LiteralPath "$destination/user-sentinel.txt") -or !(Test-Path -LiteralPath "$destination/user-recordings/retained.mp4")){throw 'Uninstaller deleted unrelated data'}
if(Test-Path 'HKLM:/Software/Microsoft/Windows/CurrentVersion/Uninstall/PcTool'){throw 'Uninstall registration remains'}
foreach($i in 0,1){
    $actual=if(Test-Path -LiteralPath $registration[$i]){(Get-Item -LiteralPath $registration[$i]).GetValue('')}else{''}
    if($actual -ne $before[$i]){throw "Previous shell registration not restored ($i): $actual"}
}
$newRun=Get-ItemPropertyValue $runKey PcTool -ErrorAction SilentlyContinue
if($newRun -ne $oldRun){throw 'Uninstaller changed a different PcTool startup entry'}
Write-Output "PASS: Unicode/space custom path, both shell extensions, original UI, packaged OCR/source, upgrade, functional archives, uninstall and user-file preservation. Evidence: $destination"
