@echo off
REM Batch wrapper around Install-ClaudeAssets.ps1 - deploy the bp-* skills +
REM bp-audit agent + AGENTS.md / copilot-instructions into a project's .claude/.
REM Auto-detects the project root when -ProjectRoot is omitted. Requires PowerShell 7.
REM Runs with -ExecutionPolicy Bypass so the execution policy never blocks it.
REM
REM Optional args:
REM   -ProjectRoot <dir>   target project root (default: auto-detect from a *.uproject)
REM   -DryRun              show what would copy without doing it
REM   -Force               overwrite managed assets
REM
REM Usage:
REM   Install-ClaudeAssets.bat
REM   Install-ClaudeAssets.bat -ProjectRoot D:\Game\MyProject

setlocal
set "SCRIPT_DIR=%~dp0"
pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%Install-ClaudeAssets.ps1" %*
set "EXIT_CODE=%ERRORLEVEL%"
endlocal & exit /b %EXIT_CODE%
