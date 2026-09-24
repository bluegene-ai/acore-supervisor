# run_scenario.ps1 - end-to-end driver for acore_supervisor.exe against controlable fakes
param(
    [Parameter(Mandatory = $true)][string]$Scenario,
    [int]$Seconds = 45,
    # path of the supervisor executable under test; defaults to
    # <repo>\..\..\release\supervisor\acore_supervisor.exe (the layout build.bat deploys to)
    [string]$SupervisorExe = '',
    # route the control-channel commands through the AGMP panel PHP code instead of writing
    # the command file here (proves the real panel -> supervisor path); pass -PhpExe and
    # -PhpVerifier when your panel checkout is somewhere else
    [switch]$UsePhp,
    [string]$PhpExe = '',
    [string]$PhpVerifier = ''
)

$ErrorActionPreference = 'Continue'
$base     = $PSScriptRoot
$repo     = Split-Path $PSScriptRoot -Parent
$runRoot  = Join-Path $base "run\$Scenario"
if (-not $SupervisorExe) { $SupervisorExe = Join-Path $repo '..\..\release\supervisor\acore_supervisor.exe' }
$resolved = $null
try { $resolved = (Resolve-Path -LiteralPath $SupervisorExe -ErrorAction Stop).Path } catch { }
$supExe   = if ($resolved) { $resolved } else { $SupervisorExe }
$psExe    = (Get-Process -Id $PID).Path
$pidFile  = Join-Path $base 'sup.pid'
$authPort = 43724

if ($UsePhp) {
    if (-not $PhpExe -or -not (Test-Path $PhpExe)) {
        Write-Host '[driver] -UsePhp needs -PhpExe <php.exe> (and usually -PhpVerifier)' -ForegroundColor Red
        exit 2
    }
    if (-not $PhpVerifier -or -not (Test-Path $PhpVerifier)) {
        Write-Host '[driver] -UsePhp needs -PhpVerifier <AGMP>/tools/verify_supervisor.php' -ForegroundColor Red
        exit 2
    }
}

Write-Host "[driver] supervisor under test: $supExe"

function Stop-Stragglers {
    foreach ($n in 'worldserver', 'authserver', 'fake_child') {
        Get-Process -Name $n -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path $pidFile) {
        $old = 0
        if ([int]::TryParse((Get-Content $pidFile -Raw).Trim(), [ref]$old)) {
            $p = Get-Process -Id $old -ErrorAction SilentlyContinue
            if ($p -and $p.ProcessName -eq 'acore_supervisor') { Stop-Process -Id $old -Force -ErrorAction SilentlyContinue }
        }
        Remove-Item $pidFile -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Milliseconds 800
}

Stop-Stragglers
Get-ChildItem env: | Where-Object { $_.Name -like 'FAKE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" -ErrorAction SilentlyContinue }
Remove-Item $runRoot -Recurse -Force -ErrorAction SilentlyContinue

$worldDir = Join-Path $runRoot '80'
$authDir  = Join-Path $runRoot 'auth'
$supDir   = Join-Path $runRoot 'sup'
foreach ($d in @($worldDir, (Join-Path $worldDir 'configs'), (Join-Path $worldDir 'logs'),
                 $authDir,  (Join-Path $authDir 'configs'),  (Join-Path $authDir 'logs'),
                 $supDir,   (Join-Path $supDir 'logs'))) {
    New-Item -ItemType Directory -Path $d -Force | Out-Null
}

Copy-Item (Join-Path $base 'fake_world.exe') (Join-Path $worldDir 'worldserver.exe') -Force
Copy-Item (Join-Path $base 'fake_auth.exe')  (Join-Path $authDir  'authserver.exe') -Force

# server-side configs: the supervisor reads LogsDir + appender name from these (LogFile = auto).
# MinRecordUpdateTimeDiff = 0 is REQUIRED for the heartbeat scenarios: the world-loop heartbeat
# line is only written when a world tick was slower than MinRecordUpdateTimeDiff
# (UpdateTime.cpp:165), so at the AzerothCore default of 100ms an idle worldserver legitimately
# writes no line at all - and the supervisor now (correctly) refuses to judge health on a
# heartbeat the server config proves cannot be produced on time. Without this line the
# healthy/hang/chatty scenarios would be testing the log-activity fallback, not the heartbeat rule.
[System.IO.File]::WriteAllLines((Join-Path $worldDir 'configs\worldserver.conf'), @(
    '[worldserver]',
    'LogsDir = "logs"',
    'Appender.Server=2,5,0,Server.log,w',
    'RecordUpdateTimeDiffInterval = 60000',
    'MinRecordUpdateTimeDiff = 0'
))
[System.IO.File]::WriteAllLines((Join-Path $authDir 'configs\authserver.conf'), @(
    '[authserver]',
    'LogsDir = "logs"',
    'Appender.Auth=2,5,0,Auth.log,w',
    "RealmServerPort = $authPort"
))

# ---------------------------------------------------------------- scenario environment
$authEnabled = 'true'
$worldMode = 'healthy'
$childExe = ''
switch ($Scenario) {
    'healthy'    { $worldMode = 'healthy' }
    'chatty'     { $worldMode = 'chatty' }
    'hang'       { $worldMode = 'hang' }
    'crash'      { $worldMode = 'crash' }
    'clean'      { $worldMode = 'clean' }
    'planned'    { $worldMode = 'planned' }
    'authwedged' { $worldMode = 'healthy'; $authMode = 'silent' }
    'authdisabled' { $worldMode = 'healthy'; $authEnabled = 'false' }
    'child'      { $worldMode = 'chatty'; $childExe = Join-Path $base 'fake_child.exe' }
    'console'    { $worldMode = 'healthy' }
    'consoleclose' { $worldMode = 'healthy' }
    'control'    { $worldMode = 'healthy' }
    'once'       { $worldMode = 'healthy' }
    default      { Write-Host "unknown scenario $Scenario"; exit 2 }
}
if (-not $authMode) { $authMode = 'healthy' }

# Counts fake-server lifecycle events in a ledger. Defined at the top level: the per-scenario blocks
# below use it (a function defined inside one scenario's block only exists once that block ran).
function Count-Ledger([string]$Path, [string]$Pattern) {
    if (-not (Test-Path $Path)) { return 0 }
    return @(Get-Content $Path | Where-Object { $_ -match $Pattern }).Count
}

# per-service settings live in config files so the tracked process IS the fake server itself
# (exactly like the real deployment, where the tracked process is worldserver.exe)
$worldLog = Join-Path $worldDir 'logs\Server.log'
$authLog  = Join-Path $authDir 'logs\Auth.log'
$worldLedger = Join-Path $runRoot 'world_ledger.txt'
$authLedger  = Join-Path $runRoot 'auth_ledger.txt'

$worldCfg = Join-Path $worldDir 'world.cfg'
[System.IO.File]::WriteAllLines($worldCfg, @(
    'FAKE_ROLE=world',
    "FAKE_LOG=$worldLog",
    "FAKE_LEDGER=$worldLedger",
    "FAKE_MODE=$worldMode",
    'FAKE_STARTUP_SEC=2',
    'FAKE_HEARTBEAT_SEC=3',
    'FAKE_STOP_SEC=10',
    'FAKE_EXIT_CODE=7',
    "FAKE_CHILD_EXE=$childExe",
    'FAKE_CHILD_LIFETIME=600'
))
$authCfg = Join-Path $authDir 'auth.cfg'
[System.IO.File]::WriteAllLines($authCfg, @(
    'FAKE_ROLE=auth',
    "FAKE_LOG=$authLog",
    "FAKE_LEDGER=$authLedger",
    "FAKE_MODE=$authMode",
    "FAKE_PORT=$authPort"
))

$ini = Join-Path $supDir 'supervisor.ini'
$authEnabledLine = "Enabled = $authEnabled"
[System.IO.File]::WriteAllLines($ini, @(
    '[general]',
    'GuardLog = logs\supervisor.log',
    'StatusFile = logs\supervisor_status.json',
    'ControlFile = logs\supervisor_control.txt',
    "InstanceName = Test_$Scenario",
    'TickMs = 300',
    '',
    '[worldserver]',
    'Enabled = true',
    'Role = world',
    "WorkDir = $worldDir",
    'Exe = worldserver.exe',
    "Args = --config `"$worldCfg`"",
    'LogFile = auto',
    'ServerConf = configs\worldserver.conf',
    'HeartbeatPattern = Update time diff:',
    'StartupReadyPattern = World Initialized In',
    'HeartbeatTimeoutSeconds = 9',
    # the fake worldserver.conf declares RecordUpdateTimeDiffInterval = 60000, but these scenarios
    # deliberately want a 9 s rule to prove detection inside a few seconds - so opt out of the
    # "never shorter than the server's own cadence" auto adjustment (1.1.2)
    'HeartbeatTimeoutMode = strict',
    'StartupTimeoutSeconds = 40',
    'StartupStallSeconds = 30',
    'StartupGraceSeconds = 20',
    'PollSeconds = 3',
    'StopGraceSeconds = 6',
    'RestartDelaySeconds = 3',
    'PlannedRestartDelaySeconds = 3',
    'MaxBackoffSeconds = 8',
    'StableRunSeconds = 30',
    'AutoRestartOnCleanExit = false',
    'AdoptExisting = false',
    'CpuCrossCheck = true',
    'RequireHeartbeat = true',
    'Console = shared',
    'ProbePort = 0',
    '',
    '[authserver]',
    $authEnabledLine,
    'Role = auth',
    "WorkDir = $authDir",
    'Exe = authserver.exe',
    "Args = --config `"$authCfg`"",
    'LogFile = auto',
    'ServerConf = configs\authserver.conf',
    'HeartbeatPattern =',
    'StartupReadyPattern =',
    'StartupTimeoutSeconds = 30',
    'StartupGraceSeconds = 10',
    'PollSeconds = 3',
    'StopGraceSeconds = 6',
    'RestartDelaySeconds = 3',
    ('ProbePort = ' + $authPort),
    'ProbeMode = auth',
    'ProbeTimeoutMs = 1500',
    'ProbeFailuresBeforeRestart = 2',
    'AdoptExisting = false',
    'Console = shared'
))

# ---------------------------------------------------------------- run
if ($Scenario -eq 'once') {
    Write-Host "[driver] acore_supervisor --once with $ini"
    $once = & $supExe --config $ini --once 2>&1
    Write-Host '================ --once output ================' -ForegroundColor Cyan
    $once | ForEach-Object { Write-Host "  $_" }
    $joinedOnce = ($once -join "`n")
    $fails = @()
    function WantOnce([bool]$c, [string]$w) { if ($c) { Write-Host "PASS: $w" -ForegroundColor Green } else { Write-Host "FAIL: $w" -ForegroundColor Red; $script:fails += $w } }
    WantOnce ($joinedOnce -match 'worldserver\.exe') 'worldservice exe resolved'
    WantOnce ($joinedOnce -match 'logfile=.*80\\logs\\Server\.log.*auto') 'LogFile=auto resolved to the real Server.log'
    WantOnce ($joinedOnce -match 'logfile=.*auth\\logs\\Auth\.log') 'auth LogFile=auto resolved to Auth.log'
    WantOnce ($joinedOnce -match 'heartbeat pattern: Update time diff:') 'world heartbeat pattern active'
    if ($fails.Count -eq 0) { Write-Host "RESULT: once ALL PASS" -ForegroundColor Green } else { Write-Host "RESULT: once $($fails.Count) FAILURE(S)" -ForegroundColor Red }
    Stop-Stragglers
    exit 0
}

Write-Host "[driver] scenario=$Scenario seconds=$Seconds launching supervisor..."
$sup = Start-Process -FilePath $supExe -ArgumentList @('--config', $ini) -PassThru -WindowStyle Hidden
Set-Content -Path $pidFile -Value $sup.Id

if ($Scenario -eq 'console' -or $Scenario -eq 'consoleclose') {
    $ctrlMode = if ($Scenario -eq 'consoleclose') { 'close' } else { 'break' }
    Start-Sleep -Seconds 12
    Write-Host "[driver] driving the console-control path (mode=$ctrlMode)"
    $out = & $psExe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $base 'send_ctrl.ps1') -TargetPid $sup.Id -Mode $ctrlMode 2>&1
    $out | ForEach-Object { Write-Host "  $_" }
    Start-Sleep -Seconds 12
} elseif ($Scenario -eq 'control') {
    # ---------------------------------------------------------------- panel -> supervisor contract
    # exactly what the AGMP panel does: write the command file atomically and poll the status file
    $statusPath = Join-Path $supDir 'logs\supervisor_status.json'
    $controlPath = Join-Path $supDir 'logs\supervisor_control.txt'

    function Send-SupervisorCommand {
        param([string]$Id, [string]$Action, [string]$Target)
        if ($UsePhp) {
            $raw = & $PhpExe $PhpVerifier --e2e "--status-file=$statusPath" "--control-file=$controlPath" "--action=$Action" "--target=$Target" 2>&1 |
                Select-Object -Last 1
            try { $obj = $raw | ConvertFrom-Json } catch { $obj = $null }
            if ($null -eq $obj) {
                Write-Host ("  -> php dispatch FAILED (no json): $raw") -ForegroundColor Red
                return ''
            }
            Write-Host ("  -> php dispatch action=$Action target=$Target success=$($obj.success) id=$($obj.id)")
            return [string]$obj.id
        }
        $tmp = "$controlPath.tmp"
        [System.IO.File]::WriteAllLines($tmp, @("id=$Id", "action=$Action", "target=$Target"))
        Move-Item -Force $tmp $controlPath
        Write-Host ("  -> sent id=$Id action=$Action target=$Target")
        return $Id
    }
    function Wait-SupervisorCommand {
        param([string]$Id, [int]$TimeoutSec = 20)
        $deadline = (Get-Date).AddSeconds($TimeoutSec)
        while ((Get-Date) -lt $deadline) {
            if (Test-Path $statusPath) {
                try {
                    $j = Get-Content $statusPath -Raw | ConvertFrom-Json
                    if ($j.lastCommand -and $j.lastCommand.id -eq $Id) { return $j.lastCommand }
                } catch { }
            }
            Start-Sleep -Milliseconds 300
        }
        return $null
    }
    function Get-Status {
        try { return (Get-Content $statusPath -Raw | ConvertFrom-Json) } catch { return $null }
    }
    function Get-ServiceStatus([string]$Name) {
        $s = Get-Status
        if (-not $s) { return $null }
        return @($s.services | Where-Object { $_.name -eq $Name })[0]
    }
    # read the ledgers directly: the variables used by the evidence block are filled in later
    # (Count-Ledger itself is defined at the top level, it is shared with other scenarios)

    $controlResults = [ordered]@{}
    Write-Host '[driver] waiting for both services to finish startup'
    Start-Sleep -Seconds 14

    Write-Host '[driver] step 1: ping'
    $id = Send-SupervisorCommand -Id 'c1' -Action 'ping' -Target 'all'
    $controlResults['ping'] = Wait-SupervisorCommand -Id $id

    Write-Host '[driver] step 2: restart worldserver'
    $worldStartsBefore = Count-Ledger $worldLedger '\[world\] START'
    $id = Send-SupervisorCommand -Id 'c2' -Action 'restart' -Target 'worldserver'
    $controlResults['restart'] = Wait-SupervisorCommand -Id $id
    Start-Sleep -Seconds 8
    $worldStartsAfterRestart = Count-Ledger $worldLedger '\[world\] START'

    Write-Host '[driver] step 3: stop authserver'
    $authStartsBefore = Count-Ledger $authLedger '\[auth\] START'
    $id = Send-SupervisorCommand -Id 'c3' -Action 'stop' -Target 'authserver'
    $controlResults['stop'] = Wait-SupervisorCommand -Id $id
    Start-Sleep -Seconds 8
    $authStatusStopped = Get-ServiceStatus 'authserver'
    $authStartsWhileStopped = Count-Ledger $authLedger '\[auth\] START'

    Write-Host '[driver] step 4: start authserver again'
    $id = Send-SupervisorCommand -Id 'c4' -Action 'start' -Target 'authserver'
    $controlResults['start'] = Wait-SupervisorCommand -Id $id
    Start-Sleep -Seconds 8
    $authStartsAfterStart = Count-Ledger $authLedger '\[auth\] START'

    Write-Host '[driver] step 5: unknown action must be rejected'
    if ($UsePhp) {
        # the panel validates before it writes anything, so the supervisor never sees it
        $raw = & $PhpExe $PhpVerifier --e2e "--status-file=$statusPath" "--control-file=$controlPath" --action=explode --target=worldserver 2>&1 | Select-Object -Last 1
        try { $obj = $raw | ConvertFrom-Json } catch { $obj = $null }
        $controlResults['rejected'] = [pscustomobject]@{
            id = ''
            result = if ($obj -and -not $obj.success) { 'rejected' } else { 'accepted' }
            message = if ($obj) { [string]$obj.message } else { "no json: $raw" }
        }
        Write-Host ("  -> php rejected=$($controlResults['rejected'].result) :: $($controlResults['rejected'].message)")
    } else {
        $id = Send-SupervisorCommand -Id 'c5' -Action 'explode' -Target 'worldserver'
        $controlResults['rejected'] = Wait-SupervisorCommand -Id $id
    }
    $controlStillPresent = Test-Path $controlPath
    if ($controlStillPresent) { Remove-Item $controlPath -Force -ErrorAction SilentlyContinue }

    Write-Host '[driver] step 6: shutdown the supervisor'
    $id = Send-SupervisorCommand -Id 'c6' -Action 'shutdown' -Target 'all'
    $controlResults['shutdown'] = Wait-SupervisorCommand -Id $id
    $sup.WaitForExit(20000) | Out-Null

    $controlVars = [ordered]@{
        worldStartsBefore = $worldStartsBefore
        worldStartsAfterRestart = $worldStartsAfterRestart
        authStartsBefore = $authStartsBefore
        authStartsWhileStopped = $authStartsWhileStopped
        authStartsAfterStart = $authStartsAfterStart
        authStateStopped = if ($authStatusStopped) { $authStatusStopped.state } else { '' }
        authStoppedByUser = if ($authStatusStopped) { [bool]$authStatusStopped.stoppedByUser } else { $false }
        rejectedNotExecuted = -not $controlStillPresent
    }
} elseif ($Scenario -eq 'authdisabled') {
    # ---- a service this supervisor does not run must be REFUSED, never silently started ----------
    # One supervisor per realm: a shared authserver is Enabled = false here, and a command naming it
    # used to reach StartService() - one click on the wrong realm's page (or a hand-written control
    # file) would start a SECOND authserver next to the one another supervisor owns. "ping" stays
    # allowed: it only asks whether the control channel is alive.
    $statusPath = Join-Path $supDir 'logs\supervisor_status.json'
    $controlPath = Join-Path $supDir 'logs\supervisor_control.txt'
    function Send-AuthCmd {
        param([string]$Id, [string]$Action, [string]$Target)
        $tmp = "$controlPath.tmp"
        [System.IO.File]::WriteAllLines($tmp, @("id=$Id", "action=$Action", "target=$Target"))
        Move-Item -Force $tmp $controlPath
        $deadline = (Get-Date).AddSeconds(20)
        while ((Get-Date) -lt $deadline) {
            if (Test-Path $statusPath) {
                try { $j = Get-Content $statusPath -Raw | ConvertFrom-Json } catch { $j = $null }
                if ($j -and $j.lastCommand -and $j.lastCommand.id -eq $Id) { return $j.lastCommand }
            }
            Start-Sleep -Milliseconds 300
        }
        return $null
    }

    Write-Host '[driver] waiting for the worldservice to finish startup'
    Start-Sleep -Seconds 16
    $disabledStatus = @((Get-Content $statusPath -Raw | ConvertFrom-Json).services | Where-Object { $_.name -eq 'authserver' })[0]

    Write-Host '[driver] step 1: every command naming the disabled service must be rejected'
    $adRestart = Send-AuthCmd -Id 'ad1' -Action 'restart' -Target 'authserver'
    $adStart = Send-AuthCmd -Id 'ad2' -Action 'start' -Target 'authserver'
    $adStop = Send-AuthCmd -Id 'ad3' -Action 'stop' -Target 'authserver'
    $adShutdown = Send-AuthCmd -Id 'ad4' -Action 'shutdown' -Target 'authserver'
    $adPing = Send-AuthCmd -Id 'ad5' -Action 'ping' -Target 'authserver'
    Write-Host ("  -> restart={0} start={1} stop={2} shutdown={3} ping={4}" -f `
        $(if ($adRestart) { $adRestart.result } else { 'none' }), $(if ($adStart) { $adStart.result } else { 'none' }), `
        $(if ($adStop) { $adStop.result } else { 'none' }), $(if ($adShutdown) { $adShutdown.result } else { 'none' }), `
        $(if ($adPing) { $adPing.result } else { 'none' }))
    $adAliveAfterRejections = -not $sup.HasExited

    Write-Host '[driver] step 2: a broadcast still restarts the service it owns'
    $adWorldBefore = Count-Ledger $worldLedger '\[world\] START'
    $adBroadcast = Send-AuthCmd -Id 'ad6' -Action 'restart' -Target 'all'
    Start-Sleep -Seconds 8
    $adWorldAfter = Count-Ledger $worldLedger '\[world\] START'
    $adAuthStarts = Count-Ledger $authLedger '\[auth\] START'

    $authDisabledVars = [ordered]@{
        reportedEnabled = if ($disabledStatus) { $disabledStatus.enabled } else { $null }
        restartResult = if ($adRestart) { [string]$adRestart.result } else { '' }
        restartMessage = if ($adRestart) { [string]$adRestart.message } else { '' }
        startResult = if ($adStart) { [string]$adStart.result } else { '' }
        stopResult = if ($adStop) { [string]$adStop.result } else { '' }
        shutdownResult = if ($adShutdown) { [string]$adShutdown.result } else { '' }
        pingResult = if ($adPing) { [string]$adPing.result } else { '' }
        broadcastResult = if ($adBroadcast) { [string]$adBroadcast.result } else { '' }
        supervisorAlive = $adAliveAfterRejections
        worldStartsBefore = $adWorldBefore
        worldStartsAfter = $adWorldAfter
        authStarts = $adAuthStarts
    }
} else {
    Start-Sleep -Seconds $Seconds
}

# ---------------------------------------------------------------- evidence
$wdAlive = -not $sup.HasExited
$wProcs = @(Get-Process -Name worldserver -ErrorAction SilentlyContinue)
$aProcs = @(Get-Process -Name authserver -ErrorAction SilentlyContinue)
$cProcs = @(Get-Process -Name fake_child -ErrorAction SilentlyContinue)
$wLedger = if (Test-Path $worldLedger) { Get-Content $worldLedger } else { @() }
$aLedger = if (Test-Path $authLedger) { Get-Content $authLedger } else { @() }
$guard = if (Test-Path (Join-Path $supDir 'logs\supervisor.log')) { Get-Content (Join-Path $supDir 'logs\supervisor.log') } else { @() }
$status = if (Test-Path (Join-Path $supDir 'logs\supervisor_status.json')) { Get-Content (Join-Path $supDir 'logs\supervisor_status.json') -Raw } else { '' }
$joined = ($guard -join "`n")

$wStarts = @($wLedger | Where-Object { $_ -match '\[world\] START' }).Count
$aStarts = @($aLedger | Where-Object { $_ -match '\[auth\] START' }).Count
$wGrace = @($wLedger | Where-Object { $_ -match '\[world\] GRACEFUL' }).Count
$aGrace = @($aLedger | Where-Object { $_ -match '\[auth\] GRACEFUL' }).Count

Write-Host ''
Write-Host '================ DRIVER EVIDENCE ================' -ForegroundColor Cyan
Write-Host ("supervisor alive     : {0} (exit {1})" -f $wdAlive, $(if ($wdAlive) { 'n/a' } else { $sup.ExitCode }))
Write-Host ("fake_world alive     : {0}" -f (($wProcs | ForEach-Object { $_.Id }) -join ','))
Write-Host ("fake_auth alive      : {0}" -f (($aProcs | ForEach-Object { $_.Id }) -join ','))
Write-Host ("fake_child alive     : {0}  <- must be empty after a job kill" -f (($cProcs | ForEach-Object { $_.Id }) -join ','))
Write-Host ("world START/GRACEFUL : {0} / {1}" -f $wStarts, $wGrace)
Write-Host ("auth  START/GRACEFUL : {0} / {1}" -f $aStarts, $aGrace)
Write-Host '--- supervisor.log ---'
$guard | ForEach-Object { Write-Host "  $_" }

$fails = @()
function Want([bool]$c, [string]$w) {
    if ($c) { Write-Host "PASS: $w" -ForegroundColor Green }
    else { Write-Host "FAIL: $w" -ForegroundColor Red; $script:fails += $w }
}

switch ($Scenario) {
    'healthy' {
        Want ($joined -match '\[worldserver\] pid \d+ finished startup') 'worldserver startup recognised'
        Want ($joined -match '\[authserver\] pid \d+ finished startup') 'authserver startup recognised'
        Want ($joined -notmatch '\[STALL\]') 'no hang was reported'
        Want ($wStarts -eq 1 -and $aStarts -eq 1) 'both services started exactly once'
        Want ($status -match '"heartbeatSeen": true') 'world-loop heartbeat was observed'
        Want ($status -match '"probeOk": true') 'auth probe succeeded'
    }
    'chatty' {
        Want ($joined -match 'world-loop heartbeat missing') 'frozen world loop detected from the heartbeat line alone'
        Want ($joined -match 'stop result: graceful') 'frozen server stopped gracefully (CTRL_BREAK honoured)'
        Want ($wGrace -ge 1) 'the fake recorded the graceful shutdown'
        Want ($wStarts -ge 2) 'frozen server was restarted'
        Want ($aStarts -eq 1) 'authserver was left alone'
        Want ($wProcs.Count -eq 1 -and $aProcs.Count -eq 1) 'exactly one world + one auth instance are running'
    }
    'hang' {
        Want ($joined -match 'world-loop heartbeat missing|no log activity') 'silent hang detected'
        Want ($wStarts -ge 2) 'hung server was restarted'
        Want ($wGrace -ge 1) 'graceful stop was used'
    }
    'crash' {
        Want ($joined -match 'abnormal exit \(code 7') 'exit code 7 treated as abnormal'
        Want ($wStarts -ge 2) 'crashed server was restarted'
        Want ($joined -match 'consecutive failures 2|consecutive failures 1') 'failure counter engaged'
        Want ($aStarts -eq 1) 'authserver unaffected by the worldserver crash'
    }
    'clean' {
        Want ($joined -match 'clean shutdown \(exit code 0\)') 'exit code 0 treated as a clean stop'
        Want ($wStarts -eq 1) 'clean exit did not restart the worldserver'
        Want ($aProcs.Count -eq 1) 'authserver keeps running'
        Want ($wdAlive) 'supervisor stays alive for the remaining service'
    }
    'authwedged' {
        Want ($joined -match 'probe failed 2/2') 'wedged authserver detected by the probe'
        Want ($aStarts -ge 2) 'wedged authserver was restarted'
        Want ($aGrace -ge 1) 'graceful stop used for the authserver'
        Want ($wStarts -eq 1) 'worldserver untouched'
    }
    'planned' {
        Want ($joined -match 'planned restart requested \(exit code 2\)') 'exit code 2 seen as a planned restart'
        Want ($joined -match 'consecutive failures 0') 'a planned restart is not counted as a failure'
        Want ($wStarts -ge 2) 'server was restarted after the planned restart'
        Want ($aStarts -eq 1) 'authserver untouched'
    }
    'child' {
        Want ($wStarts -ge 2) 'worldserver was restarted (heartbeat rule)'
        Want ($cProcs.Count -eq $wProcs.Count) 'no orphaned child: every live child belongs to a live worldserver'
        Want ($joined -match 'world-loop heartbeat missing') 'the restart was driven by the heartbeat rule'
    }
    'console' {
        Want ($joined -match 'Ctrl\+C received') 'console control event reached the supervisor'
        Want (-not $wdAlive) 'supervisor shut down'
        Want ($sup.ExitCode -eq 0) 'supervisor exit code 0'
        Want ($wGrace -ge 1 -and $aGrace -ge 1) 'both servers were stopped gracefully'
        Want ($wProcs.Count -eq 0 -and $aProcs.Count -eq 0) 'no server process was left behind'
    }
    'consoleclose' {
        Want ($joined -match 'console closing / session ending') 'window close (WM_CLOSE) reached the supervisor handler'
        Want (-not $wdAlive) 'supervisor shut down after the window closed'
        Want ($wGrace -ge 1 -and $aGrace -ge 1) 'both servers got the graceful shutdown signal'
        Want ($wProcs.Count -eq 0 -and $aProcs.Count -eq 0) 'no orphaned server after closing the window'
    }
    'authdisabled' {
        Write-Host '--- disabled service (shared authserver) results ---' -ForegroundColor Cyan
        Write-Host ("  reported enabled                  : {0}" -f $authDisabledVars.reportedEnabled)
        Write-Host ("  restart/start/stop/shutdown       : {0} / {1} / {2} / {3}" -f $authDisabledVars.restartResult, $authDisabledVars.startResult, $authDisabledVars.stopResult, $authDisabledVars.shutdownResult)
        Write-Host ("  ping                              : {0} (allowed on purpose)" -f $authDisabledVars.pingResult)
        Write-Host ("  broadcast restart                 : {0}, world starts {1} -> {2}, auth starts {3}" -f $authDisabledVars.broadcastResult, $authDisabledVars.worldStartsBefore, $authDisabledVars.worldStartsAfter, $authDisabledVars.authStarts)
        Write-Host ("  refusal message                   : {0}" -f $authDisabledVars.restartMessage)

        Want ($authDisabledVars.reportedEnabled -eq $false) 'the ini-disabled service is reported as enabled=false'
        Want ($authDisabledVars.restartResult -eq 'rejected') 'restart of a disabled service is rejected'
        Want ($authDisabledVars.restartMessage -match 'disabled in this supervisor') 'the refusal says why'
        Want ($authDisabledVars.startResult -eq 'rejected') 'start of a disabled service is rejected'
        Want ($authDisabledVars.stopResult -eq 'rejected') 'stop of a disabled service is rejected'
        Want ($authDisabledVars.shutdownResult -eq 'rejected') 'shutdown of a disabled service is rejected'
        Want ($authDisabledVars.supervisorAlive) 'the supervisor is still alive after a shutdown aimed at a disabled service'
        Want ($authDisabledVars.pingResult -eq 'ok') 'ping still answers (control-channel check, no service touched)'
        Want ($authDisabledVars.authStarts -eq 0) 'the disabled service was never started'
        Want ($aProcs.Count -eq 0) 'no process of the disabled service exists'
        Want ($authDisabledVars.broadcastResult -eq 'ok') 'a broadcast is still accepted'
        Want ($authDisabledVars.worldStartsAfter -gt $authDisabledVars.worldStartsBefore) 'the broadcast restarted the service this supervisor owns'
        Want ($joined -match 'disabled in this supervisor') 'the refusal is in the supervisor log'
    }
    'control' {
        Write-Host '--- control channel results ---' -ForegroundColor Cyan
        foreach ($k in $controlResults.Keys) {
            $c = $controlResults[$k]
            if ($c) { Write-Host ("  {0,-9} id={1} result={2} :: {3}" -f $k, $c.id, $c.result, $c.message) }
            else { Write-Host ("  {0,-9} <no response>" -f $k) -ForegroundColor Red }
        }
        Write-Host ("  world starts before/after restart : {0} -> {1}" -f $controlVars.worldStartsBefore, $controlVars.worldStartsAfterRestart)
        Write-Host ("  auth starts before/stopped/after  : {0} -> {1} -> {2}" -f $controlVars.authStartsBefore, $controlVars.authStartsWhileStopped, $controlVars.authStartsAfterStart)
        Write-Host ("  auth state/stoppedByUser after stop: {0} / {1}" -f $controlVars.authStateStopped, $controlVars.authStoppedByUser)

        Want ($controlResults['ping'] -and $controlResults['ping'].result -eq 'ok') 'ping answered through the control file'
        Want ($controlResults['restart'] -and $controlResults['restart'].result -eq 'ok') 'restart accepted'
        Want ($controlVars.worldStartsAfterRestart -gt $controlVars.worldStartsBefore) 'restart worldserver really restarted it'
        Want ($wGrace -ge 1) 'the restarted worldserver was stopped gracefully'
        Want ($controlResults['stop'] -and $controlResults['stop'].result -eq 'ok') 'stop accepted'
        Want ($controlVars.authStateStopped -eq 'stopped') 'authserver reports state=stopped'
        Want ($controlVars.authStoppedByUser) 'authserver reports stoppedByUser=true'
        Want ($controlVars.authStartsWhileStopped -eq $controlVars.authStartsBefore) 'a stopped service is not restarted automatically'
        Want ($controlResults['start'] -and $controlResults['start'].result -eq 'ok') 'start accepted'
        Want ($controlVars.authStartsAfterStart -gt $controlVars.authStartsWhileStopped) 'start brought the authserver back'
        Want ($controlResults['rejected'] -and $controlResults['rejected'].result -eq 'rejected') 'unknown action is rejected, not executed'
        Want ($controlVars.rejectedNotExecuted) 'a rejected command is not left behind in the control file'
        Want ($controlResults['shutdown'] -and $controlResults['shutdown'].result -eq 'ok') 'shutdown accepted'
        Want (-not $wdAlive) 'supervisor exited after the shutdown command'
        Want ($sup.ExitCode -eq 0) 'supervisor exit code 0'
        Want ($wProcs.Count -eq 0 -and $aProcs.Count -eq 0) 'no server left running after shutdown'
        Want ($aGrace -ge 1) 'both servers were stopped gracefully on shutdown'
    }
}

Write-Host ''
if ($fails.Count -eq 0) { Write-Host "RESULT: $Scenario ALL PASS" -ForegroundColor Green }
else { Write-Host "RESULT: $Scenario $($fails.Count) FAILURE(S)" -ForegroundColor Red }

Stop-Stragglers
