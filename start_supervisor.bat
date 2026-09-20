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
rem =========================================================================================
setlocal EnableExtensions EnableDelayedExpansion
title Svr-80 [supervisor]

set "HERE=%~dp0"
set "EXE=%HERE%acore_supervisor.exe"
set "INI=%HERE%supervisor.ini"

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
