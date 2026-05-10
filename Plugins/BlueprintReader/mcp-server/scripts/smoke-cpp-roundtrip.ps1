# Live smoke test for C++ → BPIR via parse_cpp_function.
# Closes the BP↔C++ loop end-to-end: take a hand-written C++ snippet,
# parse it to BPIR, feed the BPIR to compile_function to materialize a
# real BP function — confirming the parser → BPIR → compile_function
# pipeline produces a graph the editor accepts.
#
# Side-effect audit: creates a function on the destination BP. The BP
# itself is created if missing (idempotent). Re-running replaces the
# function definition (compile_function rebuilds the body).
#
# Usage:
#   pwsh -File scripts\smoke-cpp-roundtrip.ps1 `
#        -Exe build\Release\bp-reader-mcp.exe `
#        [-DestBp /Game/AI/BP_FromCpp] `
#        [-Function TakeDamage]

param(
    [Parameter(Mandatory=$true)] [string]$Exe,
    [string]$DestBp   = "/Game/AI/BP_FromCpp",
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

# A representative C++ snippet covering the major control-flow + expression
# forms the parser supports. If this round-trips, the typical
# editor-written function will too.
$cppSource = @"
bool $Function(float Damage) {
    if (bIsAlive) {
        Health -= Damage;
        if (Health <= 0) {
            return true;
        }
    }
    return false;
}
"@

$messages = @()
$messages += Frame (@{
    jsonrpc = "2.0"; id = 1; method = "initialize"
    params  = @{ protocolVersion = "2024-11-05"; capabilities = @{}; clientInfo = @{ name = "smoke-cpp-roundtrip"; version = "1.0" } }
} | ConvertTo-Json -Depth 8 -Compress)
$messages += Frame (@{
    jsonrpc = "2.0"; method = "notifications/initialized"; params = @{}
} | ConvertTo-Json -Depth 4 -Compress)

# 1. Make sure the destination BP exists. create_blueprint is idempotent.
$messages += Frame (@{
    jsonrpc = "2.0"; id = 2; method = "tools/call"
    params  = @{ name = "create_blueprint"
                 arguments = @{ asset_path = $DestBp; parent_class = "Actor" } }
} | ConvertTo-Json -Depth 8 -Compress)

# 2. Parse the C++ snippet → BPIR.
$messages += Frame (@{
    jsonrpc = "2.0"; id = 3; method = "tools/call"
    params  = @{ name = "parse_cpp_function"
                 arguments = @{ source = $cppSource } }
} | ConvertTo-Json -Depth 8 -Compress)

$payload = -join $messages
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.RedirectStandardInput  = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError  = $true
$psi.UseShellExecute = $false
$psi.CreateNoWindow  = $true

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
$stdin.Write($bytes, 0, $bytes.Length); $stdin.Flush()
$proc.StandardInput.Close()

$stdout = $proc.StandardOutput.BaseStream
$ms = New-Object System.IO.MemoryStream
$buf = New-Object byte[] 4096
while (($n = $stdout.Read($buf, 0, $buf.Length)) -gt 0) { $ms.Write($buf, 0, $n) }
$proc.WaitForExit()
$raw = [System.Text.Encoding]::UTF8.GetString($ms.ToArray())

# Parse frames.
$pos = 0; $frames = @()
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

$parseFrame = $frames | Where-Object { (ConvertFrom-Json $_).id -eq 3 } | Select-Object -First 1
if (-not $parseFrame) {
    Write-Host "FAIL: no parse_cpp_function response"
    Write-Host $proc.StandardError.ReadToEnd()
    exit 1
}
$obj = ConvertFrom-Json $parseFrame
if ($obj.error) {
    Write-Host ("FAIL: parse error: {0}" -f $obj.error.message)
    exit 1
}
$bpir = (ConvertFrom-Json $obj.result.content[0].text).bpir
if (-not $bpir) {
    Write-Host "FAIL: parse returned empty bpir"
    exit 1
}
if ($bpir.kind -ne "function") {
    Write-Host ("FAIL: expected kind=function, got '{0}'" -f $bpir.kind)
    exit 1
}
if ($bpir.name -ne $Function) {
    Write-Host ("FAIL: expected name={0}, got '{1}'" -f $Function, $bpir.name)
    exit 1
}
if (-not $bpir.body -or $bpir.body.Count -lt 2) {
    Write-Host ("FAIL: body has {0} stmts, expected >= 2" -f $bpir.body.Count)
    exit 1
}
Write-Host ("PASS: parse produced BPIR for {0} ({1} body stmts)" -f $bpir.name, $bpir.body.Count)

# 3. Feed the BPIR to compile_function to materialize the BP graph.
# We open a second daemon session to keep the framing simple.
$messages2 = @()
$messages2 += Frame (@{
    jsonrpc = "2.0"; id = 1; method = "initialize"
    params  = @{ protocolVersion = "2024-11-05"; capabilities = @{}; clientInfo = @{ name = "smoke-cpp-roundtrip"; version = "1.0" } }
} | ConvertTo-Json -Depth 8 -Compress)
$messages2 += Frame (@{
    jsonrpc = "2.0"; method = "notifications/initialized"; params = @{}
} | ConvertTo-Json -Depth 4 -Compress)

# compile_function expects asset_path / function_name + the body etc.
# from the BPIR doc. Pass through inputs/outputs/locals as the BPIR
# captured them from the C++ signature.
$compileArgs = @{
    asset_path    = $DestBp
    function_name = $bpir.name
    body          = $bpir.body
}
if ($bpir.inputs)  { $compileArgs["inputs"]  = $bpir.inputs }
if ($bpir.outputs) { $compileArgs["outputs"] = $bpir.outputs }
if ($bpir.locals)  { $compileArgs["locals"]  = $bpir.locals }
$messages2 += Frame (@{
    jsonrpc = "2.0"; id = 2; method = "tools/call"
    params  = @{ name = "compile_function"; arguments = $compileArgs }
} | ConvertTo-Json -Depth 14 -Compress)

$payload2 = -join $messages2
$psi2 = New-Object System.Diagnostics.ProcessStartInfo
$psi2.FileName = $Exe
$psi2.RedirectStandardInput  = $true
$psi2.RedirectStandardOutput = $true
$psi2.RedirectStandardError  = $true
$psi2.UseShellExecute = $false
$psi2.CreateNoWindow  = $true
if (Test-Path "D:\Projects\Unreal Engine 5") {
    $psi2.EnvironmentVariables["BP_READER_ENGINE_DIR"] = "D:\Projects\Unreal Engine 5"
}
$psi2.EnvironmentVariables["BP_READER_PROJECT"] = "D:\Projects\UE5_MCP\UE5_MCP.uproject"
$psi2.EnvironmentVariables["BP_READER_BACKEND"] = "commandlet"

$proc2 = New-Object System.Diagnostics.Process
$proc2.StartInfo = $psi2
$null = $proc2.Start()
$stdin2 = $proc2.StandardInput.BaseStream
$bytes2 = [System.Text.Encoding]::UTF8.GetBytes($payload2)
$stdin2.Write($bytes2, 0, $bytes2.Length); $stdin2.Flush()
$proc2.StandardInput.Close()

$stdout2 = $proc2.StandardOutput.BaseStream
$ms2 = New-Object System.IO.MemoryStream
$buf2 = New-Object byte[] 4096
while (($n = $stdout2.Read($buf2, 0, $buf2.Length)) -gt 0) { $ms2.Write($buf2, 0, $n) }
$proc2.WaitForExit()
$raw2 = [System.Text.Encoding]::UTF8.GetString($ms2.ToArray())

# Find the compile_function response (id=2 in the second session).
$pos = 0; $frames2 = @()
while ($pos -lt $raw2.Length) {
    $headerEnd = $raw2.IndexOf("`r`n`r`n", $pos)
    if ($headerEnd -lt 0) { break }
    $header = $raw2.Substring($pos, $headerEnd - $pos)
    $clMatch = [regex]::Match($header, 'Content-Length:\s*(\d+)', 'IgnoreCase')
    if (-not $clMatch.Success) { break }
    $len = [int]$clMatch.Groups[1].Value
    $bodyStart = $headerEnd + 4
    if ($bodyStart + $len -gt $raw2.Length) { break }
    $frames2 += $raw2.Substring($bodyStart, $len)
    $pos = $bodyStart + $len
}
$compileFrame = $frames2 | Where-Object { (ConvertFrom-Json $_).id -eq 2 } | Select-Object -First 1
if (-not $compileFrame) {
    Write-Host "FAIL: no compile_function response"
    Write-Host $proc2.StandardError.ReadToEnd()
    exit 1
}
$compileObj = ConvertFrom-Json $compileFrame
if ($compileObj.error) {
    Write-Host ("FAIL: compile_function error: {0}" -f $compileObj.error.message)
    exit 1
}
Write-Host ("PASS: compile_function materialized {0}.{1} from C++ source" -f $DestBp, $bpir.name)
Write-Host "=== smoke-cpp-roundtrip: all checks passed ==="
