# Live smoke test for Phase 1 (BPIR + decompile + readable C++).
# Drives decompile_function + transpile_function through a real
# UnrealEditor-Cmd commandlet daemon against a fixture BP, asserting:
#   - decompile produces a non-empty BPIR doc
#   - the doc validates as a {kind:"function"} shape
#   - transpile produces non-empty C++ source with the function's name
#   - unsupported_count is reported (0 for clean fixtures)
#
# Side-effect audit: read-only end to end. No mutations; safe to run
# repeatedly against any BP without leaving artifacts behind.
#
# Usage:
#   pwsh -File scripts\smoke-decompile.ps1 `
#        -Exe build\Release\bp-reader-mcp.exe `
#        [-Asset /Game/AI/BP_TestEnemy] `
#        [-Function TakeDamage]

param(
    [Parameter(Mandatory=$true)] [string]$Exe,
    [string]$Asset    = "/Game/AI/BP_TestEnemy",
    [string]$Function = "TakeDamage"
)

if (-not (Test-Path $Exe)) {
    throw "Executable not found: $Exe"
}
$Exe = (Resolve-Path $Exe).Path

function Frame([string]$json) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    "Content-Length: $($bytes.Length)`r`n`r`n$json"
}

# Build the request stream. Each tool call is its own JSON-RPC frame.
$messages = @()
$messages += Frame (@{
    jsonrpc = "2.0"; id = 1; method = "initialize"
    params  = @{ protocolVersion = "2024-11-05"; capabilities = @{}; clientInfo = @{ name = "smoke-decompile"; version = "1.0" } }
} | ConvertTo-Json -Depth 8 -Compress)
$messages += Frame (@{
    jsonrpc = "2.0"; method = "notifications/initialized"; params = @{}
} | ConvertTo-Json -Depth 4 -Compress)

# 1. decompile_function — BP → BPIR. Asserts the walker reaches the
#    function's graph and produces a structured AST.
$messages += Frame (@{
    jsonrpc = "2.0"; id = 2; method = "tools/call"
    params  = @{ name = "decompile_function"
                 arguments = @{ asset_path = $Asset; function_name = $Function } }
} | ConvertTo-Json -Depth 8 -Compress)

# 2. transpile_function — BPIR → C++ source. Asserts the codegen leg
#    produces a function definition wrapping the body.
$messages += Frame (@{
    jsonrpc = "2.0"; id = 3; method = "tools/call"
    params  = @{ name = "transpile_function"
                 arguments = @{ asset_path = $Asset; function_name = $Function;
                                target_lang = "cpp"; mode = "readable" } }
} | ConvertTo-Json -Depth 8 -Compress)

$payload = -join $messages
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true

if (Test-Path "D:\Projects\Unreal Engine 5") {
    $psi.EnvironmentVariables["BP_READER_ENGINE_DIR"] = "D:\Projects\Unreal Engine 5"
}
$psi.EnvironmentVariables["BP_READER_PROJECT"] = "D:\Projects\UE5_MCP\UE5_MCP.uproject"
$psi.EnvironmentVariables["BP_READER_BACKEND"] = "commandlet"

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

# Assertions: each frame after the initialize handshake should be a
# tools/call result with the expected payload shape.
$pass = 0; $fail = 0
foreach ($f in $frames) {
    Write-Host "--- frame ---"
    Write-Host $f
    try {
        $obj = ConvertFrom-Json $f
        if ($obj.error) {
            Write-Host ("FAIL: error response: {0}" -f $obj.error.message)
            ++$fail; continue
        }
        if ($obj.id -eq 2) {
            # decompile_function — extract bpir from MCP content envelope.
            $contentText = $obj.result.content[0].text
            $bpir = (ConvertFrom-Json $contentText).bpir
            if (-not $bpir) { Write-Host "FAIL: decompile returned empty bpir"; ++$fail; continue }
            if ($bpir.kind -ne "function") { Write-Host "FAIL: bpir.kind != 'function'"; ++$fail; continue }
            if (-not $bpir.body) { Write-Host "FAIL: bpir.body missing"; ++$fail; continue }
            Write-Host ("PASS: decompile produced BPIR with {0} body stmt(s), {1} unsupported" `
                        -f $bpir.body.Count, ($bpir.unsupported_nodes ? $bpir.unsupported_nodes.Count : 0))
            ++$pass
        } elseif ($obj.id -eq 3) {
            $contentText = $obj.result.content[0].text
            $payload = ConvertFrom-Json $contentText
            $src = $payload.source
            if (-not $src) { Write-Host "FAIL: transpile returned empty source"; ++$fail; continue }
            if ($src -notmatch [regex]::Escape($Function)) {
                Write-Host ("FAIL: transpile source missing function name '{0}'" -f $Function); ++$fail; continue
            }
            Write-Host ("PASS: transpile produced {0} bytes of C++, unsupported_count={1}" `
                        -f $src.Length, $payload.unsupported_count)
            ++$pass
        } elseif ($obj.result) {
            ++$pass  # initialize handshake
        }
    } catch {
        Write-Host ("FAIL: frame parse error: {0}" -f $_.Exception.Message)
        ++$fail
    }
}
Write-Host ("=== smoke-decompile summary: {0} passed, {1} failed ===" -f $pass, $fail)
if ($fail -gt 0) { exit 1 }
