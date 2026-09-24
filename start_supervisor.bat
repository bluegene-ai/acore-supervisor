@echo off
rem =========================================================================================
rem  start_supervisor.bat - start the native AzerothCore supervisor
rem
rem  Add a shortcut to shell:startup, or a Task Scheduler task ("At log on", "Run only when
rem  user is logged on") so the servers come back after a reboot while keeping this visible
rem  console for GM commands.
rem
rem  Usage:  start_supervisor.bat                 start + supervise
rem          start_supervisor.bat --once          verify config/paths/probes, then exit
rem          start_supervisor.bat --help
rem          start_supervisor.bat /nopause ...    do not keep the window open at the end
rem
rem  The window title is derived from THIS folder plus the InstanceName in supervisor.ini, so one
rem  console per realm stays distinguishable when several supervisors run side by side:
rem      Svr [Acore80] supervisor        Svr [acoreT] supervisor-test
rem  Never hard-code the title: with one supervisor per realm every copy of this file would show
rem  the same name, and Ctrl+Tab / the taskbar gives no clue which realm a window belongs to.
rem =========================================================================================
setlocal EnableExtensions EnableDelayedExpansion

set "HERE=%~dp0"
set "EXE=%HERE%acore_supervisor.exe"
set "INI=%HERE%supervisor.ini"

rem ---- window title: <instance name from the ini> @ <this folder> ---------------------------
rem for /f reads the ini directly (no findstr): comments starting with ';' are skipped and the
rem delimiters strip the spaces around the '=' in "InstanceName    = Acore80"
set "SUPDIR=%~dp0"
for %%I in ("%SUPDIR:~0,-1%") do set "SUPDIR=%%~nxI"
set "SUPINST="
if exist "%INI%" for /f "usebackq tokens=1,* delims== " %%A in ("%INI%") do if /i "%%A"=="InstanceName" set "SUPINST=%%B"
if defined SUPINST set "SUPINST=%SUPINST: =%"
if defined SUPINST (
    title Svr [%SUPINST%] %SUPDIR%
) else (
    title Svr [%SUPDIR%]
)

set "ARGS=%*"
set "NOPAUSE="
if defined WS_NOPAUSE set "NOPAUSE=1"
if defined ARGS (
    set "TRIMMED=!ARGS:/nopause=!"
    if not "!ARGS!"=="!TRIMMED!" set "NOPAUSE=1"
    set "ARGS=!TRIMMED!"
)

if not exist "%EXE%" (
    echo [ERROR] supervisor executable not found: "%EXE%"
    set "RC=2"
    goto :finish
)

"%EXE%" --config "%INI%" !ARGS!
set "RC=!ERRORLEVEL!"

:finish
echo.
echo [start_supervisor] supervisor finished, exit code !RC!
if not defined NOPAUSE (
    echo Press any key to close this window...
    pause >nul
)
exit /b !RC!
