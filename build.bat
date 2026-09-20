@echo off
rem =========================================================================================
rem  build.bat - build acore_supervisor.exe (single file, no external dependencies)
rem  Needs MSVC (Visual Studio 2022 C++ workload).  vcvars64.bat is located automatically
rem  from %VCVARS64%, then the usual install paths.
rem =========================================================================================
setlocal EnableExtensions
cd /d "%~dp0"

set "VCVARS=%VCVARS64%"
if not defined VCVARS if exist "D:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=D:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS (
    echo [ERROR] vcvars64.bat not found. Set VCVARS64 to its full path and retry.
    exit /b 1
)

echo [build] using %VCVARS%
call "%VCVARS%" >nul
if errorlevel 1 (
    echo [ERROR] failed to initialise the MSVC environment.
    exit /b 1
)

set "OUTDIR=%~dp0..\..\release\supervisor"
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

echo [build] compiling acore_supervisor.cpp ...
pushd "%~dp0"
cl /nologo /W4 /O2 /MT /std:c++17 /EHsc /DUNICODE /D_UNICODE ^
   acore_supervisor.cpp ^
   /Fe:"%OUTDIR%\acore_supervisor.exe" /link /SUBSYSTEM:CONSOLE
set "RC=%ERRORLEVEL%"
del /q "%~dp0*.obj" >nul 2>&1
popd

if not "%RC%"=="0" (
    echo [build] FAILED
    exit /b 1
)

echo [build] OK -^> "%OUTDIR%\acore_supervisor.exe"
if not exist "%OUTDIR%\supervisor.ini" copy /y "supervisor.ini" "%OUTDIR%\supervisor.ini" >nul
if not exist "%OUTDIR%\start_supervisor.bat" copy /y "start_supervisor.bat" "%OUTDIR%\start_supervisor.bat" >nul
exit /b 0
