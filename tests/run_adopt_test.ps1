<#
==========================================================================================
 run_adopt_test.ps1 - regression test for the two production defects of 2026-09-23
==========================================================================================
 Incident: the supervisor adopted a hand-started worldserver (pid 7360), saw the world-loop
 heartbeat stop, could not stop the process (the adopted handle had no PROCESS_TERMINATE and
 CTRL_BREAK cannot reach a foreign console), and STILL started a second worldserver (pid 2204)
 next to the surviving one - two servers on the same ports and database.

 This test reproduces that path with the fake server and asserts:
   1. the adopted process is really killed (stop result "forced", not "failed"/"STILL RUNNING")
   2. the supervisor never runs two instances of the same exe at the same time
   3. the replacement process is owned (in the job object): killing the supervisor kills it
   4. a foreign instance that appears before a restart is ADOPTED, never duplicated

 Usage: pwsh -File tests\run_adopt_test.ps1 [-SupervisorExe <path>] [-KeepArtifacts]
==========================================================================================
#>
[CmdletBinding()]
param(
    [string]$SupervisorExe = '',
    [switch]$KeepArtifacts
)

$ErrorActionPreference = 'Stop'
$testsDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoDir = Split-Path -Parent $testsDir

if ([string]::IsNullOrWhiteSpace($SupervisorExe)) {
    $SupervisorExe = Join-Path $repoDir '..\..\release\supervisor\acore_supervisor.exe'
}
$SupervisorExe = [IO.Path]::GetFullPath($SupervisorExe)
$fakeSource = Join-Path $testsDir 'fake_world.exe'
if (-not (Test-Path $fakeSource)) { throw "missing $fakeSource - run tests\build_fakes.bat first" }
if (-not (Test-Path $SupervisorExe)) { throw "missing supervisor: $SupervisorExe" }

$runDir = Join-Path $testsDir 'run\adopt'
if (Test-Path $runDir) { Remove-Item $runDir -Recurse -Force }
$workDir = Join-Path $runDir '80'
$logDir = Join-Path $runDir 'logs'
New-Item -ItemType Directory -Path $workDir, (Join-Path $workDir 'logs'), $logDir -Force | Out-Null
$fakeExe = Join-Path $workDir 'fake_world.exe'
Copy-Item $fakeSource $fakeExe -Force

$serverLog = Join-Path $workDir 'logs\Server.log'
$guardLog = Join-Path $logDir 'supervisor.log'
$statusFile = Join-Path $logDir 'supervisor_status.json'
$controlFile = Join-Path $logDir 'supervisor_control.txt'
$supOut = Join-Path $logDir 'supervisor.stdout.txt'
$ledger = Join-Path $runDir 'ledger.txt'

$ini = Join-Path $runDir 'supervisor.ini'
@"
[general]
GuardLog        = $guardLog
StatusFile      = $statusFile
ControlFile     = $controlFile
InstanceName    = AdoptTest
ExitWhenAllStopped = false
TickMs          = 200

[worldserver]
Enabled                  = true
Role                     = world
WorkDir                  = $workDir
Exe                      = fake_world.exe
LogFile                  = $serverLog
HeartbeatPattern         = Update time diff:
StartupReadyPattern      = World Initialized In
HeartbeatTimeoutSeconds  = 5
LogStallSeconds          = 5
StartupTimeoutSeconds    = 15
StartupStallSeconds      = 15
StartupGraceSeconds      = 1
PollSeconds              = 2
StopGraceSeconds         = 2
RestartDelaySeconds      = 5
MaxBackoffSeconds        = 20
StableRunSeconds         = 5
AutoRestartOnCleanExit   = false
AdoptExisting            = true
CpuCrossCheck            = true
MaxWorkingSetMB          = 0
Console                  = shared

[authserver]
Enabled                  = false
"@ | Set-Content -Path $ini -Encoding ASCII

$script:results = @()
function Want([bool]$condition, [string]$label, [string]$detail = '') {
    $script:results += [pscustomobject]@{ Ok = $condition; Label = $label; Detail = $detail }
    $tag = if ($condition) { 'PASS' } else { 'FAIL' }
    $color = if ($condition) { 'Green' } else { 'Red' }
    Write-Host ("  {0}: {1}{2}" -f $tag, $label, $(if ($detail) { " :: $detail" } else { '' })) -ForegroundColor $color
}
function GuardText { if (Test-Path $guardLog) { return (Get-Content $guardLog -Raw) } return '' }
function WaitFor([scriptblock]$test, [int]$timeoutSec, [string]$what) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (& $test) { return $true }
        Start-Sleep -Milliseconds 250
    }
    Write-Host "  (timeout waiting for $what)" -ForegroundColor Yellow
    return $false
}
function Instances([string]$path) {
    @(Get-Process -Name 'fake_world' -ErrorAction SilentlyContinue | Where-Object {
        try { $_.Path -and ([IO.Path]::GetFullPath($_.Path) -ieq $path) } catch { $false }
    })
}
function Start-Fake([string]$mode, [int]$stopSec, [int]$heartbeatSec) {
    $env:FAKE_ROLE = 'world'
    $env:FAKE_MODE = $mode
    $env:FAKE_LOG = $serverLog
    $env:FAKE_LEDGER = $ledger
    $env:FAKE_STARTUP_SEC = '1'
    $env:FAKE_HEARTBEAT_SEC = "$heartbeatSec"
    $env:FAKE_STOP_SEC = "$stopSec"
    # -WindowStyle Hidden still gives the fake its OWN console: exactly the "started by hand"
    # situation in which GenerateConsoleCtrlEvent(CTRL_BREAK) cannot be delivered.
    return Start-Process -FilePath $fakeExe -WorkingDirectory $workDir -WindowStyle Hidden -PassThru
}
function Start-Sup {
    return Start-Process -FilePath $SupervisorExe -ArgumentList @('--config', $ini) -WorkingDirectory (Split-Path -Parent $SupervisorExe) `
        -RedirectStandardOutput $supOut -RedirectStandardError (Join-Path $logDir 'supervisor.stderr.txt') -PassThru -WindowStyle Hidden
}

$sup = $null
$foreign = $null
$maxConcurrent = 0
try {
    Write-Host "`n=== phase 1: adopt a hand-started server that then freezes ===" -ForegroundColor Cyan
    $manual = Start-Fake -mode 'hang' -stopSec 3 -heartbeatSec 1
    Start-Sleep -Seconds 2
    Want ($manual.HasExited -eq $false) 'hand-started fake is running' "pid $($manual.Id)"

    $sup = Start-Sup
    Want (WaitFor { (GuardText) -match 'adopting the already running pid' } 20 'the adoption') 'the running process is adopted'

    $stopped = WaitFor { (GuardText) -match 'stop result: (forced|graceful)' } 40 'the stop of the adopted process'
    $text = GuardText
    Want $stopped 'the adopted process could really be stopped' ($text -split "`n" | Select-String 'stop result' | Select-Object -Last 1)
    Want ($text -notmatch 'STILL RUNNING') 'no "STILL RUNNING after the stop" was logged'
    Want ($text -notmatch 'STILL RUNNING.*refusing to start a second instance') 'no duplicate guard had to fire'
    Want ($manual.HasExited -or -not (Get-Process -Id $manual.Id -ErrorAction SilentlyContinue)) 'the adopted pid is gone after the stop'

    # watching the whole transition: two instances of the same exe must never exist together
    $restarted = WaitFor {
        $live = Instances $fakeExe
        if ($live.Count -gt $script:maxConcurrent) { $script:maxConcurrent = $live.Count }
        ($live.Count -eq 1 -and $live[0].Id -ne $manual.Id)
    } 40 'a replacement instance'
    Want $restarted 'a replacement process was started'
    Want ($maxConcurrent -le 1) 'never more than one instance of this exe at a time' "max concurrent $maxConcurrent"

    $live = Instances $fakeExe
    $newPid = if ($live.Count -gt 0) { $live[0].Id } else { 0 }
    Want ($newPid -gt 0 -and $newPid -ne $manual.Id) 'the replacement has a different pid' "old $($manual.Id), new $newPid"
    Want ((GuardText) -match ("started pid " + $newPid)) 'the replacement start was logged' "pid $newPid"

    # the replacement is owned: killing the supervisor must kill it (job object)
    Stop-Process -Id $sup.Id -Force
    Start-Sleep -Seconds 3
    $sup = $null
    Want ((Instances $fakeExe).Count -eq 0) 'killing the supervisor also killed the process it started (job object)'

    Write-Host "`n=== phase 2: a foreign instance must be adopted, never duplicated ===" -ForegroundColor Cyan
    # keep the phase 1 log: phase 2 starts from a clean one
    Copy-Item $guardLog (Join-Path $logDir 'supervisor.phase1.log') -Force -ErrorAction SilentlyContinue
    Copy-Item $supOut (Join-Path $logDir 'supervisor.phase1.stdout.txt') -Force -ErrorAction SilentlyContinue
    Remove-Item $guardLog -Force -ErrorAction SilentlyContinue
    $sup = Start-Sup
    $started = WaitFor { (GuardText) -match 'started pid (\d+)' } 20 'the first supervised start'
    Want $started 'the supervisor starts its own instance'
    $supPid = 0
    if ((GuardText) -match 'started pid (\d+)') { $supPid = [int]$Matches[1] }

    $foreign = Start-Fake -mode 'healthy' -stopSec 600 -heartbeatSec 1
    Start-Sleep -Seconds 1
    Want ((Instances $fakeExe).Count -eq 2) 'the foreign instance runs next to the supervised one' "supervised $supPid, foreign $($foreign.Id)"

    # restart through the control channel: the stop succeeds, then the guard must not start a third one
    "id=adopt-test`naction=restart`ntarget=worldserver" | Set-Content -Path $controlFile -Encoding ASCII
    $adopted = WaitFor { (GuardText) -match 'adopting it instead of starting a second instance' } 30 'the duplicate guard'
    Want $adopted 'a foreign instance is adopted instead of starting a second one'
    Start-Sleep -Seconds 2
    $live = Instances $fakeExe
    Want ($live.Count -eq 1) 'exactly one instance is left after the restart request' "alive: $(($live | ForEach-Object { $_.Id }) -join ',')"
    Want (($live | ForEach-Object { $_.Id }) -contains $foreign.Id) 'the surviving instance is the foreign (adopted) one'
}
finally {
    if ($sup) { Stop-Process -Id $sup.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 500
    foreach ($p in Instances $fakeExe) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    if (-not $KeepArtifacts) { Start-Sleep -Milliseconds 500 }
}

Write-Host ''
$failed = @($script:results | Where-Object { -not $_.Ok }).Count
Write-Host ("RESULT: adopt scenario {0} ({1} checks)" -f $(if ($failed -eq 0) { 'ALL PASS' } else { "$failed FAILURE(S)" }), $script:results.Count) `
    -ForegroundColor $(if ($failed -eq 0) { 'Green' } else { 'Red' })
Write-Host "artifacts: $runDir"
exit $failed
