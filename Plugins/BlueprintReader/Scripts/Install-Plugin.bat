@echo off
REM Batch wrapper around Install-Plugin.ps1 - the one-shot installer: mounts the
REM plugin into a UE project, builds the MCP server (UBT or the CMake fallback),
REM writes the MCP client config, deploys the Claude/AGENTS assets, runs doctor.
REM Requires PowerShell 7 (pwsh). Runs with -ExecutionPolicy Bypass so the
REM machine's PowerShell execution policy never blocks it.
REM
REM Args are OPTIONAL when the plugin lives at <Project>\Plugins\BlueprintReader
REM (the default) - both are inferred:
REM   -EngineDir   "<...>\UE_5.8"            (default: from the .uproject EngineAssociation)
REM   -ProjectFile "<...>\<Game>.uproject"   (default: the *.uproject above the plugin)
REM
REM Optional args:
REM   -Client <ClaudeCode^|Cursor^|VSCode^|Gemini^|Codex^|All>   (default ClaudeCode)
REM   -Symlink              symlink the plugin instead of copying it
REM   -ApplyEnginePatches   run Patch-Engine.ps1 -Apply (source engines only)
REM   -SkipBuild            skip the server build (config/assets/doctor only)
REM   -Force                replace an existing mounted plugin / config
REM
REM Usage:
REM   Install-Plugin.bat                         (zero-arg: infers engine + project)
REM   Install-Plugin.bat -Client All
REM   Install-Plugin.bat -EngineDir "D:\Epic Games\UE_5.8" -ProjectFile "D:\Game\MyGame.uproject"

setlocal
set "SCRIPT_DIR=%~dp0"
pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%Install-Plugin.ps1" %*
set "EXIT_CODE=%ERRORLEVEL%"
endlocal & exit /b %EXIT_CODE%
