@echo off
REM LogScope 빌드. cmd 에서 그냥 build.bat 이라고 치면 됩니다.
REM   build.bat                     Debug 빌드
REM   build.bat -Config Release      Release 빌드
REM   build.bat -Test                빌드 후 테스트
REM   build.bat -Clean -Run          처음부터 빌드하고 실행
REM
REM PowerShell 실행 정책이 막혀 있어도 되도록 -ExecutionPolicy Bypass 를 줍니다.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build.ps1" %*
