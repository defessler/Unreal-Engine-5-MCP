# Run-LiveTests.ps1 — make the gated `[live]` doctests routine.
#
# Root cause of the old "daemon never handshakes for Lyra" pain: it was the
# ONE-TIME cold shader/DDC compile on the first UE 5.8 open of 5.6-era content,
# NOT a handshake bug. Once warm, the daemon publishes its handshake in ~15s and
# the MCP server attaches in <1s. The fix for routine live testing is therefore
# operational: keep ONE warm daemon alive and ATTACH the tests to it, instead of
# auto-spawning (and paying a cold start) per test process.
#
# This script ensures a daemon is up (reusing a live one, or spawning + warming
# one), then runs the live doctests against it — typically a few seconds total.
#
# Usage:
#   Run-LiveTests.ps1 -EngineDir "D:\Games\Epic Games\UE_5.8" `
#                     -ProjectFile "D:\Projects\UE5_MCP\LyraStarterGame.uproject"
#   add -IncludeRoundtrip to also run the (deferred) transpile [roundtrip] cases.
[CmdletBinding()]
param(
  [Parameter(Mandatory)] [string]$EngineDir,
  [Parameter(Mandatory)] [string]$ProjectFile,
  [string]$ExePath,
  [int]$HandshakeTimeoutSec = 600,   # generous: a COLD first open can compile shaders for minutes
  [switch]$IncludeRoundtrip
)
$ErrorActionPreference = "Stop"

$projDir = Split-Path -Parent $ProjectFile
$saved   = Join-Path $projDir "Saved"
$hs      = Join-Path $saved "bp-reader-cmdlet.json"
$editor  = Join-Path $EngineDir "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
if (-not $ExePath) {
  $ExePath = Join-Path (Split-Path -Parent $PSScriptRoot) "Binaries\Win64\BlueprintReaderMcpTests.exe"
}
foreach ($p in @($editor, $ProjectFile, $ExePath)) {
  if (-not (Test-Path $p)) { throw "not found: $p" }
}

function Get-LiveDaemon {
  if ((Test-Path $hs) -and (Get-Item $hs).Length -gt 0) {
    try { $j = Get-Content $hs -Raw | ConvertFrom-Json } catch { return $null }
    if ($j.pid -and (Get-Process -Id $j.pid -ErrorAction SilentlyContinue)) { return $j }
  }
  return $null
}

$daemon = Get-LiveDaemon
if ($daemon) {
  Write-Host "[live-tests] reusing warm daemon: port=$($daemon.port) pid=$($daemon.pid)"
} else {
  Write-Host "[live-tests] no daemon up — spawning + warming one (cold first open can take minutes)..."
  if (Test-Path $hs) { Remove-Item $hs -Force }
  New-Item -ItemType Directory -Force $saved | Out-Null
  $env:BP_READER_DAEMON_IDLE_SECONDS = "7200"
  $p = Start-Process -FilePath $editor -ArgumentList @(
        $ProjectFile,"-run=BPR","-Daemon","-nullrhi","-nosplash","-unattended","-nopause","-stdout"
      ) -RedirectStandardOutput (Join-Path $saved "daemon.log") `
        -RedirectStandardError (Join-Path $saved "daemon.err.log") -PassThru -WindowStyle Hidden
  $t0 = Get-Date
  while (-not $daemon) {
    if ($p.HasExited) { throw "daemon exited code=$($p.ExitCode); see $saved\daemon.err.log" }
    if (((Get-Date) - $t0).TotalSeconds -gt $HandshakeTimeoutSec) { $p.Kill(); throw "daemon did not handshake within $HandshakeTimeoutSec s" }
    Start-Sleep -Seconds 3
    $daemon = Get-LiveDaemon
  }
  Write-Host ("[live-tests] daemon ready in {0:n0}s: port={1} pid={2}" -f ((Get-Date)-$t0).TotalSeconds, $daemon.port, $daemon.pid)
}

$env:BP_READER_ENGINE_DIR = $EngineDir
$env:BP_READER_PROJECT    = $ProjectFile
$tcArgs = @('--test-case=[live]*')
if (-not $IncludeRoundtrip) { $tcArgs += '--test-case-exclude=*roundtrip*' }   # [roundtrip] = deferred transpile track

Write-Host "[live-tests] running $($tcArgs -join ' ') ..."
$rt0 = Get-Date
& $ExePath @tcArgs
$code = $LASTEXITCODE
Write-Host ("[live-tests] done in {0:n0}s (exit $code)" -f ((Get-Date)-$rt0).TotalSeconds)

# Prune throwaway assets the [live][smoke] create-type tools materialize at
# their unique smoke paths (a couple of create_* tools, e.g. create_material /
# create_material_instance, actually save Content/__bpr_smoke_<tool>__.uasset).
# Bounded + deterministic, but pruning keeps re-runs and the working tree clean.
$smokeLitter = Get-ChildItem (Join-Path $projDir "Content") -Recurse -Filter "__bpr_smoke_*" -ErrorAction SilentlyContinue
if ($smokeLitter) {
  $smokeLitter | Remove-Item -Force -ErrorAction SilentlyContinue
  Write-Host "[live-tests] pruned $($smokeLitter.Count) smoke scratch asset(s)"
}
exit $code
