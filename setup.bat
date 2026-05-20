@echo off
REM Thin shim that forwards to Scripts/setup.ps1 in PowerShell 7+.
REM All real logic lives in setup.ps1; this exists so a fresh-clone
REM user can double-click or run "setup.bat" from cmd.exe.

where pwsh >nul 2>&1
if errorlevel 1 (
    echo error: pwsh ^(PowerShell 7+^) is required but not on PATH.
    echo Install from https://github.com/PowerShell/PowerShell/releases
    exit /b 2
)

pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0Scripts\setup.ps1" %*
exit /b %ERRORLEVEL%
