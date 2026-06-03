@echo off
REM Batch wrapper around Verify-Build.ps1 - checks both halves of the
REM BlueprintReader plugin are present and prints concrete fix steps for
REM anything missing.
REM
REM Usage:
REM   Verify-Build.bat
REM   Verify-Build.bat -ProjectDir D:\YourGame\MyProject

setlocal
set "SCRIPT_DIR=%~dp0"
pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%Verify-Build.ps1" %*
set "EXIT_CODE=%ERRORLEVEL%"
endlocal & exit /b %EXIT_CODE%
