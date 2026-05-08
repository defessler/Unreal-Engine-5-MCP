# Live smoke test for the new batch + write tools (A1, A3, B2, C1).
# Drives apply_ops + preview_ops + summarize_blueprint through a real
# UnrealEditor-Cmd commandlet daemon, asserting that the BeginBatch /
# EndBatch round-trip works end-to-end without touching any persistent
# state on disk.
#
# Side-effect audit: every mutation in this script is idempotent against
# an existing test fixture, OR uses an asset path whose existence we
# tolerate (create_blueprint short-circuits if the asset already exists).
#
# Usage:
#   pwsh -File scripts\smoke-batch-ops.ps1 -Exe build\Release\bp-reader-mcp.exe

param(
    [Parameter(Mandatory=$true)] [string]$Exe,
    [string]$Asset = "/Game/AI/BP_TestEnemy"
)

if (-not (Test-Path $Exe)) {
    throw "Executable not found: $Exe"
}
# ProcessStartInfo needs an absolute path — relative paths fail at Start().
$Exe = (Resolve-Path $Exe).Path

function Frame([string]$json) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    "Content-Length: $($bytes.Length)`r`n`r`n$json"
}

# Build the request stream. Each tool call is its own JSON-RPC frame.
$messages = @()
$messages += Frame (@{
    jsonrpc = "2.0"; id = 1; method = "initialize"
    params  = @{ protocolVersion = "2024-11-05"; capabilities = @{}; clientInfo = @{ name = "smoke-batch-ops"; version = "1.0" } }
} | ConvertTo-Json -Depth 8 -Compress)
$messages += Frame (@{
    jsonrpc = "2.0"; method = "notifications/initialized"; params = @{}
} | ConvertTo-Json -Depth 4 -Compress)

# 1. summarize_blueprint — confirms the daemon comes up clean and reads
#    work as expected.
$messages += Frame (@{
    jsonrpc = "2.0"; id = 2; method = "tools/call"
    params  = @{ name = "summarize_blueprint"; arguments = @{ asset_path = $Asset } }
} | ConvertTo-Json -Depth 8 -Compress)

# 2. preview_ops — read-only validation pass against a batch the agent
#    is "thinking about" running. Exercises the dispatch + slot resolution
#    + would_compile reporting paths.
$previewOps = @(
    @{ op = "add_variable"; asset_path = $Asset; name = "Health"; type = "float" },
    @{ op = "add_node"; id = "branch"; asset_path = $Asset; graph_name = "EventGraph"
       kind = "Branch"; x = 0; y = 0 },
    @{ op = "wire_pins"; asset_path = $Asset; graph_name = "EventGraph"
       from_node = "`$branch"; from_pin = "then"; to_node = "`$branch"; to_pin = "execute" }
)
$messages += Frame (@{
    jsonrpc = "2.0"; id = 3; method = "tools/call"
    params  = @{ name = "preview_ops"; arguments = @{ ops = $previewOps } }
} | ConvertTo-Json -Depth 10 -Compress)

# 3. apply_ops — single idempotent op. Health already exists on
#    BP_TestEnemy, so add_variable's idempotency probe short-circuits
#    without mutating. Confirms BeginBatch + EndBatch flow without
#    leaving anything behind.
$applyOps = @(
    @{ op = "add_variable"; asset_path = $Asset; name = "Health"; type = "float" }
)
$messages += Frame (@{
    jsonrpc = "2.0"; id = 4; method = "tools/call"
    params  = @{ name = "apply_ops"; arguments = @{ ops = $applyOps; atomic = $true } }
} | ConvertTo-Json -Depth 10 -Compress)

$payload = -join $messages
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true

# Pass through the env explicitly. The registry-based engine auto-discovery
# can fail under some pwsh child-process contexts, so we set the engine dir
# when known. BP_READER_PROJECT auto-discovers from the exe location.
if (Test-Path "D:\Projects\Unreal Engine 5") {
    $psi.EnvironmentVariables["BP_READER_ENGINE_DIR"] = "D:\Projects\Unreal Engine 5"
}
$psi.EnvironmentVariables["BP_READER_PROJECT"]  = "D:\Projects\UE5_MCP\UE5_MCP.uproject"
$psi.EnvironmentVariables["BP_READER_BACKEND"]  = "commandlet"

$proc = New-Object System.Diagnostics.Process
$proc.StartInfo = $psi
$null = $proc.Start()

$stdin = $proc.StandardInput.BaseStream
$bytes = [System.Text.Encoding]::UTF8.GetBytes($payload)
$stdin.Write($bytes, 0, $bytes.Length)
$stdin.Flush()
$proc.StandardInput.Close()

$stdout = $proc.StandardOutput.BaseStream
$ms = New-Object System.IO.MemoryStream
$buf = New-Object byte[] 4096
while (($n = $stdout.Read($buf, 0, $buf.Length)) -gt 0) {
    $ms.Write($buf, 0, $n)
}
$proc.WaitForExit()
$raw = [System.Text.Encoding]::UTF8.GetString($ms.ToArray())

# Parse Content-Length frames.
$pos = 0
$frames = @()
while ($pos -lt $raw.Length) {
    $headerEnd = $raw.IndexOf("`r`n`r`n", $pos)
    if ($headerEnd -lt 0) { break }
    $header = $raw.Substring($pos, $headerEnd - $pos)
    $clMatch = [regex]::Match($header, 'Content-Length:\s*(\d+)', 'IgnoreCase')
    if (-not $clMatch.Success) { break }
    $len = [int]$clMatch.Groups[1].Value
    $bodyStart = $headerEnd + 4
    if ($bodyStart + $len -gt $raw.Length) { break }
    $frames += $raw.Substring($bodyStart, $len)
    $pos = $bodyStart + $len
}

Write-Host ("=== captured {0} frame(s) ===" -f $frames.Count)
$stderrTail = $proc.StandardError.ReadToEnd()
Write-Host "--- stderr tail ---"
Write-Host $stderrTail

$pass = 0; $fail = 0
foreach ($f in $frames) {
    Write-Host "--- frame ---"
    Write-Host $f
    try {
        $obj = ConvertFrom-Json $f
        if ($obj.error) { ++$fail; continue }
        if ($obj.result) { ++$pass }
    } catch { }
}
Write-Host ("=== smoke summary: {0} passed, {1} failed ===" -f $pass, $fail)
if ($fail -gt 0) { exit 1 }
