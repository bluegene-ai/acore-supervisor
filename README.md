# acore_supervisor - native supervisor for AzerothCore worldserver + authserver

A single Win32 executable (~270 KB, no runtime dependencies) that starts, watches and restarts
both servers, needs **no change to AzerothCore sources**, and keeps the **visible GM console**.
It can be used standalone or together with the
[Acore GM Panel](https://github.com/bluegene-ai/AcoreGMPanel) "Supervisor" page.

## Repository layout

```
acore_supervisor.cpp     the supervisor (single translation unit, Win32 + STL only)
build.bat                MSVC build -> ..\..\release\supervisor\acore_supervisor.exe
supervisor.ini           sample configuration (copied next to the exe when it is missing)
start_supervisor.bat     launcher (double click / Task Scheduler / startup folder)
tests/                   controllable fake worldserver/authserver + end-to-end driver
```

`build.bat` deploys into a release tree that looks like this:

```
release/
  supervisor/
    acore_supervisor.exe      the supervisor
    supervisor.ini            all settings (relative paths resolve against this file's folder)
    start_supervisor.bat      launcher
    logs/supervisor.log       its own log (rotated)
    logs/supervisor_status.json  live state, machine readable (panel / monitoring)
    logs/supervisor_control.txt  command drop box (panel -> supervisor)
  80/                         worldserver deployment (exe, configs, Data, logs)
  auth/                       authserver deployment
```

## Quick start

```
git clone https://github.com/<you>/acore-supervisor.git
cd acore-supervisor
build.bat                                   :: -> ..\..\release\supervisor\acore_supervisor.exe
copy supervisor.ini ..\..\release\supervisor\
..\..\release\supervisor\acore_supervisor.exe --config ..\..\release\supervisor\supervisor.ini --once
```

`--once` prints the resolved executable/log paths, checks that the world-loop heartbeat line is
present in `Server.log` and runs the auth probe - without starting anything. Then edit
`supervisor.ini` (`WorkDir`, `LogFile`, `ProbePort`) and start it with `start_supervisor.bat`
(or a Task Scheduler task "At log on", see section 4).

## 1. What it does

| | mechanism | why it matters |
|---|---|---|
| process ownership | `CreateProcess` + **Job Object** (`KILL_ON_JOB_CLOSE`) + IO completion port | death is an *event*, not a poll; the whole process tree is killed together; if the supervisor itself dies, Windows kills the servers with it - no orphans |
| worldserver health | the **world loop's own heartbeat line** in `Server.log`: `Update time diff: Nms with N players online` (`World::Update` -> `WorldUpdateTime::RecordUpdateTime`) | a frozen world loop cannot be hidden behind a chatty background logger; CPU-time progress is used as a cross-check |
| authserver health | TCP connect to `RealmServerPort` (3724) + one `AUTH_LOGON_CHALLENGE` for a random **non-existent** account | the real authserver logs almost nothing, so the log is useless as a signal. The probe proves acceptor + packet handler + login DB work, and produces **no** Auth.log line, no failed-login counter and no WrongPass ban (`AuthSession.cpp:322-330`) |
| stop order | `GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT)` to the server's own process group (= AzerothCore's clean `SIGBREAK` shutdown that saves characters), then `TerminateJobObject` after `StopGraceSeconds` | no data loss when the server is merely slow; a hard kill only when it is really stuck |
| restart policy | exit 0 = clean stop (not restarted), exit 2 = planned restart, anything else = failure with doubling backoff up to `MaxBackoffSeconds`; `StableRunSeconds` resets the counter | survives crash loops without hammering the DB |
| one instance only | the start path refuses to launch a second copy of a server whose executable is already running, and a process that survived a stop is never "restarted" (the stop is retried instead) | two worldservers on one database is worse than a stalled one (see section 11) |
| console | `SetConsoleCtrlHandler` handles Ctrl+C / Ctrl+Break / **window close** / logoff / shutdown | closing the window now stops the servers first (the PowerShell version orphaned them) |
| GM console | `Console = shared` keeps the server writing into the supervisor window | the GM command line keeps working exactly as before |

## 2. Build

```
build.bat        ->  ..\..\release\supervisor\acore_supervisor.exe
cl /W4 /O2 /MT /std:c++17 /EHsc /DUNICODE /D_UNICODE acore_supervisor.cpp
```
Needs the MSVC C++ toolset (`vcvars64.bat`); the script finds it via `%VCVARS64%` or the usual
Visual Studio paths. No external libraries, no CMake.

## 3. Verify before you trust it

```
release\supervisor\acore_supervisor.exe --config release\supervisor\supervisor.ini --once
```
It prints the resolved executable paths, the log file it will watch (with `LogFile = auto` this
is read out of the server's own conf, so it cannot point at the wrong file), whether the
heartbeat line is present in the log, and the result of the auth probe. Nothing is started.

### When the auth probe fails

The auth probe answers three different ways, and they mean very different things:

| `--once` / log text | what happened | what to check |
|---|---|---|
| `connect failed` | nothing accepted a TCP connection | is authserver running? is `ProbePort` the `RealmServerPort` of *this* deployment? |
| `no response to AUTH_LOGON_CHALLENGE` | connected, sent the challenge, no answer within `ProbeTimeoutMs` | the peer swallowed the packet or is hung |
| `short/empty auth response` | connected, sent the challenge, the peer **closed** (or answered fewer than 2 bytes) | who owns that port, and is it really an AzerothCore authserver? |

A well formed probe always gets an answer from AzerothCore: for the random non-existent account it
replies `00 00 04` (`AUTH_LOGON_CHALLENGE`, `0x00`, `WOW_FAIL_UNKNOWN_ACCOUNT`) - deliberately
invisible in `Auth.log` at the default `Logger.root=4`, because that line is `LOG_DEBUG`.

`tests\probe-auth.ps1` performs the same handshake by hand, prints the raw answer bytes and the
process that owns the port, so a failing probe can be pinned down in one run:

```
pwsh -File tests\probe-auth.ps1                        # 127.0.0.1:3724
pwsh -File tests\probe-auth.ps1 -Port 3724 -TimeoutMs 3000
```

It distinguishes the three cases above and names the returned fail code
(`0x04` unknown account, `0x03` banned, `0x09` version invalid, ...). Needs no admin rights on a
normal desktop session; where port ownership cannot be queried it prints the `netstat -ano` line
to run instead.

## 4. Run

```
release\supervisor\start_supervisor.bat
```
Keep the window open - it is the GM console (server output and command line). The supervisor's
own lines are prefixed with a timestamp and `[INFO]/[OK]/[WARN]/[ERROR]/[STALL]`.

To start it automatically after a reboot **with the visible console**, log on automatically and
add a Task Scheduler task:

* Trigger: **At log on** (of the account that runs the server)
* Action: `release\supervisor\start_supervisor.bat`
* Condition: **Run only when user is logged on**  <- this is what keeps the console visible
* (a shortcut in `shell:startup` works just as well)

Do **not** install it as a Windows service: services run in session 0 without a visible console,
which would break the GM command line.

## 5. Prerequisite settings in the server configs

The worldserver heartbeat is only written regularly if `worldserver.conf` has:

```
RecordUpdateTimeDiffInterval = 60000     ; write the record once a minute (default 300000)
MinRecordUpdateTimeDiff      = 0         ; always record, not only when a tick was slow (default 100)
```

(already set in this deployment; cost is ~5 log lines per minute). Without it the supervisor
warns once and falls back to log-activity + CPU cross-check monitoring.

Both lines matter for the *cadence*, which is what a timeout can be compared against. AzerothCore
writes the line from `WorldUpdateTime::RecordUpdateTime`:

```cpp
if (_recordUpdateTimeInverval > 0ms && diff > _recordUpdateTimeMin.count())   // diff > Min...
    if (GetMSTimeDiff(_lastRecordTime, gameTimeMs) > _recordUpdateTimeInverval)
        LOG_INFO("time.update", "Update time diff: {}ms with {} players online", ...);
```

So with the **default `MinRecordUpdateTimeDiff = 100`** the line appears only when a world tick took
longer than 100 ms: on a quiet or lightly loaded server that can be minutes apart, and
`HeartbeatTimeoutSeconds = 180` then produces a false stall. Set it to `0` (and, if the server is
very quiet, keep the timeout at 3x the configured interval), or raise `HeartbeatTimeoutSeconds`
to cover the worst realistic tick interval.

The line is written through the `time.update` logger (`Logger.time.update=<level>,<appenders>` in
`worldserver.conf`; level 4 = info is enough). If it is not configured, the supervisor logs
`heartbeat line ... never appeared` once and falls back to log activity + CPU.

## 6. Settings worth knowing (`supervisor.ini`)

| key | meaning |
|---|---|
| `LogFile` | `auto` = take `LogsDir` **and** the role's own appender (`Appender.Server` / `Appender.Auth`) from the server conf; or an explicit path |
| `HeartbeatPattern` | substring that identifies a world-loop tick; empty = no heartbeat rule (authserver) |
| `HeartbeatTimeoutSeconds` | no heartbeat for this long while the process lives = frozen world loop -> restart |
| `StartupReadyPattern` / `StartupTimeoutSeconds` / `StartupStallSeconds` | startup grace so a slow map load is never mistaken for a hang |
| `StopGraceSeconds` | how long to wait for the clean `CTRL_BREAK` shutdown before killing |
| `RestartDelaySeconds` / `MaxBackoffSeconds` / `StableRunSeconds` | restart policy |
| `AutoRestartOnCleanExit` | `false`: a clean shutdown (exit 0) stays down, as a GM intends |
| `AdoptExisting` | if a server is already running, supervise it instead of starting a second copy |
| `ProbePort` / `ProbeMode` | `auth` (default, strong) or `tcp` (connect only) |
| `MaxWorkingSetMB` | restart when the process exceeds this working set (0 = off) - leak guard |
| `Console` | `shared` (default) / `own` (server gets its own window) / `hidden` |

## 8. Control channel (used by the AGMP panel)

The supervisor can be driven from outside through a small command drop box, which is how the
panel's **Supervisor** page (`/supervisor`) manages it. A web request runs in a different
session and can therefore not signal the desktop-session supervisor directly, so the panel
writes a file and the supervisor executes it:

```
logs/supervisor_control.txt        (panel writes, atomically, one command per file)
    id=60406dd14a77
    action=restart                 start | stop | restart | ping | shutdown
    target=worldserver             worldserver | authserver | all
```

The supervisor renames the file to `<file>.running` before executing it (so a command can never
run twice, even across a restart), deletes it afterwards and publishes the outcome in
`logs/supervisor_status.json`:

```json
"lastCommand": { "id": "60406dd14a77", "action": "restart", "target": "worldserver",
                 "result": "ok", "message": "ok: worldserver: restarted (graceful)" }
```

Semantics: `stop` suppresses automatic restarts until a `start` arrives (`stoppedByUser` is set
in the status file); `restart` is treated as an operator action, not as a crash (the failure
counter is reset); `shutdown` stops every service gracefully and then exits the supervisor.
Unknown actions/targets are rejected without touching any service. With the default
`ExitWhenAllStopped = false` the supervisor keeps running after everything is stopped, so the
panel can start services again; the page shows "supervisor not running" when the status file
goes stale (so a dead supervisor is never mistaken for a healthy one).

Panel side: `config/supervisor.php` (auto-detects `release/supervisor`), the page, its JSON APIs
(`/supervisor/api/status`, `/supervisor/api/log`, `POST /supervisor/api/command`), capability
gates `supervisor.view` / `supervisor.control` and audit logging (`panel_audit`, module
`supervisor`). Starting the supervisor itself from the panel is optional
(`allow_start` + `start_task_name` → `schtasks /run`), because only a scheduled task can launch
a process inside the interactive session.

## 9. Tests

`tests\` contains a controllable fake worldserver/authserver (`fake_acore.cs`) and a driver:

```
tests\build_fakes.bat                                        :: needs csc.exe (ships with Windows)
powershell -File tests\run_scenario.ps1 -Scenario healthy -Seconds 40
powershell -File tests\run_scenario.ps1 -Scenario control            :: panel contract
powershell -File tests\run_scenario.ps1 -Scenario control -UsePhp `
    -PhpExe C:\php\php.exe -PhpVerifier ..\AGMP\tools\verify_supervisor.php
```
Scenarios: `once`, `healthy`, `chatty` (frozen world loop behind a chatty logger),
`hang`, `crash`, `planned`, `clean`, `authwedged` (authserver accepts but never answers),
`child` (job-object tree kill), `console` (Ctrl+Break), `consoleclose` (WM_CLOSE = clicking X),
`control` (the panel contract: ping / restart / stop / start / rejected / shutdown).
The driver resolves the supervisor binary relative to the repository
(`..\..\release\supervisor\acore_supervisor.exe`), so pass `-SupervisorExe` when yours lives
elsewhere. All scenarios pass.

Panel-side verification: `php tools/verify_supervisor.php` (50 checks: path auto-detection,
status parsing, health mapping, staleness, command validation, atomic write, log tail, page
rendering, navigation/asset registration, base-path handling, API answers) plus `php -l` on
every touched file.

## 10. Rollback

The supervisor is additive: stopping it and removing `release\supervisor\` leaves the servers
exactly as they were. In the deployment this tool came from, the previous PowerShell watchdog
(`watch_dog.bat` + `worldserver_watchdog.ps1`) is still present as a fallback - never run both
at the same time, two supervisors would fight over the same servers.

## 11. Adopted processes and the one-instance guarantee (1.1.1)

`AdoptExisting = true` lets the supervisor take over a server that is already running, but a
hand-started process is not a child: it is not in the supervisor's job object, it does not share
the supervisor's console, and it was opened by someone else. Three consequences, all of them
handled explicitly since 1.1.1:

| Situation | Behaviour |
|---|---|
| `CTRL_BREAK` to an adopted process | fails (`Win32 error 87`) - a foreign console cannot be signalled. Logged at WARN with the reason; the process can only be killed, never shut down gracefully. |
| Killing an adopted process | the handle is opened **with `PROCESS_TERMINATE`**. Before 1.1.1 it was not, so `TerminateProcess` failed with `ERROR_ACCESS_DENIED` **silently** and the process survived. If the right cannot be obtained at all, the supervisor logs an ERROR at adoption and treats the process as unstoppable. |
| The process survives a stop | the supervisor **refuses to start a second instance**: it keeps the handle, retries the stop every `max(RestartDelaySeconds, 15)` seconds and logs `pid X is STILL RUNNING after the stop (...) - refusing to start a second instance`. Before 1.1.1 it restarted anyway, which is how two worldservers ended up running side by side (production incident 2026-09-23). |

Independently of that, `StartService` checks for any other process running the same executable
before launching one, and adopts it (with `AdoptExisting`) or refuses with an ERROR. Two
worldservers, or two authservers, on one machine and one database are never a valid state.

Recommendation: do not hand-start the servers on a machine that has the supervisor. Stop them once
and let `start_supervisor.bat` launch them - only then are they in the job object (killed with the
supervisor, no orphans), in the shared GM console (`Console = shared`) and stoppable gracefully.

Also fixed in 1.1.1: the "cpu last advanced …s ago" diagnostic used an unsigned subtraction between
a tick captured at the start of a health check and a CPU sample taken later in the same check; it
wrapped to ~1.8e16 seconds and, worse, made the CPU cross-check in the log-activity fallback always
report "CPU also stalled", so it never protected a quiet-but-busy server. Ages are now clamped.

Regression test: `pwsh -File tests\run_adopt_test.ps1` reproduces the incident (hand-started fake
that freezes) and asserts that the adopted process is really killed, that a replacement is started
only afterwards, that two instances never coexist, that the replacement dies with the supervisor,
and that a foreign instance is adopted instead of duplicated (16 checks).

## 12. License

GPL-2.0 (see `LICENSE`) - the same license as AzerothCore and the Acore GM Panel.
