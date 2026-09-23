<#
==========================================================================================
 run_stale_log_test.ps1 - regression test for the 1.1.4 "judge only what the process writes"
 log-window fix
==========================================================================================
 Incident: the supervisor started a worldserver and reported

     [worldserver] pid 4732 finished startup after 1s; health rule: world-loop heartbeat

 while that same run needed 19 seconds ("WORLD: World Initialized In 0 Minutes 19 Seconds").
 Cause: StartServiceProcess() reset the log read offset to 0, so the FIRST health check read the
 whole pre-existing Server.log - and the previous run's "World Initialized In" and
 "Update time diff:" lines satisfied the startup rule (and the heartbeat rule) immediately. The
 startup timeout / startup stall guards therefore never protected a newly launched server.

 1.1.4 captures the pre-existing end of the log (plus a 32 byte fingerprint that proves the file
 was not rewritten in the meantime) and reads only what the new process appends.

 This test asserts:
   phase A  a fresh process whose log already contains a stale ready+heartbeat line is NOT reported
            as started; the startup timeout does its job instead
   phase B  the fix does not break a truncating appender (AzerothCore's default mode "w"): the fake
            truncates the seeded log and its own fresh ready line is still recognised, and only
            after it was really written

 Usage: pwsh -File tests\run_stale_log_test.ps1 [-SupervisorExe <path>] [-KeepArtifacts]
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

$runDir = Join-Path $testsDir 'run\stalelog'
if (Test-Path $runDir) { Remove-Item $runDir -Recurse -Force }

$script:results = @()
function Want([bool]$condition, [string]$label, [string]$detail = '') {
    $script:results += [pscustomobject]@{ Ok = $condition; Label = $label; Detail = $detail }
    $tag = if ($condition) { 'PASS' } else { 'FAIL' }
    $color = if ($condition) { 'Green' } else { 'Red' }
    Write-Host ("  {0}: {1}{2}" -f $tag, $label, $(if ($detail) { " :: $detail" } else { '' })) -ForegroundColor $color
}
function GuardText([string]$path) { if (Test-Path $path) { return (Get-Content $path -Raw) } return '' }
function WaitFor([scriptblock]$test, [int]$timeoutSec, [string]$what) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (& $test) { return $true }
        Start-Sleep -Milliseconds 250
    }
    Write-Host "  (timeout waiting for $what)" -ForegroundColor Yellow
    return $false
}
function Stop-Fakes {
    foreach ($p in @(Get-Process -Name 'fake_world' -ErrorAction SilentlyContinue)) {
        try { if ($p.Path -and ([IO.Path]::GetFullPath($p.Path)).StartsWith($runDir, 'OrdinalIgnoreCase')) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } } catch { }
    }
}

# ---------------------------------------------------------------- one phase = one clean run tree
# The log is seeded with the lines the supervisor keys on, exactly as a previous run of the same
# server would have left them behind (the fake appends nothing in mode "nolog").
function New-Phase {
    param([string]$Name, [string]$Mode, [int]$StartupSec, [int]$StartupTimeoutSec, [int]$SeedMinutes)

    $dir          = Join-Path $runDir $Name
    $workDir      = Join-Path $dir '80'
    $logsDir      = Join-Path $dir 'logs'
    $serverLog    = Join-Path $workDir 'logs\Server.log'
    $fakeCfg      = Join-Path $workDir 'world.cfg'
    $guardLog     = Join-Path $logsDir 'supervisor.log'
    $statusFile   = Join-Path $logsDir 'supervisor_status.json'
    $controlFile  = Join-Path $logsDir 'supervisor_control.txt'
    $ini          = Join-Path $dir 'supervisor.ini'

    New-Item -ItemType Directory -Path $workDir, (Join-Path $workDir 'logs'), $logsDir -Force | Out-Null
    Copy-Item $fakeSource (Join-Path $workDir 'fake_world.exe') -Force

    # the stale tail: a ready marker and a heartbeat line from the PREVIOUS run
    [IO.File]::WriteAllLines($serverLog, @(
        'AzerothCore rev. fake-build (Win64, Release, Static) (worldserver-daemon)',
        "<Ctrl-C> to stop.",
        "WORLD: World Initialized In 0 Minutes $SeedMinutes Seconds",
        'Update time diff: 12ms with 7 players online'
    ))

    [IO.File]::WriteAllLines($fakeCfg, @(
        'FAKE_ROLE=world',
        "FAKE_LOG=$serverLog",
        "FAKE_LEDGER=$(Join-Path $dir 'ledger.txt')",
        "FAKE_MODE=$Mode",
        "FAKE_STARTUP_SEC=$StartupSec",
        'FAKE_HEARTBEAT_SEC=1',
        'FAKE_STOP_SEC=60'
    ))

    [IO.File]::WriteAllLines($ini, @(
        '[general]',
        "GuardLog = $guardLog",
        "StatusFile = $statusFile",
        "ControlFile = $controlFile",
        "InstanceName = StaleLog_$Name",
        'ExitWhenAllStopped = false',
        'TickMs = 200',
        '',
        '[worldserver]',
        'Enabled = true',
        'Role = world',
        "WorkDir = $workDir",
        'Exe = fake_world.exe',
        "Args = --config `"$fakeCfg`"",
        "LogFile = $serverLog",
        'HeartbeatPattern = Update time diff:',
        'StartupReadyPattern = World Initialized In',
        # long enough that it cannot be the rule that fires in phase A: the startup timeout must be
        'HeartbeatTimeoutSeconds = 300',
        'HeartbeatTimeoutMode = strict',
        # NB: interpolate, never `'key = ' + $int` - the comma operator binds tighter than '+' and
        # would split the value into its own (useless) array element
        "StartupTimeoutSeconds = $StartupTimeoutSec",
        'StartupStallSeconds = 300',
        'StartupGraceSeconds = 1',
        'PollSeconds = 1',
        'StopGraceSeconds = 3',
        'RestartDelaySeconds = 2',
        'PlannedRestartDelaySeconds = 2',
        'MaxBackoffSeconds = 4',
        'StableRunSeconds = 60',
        'MaxRestartsPerHour = 0',
        'AutoRestartOnCleanExit = false',
        'AdoptExisting = false',
        'CpuCrossCheck = true',
        'RequireHeartbeat = true',
        'MaxWorkingSetMB = 0',
        'Console = shared',
        'ProbePort = 0',
        '',
        '[authserver]',
        'Enabled = false'
    ))

    return [pscustomobject]@{
        Name = $Name; Dir = $dir; Ini = $ini; GuardLog = $guardLog
        ServerLog = $serverLog; WorkDir = $workDir; FakeCfg = $fakeCfg
    }
}

function Start-Sup([object]$phase) {
    return Start-Process -FilePath $SupervisorExe -ArgumentList @('--config', $phase.Ini) `
        -WorkingDirectory (Split-Path -Parent $SupervisorExe) `
        -RedirectStandardOutput (Join-Path $phase.Dir 'supervisor.stdout.txt') `
        -RedirectStandardError (Join-Path $phase.Dir 'supervisor.stderr.txt') `
        -PassThru -WindowStyle Hidden
}

function Stop-Sup($proc) {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Seconds 1
    Stop-Fakes
}

Write-Host "stale-log regression test against $SupervisorExe" -ForegroundColor Cyan

$sup = $null
try {
    # ------------------------------------------------------------------ phase A: nolog fake
    # mode "nolog" never touches the file, so the log keeps exactly the stale content. Before the
    # fix the supervisor declared this server started after 1s; now nothing satisfies the rule and
    # the startup timeout has to fire.
    Write-Host "`n=== phase A: a fresh process that writes nothing (stale lines only) ===" -ForegroundColor Cyan
    $a = New-Phase -Name 'A' -Mode 'nolog' -StartupSec 1 -StartupTimeoutSec 6 -SeedMinutes 19
    $sup = Start-Sup $a
    $verdict = WaitFor { (GuardText $a.GuardLog) -match 'startup did not finish|finished startup' } 30 'the phase A verdict'
    $tA = GuardText $a.GuardLog
    Want $verdict 'the supervisor reached a startup verdict'
    Want ($tA -notmatch 'finished startup') 'the stale "World Initialized In" line did NOT satisfy the startup rule'
    Want ($tA -match 'startup did not finish within 6s') 'the startup timeout fired instead' `
         (($tA -split "`n" | Select-String 'startup did not finish' | Select-Object -First 1) -replace '\s+', ' ')
    Want ($tA -match '\[worldserver\] started pid \d+') 'the service was really launched'
    Stop-Sup $sup
    $sup = $null

    # ------------------------------------------------------------------ phase B: healthy fake
    # The fake opens its log with FileMode.Create (AzerothCore's Appender mode "w"): the seeded
    # content is truncated away, so the captured offset must be dropped and the fake's OWN ready
    # line awaited. The reported startup time proves it waited instead of trusting the stale one.
    Write-Host "`n=== phase B: truncating appender (mode w) writes a fresh log ===" -ForegroundColor Cyan
    $b = New-Phase -Name 'B' -Mode 'healthy' -StartupSec 3 -StartupTimeoutSec 20 -SeedMinutes 19
    $sup = Start-Sup $b
    $ready = WaitFor { (GuardText $b.GuardLog) -match 'finished startup' } 30 'phase B startup'
    $tB = GuardText $b.GuardLog
    Want $ready 'the fake''s own fresh ready line was recognised'
    $m = [regex]::Match($tB, 'finished startup after (\d+)s')
    $after = if ($m.Success) { [int]$m.Groups[1].Value } else { -1 }
    Want ($after -ge 2) 'it waited for the fresh line instead of the stale one' "finished startup after ${after}s"
    Want ($after -le 15) 'startup was still recognised promptly' "finished startup after ${after}s"
    Want ($tB -notmatch 'startup did not finish') 'no startup timeout was needed in phase B'
    $fresh = if (Test-Path $b.ServerLog) { Get-Content $b.ServerLog -Raw } else { '' }
    Want ($fresh -match 'World Initialized In 0 Minutes 3 Seconds') 'the log really is the fake''s own fresh content'
    Start-Sleep -Seconds 3
    Want ((GuardText $b.GuardLog) -notmatch 'restarting in') 'the healthy fake was left running'
    Stop-Sup $sup
    $sup = $null
}
finally {
    Stop-Sup $sup
    if (-not $KeepArtifacts -and (Test-Path $runDir)) {
        # keep the guard logs of a FAILING run for inspection
        if (($script:results | Where-Object { -not $_.Ok }).Count -eq 0) { Remove-Item $runDir -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

$failed = @($script:results | Where-Object { -not $_.Ok })
Write-Host ''
if ($failed.Count -eq 0) {
    Write-Host "RESULT: stale-log regression ALL PASS ($($script:results.Count) checks)" -ForegroundColor Green
    exit 0
}
Write-Host "RESULT: stale-log regression $($failed.Count) FAILURE(S) of $($script:results.Count) checks" -ForegroundColor Red
$failed | ForEach-Object { Write-Host "  - $($_.Label) $($_.Detail)" -ForegroundColor Red }
if (Test-Path $runDir) { Write-Host "  artifacts kept in $runDir" -ForegroundColor Yellow }
exit 1
