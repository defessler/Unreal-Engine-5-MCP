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

# Resolve the exe. The post-UBT-migration layout puts the binary at
# <Project>/Binaries/Win64/BlueprintReaderMcp.exe (UBT's default output
# path for Program targets in a project context). Fall back to the
# pre-UBT CMake path for users on older plugin pulls.
if (-not $Exe) {
    $candidates = @(
        (Join-Path $ProjectDir 'Binaries\Win64\BlueprintReaderMcp.exe'),
        # Legacy CMake build artifact path. Kept so users mid-upgrade don't
        # get a hard error before they've rebuilt; remove once everyone's
        # past the UBT migration.
        (Join-Path $PluginDir  'mcp-server\build\Release\bp-reader-mcp.exe')
    )
    foreach ($c in $candidates) {
        if (Test-Path -LiteralPath $c) { $Exe = $c; break }
    }
}
if (-not $Exe -or -not (Test-Path -LiteralPath $Exe)) {
    Write-Error @"
$tag BlueprintReaderMcp.exe not found.
  Tried:
    $($ProjectDir)\Binaries\Win64\BlueprintReaderMcp.exe (UBT build, post-PR #75)
    $($PluginDir)\mcp-server\build\Release\bp-reader-mcp.exe (legacy CMake build)

  Build with:
    Build.bat BlueprintReaderMcp Win64 Development -project="$($uproject -or '<your.uproject>')"
  Or use the wrapper:
    .\Plugins\BlueprintReader\Scripts\Build-MCPServer.ps1 -EngineDir "<engine>" -ProjectFile "<your.uproject>"
"@
    exit 1
}

# Auto-load env from .mcp.json if the params weren't passed. Also forwards
# any *other* BP_READER_* vars that live in the JSON's env block -- this is
# the escape hatch for IDEs that drop env entries past some count when
# launching MCP servers (observed with JetBrains Copilot in Rider/IDEA).
$mcpJson = Join-Path $ProjectDir '.mcp.json'
$envBlock = $null
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

# Apply the four "first-class" params (the ones with explicit -Foo overrides).
# Server defaults handle anything still unset.
if ($Backend)   { $env:BP_READER_BACKEND    = $Backend }
if ($EngineDir) { $env:BP_READER_ENGINE_DIR = $EngineDir }
if ($UProject)  { $env:BP_READER_PROJECT    = $UProject }
if ($Prewarm)   { $env:BP_READER_PREWARM    = $Prewarm }

# Forward the rest of .mcp.json's env block verbatim (BP_READER_EDITOR_ARGS,
# BP_READER_STARTUP_TIMEOUT_SECONDS, BP_READER_TIMEOUT_SECONDS, anything else
# the user added). Skip anything already set in the parent shell so direct
# `$env:FOO = bar` calls before invoking us still win.
$forwarded = @()
$handled = @('BP_READER_BACKEND','BP_READER_ENGINE_DIR','BP_READER_PROJECT','BP_READER_PREWARM')
if ($envBlock) {
    foreach ($prop in $envBlock.PSObject.Properties) {
        if ($prop.Name -in $handled) { continue }
        if (Test-Path "env:$($prop.Name)") { continue }   # respect parent-shell override
        Set-Item -Path "env:$($prop.Name)" -Value $prop.Value
        $forwarded += $prop.Name
    }
}

Write-Host "$tag launching:" -ForegroundColor Cyan
Write-Host "  exe       = $Exe"
Write-Host "  backend   = $($env:BP_READER_BACKEND  -replace '^$','(default: mock)')"
Write-Host "  engineDir = $($env:BP_READER_ENGINE_DIR -replace '^$','(unset)')"
Write-Host "  uproject  = $($env:BP_READER_PROJECT   -replace '^$','(unset)')"
Write-Host "  prewarm   = $($env:BP_READER_PREWARM   -replace '^$','(default: 0)')"
if ($forwarded.Count -gt 0) {
    Write-Host "  forwarded from .mcp.json: $($forwarded -join ', ')"
}
Write-Host ""
Write-Host "$tag server reads JSON-RPC frames on stdin; stderr is shown below." -ForegroundColor DarkGray
Write-Host "$tag Ctrl-C (or close stdin) to stop." -ForegroundColor DarkGray
Write-Host ""

# Run in foreground so the user sees stderr live and can pipe stdin.
& $Exe
exit $LASTEXITCODE
