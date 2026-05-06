@echo off
REM Batch wrapper around Start-MCPServer.ps1 - double-click-friendly launcher
REM that runs bp-reader-mcp.exe with env loaded from <ProjectDir>\.mcp.json
REM (commandlet mode by default for this project).
REM
REM Usage:
REM   Start-MCPServer.bat
REM   Start-MCPServer.bat -Backend mock
REM   Start-MCPServer.bat -Prewarm 0
REM   Start-MCPServer.bat -EngineDir "D:\OtherEngine" -UProject "D:\Other\Other.uproject"
REM
REM Any args after the script name are passed through to the PS1 unchanged.

setlocal
set "SCRIPT_DIR=%~dp0"
pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%Start-MCPServer.ps1" %*
set "EXIT_CODE=%ERRORLEVEL%"
endlocal & exit /b %EXIT_CODE%
