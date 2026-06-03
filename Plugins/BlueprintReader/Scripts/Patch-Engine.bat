@echo off
REM Batch wrapper around Patch-Engine.ps1 - apply the three source-engine
REM .Build.cs patches the plugin depends on (PrivateIncludePaths resolution).
REM Source engines only; not needed on an installed/Launcher engine.
REM Requires PowerShell 7. Runs with -ExecutionPolicy Bypass so the execution
REM policy never blocks it.
REM
REM Args:
REM   -EngineDir "<...>\UnrealEngine"   (optional: inferred from the in-project
REM                                      .uproject EngineAssociation when omitted)
REM   -Apply                            apply the patches (omit to validate / preview)
REM
REM Usage:
REM   Patch-Engine.bat -Apply                                  (zero-arg engine infer)
REM   Patch-Engine.bat -EngineDir "D:\Unreal Engine 5" -Apply

setlocal
set "SCRIPT_DIR=%~dp0"
pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%Patch-Engine.ps1" %*
set "EXIT_CODE=%ERRORLEVEL%"
endlocal & exit /b %EXIT_CODE%
