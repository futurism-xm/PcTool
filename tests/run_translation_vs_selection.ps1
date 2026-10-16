param(
    [string]$Executable = "$PSScriptRoot/../out/build/audit-release/PcToolTranslationTests.exe",
    [string]$Output = "$PSScriptRoot/../out/translation-fix-20261015",
    [string]$VisualStudio,
    [switch]$PausePcTool
)
$ErrorActionPreference = 'Stop'
$Executable = (Resolve-Path -LiteralPath $Executable).Path
if (-not $VisualStudio) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $VisualStudio = & $vswhere -latest -products '*' -find 'Common7/IDE/devenv.exe' | Select-Object -First 1
    }
    if (-not $VisualStudio) { throw 'Visual Studio is unavailable; specify -VisualStudio with the devenv.exe path' }
}
$VisualStudio = (Resolve-Path -LiteralPath $VisualStudio).Path
New-Item -ItemType Directory -Path $Output -Force | Out-Null
$fixturePath = Join-Path (Resolve-Path -LiteralPath $Output).Path 'SelectionFixture.cs'
[IO.File]::WriteAllText($fixturePath, "// PcTool selection fixture`r`n// 中文取词 English 123", [Text.UTF8Encoding]::new($true))
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = $VisualStudio
# Without /Edit this opens a new instance, rather than reusing the user's IDE.
$start.Arguments = '"' + $fixturePath + '" /NoSplash /Command "Edit.SelectAll"'
$start.UseShellExecute = $false
$fixtureProcess = $null
$oldTrace = $env:PCTOOL_TRANSLATION_TRACE
$paused = @()
try {
    if ($PausePcTool) {
        foreach ($app in @(Get-Process PcTool -ErrorAction SilentlyContinue | Where-Object {$_.SessionId -eq (Get-Process -Id $PID).SessionId})) {
            $path = $app.Path
            if (-not $path) { throw 'Cannot verify running PcTool executable' }
            & $path --exit-existing
            if (-not $app.WaitForExit(10000)) { throw 'PcTool did not exit gracefully; refusing to terminate it' }
            $paused += $path
        }
    }
    $fixtureProcess = [Diagnostics.Process]::Start($start)
    $limit = [DateTime]::UtcNow.AddSeconds(60)
    do {
        $fixtureProcess.Refresh()
        if ($fixtureProcess.HasExited) { throw 'Isolated Visual Studio exited before the fixture was ready' }
        if ($fixtureProcess.MainWindowHandle -ne [IntPtr]::Zero -and $fixtureProcess.MainWindowTitle -like '*SelectionFixture*') { break }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $limit)
    $fixtureWindow = $fixtureProcess.MainWindowHandle.ToInt64()
    if (-not $fixtureWindow -or $fixtureProcess.MainWindowTitle -notlike '*SelectionFixture*') { throw 'Isolated Visual Studio fixture was not ready' }
    Write-Output "Isolated Visual Studio fixture: pid=$($fixtureProcess.Id), hwnd=$fixtureWindow"
    $env:PCTOOL_TRANSLATION_TRACE = '1'
    & $Executable selection-external $fixtureWindow
    $fixtureExit = $LASTEXITCODE
} finally {
    $env:PCTOOL_TRANSLATION_TRACE = $oldTrace
    if ($fixtureProcess) {
        if (-not $fixtureProcess.HasExited) { [void]$fixtureProcess.CloseMainWindow() }
        $fixtureProcess.Dispose()
    }
    foreach ($path in $paused) { Start-Process -FilePath $path -ArgumentList '--background' -WindowStyle Hidden }
}
exit $fixtureExit
