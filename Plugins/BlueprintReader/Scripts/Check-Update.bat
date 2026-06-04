@echo off
REM Batch wrapper for Check-Update.ps1 - check for a newer BlueprintReader
REM release on GitHub and cache the result for doctor / server startup.
REM
REM Args are OPTIONAL when the plugin lives at <Project>\Plugins\BlueprintReader.
REM
REM Usage:
REM   Check-Update.bat                    (checks, prints result, exits 0)
REM   Check-Update.bat -ExitCodeOnUpdate  (exits 2 if an update is available)

setlocal
set "SCRIPT_DIR=%~dp0"
pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%Check-Update.ps1" %*
set "EXIT_CODE=%ERRORLEVEL%"
endlocal & exit /b %EXIT_CODE%
