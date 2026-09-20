@echo off
rem =========================================================================================
rem  build_fakes.bat - build the test fixtures used by run_scenario.ps1
rem
rem  Compiles tests\fake_acore.cs with the .NET Framework C# compiler (ships with Windows) into
rem  three executables that stand in for worldserver.exe, authserver.exe and a long living child
rem  process.  Nothing else in this repository needs them.
rem =========================================================================================
setlocal EnableExtensions
cd /d "%~dp0"

set "CSC=%SystemRoot%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if not exist "%CSC%" set "CSC=%SystemRoot%\Microsoft.NET\Framework\v4.0.30319\csc.exe"
if not exist "%CSC%" (
    echo [build_fakes] csc.exe not found - install .NET Framework 4.x or build the fixtures elsewhere.
    exit /b 1
)

echo [build_fakes] using %CSC%
"%CSC%" /nologo /optimize+ /out:"%~dp0fake_world.exe" "%~dp0fake_acore.cs"
if errorlevel 1 exit /b 1
copy /y "%~dp0fake_world.exe" "%~dp0fake_auth.exe" >nul
copy /y "%~dp0fake_world.exe" "%~dp0fake_child.exe" >nul
echo [build_fakes] OK -^> fake_world.exe, fake_auth.exe, fake_child.exe
exit /b 0
