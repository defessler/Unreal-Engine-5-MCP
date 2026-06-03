@echo off
REM Batch wrapper around Dump-Tools.ps1 - regenerate the tool catalog
REM (docs/TOOLS.md + docs/tools.json) from the live server, or check for drift.
REM Run after adding/removing/renaming a tool. Requires PowerShell 7.
REM Runs with -ExecutionPolicy Bypass so the execution policy never blocks it.
REM
REM Optional args:
REM   -Check    CI mode: exit 1 on catalog drift instead of rewriting the files
REM
REM Usage:
REM   Dump-Tools.bat
REM   Dump-Tools.bat -Check

setlocal
set "SCRIPT_DIR=%~dp0"
pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%Dump-Tools.ps1" %*
set "EXIT_CODE=%ERRORLEVEL%"
endlocal & exit /b %EXIT_CODE%
