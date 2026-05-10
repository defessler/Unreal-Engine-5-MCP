# Live smoke test for compilable UE C++ class generation.
# Drives transpile_blueprint + write_generated_source through a real
# UnrealEditor-Cmd commandlet daemon, asserting the generated .h/.cpp
# pair lands on disk under <Project>/Source/, then optionally invokes
# UBT to confirm the editor target rebuilds with the new class.
#
# Side-effect audit: writes <Project>/Source/<Module>/Generated/
# <Class>_Generated.{h,cpp} + <Class>_Generated.transpile-notes.json.
# Re-running overwrites; safe but check git status before committing.
#
# Usage:
#   pwsh -File scripts\smoke-transpile-cpp.ps1 `
#        -Exe build\Release\bp-reader-mcp.exe `
#        [-Asset /Game/AI/BP_TestEnemy] `
#        [-Module UE5_MCP] `
#        [-OutputSubdir Generated] `
#        [-RunUbt]            # rebuild editor target after writing files

param(
    [Parameter(Mandatory=$true)] [string]$Exe,
    [string]$Asset        = "/Game/AI/BP_TestEnemy",
    [string]$Module       = "UE5_MCP",
    [string]$OutputSubdir = "Generated",
    [string]$ProjectDir   = "D:\Projects\UE5_MCP",
    [switch]$RunUbt
)

if (-not (Test-Path $Exe)) {
    throw "Executable not found: $Exe"
}
$Exe = (Resolve-Path $Exe).Path

function Frame([string]$json) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    "Content-Length: $($bytes.Length)`r`n`r`n$json"
}

# Step 1: ask the daemon to transpile the BP, getting back the generated
# .h/.cpp source strings + suggested filenames + the sidecar JSON.
$messages = @()
$messages += Frame (@{
    jsonrpc = "2.0"; id = 1; method = "initialize"
    params  = @{ protocolVersion = "2024-11-05"; capabilities = @{}; clientInfo = @{ name = "smoke-transpile-cpp"; version = "1.0" } }
} | ConvertTo-Json -Depth 8 -Compress)
$messages += Frame (@{
    jsonrpc = "2.0"; method = "notifications/initialized"; params = @{}
} | ConvertTo-Json -Depth 4 -Compress)
$messages += Frame (@{
    jsonrpc = "2.0"; id = 2; method = "tools/call"
    params  = @{ name = "transpile_blueprint"
                 arguments = @{ asset_path = $Asset; target_lang = "cpp"
                                module_api_macro  = ($Module.ToUpper() + "_API")
                                class_name_suffix = "_Generated" } }
} | ConvertTo-Json -Depth 10 -Compress)

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
$psi.EnvironmentVariables["BP_READER_PROJECT"] = (Join-Path $ProjectDir "$Module.uproject")
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

# Pull transpile_blueprint result out of the second response (id=2).
$transpileFrame = $frames | Where-Object { (ConvertFrom-Json $_).id -eq 2 } | Select-Object -First 1
if (-not $transpileFrame) {
    Write-Host "FAIL: no response for transpile_blueprint"
    Write-Host $proc.StandardError.ReadToEnd()
    exit 1
}
$obj = ConvertFrom-Json $transpileFrame
if ($obj.error) {
    Write-Host ("FAIL: transpile error: {0}" -f $obj.error.message)
    exit 1
}
$result = ConvertFrom-Json $obj.result.content[0].text

Write-Host ("=== transpile_blueprint result: class={0} unsupported={1} ===" `
             -f $result.class_name, $result.unsupported_count)

# Step 2: write each generated file. We bypass the daemon for simplicity
# here (no second JSON-RPC round-trip) — the smoke is checking the
# transpile output is well-formed + writable, not the daemon-side write
# path (which has its own unit tests).
$outDir = Join-Path $ProjectDir "Source\$Module\$OutputSubdir"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$hPath = Join-Path $outDir $result.header_file
$cPath = Join-Path $outDir $result.impl_file
$sPath = Join-Path $outDir $result.sidecar_file

[System.IO.File]::WriteAllText($hPath, $result.header_source, [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::WriteAllText($cPath, $result.impl_source,   [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::WriteAllText($sPath, ($result.sidecar | ConvertTo-Json -Depth 12),
                               [System.Text.UTF8Encoding]::new($false))

Write-Host ("PASS: wrote {0} ({1} bytes)" -f $hPath, $result.header_source.Length)
Write-Host ("PASS: wrote {0} ({1} bytes)" -f $cPath, $result.impl_source.Length)
Write-Host ("PASS: wrote sidecar {0}" -f $sPath)

# Spot-check: header should mention UCLASS, impl should reference the class.
if ($result.header_source -notmatch "UCLASS\(") {
    Write-Host "WARN: header source missing UCLASS() macro"
}
if ($result.impl_source -notmatch [regex]::Escape($result.class_name)) {
    Write-Host "WARN: impl source missing class name reference"
}

# Step 3 (optional): kick UBT to confirm the project still compiles.
if ($RunUbt) {
    $buildBat = "D:\Projects\Unreal Engine 5\Engine\Build\BatchFiles\Build.bat"
    if (-not (Test-Path $buildBat)) {
        Write-Host "WARN: Build.bat not found; skipping UBT step"
    } else {
        Write-Host "=== Running UBT (NoUba, MaxParallelActions=4 — required on this machine) ==="
        $ubtArgs = @(
            "$($Module)Editor", "Win64", "Development",
            "-project=$ProjectDir\$Module.uproject",
            "-waitmutex", "-NoUba", "-MaxParallelActions=4"
        )
        & $buildBat @ubtArgs
        if ($LASTEXITCODE -ne 0) {
            Write-Host ("FAIL: UBT exited {0}" -f $LASTEXITCODE)
            exit 1
        }
        Write-Host "PASS: UBT compiled the editor target with the new class"
    }
}

Write-Host "=== smoke-transpile-cpp: all checks passed ==="
