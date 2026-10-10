param(
    [int]$HealthySeconds = 120
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$portalDev = Join-Path $repo 'src\PortalDev\bin\x64\Release\PortalDev.exe'
$gameDirectory = Join-Path $repo 'game\dev\GTA San Andreas'
$game = Join-Path $gameDirectory 'gta-sa.EXE'
$log = Join-Path $gameDirectory 'portal.log'
$dump = Join-Path $gameDirectory 'PortalSA-crash.dmp'
$dumpDirectory = Join-Path $repo 'artifacts\crash-dumps'

if (Get-Process -Name PortalDev, 'gta-sa' -ErrorAction SilentlyContinue) {
    throw 'PortalDev or GTA SA is already running. Close both before an automated diagnostic run.'
}

if (Test-Path -LiteralPath $dump) {
    $archivedDump = Join-Path $gameDirectory ('PortalSA-crash-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.dmp')
    Move-Item -LiteralPath $dump -Destination $archivedDump
}

$startTime = Get-Date
$devProcess = Start-Process -FilePath $portalDev -ArgumentList '--auto-run-sa' -WorkingDirectory (Split-Path -Parent $portalDev) -PassThru
Start-Sleep -Seconds 1
if ($devProcess.HasExited) {
    throw "PortalDev exited during startup with code $($devProcess.ExitCode)."
}

$gameProcess = $null
$gameStartDeadline = (Get-Date).AddSeconds(10)
while (-not $gameProcess -and (Get-Date) -lt $gameStartDeadline) {
    Start-Sleep -Milliseconds 200
    $gameProcess = Get-Process -Name 'gta-sa' -ErrorAction SilentlyContinue |
        Sort-Object StartTime -Descending |
        Select-Object -First 1
}
if (-not $gameProcess) {
    throw 'PortalDev did not launch GTA SA within 10 seconds.'
}
Write-Host "DIAGNOSTIC_STARTED portalDevPid=$($devProcess.Id) gtaSaPid=$($gameProcess.Id) at=$($startTime.ToString('o'))"

$deadline = $startTime.AddSeconds($HealthySeconds)
while (-not $gameProcess.HasExited -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
    $gameProcess.Refresh()
}

if ($gameProcess.HasExited) {
    Write-Host "GTA_SA_EXITED code=$($gameProcess.ExitCode) at=$((Get-Date).ToString('o'))"
    if (Test-Path -LiteralPath $log) {
        $matches = Select-String -LiteralPath $log -Pattern '^PortalSA loaded:'
        $last = $matches | Select-Object -Last 1
        if ($last) {
            Write-Host "LAST_STARTUP_LINE=$($last.LineNumber)"
            Get-Content -LiteralPath $log | Select-Object -Skip ($last.LineNumber - 1)
        }
    }
    if (Test-Path -LiteralPath $dump) {
        Get-Item -LiteralPath $dump | Select-Object FullName,Length,LastWriteTime
    }
    Get-ChildItem -LiteralPath $dumpDirectory -Filter '*.dmp' -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -ge $startTime } |
        Sort-Object LastWriteTime -Descending |
        Select-Object FullName,Length,LastWriteTime
    exit 2
}

Write-Host "GTA_SA_STILL_RUNNING seconds=$HealthySeconds pid=$($gameProcess.Id)"
exit 0
