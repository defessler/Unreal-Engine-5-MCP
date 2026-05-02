# Roundtrip a 4-step JSON-RPC sequence through bp-reader-mcp.exe and capture
# each response. Used to verify Phase 1 end-to-end with a real
# UnrealEditor-Cmd commandlet backend.
#
# Usage:
#   $env:BP_READER_BACKEND = "commandlet"
#   $env:BP_READER_ENGINE_DIR = "D:\Projects\UE5_AI_BP\UnrealEngine"
#   $env:BP_READER_PROJECT = "D:\Projects\UE5_AI_BP\UE5_AI_BP.uproject"
#   pwsh -File scripts\roundtrip.ps1 -Exe build\Release\bp-reader-mcp.exe -Asset /Game/AI/BP_TestEnemy

param(
    [Parameter(Mandatory=$true)] [string]$Exe,
    [Parameter(Mandatory=$true)] [string]$Asset,
    [string]$Path = "/Game/AI"
)

if (-not (Test-Path $Exe)) {
    throw "Executable not found: $Exe"
}

# JSON-RPC over LSP-style framing (Content-Length: N\r\n\r\n<body>).
function Frame([string]$json) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    "Content-Length: $($bytes.Length)`r`n`r`n$json"
}

$messages = @()
$messages += Frame (@{
    jsonrpc = "2.0"; id = 1; method = "initialize"
    params = @{ protocolVersion = "2024-11-05"; capabilities = @{}; clientInfo = @{ name = "phase1-roundtrip"; version = "1.0" } }
} | ConvertTo-Json -Depth 8 -Compress)
$messages += Frame (@{
    jsonrpc = "2.0"; method = "notifications/initialized"; params = @{}
} | ConvertTo-Json -Depth 4 -Compress)
$messages += Frame (@{
    jsonrpc = "2.0"; id = 2; method = "tools/call"
    params = @{ name = "list_blueprints"; arguments = @{ path = $Path } }
} | ConvertTo-Json -Depth 6 -Compress)
$messages += Frame (@{
    jsonrpc = "2.0"; id = 3; method = "tools/call"
    params = @{ name = "read_blueprint"; arguments = @{ asset_path = $Asset } }
} | ConvertTo-Json -Depth 6 -Compress)

$payload = -join $messages

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true

$proc = New-Object System.Diagnostics.Process
$proc.StartInfo = $psi
$null = $proc.Start()

# Write framed payload (binary).
$stdin = $proc.StandardInput.BaseStream
$bytes = [System.Text.Encoding]::UTF8.GetBytes($payload)
$stdin.Write($bytes, 0, $bytes.Length)
$stdin.Flush()
# Closing stdin signals EOF; the server's Run loop exits on EOF.
$proc.StandardInput.Close()

# Read the response stream until EOF. Output is the same Content-Length framing.
$stdout = $proc.StandardOutput.BaseStream
$ms = New-Object System.IO.MemoryStream
$buf = New-Object byte[] 4096
while (($n = $stdout.Read($buf, 0, $buf.Length)) -gt 0) {
    $ms.Write($buf, 0, $n)
}
$proc.WaitForExit()
$rawBytes = $ms.ToArray()
$raw = [System.Text.Encoding]::UTF8.GetString($rawBytes)

# Parse each Content-Length frame.
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
    $body = $raw.Substring($bodyStart, $len)
    $frames += $body
    $pos = $bodyStart + $len
}

Write-Host ("=== captured {0} frame(s); stderr tail ===`n{1}" -f $frames.Count, $proc.StandardError.ReadToEnd())
foreach ($f in $frames) {
    Write-Host "--- frame ---"
    Write-Host $f
}
