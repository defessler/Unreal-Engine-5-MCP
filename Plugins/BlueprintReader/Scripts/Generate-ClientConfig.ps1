#requires -Version 7
# Generates MCP client configuration files for bp-reader-mcp.
#
# Each client has its own config format and path. This script writes
# whichever client(s) you ask for. JSON formats are merged with existing
# entries (we only overwrite the `bp-reader` entry; other servers stay).
# Codex's TOML format is write-once — the script refuses to overwrite
# an existing TOML file without -Force, to avoid clobbering hand-tuned
# settings under a format we don't try to preserve.
#
# Supported clients (matching Epic 5.8's ModelContextProtocol plugin):
#   * ClaudeCode  → <BaseDir>/.mcp.json
#   * Cursor      → <BaseDir>/.cursor/mcp.json
#   * VSCode      → <BaseDir>/.vscode/mcp.json
#   * Gemini      → <BaseDir>/.gemini/settings.json
#   * Codex       → <BaseDir>/.codex/config.toml
#   * All         → all of the above
#
# Defaults assume the script lives at <ProjectDir>/Plugins/BlueprintReader/Scripts/.
# BaseDir defaults to <ProjectDir>; override for multi-project setups.
#
# Usage:
#   pwsh -File <thisScript> -Client ClaudeCode
#   pwsh -File <thisScript> -Client All -BaseDir D:\Projects\MyProj
#   pwsh -File <thisScript> -Client Codex -Force         # overwrite TOML

param(
    [Parameter(Mandatory=$true)]
    [ValidateSet('ClaudeCode','Cursor','VSCode','Gemini','Codex','All')]
    [string]$Client,

    [string]$ProjectDir,
    [string]$PluginDir,
    [string]$BaseDir,

    # Path to the MCP server exe. Defaults to the plugin's own Binaries:
    # <ProjectDir>/Plugins/BlueprintReader/Binaries/Win64/BlueprintReaderMcp.exe.
    [string]$ServerExe,

    # Server entry name to use in each client's config (the key under
    # mcpServers). Defaults to 'bp-reader' — matches our README + the
    # entry Claude Code looks for.
    [string]$ServerName = 'bp-reader',

    # If $true, overwrite Codex TOML even when it already exists. Pure
    # safety guard — JSON merges always preserve sibling entries, so
    # this flag is only consulted on the Codex path.
    [switch]$Force,

    # Default env vars to set per client entry. Reasonable defaults match
    # the README. Pass an empty hashtable to write no env block.
    [hashtable]$ServerEnv
)

$ErrorActionPreference = 'Stop'

# ---- resolve paths ----------------------------------------------------------

$scriptDir = Split-Path -Parent $PSCommandPath
if (-not $PluginDir)  { $PluginDir  = Split-Path -Parent $scriptDir }     # <plugin>/Scripts/ → <plugin>
if (-not $ProjectDir) { $ProjectDir = Split-Path -Parent (Split-Path -Parent $PluginDir) } # <plugin>/.. → Plugins → ProjectDir
if (-not $BaseDir)    { $BaseDir    = $ProjectDir }
if (-not $ServerExe)  { $ServerExe  = Join-Path $ProjectDir 'Plugins\BlueprintReader\Binaries\Win64\BlueprintReaderMcp.exe' }

if (-not (Test-Path -LiteralPath $ServerExe)) {
    Write-Warning "Server exe not found at $ServerExe — config files will reference a path that doesn't exist yet."
    Write-Warning "Build the server first via Plugins/BlueprintReader/Scripts/Build-MCPServer.ps1 or include the plugin in your editor target."
}

if (-not $ServerEnv) {
    # Sensible defaults — same vars Start-MCPServer.ps1 considers.
    $ServerEnv = @{
        'BP_READER_BACKEND' = 'auto'
        'BP_READER_PROJECT' = (Get-ChildItem -LiteralPath $ProjectDir -Filter '*.uproject' -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName)
    }
    # If we couldn't auto-discover a .uproject, drop the key so the server
    # falls back to its own auto-discovery instead of seeing an empty string.
    if ([string]::IsNullOrEmpty($ServerEnv['BP_READER_PROJECT'])) {
        $ServerEnv.Remove('BP_READER_PROJECT')
    }
}

# ---- per-client writers -----------------------------------------------------

function Read-JsonFileOrEmpty([string]$path) {
    if (Test-Path -LiteralPath $path) {
        try {
            return Get-Content -LiteralPath $path -Raw | ConvertFrom-Json -AsHashtable
        } catch {
            Write-Warning "Existing config at $path is not valid JSON; overwriting."
            return @{}
        }
    }
    return @{}
}

function Save-JsonFile([string]$path, [hashtable]$content) {
    $dir = Split-Path -Parent $path
    if ($dir -and -not (Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    $json = $content | ConvertTo-Json -Depth 10
    Set-Content -LiteralPath $path -Value $json -Encoding utf8 -NoNewline
}

function Build-ClaudeCodeEntry {
    # Claude Code + Gemini + Cursor share the same per-server entry shape.
    return @{
        command = $ServerExe
        args    = @()
        env     = $ServerEnv
    }
}

function Write-ClaudeCodeConfig([string]$baseDir) {
    $path = Join-Path $baseDir '.mcp.json'
    $cfg = Read-JsonFileOrEmpty $path
    if (-not $cfg.ContainsKey('mcpServers')) { $cfg['mcpServers'] = @{} }
    $cfg['mcpServers'][$ServerName] = Build-ClaudeCodeEntry
    Save-JsonFile $path $cfg
    return $path
}

function Write-CursorConfig([string]$baseDir) {
    $path = Join-Path $baseDir '.cursor\mcp.json'
    $cfg = Read-JsonFileOrEmpty $path
    if (-not $cfg.ContainsKey('mcpServers')) { $cfg['mcpServers'] = @{} }
    $cfg['mcpServers'][$ServerName] = Build-ClaudeCodeEntry
    Save-JsonFile $path $cfg
    return $path
}

function Write-VSCodeConfig([string]$baseDir) {
    # VS Code / Copilot uses a slightly different shape: `servers` (not
    # `mcpServers`), and each entry needs an explicit `type: "stdio"`.
    $path = Join-Path $baseDir '.vscode\mcp.json'
    $cfg = Read-JsonFileOrEmpty $path
    if (-not $cfg.ContainsKey('servers')) { $cfg['servers'] = @{} }
    $cfg['servers'][$ServerName] = @{
        type    = 'stdio'
        command = $ServerExe
        args    = @()
        env     = $ServerEnv
    }
    Save-JsonFile $path $cfg
    return $path
}

function Write-GeminiConfig([string]$baseDir) {
    $path = Join-Path $baseDir '.gemini\settings.json'
    $cfg = Read-JsonFileOrEmpty $path
    if (-not $cfg.ContainsKey('mcpServers')) { $cfg['mcpServers'] = @{} }
    $cfg['mcpServers'][$ServerName] = Build-ClaudeCodeEntry
    Save-JsonFile $path $cfg
    return $path
}

function Format-TomlString([string]$s) {
    # Basic TOML string escaping. Backslash + quote are the two we have to worry about.
    return '"' + ($s -replace '\\','\\\\' -replace '"','\"') + '"'
}

function Write-CodexConfig([string]$baseDir) {
    # Codex uses TOML and write-once semantics. We don't try to round-trip
    # an existing TOML through a parser; if the file is already there,
    # the user owns it. -Force overrides.
    $path = Join-Path $baseDir '.codex\config.toml'
    if ((Test-Path -LiteralPath $path) -and -not $Force) {
        Write-Warning "Codex config already exists at $path; not overwriting. Pass -Force to replace."
        return $null
    }
    $dir = Split-Path -Parent $path
    if (-not (Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    $lines = @(
        "# Generated by BlueprintReader/Scripts/Generate-ClientConfig.ps1.",
        "# Manage by re-running with -Force, or edit directly.",
        '',
        "[mcp_servers.$ServerName]",
        ('command = ' + (Format-TomlString $ServerExe)),
        'args = []'
    )
    if ($ServerEnv.Count -gt 0) {
        $lines += ''
        $lines += "[mcp_servers.$ServerName.env]"
        foreach ($k in $ServerEnv.Keys) {
            $v = [string]$ServerEnv[$k]
            $lines += ('{0} = {1}' -f $k, (Format-TomlString $v))
        }
    }
    Set-Content -LiteralPath $path -Value ($lines -join "`n") -Encoding utf8 -NoNewline
    return $path
}

# ---- dispatch ---------------------------------------------------------------

$targets = if ($Client -eq 'All') { @('ClaudeCode','Cursor','VSCode','Gemini','Codex') } else { @($Client) }
$written = @()

foreach ($t in $targets) {
    $p = switch ($t) {
        'ClaudeCode' { Write-ClaudeCodeConfig $BaseDir }
        'Cursor'     { Write-CursorConfig     $BaseDir }
        'VSCode'     { Write-VSCodeConfig     $BaseDir }
        'Gemini'     { Write-GeminiConfig     $BaseDir }
        'Codex'      { Write-CodexConfig      $BaseDir }
    }
    if ($null -ne $p) {
        Write-Host "wrote $p"
        $written += $p
    }
}

Write-Host ("done — {0} file(s) updated" -f $written.Count)
