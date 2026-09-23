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
| `auth closed the connection without answering` | connected, sent the challenge, the peer **closed without any reply** (FIN or reset) | the authserver rejected the **packet**: check the `size` field is a *little endian* uint16 equal to `30 + account length`. A healthy AzerothCore authserver never closes silently - this was the 1.1.3 bug, see below. |
| `short auth response (N byte)` | the peer answered, but with fewer than 2 bytes | who owns that port, and is it really an AzerothCore authserver? |

A well formed probe always gets an answer from AzerothCore: for the random non-existent account it
replies `00 00 04` (`AUTH_LOGON_CHALLENGE`, `0x00`, `WOW_FAIL_UNKNOWN_ACCOUNT`) - deliberately
invisible in `Auth.log` at the default `Logger.root=4`, because that line is `LOG_DEBUG`.
`sAuthLogonChallenge_C` is `#pragma pack(1)` and the server reinterprets the raw receive buffer, so
every field is native x86: `size` is a **little endian** `uint16` and `build` a little endian
`uint16`, while `size` must equal `30 + len(account)` (`AuthSession.cpp:245-254, 289`).

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

**Since 1.1.2 the supervisor reads both options out of the server config** (`ServerConf`, which it
already parses for `LogFile = auto`) and enforces

```
effective timeout = max(HeartbeatTimeoutSeconds, RecordUpdateTimeDiffInterval + 120s)
```

unless `HeartbeatTimeoutMode = strict` is set (which keeps `HeartbeatTimeoutSeconds` exactly as
written and only warns). That rule exists so a timeout shorter than the interval - a guaranteed
false stall, and the exact shape of the 2026-09-23 incident where production had
`RecordUpdateTimeDiffInterval = 300000` with a 180 s timeout - can no longer kill a healthy server.
It logs both values at startup, warns when it had to raise the timeout (`a HEALTHY server would be
reported as stalled`), warns when `MinRecordUpdateTimeDiff > 0`, and publishes `heartbeatTimeoutSec`
(enforced), `heartbeatTimeoutConfiguredSec`, `heartbeatIntervalSec` and `heartbeatMinRecordMs` in
the status JSON. `--once` prints the same summary, including "server config not readable - assuming
the AzerothCore defaults" when `ServerConf` is missing (in that case the configured timeout is used
unchanged).

## 6. Settings worth knowing (`supervisor.ini`)

| key | meaning |
|---|---|
| `LogFile` | `auto` = take `LogsDir` **and** the role's own appender (`Appender.Server` / `Appender.Auth`) from the server conf; or an explicit path |
| `HeartbeatPattern` | substring that identifies a world-loop tick; empty = no heartbeat rule (authserver) |
| `HeartbeatTimeoutSeconds` | no heartbeat for this long while the process lives = frozen world loop -> restart |
| `HeartbeatTimeoutMode` | `auto` (default) never uses a timeout shorter than the server's `RecordUpdateTimeDiffInterval`; `strict` uses `HeartbeatTimeoutSeconds` as written (only warns) - for tests or when you knowingly accept sub-interval detection |
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

### Also in 1.1.2: the stall that killed the healthy server

The same production incident had a second, independent cause: the supervisor's
`HeartbeatTimeoutSeconds = 180` was **shorter than the heartbeat cadence the server configures**
(`RecordUpdateTimeDiffInterval = 300000`), so a perfectly healthy worldserver - 12-13 players
online, a log line every minute, world ticks of 100-120 ms - was reported as "world-loop heartbeat
missing for 181s" and killed. The production log proves the cadence: the heartbeat lines appear at
09:41, 09:46, 09:51, 09:56, 10:01, 10:06, 10:11 and 10:16, exactly five minutes apart.

1.1.2 reads `RecordUpdateTimeDiffInterval` / `MinRecordUpdateTimeDiff` from the server config and
enforces `max(HeartbeatTimeoutSeconds, interval + 120s)` (see section 5), warns loudly when the
configured timeout had to be raised, and reports the cadence in the status JSON and in `--once`.

### Also in 1.1.3: the auth probe that killed a healthy authserver (the `size` field)

Enabling the `[authserver]` guard restarted the authserver every ~30s, forever, with

```
[authserver] probe failed 3/3: short/empty auth response
[authserver] probe failed 3 times in a row (short/empty auth response) - pid 12636
[authserver] probe failed 3 times in a row (short/empty auth response) - restarting in 20s (restart #2, consecutive failures 2)
```

while `Auth.log` showed a completely normal startup and shutdown and the auth server answered real
clients. The health signal was fine - the **probe packet was not**.

`BuildProbeChallenge()` wrote the `sAuthLogonChallenge_C::size` field **big endian**:

```cpp
p.push_back((char)((size >> 8) & 0xFF));   // "big endian" - wrong
p.push_back((char)(size & 0xFF));
```

`AuthSession::ReadHandler()` reinterprets the raw receive buffer and reads `challenge->size` as a
native `uint16`, so a 43 byte challenge became `0x2B00` = 11008 on the server, `4 + 11008` blew past
`MAX_ACCEPTED_CHALLENGE_SIZE` (51) and the socket was closed **without any reply**
(`AuthSession.cpp:245-254`). The probe can only see "connected, then the peer closed", which it
reported as `short/empty auth response` - a message that sent the reader hunting for a foreign
process on port 3724 instead of at its own packet.

Three things made this survive a full test suite and a dedicated diagnostic script:

* the field is written **little endian** now (`size = 30 + account length`, `AuthSession.cpp:289`);
* `tests\fake_acore.cs` parsed `size` big endian too, so the fixture agreed with the broken probe -
  it now parses native/little endian, as the real server does, and answers the real 3 byte
  `00 00 04` instead of a 2 byte shape;
* `tests\probe-auth.ps1` (the byte level diagnostic) built the same big endian packet, so it
  reproduced the supervisor's own bug and blamed the deployment. It is fixed and kept in sync.

The probe also distinguishes the failure modes better now: a silent close is reported as
`auth closed the connection without answering` (it means the server rejected the *packet*), a short
answer as `short auth response (N byte)`, and the fail code is read from the third byte
(`00 00 04` -> `fail code 0x04`) instead of the always-`0x00` second byte.

Verified on this deployment against the real `authserver.exe` of rev `2c4fe4c32f0b+` (build 12340):
the old packet gets `recv() == 0` - the peer closes with no reply - and the fixed packet gets
`00 00 04`. `tests\run_scenario.ps1` (all scenarios) and `--once` pass afterwards.

## 13. Several realms on one machine (one supervisor per realm)

One `acore_supervisor.exe` supervises **exactly one** worldserver + one authserver: the service
sections are fixed (`[worldserver]`, `[authserver]` - any other section name is ignored), because
each service needs its own working directory, log, probe port and restart policy. A machine running
several realms therefore runs **one supervisor process per realm** - which is also the better shape:
every realm keeps its own visible GM console, its own job object (no cross-realm kills), its own
restart policy and its own failure domain.

```
D:\AzerothCore\release\
    supervisor\        first instance    acore_supervisor.exe + supervisor.ini + logs\
    supervisor-b\      second instance   (same exe, its own ini/logs)
    realm-a\  realm-b\ the realm deployments (own exe / Data / configs)
    auth\              the shared authserver - supervised by exactly ONE instance
```

What must differ per instance, and why:

| Key | Why |
|---|---|
| `InstanceName` | single-instance mutex `Global\AcoreSupervisor_<InstanceName>`; a second process with the same name logs `another supervisor instance is already running` and exits |
| `GuardLog` / `StatusFile` / `ControlFile` | otherwise the instances overwrite each other's state and steal each other's commands (one folder per instance keeps them separate automatically) |
| `WorkDir` / `Exe` / `ServerConf` | each realm's own worldserver |
| the shared `authserver` | enable it in **one** instance only (`Enabled = false` in the others) - two supervisors would adopt and stop the same process |
| `ProbePort` | only when the realm runs its own authserver |
| console window | start each instance separately so each realm gets its own GM console; never run two supervisors inside one console (`Console = shared` attaches every server to the same console, whose input is shared) |

**The rule that matters most**: on one machine a given server executable may be supervised by
**exactly one** instance. Process identity is the full exe path (`FindProcessesByExe`), so two
supervisors pointing at the same `worldserver.exe` path would treat the other realm's process as
"already running" - one adopts it (and later stops the wrong server), the other refuses to start.
Give every realm its own exe path (a per-realm release tree, as above) and the problem disappears.

Example `supervisor-b\supervisor.ini` (only the differences from the first instance):

```ini
[general]
InstanceName = AcoreRealmB
GuardLog     = logs\supervisor.log
StatusFile   = logs\supervisor_status.json
ControlFile  = logs\supervisor_control.txt

[worldserver]
Enabled    = true
WorkDir    = ..\realm-b
ServerConf = configs\worldserver.conf
Console    = shared

[authserver]
Enabled = false          ; the shared authserver belongs to the other instance
```

Autostart: one Task Scheduler task per instance (trigger "At log on", "Run only when user is logged
on"), or one shortcut per instance in `shell:startup`. `start_supervisor.bat` always uses the
`supervisor.ini` next to itself, so either give each instance its own folder (as above) or launch the
shared exe directly with `acore_supervisor.exe --config <instance>\supervisor.ini`.

AGMP panel: list the instances in `config/supervisor.php` (`'instances' => [...]`), which adds an
instance switcher to `/supervisor` and routes every call with `?instance=<id>`; see the panel README.

## 14. License

GPL-2.0 (see `LICENSE`) - the same license as AzerothCore and the Acore GM Panel.
