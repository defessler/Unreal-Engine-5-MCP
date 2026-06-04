@echo off
REM Setup-Plugin.bat - one-click plugin setup, from the plugin root.
REM
REM Pulls the latest BlueprintReader plugin from GitHub and configures it for the
REM surrounding UE project (MCP client config + Claude/AGENTS assets + doctor)
REM WITHOUT building anything. Self-updating: if the update changes the setup
REM scripts, the new versions take effect on this same pass.
REM
REM Drop the plugin into <Project>\Plugins\BlueprintReader and run this from there.
REM Paths are inferred - no arguments required. Runs with -ExecutionPolicy Bypass
REM so the machine's PowerShell policy never blocks it. Requires PowerShell 7
REM (pwsh); the plugin is fetched as a ZIP over HTTPS (no git needed).
REM
REM Optional args (forwarded to Scripts\Update-Plugin.ps1):
REM   -Client <ClaudeCode^|Cursor^|VSCode^|Gemini^|Codex^|All>   (default ClaudeCode)
REM   -Ref <branch-or-tag>                   (default main)
REM   -ProjectFile "<...>\<Game>.uproject"   (default: the *.uproject above the plugin)
REM   -Force                                 replace an existing config
REM
REM Usage:
REM   Setup-Plugin.bat                 (infers project, pulls main, configures)
REM   Setup-Plugin.bat -Client All
REM
REM (To BUILD the MCP server too, use Scripts\Build-MCPServer.bat. For a full
REM install including the build, use Scripts\Install-Plugin.bat.)

setlocal
set "PLUGIN_ROOT=%~dp0"
pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%PLUGIN_ROOT%Scripts\Update-Plugin.ps1" %*
set "EXIT_CODE=%ERRORLEVEL%"
endlocal & exit /b %EXIT_CODE%
