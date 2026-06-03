@echo off
REM Batch wrapper around Generate-ClientConfig.ps1 - writes MCP client config
REM file(s) pointing at the plugin's BlueprintReaderMcp.exe. Requires PowerShell 7.
REM Runs with -ExecutionPolicy Bypass so the execution policy never blocks it.
REM
REM Required args:
REM   -Client <ClaudeCode^|Cursor^|VSCode^|Gemini^|Codex^|All>
REM
REM Optional args:
REM   -BaseDir <dir>      where to write the config (default: project dir)
REM   -ServerExe <path>   override the server exe path
REM   -ServerName <name>  config entry name (default: bp-reader)
REM   -Force              overwrite an existing Codex TOML
REM
REM Usage:
REM   Generate-ClientConfig.bat -Client ClaudeCode
REM   Generate-ClientConfig.bat -Client All

setlocal
set "SCRIPT_DIR=%~dp0"
pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%Generate-ClientConfig.ps1" %*
set "EXIT_CODE=%ERRORLEVEL%"
endlocal & exit /b %EXIT_CODE%
