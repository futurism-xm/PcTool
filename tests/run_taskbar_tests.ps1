param([Parameter(Mandatory=$true)][string]$Executable,
      [Parameter(Mandatory=$true)][string]$Output)
$ErrorActionPreference = 'Stop'
$fixture = Join-Path $Output ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture -Force | Out-Null
$testExecutable = Join-Path $fixture 'PcToolTaskbarTests.exe'
Copy-Item -LiteralPath $Executable -Destination $testExecutable
New-Item -ItemType File -Path (Join-Path $fixture '.pctool-settings-fixture') | Out-Null
& $testExecutable
if ($LASTEXITCODE -ne 0) { throw "Taskbar tests failed ($LASTEXITCODE); fixture: $fixture" }
