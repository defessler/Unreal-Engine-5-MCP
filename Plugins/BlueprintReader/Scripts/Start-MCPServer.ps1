# Manual launcher for bp-reader-mcp.exe.
#
# Useful for:
#   * Driving the server from a script or piping JSON-RPC frames at it
#     by hand for debugging.
#   * Pre-warming an editor daemon outside a Claude session (the server
#     stays alive until stdin closes; in another shell you can pipe
#     framed requests at it via TCP/named pipe via your own glue).
#   * Confirming the exe boots with the right env before opening Claude.
#
# Auto-discovers env from <ProjectDir>/.mcp.json (the same file Claude
# Code reads), so behavior matches what Claude would launch with. Pass
# script params or set $env:BP_READER_* in your shell to override.
#
# Usage:
#   pwsh -File <thisScript>
#   pwsh -File <thisScript> -Backend mock          # force mock backend
#   pwsh -File <thisScript> -Prewarm 0             # skip editor pre-warm
#
# To send a request manually (PowerShell):
#   $body = '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"manual","version":"0"}}}'
#   "Content-Length: $($body.Length)`r`n`r`n$body" | pwsh -File <thisScript>

param(
    # Defaults assume the script lives at <ProjectDir>/Plugins/BlueprintReader/Scripts/.
    # Override -ProjectDir if you've copied the plugin somewhere else.
    [string]$ProjectDir,
    [string]$PluginDir,

    # All four env vars the server reads. If unset here, picked up from
    # <ProjectDir>/.mcp.json's mcpServers.bp-reader.env block.
    [string]$Backend,
    [string]$EngineDir,
    [string]$UProject,
    [string]$Prewarm,

    # Force a particular exe (e.g. a Debug build).
    [string]$Exe
)

$ErrorActionPreference = "Stop"
$tag = "[bp-reader/start]"

# Resolve script-relative defaults.
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $PluginDir)  { $PluginDir  = (Resolve-Path (Join-Path $scriptDir '..')).Path }
if (-not $ProjectDir) { $ProjectDir = (Resolve-Path (Join-Path $PluginDir '..\..')).Path }

# Find the MCP server dir. New layout has it inside the plugin; older
# checkouts had it at the project root.
$mcpDir = $null
foreach ($candidate in @(
    (Join-Path $PluginDir  'mcp-server'),
    (Join-Path $ProjectDir 'mcp-server')
)) {
    if (Test-Path (Join-Path $candidate 'CMakeLists.txt')) { $mcpDir = $candidate; break }
}
if (-not $mcpDir) {
    Write-Error "$tag couldn't locate mcp-server/ under $PluginDir or $ProjectDir."
    exit 1
}

# Resolve the exe.
if (-not $Exe) {
    $Exe = Join-Path $mcpDir 'build\Release\bp-reader-mcp.exe'
}
if (-not (Test-Path $Exe)) {
    Write-Error "$tag bp-reader-mcp.exe not found at $Exe. Build the plugin first (or run Build-MCPServer.ps1)."
    exit 1
}

# Auto-load env from .mcp.json if the params weren't passed.
$mcpJson = Join-Path $ProjectDir '.mcp.json'
if (Test-Path $mcpJson) {
    try {
        $cfg = Get-Content $mcpJson -Raw | ConvertFrom-Json
        $envBlock = $cfg.mcpServers.'bp-reader'.env
        if ($envBlock) {
            if (-not $Backend   -and $envBlock.BP_READER_BACKEND)    { $Backend   = $envBlock.BP_READER_BACKEND }
            if (-not $EngineDir -and $envBlock.BP_READER_ENGINE_DIR) { $EngineDir = $envBlock.BP_READER_ENGINE_DIR }
            if (-not $UProject  -and $envBlock.BP_READER_PROJECT)    { $UProject  = $envBlock.BP_READER_PROJECT }
            if (-not $Prewarm   -and $envBlock.BP_READER_PREWARM)    { $Prewarm   = $envBlock.BP_READER_PREWARM }
        }
    } catch {
        Write-Host "$tag warning: couldn't parse $mcpJson ($($_.Exception.Message)); proceeding with shell env only."
    }
}

# Apply (only what's set; let the server's own defaults handle the rest).
if ($Backend)   { $env:BP_READER_BACKEND    = $Backend }
if ($EngineDir) { $env:BP_READER_ENGINE_DIR = $EngineDir }
if ($UProject)  { $env:BP_READER_PROJECT    = $UProject }
if ($Prewarm)   { $env:BP_READER_PREWARM    = $Prewarm }

Write-Host "$tag launching:" -ForegroundColor Cyan
Write-Host "  exe       = $Exe"
Write-Host "  backend   = $($env:BP_READER_BACKEND  -replace '^$','(default: mock)')"
Write-Host "  engineDir = $($env:BP_READER_ENGINE_DIR -replace '^$','(unset)')"
Write-Host "  uproject  = $($env:BP_READER_PROJECT   -replace '^$','(unset)')"
Write-Host "  prewarm   = $($env:BP_READER_PREWARM   -replace '^$','(default: 0)')"
Write-Host ""
Write-Host "$tag server reads JSON-RPC frames on stdin; stderr is shown below." -ForegroundColor DarkGray
Write-Host "$tag Ctrl-C (or close stdin) to stop." -ForegroundColor DarkGray
Write-Host ""

# Run in foreground so the user sees stderr live and can pipe stdin.
& $Exe
exit $LASTEXITCODE
