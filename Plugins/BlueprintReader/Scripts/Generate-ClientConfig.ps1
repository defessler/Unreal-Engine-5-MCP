#requires -Version 7
# Generates MCP client configuration files for bp-reader-mcp.
#
# Each client has its own config format and path. This script writes
# whichever client(s) you ask for. JSON formats are merged with existing
# entries (we only overwrite the `bp-reader` entry; other servers stay).
# Codex's TOML format is write-once - the script refuses to overwrite
# an existing TOML file without -Force, to avoid clobbering hand-tuned
# settings under a format we don't try to preserve.
#
# Supported clients (matching Epic 5.8's ModelContextProtocol plugin):
#   * ClaudeCode  -> <BaseDir>/.mcp.json
#   * Cursor      -> <BaseDir>/.cursor/mcp.json
#   * VSCode      -> <BaseDir>/.vscode/mcp.json
#   * Gemini      -> <BaseDir>/.gemini/settings.json
#   * Codex       -> <BaseDir>/.codex/config.toml
#   * All         -> all of the above
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
    # mcpServers). Defaults to 'bp-reader' - matches our README + the
    # entry Claude Code looks for.
    [string]$ServerName = 'bp-reader',

    # If $true, overwrite Codex TOML even when it already exists. Pure
    # safety guard - JSON merges always preserve sibling entries, so
    # this flag is only consulted on the Codex path.
    [switch]$Force,

    # Default env vars to set per client entry. Reasonable defaults match
    # the README. Pass an empty hashtable to write no env block.
    [hashtable]$ServerEnv
)

$ErrorActionPreference = 'Stop'

# ---- resolve paths ----------------------------------------------------------

$scriptDir = Split-Path -Parent $PSCommandPath
if (-not $PluginDir)  { $PluginDir  = Split-Path -Parent $scriptDir }     # <plugin>/Scripts/ -> <plugin>
if (-not $ProjectDir) { $ProjectDir = Split-Path -Parent (Split-Path -Parent $PluginDir) } # <plugin>/.. -> Plugins -> ProjectDir
if (-not $BaseDir)    { $BaseDir    = $ProjectDir }
if (-not $ServerExe)  { $ServerExe  = Join-Path $ProjectDir 'Plugins\BlueprintReader\Binaries\Win64\BlueprintReaderMcp.exe' }

if (-not (Test-Path -LiteralPath $ServerExe)) {
    Write-Warning "Server exe not found at $ServerExe - config files will reference a path that doesn't exist yet."
    Write-Warning "Build the server first via Plugins/BlueprintReader/Scripts/Build-MCPServer.ps1 or include the plugin in your editor target."
}

if (-not $ServerEnv) {
    # Sensible defaults - same vars Start-MCPServer.ps1 considers.
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
            # REL-1: NEVER fall through to an empty hashtable here — the caller
            # merges into whatever we return and saves it back, so returning @{}
            # would replace the user's entire config (every other MCP server
            # entry, every unrelated setting) because of one stray comma. Refuse
            # this client's write instead and tell the user how to recover.
            throw ("Existing config at $path is not valid JSON ($($_.Exception.Message)). " +
                   "Refusing to overwrite it - your other MCP servers/settings would be lost. " +
                   "Fix or delete that file, then re-run this script.")
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
    # REL-1/REL-3: back up the pre-edit file, then publish atomically
    # (temp file in the same dir + rename) so a crash mid-write can never
    # leave a truncated config behind.
    if (Test-Path -LiteralPath $path) {
        Copy-Item -LiteralPath $path -Destination "$path.bak" -Force
    }
    $tmp = "$path.tmp.$PID"
    Set-Content -LiteralPath $tmp -Value $json -Encoding utf8 -NoNewline
    Move-Item -LiteralPath $tmp -Destination $path -Force
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
    # REL-3: same backup + atomic-publish discipline as the JSON writers
    # (only reachable with -Force when the file already exists).
    if (Test-Path -LiteralPath $path) {
        Copy-Item -LiteralPath $path -Destination "$path.bak" -Force
    }
    $tmp = "$path.tmp.$PID"
    Set-Content -LiteralPath $tmp -Value ($lines -join "`n") -Encoding utf8 -NoNewline
    Move-Item -LiteralPath $tmp -Destination $path -Force
    return $path
}

# ---- dispatch ---------------------------------------------------------------

$targets = if ($Client -eq 'All') { @('ClaudeCode','Cursor','VSCode','Gemini','Codex') } else { @($Client) }
$written = @()
$failed  = @()

foreach ($t in $targets) {
    # REL-1: a corrupt existing config aborts THAT client's write (throw from
    # Read-JsonFileOrEmpty) without blocking the other clients' writes.
    try {
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
    } catch {
        Write-Warning "${t}: $($_.Exception.Message)"
        $failed += $t
    }
}

Write-Host ("done - {0} file(s) updated" -f $written.Count)
if ($failed.Count -gt 0) {
    Write-Warning ("{0} client config(s) NOT written: {1}" -f $failed.Count, ($failed -join ', '))
    exit 1
}
