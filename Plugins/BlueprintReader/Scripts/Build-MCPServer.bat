@echo off
REM Batch wrapper around Build-MCPServer.ps1. Same logic UBT runs as a
REM PreBuildStep - smart skip when bp-reader-mcp.exe is fresh, otherwise
REM cmake configure (first time only) + cmake --build. Useful for building
REM the MCP server standalone without going through UBT.
REM
REM Usage:
REM   Build-MCPServer.bat
REM   Build-MCPServer.bat -Config Debug
REM
REM Defaults assume the script lives at
REM <ProjectDir>\Plugins\BlueprintReader\Scripts\.

setlocal
set "SCRIPT_DIR=%~dp0"
set "PLUGIN_DIR=%SCRIPT_DIR%.."
REM ProjectDir = two levels above the plugin (Plugins\BlueprintReader\.. = Plugins\, ..\.. = project root)
for %%I in ("%PLUGIN_DIR%\..\..") do set "PROJECT_DIR=%%~fI"

pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%Build-MCPServer.ps1" -ProjectDir "%PROJECT_DIR%" -PluginDir "%PLUGIN_DIR%" %*
set "EXIT_CODE=%ERRORLEVEL%"
endlocal & exit /b %EXIT_CODE%
