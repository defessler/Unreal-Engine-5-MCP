# Dump-Tools.ps1 — regenerate the tool catalog (docs/TOOLS.md + docs/tools.json)
# from the live ToolRegistry via `BlueprintReaderMcp --dump-tools`.
#
# Run this after adding/removing/renaming a tool so the catalog + the tool
# count stay accurate with zero hand-maintenance. The generated files are the
# single source of truth for "what tools exist" — docs/AGENTS.md/README defer
# to them instead of hardcoding counts (which used to drift across 6+ files).
#
#   pwsh Plugins/BlueprintReader/Scripts/Dump-Tools.ps1            # regenerate
#   pwsh Plugins/BlueprintReader/Scripts/Dump-Tools.ps1 -Check     # CI: fail on drift
[CmdletBinding()]
param([switch]$Check)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
# The server exe lives in the plugin's own Binaries (portable with the plugin);
# fall back to the legacy repo Binaries/Win64 for older builds.
$exe  = Join-Path $repo 'Plugins\BlueprintReader\Binaries\Win64\BlueprintReaderMcp.exe'
if (-not (Test-Path $exe)) {
    $legacy = Join-Path $repo 'Binaries\Win64\BlueprintReaderMcp.exe'
    if (Test-Path $legacy) { $exe = $legacy }
}
if (-not (Test-Path $exe)) {
    throw "BlueprintReaderMcp.exe not found at $exe - build it first (Build-MCPServer.ps1 / build-mcp-cmake.ps1)."
}

$docs = Join-Path $repo 'docs'
New-Item -ItemType Directory -Force $docs | Out-Null
$mdPath   = Join-Path $docs 'TOOLS.md'
$jsonPath = Join-Path $docs 'tools.json'

# PowerShell line-captures normalize CRLF -> per-line; re-join with LF so the
# checked-in files have stable, platform-independent line endings.
$md   = ((& $exe dump-tools --md)   -join "`n") + "`n"
$json = ((& $exe dump-tools --json) -join "`n") + "`n"
$enc  = New-Object System.Text.UTF8Encoding($false)

if ($Check) {
    $drift = @()
    foreach ($p in @(@{Path=$mdPath; New=$md}, @{Path=$jsonPath; New=$json})) {
        $cur = (Test-Path $p.Path) ? ([IO.File]::ReadAllText($p.Path) -replace "`r`n","`n") : ''
        if ($cur -ne $p.New) { $drift += $p.Path }
    }
    if ($drift.Count) {
        Write-Host "Tool catalog is STALE — regenerate + commit:" -ForegroundColor Red
        $drift | ForEach-Object { Write-Host "  $_" }
        Write-Host "  pwsh Plugins/BlueprintReader/Scripts/Dump-Tools.ps1" -ForegroundColor Yellow
        exit 1
    }
    Write-Host "Tool catalog is up to date." -ForegroundColor Green
    exit 0
}

[IO.File]::WriteAllText($mdPath,   $md,   $enc)
[IO.File]::WriteAllText($jsonPath, $json, $enc)
$count = ($json | ConvertFrom-Json).Count
Write-Host "Wrote $mdPath + $jsonPath ($count tools)." -ForegroundColor Green
