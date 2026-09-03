@echo off
setlocal
REM ---------------------------------------------------------------------------
REM LogScope build entry point.
REM
REM   build.bat                     Debug build
REM   build.bat -Config Release     Release build
REM   build.bat -Test               build, then run tests
REM   build.bat -Clean -Run         clean rebuild, then run
REM
REM This file is a thin wrapper: all logic lives in scripts\build.ps1.
REM Kept ASCII-only on purpose. A .bat with UTF-8 text is misread by cmd.exe
REM on non-UTF-8 code pages (CP949 on Korean Windows).
REM ---------------------------------------------------------------------------

set "PS1=%~dp0scripts\build.ps1"

echo [build.bat] repo   : %~dp0
echo [build.bat] script : %PS1%
echo.

if not exist "%PS1%" (
  echo [build.bat] ERROR: script not found.
  echo [build.bat] Run this from the repository root, or re-clone the repo.
  exit /b 1
)

where powershell.exe >nul 2>&1
if errorlevel 1 (
  echo [build.bat] ERROR: powershell.exe not found on PATH.
  exit /b 1
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%PS1%" %*
set "RC=%ERRORLEVEL%"

echo.
if not "%RC%"=="0" (
  echo [build.bat] FAILED - exit code %RC%
) else (
  echo [build.bat] OK
)
exit /b %RC%
