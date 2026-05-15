@echo off
REM Batch wrapper around Build-MCPServer.ps1. Invokes UBT to build the
REM two BlueprintReaderMcp* Program targets (MCP server + tests). The
REM MCP server is no longer a PreBuildStep — it's its own UBT target.
REM
REM Required args:
REM   -EngineDir   "<...>\UnrealEngine"
REM   -ProjectFile "<...>\<Game>.uproject"
REM
REM Optional args:
REM   -Config <Debug|DebugGame|Development|Test|Shipping>   (default Development)
REM   -Targets <All|Mcp|Tests>                              (default All)
REM   -ExtraArgs "<flags>"                                  (e.g. -NoUba -MaxParallelActions=4)
REM
REM Usage:
REM   Build-MCPServer.bat -EngineDir "D:\Unreal Engine 5" -ProjectFile "D:\Game\MyGame.uproject"
REM
REM (To build the editor target, use Build.bat directly. This script is
REM for the MCP-server-only path.)

setlocal
set "SCRIPT_DIR=%~dp0"
pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%Build-MCPServer.ps1" %*
set "EXIT_CODE=%ERRORLEVEL%"
endlocal & exit /b %EXIT_CODE%
