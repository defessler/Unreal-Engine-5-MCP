# Strip Lyra Assets Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove every Lyra-origin `.uasset` / `.umap` from the repo, ship `setup.bat` to restore them on demand (local Epic install first, GitHub Release fallback), and rewrite git history so the pack actually shrinks.

**Architecture:** Three PowerShell scripts under `Scripts/`. `LyraAssetCommon.ps1` is a pure-function library (manifest building, install detection, hashing). `setup.ps1` is the user-facing restorer (called via `setup.bat`). `Publish-LyraAssetsRelease.ps1` is the operator's bundler/publisher. A homegrown test runner (`Test-LyraAssetCommon.ps1`) drives the pure functions with assertion failures = non-zero exit. The manifest (`Scripts/lyra-assets-manifest.json`) is the committed source of truth for the asset set. Git history rewrite uses `git-filter-repo` against a regex spec.

**Tech Stack:** PowerShell 7+, GNU tar with zstd, `git-filter-repo` (pip), `gh` CLI, robocopy (Windows-built-in), certutil (Windows-built-in).

**Phases:**
- **Phase A (Tasks 1-13):** Write scripts, generate manifest, commit. Fully reversible.
- **HARD STOP** between A and B — explicit user "go" required.
- **Phase B (Tasks 14-17):** Operator-driven runbook. History rewrite, force-push. Irreversible.

**Spec:** [`docs/superpowers/specs/2026-05-19-strip-lyra-assets-design.md`](../specs/2026-05-19-strip-lyra-assets-design.md)

---

## File Structure

```
setup.bat                                     ← shim, ~3 lines, calls Scripts/setup.ps1
Scripts/
  LyraAssetCommon.ps1                         ← shared pure functions (no side effects beyond filesystem reads)
  setup.ps1                                   ← restorer (uses LyraAssetCommon)
  Publish-LyraAssetsRelease.ps1               ← operator-only bundler/publisher
  Test-LyraAssetCommon.ps1                    ← homegrown test runner; non-zero exit on failure
  strip-lyra-paths.txt                        ← regex spec for git-filter-repo
  lyra-assets-manifest.json                   ← committed artifact (~1 MB JSON, 8682 entries)
.gitignore                                    ← simplify (remove now-redundant per-file entries)
README.md                                     ← add "After clone: run setup.bat" section
CLAUDE.md                                     ← note asset restoration in Repo layout
```

### Module boundaries

`LyraAssetCommon.ps1` exposes these functions and is dot-sourced by `setup.ps1` and `Publish-LyraAssetsRelease.ps1`:

| Function                  | Inputs                                      | Output                                              |
|---------------------------|---------------------------------------------|-----------------------------------------------------|
| `Get-LyraAssetPaths`      | `-RepoRoot <string>`                        | `string[]` repo-relative paths of every Lyra asset  |
| `Get-FileManifestEntries` | `-RepoRoot`, `-RelativePaths <string[]>`    | `PSCustomObject[]` `{path, size, sha256}`           |
| `Build-Manifest`          | `-RepoRoot`, `-Tag <string>`                | `PSCustomObject` full manifest with header + files  |
| `Test-Manifest`           | `-RepoRoot`, `-Manifest <PSCustomObject>`   | `PSCustomObject` `{Ok, Missing[], Mismatch[]}`      |
| `Find-LyraInstallPaths`   | `-LauncherDatPath <string>`                 | `string[]` of `InstallLocation` dirs containing Lyra |
| `Get-RestorePathMap`      | `-LyraInstallRoot`, `-RepoRoot`             | `hashtable` `{src -> dst}` for robocopy             |

`setup.ps1` is the orchestrator: parses args, calls common functions in sequence, handles user output. `Publish-LyraAssetsRelease.ps1` reuses `Build-Manifest` and `Get-LyraAssetPaths`, adds tar+zstd packing and `gh release create`.

---

## Phase A — Build the restorer + bundler

### Task 1: Pre-flight — install git-filter-repo, scaffold dirs

**Files:**
- Create: `Scripts/` (dir)
- No commit yet — verification step

- [ ] **Step 1: Install git-filter-repo**

```bash
pip install git-filter-repo
```

Expected: exit 0. `git-filter-repo --version` reports something like `git-filter-repo 2.45.0`.

- [ ] **Step 2: Verify the tooling we'll need**

```bash
pwsh --version              # expect 7.x
tar --version | head -1     # expect GNU tar 1.31+ for --zstd
gh --version                # expect 2.x
git filter-repo --version   # expect 2.x
zstd --version              # optional but useful: tar will use it under the hood
```

Expected: all four print versions; no errors.

- [ ] **Step 3: Create Scripts/ dir if missing**

```bash
mkdir -p Scripts
```

No commit on this task — it's preparation.

---

### Task 2: Skeleton `LyraAssetCommon.ps1` + test runner

**Files:**
- Create: `Scripts/LyraAssetCommon.ps1`
- Create: `Scripts/Test-LyraAssetCommon.ps1`

- [ ] **Step 1: Write LyraAssetCommon.ps1 skeleton**

```powershell
# Scripts/LyraAssetCommon.ps1
# Shared pure functions for Lyra asset manifest building, install detection,
# and verification. Dot-sourced by setup.ps1 and Publish-LyraAssetsRelease.ps1.

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

# Bumped when manifest schema changes. setup.ps1 refuses to operate on a
# manifest with a different schema_version.
$script:ManifestSchemaVersion = 1

# Globs (relative to RepoRoot) whose .uasset/.umap contents are considered
# "Lyra assets" and removed by setup.bat / restored by setup.bat. Two
# exemptions: Content/AI/BP_TestEnemy.uasset and BP_TestPickup.uasset stay
# tracked because UBPRSeedCommandlet produces them and live tests assert on
# /Game/AI/BP_TestEnemy and /Game/AI/BP_TestPickup by literal path.
$script:LyraAssetGlobs = @(
    'Content',
    'Plugins/LyraExampleContent/Content',
    'Plugins/LyraExtTool/Content',
    'Plugins/PocketWorlds/Content',
    'Plugins/GameFeatures/ShooterCore/Content',
    'Plugins/GameFeatures/ShooterTests/Content',
    'Plugins/GameFeatures/TopDownArena/Content',
    'Plugins/GameFeatures/ShooterMaps/Content',
    'Plugins/GameFeatures/ShooterExplorer/Content'
)

$script:LyraAssetExemptions = @(
    'Content/AI/BP_TestEnemy.uasset',
    'Content/AI/BP_TestPickup.uasset'
)

function Get-LyraAssetPaths {
    [CmdletBinding()]
    param([Parameter(Mandatory)] [string] $RepoRoot)
    throw 'not implemented'
}

function Get-FileManifestEntries {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string]   $RepoRoot,
        [Parameter(Mandatory)] [string[]] $RelativePaths
    )
    throw 'not implemented'
}

function Build-Manifest {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string] $RepoRoot,
        [Parameter(Mandatory)] [string] $Tag
    )
    throw 'not implemented'
}

function Test-Manifest {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string]         $RepoRoot,
        [Parameter(Mandatory)] [PSCustomObject] $Manifest
    )
    throw 'not implemented'
}

function Find-LyraInstallPaths {
    [CmdletBinding()]
    param([Parameter(Mandatory)] [string] $LauncherDatPath)
    throw 'not implemented'
}

function Get-RestorePathMap {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string] $LyraInstallRoot,
        [Parameter(Mandatory)] [string] $RepoRoot
    )
    throw 'not implemented'
}
```

- [ ] **Step 2: Write Test-LyraAssetCommon.ps1 (homegrown test runner)**

```powershell
# Scripts/Test-LyraAssetCommon.ps1
# Lightweight test runner for LyraAssetCommon.ps1.
# Each test is a script block that throws on failure.
# Exit code: 0 if all pass, 1 if any fail. No external test framework.

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

. "$PSScriptRoot/LyraAssetCommon.ps1"

$script:Tests   = @()
$script:Passed  = 0
$script:Failed  = @()

function Test ([string] $Name, [scriptblock] $Body) {
    $script:Tests += [pscustomobject]@{ Name = $Name; Body = $Body }
}

function AssertEqual ($Expected, $Actual, [string] $Message = '') {
    # Deep-equal via JSON serialization. Works for primitives, arrays, hashtables, PSCustomObjects.
    $e = $Expected | ConvertTo-Json -Depth 20 -Compress
    $a = $Actual   | ConvertTo-Json -Depth 20 -Compress
    if ($e -ne $a) {
        throw "AssertEqual failed${($Message ? ": $Message" : '')}: expected $e, got $a"
    }
}

function AssertTrue ($Condition, [string] $Message = '') {
    if (-not $Condition) { throw "AssertTrue failed: $Message" }
}

function AssertThrows ([scriptblock] $Body, [string] $MatchPattern = '') {
    try   { & $Body | Out-Null }
    catch {
        if ($MatchPattern -and ($_.Exception.Message -notmatch $MatchPattern)) {
            throw "AssertThrows: expected match '$MatchPattern', got '$($_.Exception.Message)'"
        }
        return
    }
    throw 'AssertThrows: expected exception but none thrown'
}

# --- TESTS GO HERE (added in subsequent tasks) ---

# --- RUN ---

foreach ($t in $script:Tests) {
    try {
        & $t.Body
        $script:Passed++
        Write-Host "  pass: $($t.Name)" -ForegroundColor Green
    } catch {
        $script:Failed += [pscustomobject]@{ Name = $t.Name; Error = $_.Exception.Message }
        Write-Host "  FAIL: $($t.Name)" -ForegroundColor Red
        Write-Host "        $($_.Exception.Message)" -ForegroundColor DarkRed
    }
}

Write-Host ''
Write-Host "Passed: $($script:Passed)  Failed: $($script:Failed.Count)" -ForegroundColor ($script:Failed.Count ? 'Red' : 'Green')

if ($script:Failed.Count) { exit 1 } else { exit 0 }
```

- [ ] **Step 3: Run the test runner — expect 0 tests, exit 0**

```bash
pwsh -NoProfile -File Scripts/Test-LyraAssetCommon.ps1
echo "exit: $?"
```

Expected output:
```
Passed: 0  Failed: 0
exit: 0
```

- [ ] **Step 4: Commit**

```bash
git add Scripts/LyraAssetCommon.ps1 Scripts/Test-LyraAssetCommon.ps1
git commit -m "$(cat <<'EOF'
chore(scripts): scaffold LyraAssetCommon module + test runner

Pure-function library skeleton (Get-LyraAssetPaths,
Build-Manifest, Find-LyraInstallPaths, etc.) plus a dependency-free
test runner that throws on assertion failure and exits non-zero.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Implement & test `Get-LyraAssetPaths`

**Files:**
- Modify: `Scripts/LyraAssetCommon.ps1` (replace stub)
- Modify: `Scripts/Test-LyraAssetCommon.ps1` (add tests)

- [ ] **Step 1: Add the failing tests**

Insert before `# --- RUN ---` in `Scripts/Test-LyraAssetCommon.ps1`:

```powershell
Test 'Get-LyraAssetPaths: returns array of strings' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        New-Item -ItemType Directory -Path "$tmp/Content/Foo" -Force | Out-Null
        Set-Content -Path "$tmp/Content/Foo/Bar.uasset" -Value 'x'
        $paths = Get-LyraAssetPaths -RepoRoot $tmp.FullName
        AssertTrue ($paths -is [array] -or $paths -is [string]) 'should return array or single string'
        AssertTrue ($paths -contains 'Content/Foo/Bar.uasset') "expected Content/Foo/Bar.uasset, got: $($paths -join ', ')"
    } finally { Remove-Item -Recurse -Force $tmp }
}

Test 'Get-LyraAssetPaths: excludes BP_TestEnemy and BP_TestPickup' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        New-Item -ItemType Directory -Path "$tmp/Content/AI" -Force | Out-Null
        Set-Content -Path "$tmp/Content/AI/BP_TestEnemy.uasset"  -Value 'x'
        Set-Content -Path "$tmp/Content/AI/BP_TestPickup.uasset" -Value 'x'
        Set-Content -Path "$tmp/Content/AI/BP_OtherLyra.uasset"  -Value 'x'
        $paths = @(Get-LyraAssetPaths -RepoRoot $tmp.FullName)
        AssertTrue ($paths -notcontains 'Content/AI/BP_TestEnemy.uasset')  'should exclude BP_TestEnemy'
        AssertTrue ($paths -notcontains 'Content/AI/BP_TestPickup.uasset') 'should exclude BP_TestPickup'
        AssertTrue ($paths -contains    'Content/AI/BP_OtherLyra.uasset')  'should include other Lyra BPs'
    } finally { Remove-Item -Recurse -Force $tmp }
}

Test 'Get-LyraAssetPaths: ignores non-asset extensions' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        New-Item -ItemType Directory -Path "$tmp/Content" -Force | Out-Null
        Set-Content -Path "$tmp/Content/A.uasset" -Value 'x'
        Set-Content -Path "$tmp/Content/B.umap"   -Value 'x'
        Set-Content -Path "$tmp/Content/C.txt"    -Value 'x'
        Set-Content -Path "$tmp/Content/D.cpp"    -Value 'x'
        $paths = @(Get-LyraAssetPaths -RepoRoot $tmp.FullName)
        AssertEqual @('Content/A.uasset', 'Content/B.umap') ($paths | Sort-Object)
    } finally { Remove-Item -Recurse -Force $tmp }
}

Test 'Get-LyraAssetPaths: walks plugin Content/ dirs' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        $p = "$tmp/Plugins/GameFeatures/ShooterCore/Content"
        New-Item -ItemType Directory -Path $p -Force | Out-Null
        Set-Content -Path "$p/Pawn.uasset" -Value 'x'
        $paths = @(Get-LyraAssetPaths -RepoRoot $tmp.FullName)
        AssertTrue ($paths -contains 'Plugins/GameFeatures/ShooterCore/Content/Pawn.uasset') $paths
    } finally { Remove-Item -Recurse -Force $tmp }
}
```

- [ ] **Step 2: Run tests — expect 4 failures (stub throws 'not implemented')**

```bash
pwsh -NoProfile -File Scripts/Test-LyraAssetCommon.ps1
```

Expected: `Passed: 0  Failed: 4`, exit 1.

- [ ] **Step 3: Implement `Get-LyraAssetPaths`**

Replace the stub in `Scripts/LyraAssetCommon.ps1`:

```powershell
function Get-LyraAssetPaths {
    [CmdletBinding()]
    param([Parameter(Mandatory)] [string] $RepoRoot)

    $RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
    $results  = New-Object System.Collections.Generic.List[string]
    $exempt   = [System.Collections.Generic.HashSet[string]]::new(
        [string[]]$script:LyraAssetExemptions,
        [System.StringComparer]::OrdinalIgnoreCase)

    foreach ($glob in $script:LyraAssetGlobs) {
        $absDir = Join-Path $RepoRoot $glob
        if (-not (Test-Path -LiteralPath $absDir)) { continue }
        Get-ChildItem -LiteralPath $absDir -Recurse -File -Include '*.uasset','*.umap' |
            ForEach-Object {
                $rel = $_.FullName.Substring($RepoRoot.Length).TrimStart('\','/').Replace('\','/')
                if (-not $exempt.Contains($rel)) { $results.Add($rel) }
            }
    }

    return $results.ToArray()
}
```

- [ ] **Step 4: Run tests — expect 4 passes**

```bash
pwsh -NoProfile -File Scripts/Test-LyraAssetCommon.ps1
```

Expected: `Passed: 4  Failed: 0`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add Scripts/LyraAssetCommon.ps1 Scripts/Test-LyraAssetCommon.ps1
git commit -m "$(cat <<'EOF'
feat(scripts): Get-LyraAssetPaths enumerates Lyra .uasset/.umap

Walks the configured glob roots under RepoRoot, returns repo-relative
paths of every .uasset and .umap, excluding the two BPRSeed-produced
test BPs that live tests assert on.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Implement & test `Get-FileManifestEntries`

**Files:**
- Modify: `Scripts/LyraAssetCommon.ps1`
- Modify: `Scripts/Test-LyraAssetCommon.ps1`

- [ ] **Step 1: Add the failing tests**

```powershell
Test 'Get-FileManifestEntries: computes SHA-256 and size' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        $content = 'hello world'
        Set-Content -Path "$tmp/a.txt" -NoNewline -Value $content
        # SHA-256 of 'hello world': b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9
        $entries = @(Get-FileManifestEntries -RepoRoot $tmp.FullName -RelativePaths @('a.txt'))
        AssertEqual 1 $entries.Count
        AssertEqual 'a.txt'                                                              $entries[0].path
        AssertEqual 11                                                                   $entries[0].size
        AssertEqual 'b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9'   $entries[0].sha256
    } finally { Remove-Item -Recurse -Force $tmp }
}

Test 'Get-FileManifestEntries: handles multiple files in stable order' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        Set-Content -Path "$tmp/b.txt" -NoNewline -Value 'b'
        Set-Content -Path "$tmp/a.txt" -NoNewline -Value 'a'
        $entries = @(Get-FileManifestEntries -RepoRoot $tmp.FullName -RelativePaths @('b.txt','a.txt'))
        AssertEqual 'b.txt' $entries[0].path
        AssertEqual 'a.txt' $entries[1].path
    } finally { Remove-Item -Recurse -Force $tmp }
}
```

- [ ] **Step 2: Run tests — expect 2 new failures**

```bash
pwsh -NoProfile -File Scripts/Test-LyraAssetCommon.ps1
```

Expected: `Passed: 4  Failed: 2`.

- [ ] **Step 3: Implement**

Replace stub:

```powershell
function Get-FileManifestEntries {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string]   $RepoRoot,
        [Parameter(Mandatory)] [string[]] $RelativePaths
    )

    $RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
    foreach ($rel in $RelativePaths) {
        $abs  = Join-Path $RepoRoot $rel
        $info = Get-Item -LiteralPath $abs
        $hash = (Get-FileHash -LiteralPath $abs -Algorithm SHA256).Hash.ToLowerInvariant()
        [pscustomobject][ordered]@{
            path   = $rel.Replace('\','/')
            size   = [int64]$info.Length
            sha256 = $hash
        }
    }
}
```

- [ ] **Step 4: Run — expect 6 passes**

```bash
pwsh -NoProfile -File Scripts/Test-LyraAssetCommon.ps1
```

Expected: `Passed: 6  Failed: 0`.

- [ ] **Step 5: Commit**

```bash
git add Scripts/LyraAssetCommon.ps1 Scripts/Test-LyraAssetCommon.ps1
git commit -m "feat(scripts): Get-FileManifestEntries computes {path,size,sha256}

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: Implement & test `Build-Manifest`

**Files:**
- Modify: `Scripts/LyraAssetCommon.ps1`
- Modify: `Scripts/Test-LyraAssetCommon.ps1`

- [ ] **Step 1: Add failing tests**

```powershell
Test 'Build-Manifest: schema_version, tag, sorted files' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        New-Item -ItemType Directory -Path "$tmp/Content" -Force | Out-Null
        Set-Content -Path "$tmp/Content/Z.uasset" -NoNewline -Value 'z'
        Set-Content -Path "$tmp/Content/A.uasset" -NoNewline -Value 'a'
        $m = Build-Manifest -RepoRoot $tmp.FullName -Tag 'lyra-assets-v1'
        AssertEqual 1                  $m.schema_version
        AssertEqual 'lyra-assets-v1'   $m.tag
        AssertEqual 2                  $m.total_files
        AssertEqual 2                  $m.total_bytes
        AssertEqual 'Content/A.uasset' $m.files[0].path
        AssertEqual 'Content/Z.uasset' $m.files[1].path
    } finally { Remove-Item -Recurse -Force $tmp }
}

Test 'Build-Manifest: empty asset set produces valid manifest' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        $m = Build-Manifest -RepoRoot $tmp.FullName -Tag 'lyra-assets-v1'
        AssertEqual 0 $m.total_files
        AssertEqual 0 $m.total_bytes
        AssertEqual @() @($m.files)
    } finally { Remove-Item -Recurse -Force $tmp }
}
```

- [ ] **Step 2: Run — expect 2 new failures**

- [ ] **Step 3: Implement**

```powershell
function Build-Manifest {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string] $RepoRoot,
        [Parameter(Mandatory)] [string] $Tag
    )

    $paths   = Get-LyraAssetPaths -RepoRoot $RepoRoot
    $sorted  = @($paths | Sort-Object -CaseSensitive:$false)
    $entries = @()
    if ($sorted.Count -gt 0) {
        $entries = @(Get-FileManifestEntries -RepoRoot $RepoRoot -RelativePaths $sorted)
    }
    $totalBytes = ($entries | Measure-Object -Property size -Sum).Sum
    if ($null -eq $totalBytes) { $totalBytes = 0 }

    [pscustomobject][ordered]@{
        schema_version = $script:ManifestSchemaVersion
        tag            = $Tag
        generated_at   = (Get-Date).ToUniversalTime().ToString('o')
        total_files    = $entries.Count
        total_bytes    = [int64]$totalBytes
        files          = $entries
    }
}
```

- [ ] **Step 4: Run — expect 8 passes**

- [ ] **Step 5: Commit**

```bash
git add Scripts/LyraAssetCommon.ps1 Scripts/Test-LyraAssetCommon.ps1
git commit -m "feat(scripts): Build-Manifest produces sorted, schema-versioned manifest

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task 6: Implement & test `Test-Manifest`

**Files:**
- Modify: `Scripts/LyraAssetCommon.ps1`
- Modify: `Scripts/Test-LyraAssetCommon.ps1`

- [ ] **Step 1: Add failing tests**

```powershell
Test 'Test-Manifest: Ok when all files present and hashes match' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        New-Item -ItemType Directory -Path "$tmp/Content" -Force | Out-Null
        Set-Content -Path "$tmp/Content/A.uasset" -NoNewline -Value 'a'
        $m = Build-Manifest -RepoRoot $tmp.FullName -Tag 'lyra-assets-v1'
        $r = Test-Manifest  -RepoRoot $tmp.FullName -Manifest $m
        AssertTrue $r.Ok 'expected Ok'
        AssertEqual 0 $r.Missing.Count
        AssertEqual 0 $r.Mismatch.Count
    } finally { Remove-Item -Recurse -Force $tmp }
}

Test 'Test-Manifest: detects missing files' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        New-Item -ItemType Directory -Path "$tmp/Content" -Force | Out-Null
        Set-Content -Path "$tmp/Content/A.uasset" -NoNewline -Value 'a'
        $m = Build-Manifest -RepoRoot $tmp.FullName -Tag 'lyra-assets-v1'
        Remove-Item "$tmp/Content/A.uasset"
        $r = Test-Manifest -RepoRoot $tmp.FullName -Manifest $m
        AssertTrue (-not $r.Ok)
        AssertEqual @('Content/A.uasset') $r.Missing
    } finally { Remove-Item -Recurse -Force $tmp }
}

Test 'Test-Manifest: detects mismatched hashes' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        New-Item -ItemType Directory -Path "$tmp/Content" -Force | Out-Null
        Set-Content -Path "$tmp/Content/A.uasset" -NoNewline -Value 'a'
        $m = Build-Manifest -RepoRoot $tmp.FullName -Tag 'lyra-assets-v1'
        Set-Content -Path "$tmp/Content/A.uasset" -NoNewline -Value 'b'
        $r = Test-Manifest -RepoRoot $tmp.FullName -Manifest $m
        AssertTrue (-not $r.Ok)
        AssertEqual @('Content/A.uasset') $r.Mismatch
    } finally { Remove-Item -Recurse -Force $tmp }
}
```

- [ ] **Step 2: Run — expect 3 new failures**

- [ ] **Step 3: Implement**

```powershell
function Test-Manifest {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string]         $RepoRoot,
        [Parameter(Mandatory)] [PSCustomObject] $Manifest
    )

    if ($Manifest.schema_version -ne $script:ManifestSchemaVersion) {
        throw "Manifest schema_version $($Manifest.schema_version) does not match expected $($script:ManifestSchemaVersion)"
    }

    $missing  = New-Object System.Collections.Generic.List[string]
    $mismatch = New-Object System.Collections.Generic.List[string]
    $RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path

    foreach ($entry in $Manifest.files) {
        $abs = Join-Path $RepoRoot $entry.path
        if (-not (Test-Path -LiteralPath $abs)) {
            $missing.Add($entry.path)
            continue
        }
        $actual = (Get-FileHash -LiteralPath $abs -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $entry.sha256) {
            $mismatch.Add($entry.path)
        }
    }

    [pscustomobject][ordered]@{
        Ok       = ($missing.Count -eq 0 -and $mismatch.Count -eq 0)
        Missing  = $missing.ToArray()
        Mismatch = $mismatch.ToArray()
    }
}
```

- [ ] **Step 4: Run — expect 11 passes**

- [ ] **Step 5: Commit**

```bash
git add Scripts/LyraAssetCommon.ps1 Scripts/Test-LyraAssetCommon.ps1
git commit -m "feat(scripts): Test-Manifest validates filesystem against manifest

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task 7: Implement & test `Find-LyraInstallPaths`

**Files:**
- Modify: `Scripts/LyraAssetCommon.ps1`
- Modify: `Scripts/Test-LyraAssetCommon.ps1`

- [ ] **Step 1: Add failing tests**

```powershell
Test 'Find-LyraInstallPaths: returns matching InstallLocation dirs' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        $datDir = Join-Path $tmp.FullName 'launcher'
        New-Item -ItemType Directory -Path $datDir -Force | Out-Null
        $datPath = Join-Path $datDir 'LauncherInstalled.dat'

        # Build a synthetic Lyra install and a non-Lyra install
        $lyraDir  = Join-Path $tmp.FullName 'EpicGames/Lyra'
        $otherDir = Join-Path $tmp.FullName 'EpicGames/Fortnite'
        New-Item -ItemType Directory -Path $lyraDir  -Force | Out-Null
        New-Item -ItemType Directory -Path $otherDir -Force | Out-Null
        Set-Content -Path (Join-Path $lyraDir 'LyraStarterGame.uproject') -Value '{}'

        $dat = [pscustomobject]@{
            InstallationList = @(
                [pscustomobject]@{ InstallLocation = $lyraDir;  AppName = 'UE_Lyra'; AppVersion = '5.7.4' }
                [pscustomobject]@{ InstallLocation = $otherDir; AppName = 'Fortnite'; AppVersion = '1.0' }
            )
        }
        $dat | ConvertTo-Json -Depth 10 | Set-Content -Path $datPath -Encoding utf8

        $found = @(Find-LyraInstallPaths -LauncherDatPath $datPath)
        AssertEqual 1 $found.Count
        AssertEqual $lyraDir $found[0]
    } finally { Remove-Item -Recurse -Force $tmp }
}

Test 'Find-LyraInstallPaths: returns empty when no Lyra entry' {
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "lyra-test-$([guid]::NewGuid())") -Force
    try {
        $datPath = Join-Path $tmp.FullName 'LauncherInstalled.dat'
        ([pscustomobject]@{ InstallationList = @() }) | ConvertTo-Json | Set-Content -Path $datPath -Encoding utf8
        $found = @(Find-LyraInstallPaths -LauncherDatPath $datPath)
        AssertEqual 0 $found.Count
    } finally { Remove-Item -Recurse -Force $tmp }
}

Test 'Find-LyraInstallPaths: handles missing dat file gracefully' {
    AssertThrows { Find-LyraInstallPaths -LauncherDatPath 'X:\does\not\exist.dat' } 'not found'
}
```

- [ ] **Step 2: Run — expect 3 new failures**

- [ ] **Step 3: Implement**

```powershell
function Find-LyraInstallPaths {
    [CmdletBinding()]
    param([Parameter(Mandatory)] [string] $LauncherDatPath)

    if (-not (Test-Path -LiteralPath $LauncherDatPath)) {
        throw "LauncherInstalled.dat not found at: $LauncherDatPath"
    }

    $dat = Get-Content -LiteralPath $LauncherDatPath -Raw | ConvertFrom-Json
    $hits = New-Object System.Collections.Generic.List[string]

    foreach ($entry in @($dat.InstallationList)) {
        $loc = $entry.InstallLocation
        if (-not $loc) { continue }
        $uproject = Join-Path $loc 'LyraStarterGame.uproject'
        if (Test-Path -LiteralPath $uproject) {
            $hits.Add($loc)
        }
    }

    return $hits.ToArray()
}
```

- [ ] **Step 4: Run — expect 14 passes**

- [ ] **Step 5: Commit**

```bash
git add Scripts/LyraAssetCommon.ps1 Scripts/Test-LyraAssetCommon.ps1
git commit -m "feat(scripts): Find-LyraInstallPaths parses Epic Launcher manifest

Detects existence of LyraStarterGame.uproject under each
InstallLocation rather than matching AppName strings, since Epic
has shifted Lyra's app name across releases.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task 8: Implement & test `Get-RestorePathMap`

**Files:**
- Modify: `Scripts/LyraAssetCommon.ps1`
- Modify: `Scripts/Test-LyraAssetCommon.ps1`

- [ ] **Step 1: Add failing tests**

```powershell
Test 'Get-RestorePathMap: maps each glob root to identical relative dest' {
    $map = Get-RestorePathMap -LyraInstallRoot 'C:/L' -RepoRoot 'D:/R'
    AssertEqual 'C:/L/Content'                                  $map['Content']
    AssertEqual 'D:/R/Content'                                  $map['Content_dst']
    AssertTrue ($map.ContainsKey('Plugins/GameFeatures/ShooterCore/Content'))
    AssertEqual 'C:/L/Plugins/GameFeatures/ShooterCore/Content' $map['Plugins/GameFeatures/ShooterCore/Content']
    AssertEqual 'D:/R/Plugins/GameFeatures/ShooterCore/Content' $map['Plugins/GameFeatures/ShooterCore/Content_dst']
}
```

- [ ] **Step 2: Run — expect 1 failure**

- [ ] **Step 3: Implement**

```powershell
function Get-RestorePathMap {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string] $LyraInstallRoot,
        [Parameter(Mandatory)] [string] $RepoRoot
    )

    $map = [ordered]@{}
    foreach ($glob in $script:LyraAssetGlobs) {
        $map[$glob]            = (Join-Path $LyraInstallRoot $glob).Replace('\','/')
        $map["${glob}_dst"]    = (Join-Path $RepoRoot        $glob).Replace('\','/')
    }
    return $map
}
```

- [ ] **Step 4: Run — expect 15 passes**

- [ ] **Step 5: Commit**

```bash
git add Scripts/LyraAssetCommon.ps1 Scripts/Test-LyraAssetCommon.ps1
git commit -m "feat(scripts): Get-RestorePathMap pairs src/dst dirs for robocopy

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task 9: Wire `setup.ps1` (the user-facing restorer)

**Files:**
- Create: `Scripts/setup.ps1`

- [ ] **Step 1: Write the script**

```powershell
# Scripts/setup.ps1
# Restore Lyra assets that are not tracked in this repo. Run after a
# fresh clone, or any time the asset working tree was cleaned.
#
# Default flow:
#   1. Try to locate a Lyra Starter Game install via Epic Games
#      Launcher's LauncherInstalled.dat.
#   2. If found, robocopy /MIR Content/ and Plugins/*/Content/ from
#      that install into this repo.
#   3. Otherwise, download the bundle from this repo's GitHub
#      Release tag (default: lyra-assets-v1), verify SHA-256, extract.
#   4. Validate every restored file against the committed manifest
#      (Scripts/lyra-assets-manifest.json). Warn on hash mismatches
#      (UE auto-upgrades older asset versions on load).

[CmdletBinding()]
param(
    [ValidateSet('auto','local','release')]
    [string] $Source = 'auto',
    [string] $ReleaseTag = 'lyra-assets-v1',
    [string] $RepoOwner  = 'defessler',
    [string] $RepoName   = 'Unreal-Engine-5-MCP',
    [switch] $Force,
    [switch] $DryRun,
    [switch] $VerifyOnly
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

. "$PSScriptRoot/LyraAssetCommon.ps1"

$RepoRoot     = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$ManifestPath = Join-Path $PSScriptRoot 'lyra-assets-manifest.json'

function Write-Step ([string] $Msg) { Write-Host "==> $Msg" -ForegroundColor Cyan }
function Write-Warn ([string] $Msg) { Write-Host "    warning: $Msg" -ForegroundColor Yellow }

Write-Step "Repo root: $RepoRoot"

if (-not (Test-Path -LiteralPath $ManifestPath)) {
    throw "Manifest missing at $ManifestPath. This script must be run from a clone of the repo, after the manifest has been committed."
}
$manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json

# --- Verify-only mode: just check current working tree against manifest ---
if ($VerifyOnly) {
    Write-Step 'Verifying current working tree against manifest'
    $r = Test-Manifest -RepoRoot $RepoRoot -Manifest $manifest
    if ($r.Ok) {
        Write-Host "  $($manifest.total_files) files present and hashes match." -ForegroundColor Green
        exit 0
    }
    Write-Warn "$($r.Missing.Count) missing, $($r.Mismatch.Count) mismatched"
    foreach ($m in ($r.Missing  | Select-Object -First 5)) { Write-Host "    missing:  $m" }
    foreach ($m in ($r.Mismatch | Select-Object -First 5)) { Write-Host "    mismatch: $m" }
    exit 1
}

# --- Source resolution ---
$selected = $Source
$lyraRoot = $null

if ($Source -in @('auto','local')) {
    $datPath = Join-Path $env:ProgramData 'Epic\UnrealEngineLauncher\LauncherInstalled.dat'
    if (Test-Path -LiteralPath $datPath) {
        try {
            $installs = @(Find-LyraInstallPaths -LauncherDatPath $datPath)
            if ($installs.Count -gt 0) {
                $lyraRoot = $installs[0]
                $selected = 'local'
                Write-Step "Found local Lyra install: $lyraRoot"
            }
        } catch { Write-Warn "Could not parse LauncherInstalled.dat: $($_.Exception.Message)" }
    } else {
        Write-Warn "LauncherInstalled.dat not found at $datPath"
    }
    if (-not $lyraRoot -and $Source -eq 'local') {
        throw 'No local Lyra install found. Re-run with --Source release.'
    }
    if (-not $lyraRoot) { $selected = 'release' }
}

# --- Pre-flight: refuse if there are uncommitted asset changes ---
if (-not $Force) {
    Push-Location $RepoRoot
    try {
        $dirty = git status --porcelain -- Content Plugins/LyraExampleContent/Content Plugins/LyraExtTool/Content Plugins/PocketWorlds/Content Plugins/GameFeatures 2>$null
        if ($dirty) {
            Write-Host "Uncommitted changes detected under restore-target paths:" -ForegroundColor Red
            Write-Host $dirty
            throw 'Refusing to overwrite local work. Re-run with --Force to bypass.'
        }
    } finally { Pop-Location }
}

# --- Local restore ---
if ($selected -eq 'local') {
    Write-Step 'Mirroring asset dirs from local Lyra install'
    $map = Get-RestorePathMap -LyraInstallRoot $lyraRoot -RepoRoot $RepoRoot
    foreach ($glob in $script:LyraAssetGlobs) {
        $src = $map[$glob]
        $dst = $map["${glob}_dst"]
        if (-not (Test-Path -LiteralPath $src)) {
            Write-Warn "Source missing in Lyra install: $src (skipping)"
            continue
        }
        if ($DryRun) {
            Write-Host "    [dry-run] robocopy /MIR $src -> $dst"
            continue
        }
        New-Item -ItemType Directory -Path $dst -Force | Out-Null
        & robocopy $src $dst /MIR /XO /NFL /NDL /NJH /NJS /NP /R:2 /W:5 /MT:8 | Out-Null
        # robocopy exit codes 0-7 = success (0=no changes, 1-3=copied, 4-7=warnings)
        if ($LASTEXITCODE -ge 8) {
            throw "robocopy failed copying $src -> $dst (exit $LASTEXITCODE)"
        }
    }
}

# --- Release restore ---
if ($selected -eq 'release') {
    Write-Step "Downloading bundle from GitHub Release: $ReleaseTag"
    $bundleName = "lyra-assets-${ReleaseTag}.tar.zst"
    $bundleUrl  = "https://github.com/$RepoOwner/$RepoName/releases/download/$ReleaseTag/$bundleName"
    $shaUrl     = "$bundleUrl.sha256"
    $bundlePath = Join-Path $env:TEMP $bundleName
    $shaPath    = "$bundlePath.sha256"

    if ($DryRun) {
        Write-Host "    [dry-run] curl -L $bundleUrl -> $bundlePath"
        Write-Host "    [dry-run] verify SHA-256, extract under $RepoRoot"
    } else {
        & curl --fail --location --silent --show-error --output $bundlePath $bundleUrl
        if ($LASTEXITCODE -ne 0) { throw "curl failed downloading bundle (exit $LASTEXITCODE)" }
        & curl --fail --location --silent --show-error --output $shaPath    $shaUrl
        if ($LASTEXITCODE -ne 0) { throw "curl failed downloading sha256 (exit $LASTEXITCODE)" }

        $expected = (Get-Content -LiteralPath $shaPath -Raw).Split()[0].ToLowerInvariant()
        $actual   = (Get-FileHash -LiteralPath $bundlePath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($expected -ne $actual) {
            throw "Bundle SHA-256 mismatch. expected=$expected actual=$actual"
        }
        Write-Step "Extracting bundle into $RepoRoot"
        & tar -x --zstd -f $bundlePath -C $RepoRoot
        if ($LASTEXITCODE -ne 0) { throw "tar extract failed (exit $LASTEXITCODE)" }
        Remove-Item -LiteralPath $bundlePath -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $shaPath    -ErrorAction SilentlyContinue
    }
}

# --- Verification (skipped in dry-run) ---
if (-not $DryRun) {
    Write-Step 'Verifying restored files against manifest'
    $r = Test-Manifest -RepoRoot $RepoRoot -Manifest $manifest
    if ($r.Missing.Count) {
        Write-Warn "$($r.Missing.Count) files still missing after restore:"
        foreach ($m in ($r.Missing | Select-Object -First 5)) { Write-Host "    $m" }
        if ($r.Missing.Count -gt 5) { Write-Host "    ... and $($r.Missing.Count - 5) more" }
    }
    if ($r.Mismatch.Count) {
        Write-Warn "$($r.Mismatch.Count) files have different hashes than the bundled manifest"
        Write-Host '    (this is normal when restoring from a local Lyra install with a different patch version)'
    }
    $totalMb = [math]::Round($manifest.total_bytes / 1MB, 1)
    Write-Host ''
    Write-Step "Restored $($manifest.total_files - $r.Missing.Count)/$($manifest.total_files) assets ($totalMb MB expected). Source: $selected."
}

exit 0
```

- [ ] **Step 2: Smoke test --VerifyOnly (against an empty manifest)**

We don't have the real manifest yet (built in Task 12). For now, create a tiny temporary manifest to prove the script wires correctly:

```bash
# This is a one-off probe — the real manifest comes from Task 12.
pwsh -NoProfile -Command @'
. ./Scripts/LyraAssetCommon.ps1
$tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "probe-$([guid]::NewGuid())") -Force
$m = Build-Manifest -RepoRoot $tmp.FullName -Tag 'probe'
$m | ConvertTo-Json -Depth 10 | Set-Content ./Scripts/lyra-assets-manifest.json -Encoding utf8
'@
pwsh -NoProfile -File ./Scripts/setup.ps1 -VerifyOnly
echo "exit: $?"
rm Scripts/lyra-assets-manifest.json
```

Expected: prints `0 files present and hashes match.`, exit 0.

- [ ] **Step 3: Smoke test --DryRun --Source=release**

```bash
# Create a placeholder manifest again
pwsh -NoProfile -Command @'
. ./Scripts/LyraAssetCommon.ps1
$tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "probe-$([guid]::NewGuid())") -Force
$m = Build-Manifest -RepoRoot $tmp.FullName -Tag 'probe'
$m | ConvertTo-Json -Depth 10 | Set-Content ./Scripts/lyra-assets-manifest.json -Encoding utf8
'@
pwsh -NoProfile -File ./Scripts/setup.ps1 -Source release -DryRun
echo "exit: $?"
rm Scripts/lyra-assets-manifest.json
```

Expected: prints `[dry-run] curl -L https://github.com/defessler/Unreal-Engine-5-MCP/releases/download/lyra-assets-v1/...`, exit 0.

- [ ] **Step 4: Commit**

```bash
git add Scripts/setup.ps1
git commit -m "$(cat <<'EOF'
feat(scripts): setup.ps1 restores Lyra assets (hybrid local/release)

Default --Source=auto probes Epic Games Launcher install first via
LauncherInstalled.dat; falls back to GitHub Release bundle. After
restore (or in --VerifyOnly mode), validates the working tree
against Scripts/lyra-assets-manifest.json. --DryRun prints the
plan without touching anything.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 10: Write `setup.bat` shim

**Files:**
- Create: `setup.bat`

- [ ] **Step 1: Write the shim**

```batch
@echo off
REM Thin shim that forwards to Scripts/setup.ps1 in PowerShell 7+.
REM All real logic lives in setup.ps1; this exists so a fresh-clone
REM user can double-click or run "setup.bat" from cmd.exe.

where pwsh >nul 2>&1
if errorlevel 1 (
    echo error: pwsh (PowerShell 7+) is required but not on PATH.
    echo Install from https://github.com/PowerShell/PowerShell/releases
    exit /b 2
)

pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0Scripts\setup.ps1" %*
exit /b %ERRORLEVEL%
```

- [ ] **Step 2: Smoke test**

```bash
# With a placeholder manifest in place
pwsh -NoProfile -Command @'
. ./Scripts/LyraAssetCommon.ps1
$tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "probe-$([guid]::NewGuid())") -Force
$m = Build-Manifest -RepoRoot $tmp.FullName -Tag 'probe'
$m | ConvertTo-Json -Depth 10 | Set-Content ./Scripts/lyra-assets-manifest.json -Encoding utf8
'@
cmd //c setup.bat -Source release -DryRun
echo "exit: $?"
rm Scripts/lyra-assets-manifest.json
```

Expected: same output as Task 9 Step 3, exit 0.

- [ ] **Step 3: Commit**

```bash
git add setup.bat
git commit -m "feat: setup.bat shim forwards to Scripts/setup.ps1

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task 11: Write `Publish-LyraAssetsRelease.ps1`

**Files:**
- Create: `Scripts/Publish-LyraAssetsRelease.ps1`

- [ ] **Step 1: Write the publisher**

```powershell
# Scripts/Publish-LyraAssetsRelease.ps1
# Operator script — must run on a checkout that still has the Lyra
# assets present. Builds Scripts/lyra-assets-manifest.json, packs the
# bundle as lyra-assets-<Tag>.tar.zst at repo root, writes a .sha256
# sidecar, and (with -Upload) publishes them as a GitHub Release.

[CmdletBinding()]
param(
    [string] $Tag = 'lyra-assets-v1',
    [switch] $BuildManifestOnly,
    [switch] $BundleOnly,
    [switch] $Upload,
    [switch] $DryRun
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

. "$PSScriptRoot/LyraAssetCommon.ps1"

$RepoRoot     = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$ManifestPath = Join-Path $PSScriptRoot 'lyra-assets-manifest.json'
$BundleName   = "lyra-assets-${Tag}.tar.zst"
$BundlePath   = Join-Path $RepoRoot $BundleName
$ShaPath      = "$BundlePath.sha256"

function Write-Step ([string] $Msg) { Write-Host "==> $Msg" -ForegroundColor Cyan }

# --- 1. Manifest ---
Write-Step "Building manifest for tag $Tag"
$manifest = Build-Manifest -RepoRoot $RepoRoot -Tag $Tag
$totalMb  = [math]::Round($manifest.total_bytes / 1MB, 1)
Write-Host "    files: $($manifest.total_files)"
Write-Host "    bytes: $($manifest.total_bytes) ($totalMb MB)"

if ($manifest.total_files -lt 1000) {
    throw "Refusing to publish: only $($manifest.total_files) files found. Likely run on a stripped checkout."
}

if ($DryRun) {
    Write-Host "    [dry-run] would write $ManifestPath"
} else {
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $ManifestPath -Encoding utf8
    Write-Host "    wrote: $ManifestPath"
}

if ($BuildManifestOnly) { exit 0 }

# --- 2. Bundle ---
Write-Step "Packing bundle $BundleName (tar --zstd -19)"
$listPath = Join-Path $env:TEMP "lyra-paths-$([guid]::NewGuid()).txt"
$manifest.files | ForEach-Object { $_.path } | Set-Content -LiteralPath $listPath -Encoding utf8

if ($DryRun) {
    Write-Host "    [dry-run] tar -c --zstd -f $BundlePath -T $listPath -C $RepoRoot"
} else {
    Push-Location $RepoRoot
    try {
        $env:ZSTD_CLEVEL = '19'
        & tar -c --zstd -f $BundlePath -T $listPath
        if ($LASTEXITCODE -ne 0) { throw "tar failed (exit $LASTEXITCODE)" }
    } finally {
        $env:ZSTD_CLEVEL = $null
        Pop-Location
        Remove-Item -LiteralPath $listPath -ErrorAction SilentlyContinue
    }
    $bundleMb = [math]::Round((Get-Item $BundlePath).Length / 1MB, 1)
    Write-Host "    bundle: $BundlePath ($bundleMb MB)"
}

# --- 3. Hash ---
Write-Step "Hashing bundle"
if ($DryRun) {
    Write-Host "    [dry-run] write $ShaPath"
} else {
    $hash = (Get-FileHash -LiteralPath $BundlePath -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $BundleName" | Set-Content -LiteralPath $ShaPath -Encoding ascii
    Write-Host "    sha256: $hash"
}

if ($BundleOnly) { exit 0 }

# --- 4. Upload ---
if (-not $Upload) {
    Write-Host ''
    Write-Step "Bundle ready. Re-run with -Upload to publish to GitHub Release '$Tag'."
    exit 0
}

Write-Step "Publishing GitHub Release $Tag"
if ($DryRun) {
    Write-Host "    [dry-run] gh release create $Tag --title '$Tag' $BundlePath $ShaPath $ManifestPath"
} else {
    & gh release create $Tag --title $Tag --notes "Lyra asset bundle. $($manifest.total_files) files, $totalMb MB. Restore with setup.bat." $BundlePath $ShaPath $ManifestPath
    if ($LASTEXITCODE -ne 0) { throw "gh release create failed (exit $LASTEXITCODE)" }
}

Write-Host ''
Write-Step "Done."
```

- [ ] **Step 2: Smoke test --DryRun**

```bash
pwsh -NoProfile -File Scripts/Publish-LyraAssetsRelease.ps1 -DryRun
echo "exit: $?"
```

Expected: prints `files: 8682`, `bytes: ~2050000000`, dry-run lines for write/tar/hash/release, exit 0.

- [ ] **Step 3: Commit**

```bash
git add Scripts/Publish-LyraAssetsRelease.ps1
git commit -m "$(cat <<'EOF'
feat(scripts): Publish-LyraAssetsRelease.ps1 builds + publishes bundle

Builds Scripts/lyra-assets-manifest.json from current HEAD, packs
the matching files into lyra-assets-<Tag>.tar.zst (zstd-19), writes
a .sha256 sidecar, and with -Upload publishes everything via gh
release create. -BuildManifestOnly / -BundleOnly / -DryRun for
stage-by-stage operator control.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 12: Generate and commit `Scripts/lyra-assets-manifest.json`

**Files:**
- Create: `Scripts/lyra-assets-manifest.json` (generated; ~1 MB)

- [ ] **Step 1: Run the manifest builder**

```bash
pwsh -NoProfile -File Scripts/Publish-LyraAssetsRelease.ps1 -BuildManifestOnly
```

Expected output includes `files: 8682` and `wrote: .../Scripts/lyra-assets-manifest.json`.

- [ ] **Step 2: Sanity-check the manifest**

```bash
pwsh -NoProfile -Command @'
$m = Get-Content Scripts/lyra-assets-manifest.json -Raw | ConvertFrom-Json
Write-Host "files: $($m.total_files)"
Write-Host "bytes: $($m.total_bytes)"
Write-Host "first: $($m.files[0].path)"
Write-Host "last:  $($m.files[-1].path)"
$exempt = $m.files | Where-Object { $_.path -like '*BP_TestEnemy*' -or $_.path -like '*BP_TestPickup*' }
Write-Host "exempt count (must be 0): $($exempt.Count)"
'@
```

Expected: `files: 8682`, exempt count `0`, first/last paths are recognisable Lyra asset paths.

- [ ] **Step 3: Round-trip test against current working tree**

```bash
pwsh -NoProfile -File Scripts/setup.ps1 -VerifyOnly
```

Expected: `8682 files present and hashes match.`, exit 0.

- [ ] **Step 4: Commit**

```bash
git add Scripts/lyra-assets-manifest.json
git commit -m "$(cat <<'EOF'
chore(scripts): bake lyra-assets-manifest.json from current HEAD

Lists 8682 .uasset/.umap files with size + SHA-256, sorted by path.
setup.bat reads this to validate restored installs. Source of truth
for what setup.bat expects to restore.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 13: Write `strip-lyra-paths.txt` and validate with `--analyze`

**Files:**
- Create: `Scripts/strip-lyra-paths.txt`

- [ ] **Step 1: Write the path spec**

```
regex:^Content/(?!AI/BP_TestEnemy\.uasset$|AI/BP_TestPickup\.uasset$).*\.(uasset|umap)$
regex:^Plugins/LyraExampleContent/Content/.*
regex:^Plugins/LyraExtTool/Content/.*
regex:^Plugins/PocketWorlds/Content/.*
regex:^Plugins/GameFeatures/(ShooterCore|ShooterTests|TopDownArena|ShooterMaps|ShooterExplorer)/Content/.*
```

- [ ] **Step 2: Dry-run filter-repo --analyze against a fresh clone**

```bash
# Make a non-destructive clone in a sibling dir
cd ..
git clone --no-local UE5_MCP UE5_MCP-filter-analyze
cd UE5_MCP-filter-analyze
git filter-repo --analyze
ls .git/filter-repo/analysis/
```

Expected: `path-deleted-sizes.txt`, `path-all-sizes.txt`, `directories-deleted-sizes.txt`, etc.

- [ ] **Step 3: Verify the path spec catches what we expect**

```bash
git filter-repo --invert-paths --paths-from-file ../UE5_MCP/Scripts/strip-lyra-paths.txt --dry-run 2>&1 | tail -20
# Then check what would remain:
git ls-tree -r HEAD --long | awk '$5 ~ /\.(uasset|umap)$/ {print $5}' | wc -l
```

Expected: dry-run reports successful path-rewrite plan. Asset count remaining = 2 (the two test BPs).

- [ ] **Step 4: Clean up the analyze clone, return to main repo**

```bash
cd ../UE5_MCP
rm -rf ../UE5_MCP-filter-analyze
```

- [ ] **Step 5: Commit**

```bash
git add Scripts/strip-lyra-paths.txt
git commit -m "$(cat <<'EOF'
chore(scripts): strip-lyra-paths.txt — regex spec for git-filter-repo

Strips every .uasset/.umap under Content/ (except the two
BPRSeed-produced test BPs) and every Plugins/*/Content/ asset for
the bundled Lyra plugins. Used by Phase B of the asset-strip
runbook documented in docs/superpowers/plans/2026-05-19-strip-lyra-assets.md.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 14: Update `.gitignore`, `README.md`, `CLAUDE.md`

**Files:**
- Modify: `.gitignore` (remove redundant entries)
- Modify: `README.md` (add restore step)
- Modify: `CLAUDE.md` (note asset state)

- [ ] **Step 1: Read current `.gitignore` lines 49-54**

```bash
sed -n '49,54p' .gitignore
```

This block lists 4 specific large textures. Once filter-repo strips them along with everything else, these per-file ignores are redundant — the asset paths won't exist in HEAD, and `setup.bat` restores them in-place. Remove the block (but keep the comment about Git LFS, since it remains true advice for future >50MB assets).

- [ ] **Step 2: Edit `.gitignore`**

Replace lines 49-54 with a shorter note pointing to setup.bat:

```
# Lyra assets are not tracked in this repo (~2 GB working tree, ~1.4 GB
# pack savings). Run `setup.bat` after cloning to restore them — see
# docs/superpowers/specs/2026-05-19-strip-lyra-assets-design.md for
# the design and Scripts/lyra-assets-manifest.json for the file list.
#
# Pattern-based: ignore .uasset/.umap anywhere under Content/, then
# un-ignore the two BPRSeed test BPs that must stay tracked. The plugin
# Content/ trees have no exemptions, so they ignore by directory.
Content/**/*.uasset
Content/**/*.umap
!Content/AI/BP_TestEnemy.uasset
!Content/AI/BP_TestPickup.uasset
Plugins/LyraExampleContent/Content/
Plugins/LyraExtTool/Content/
Plugins/PocketWorlds/Content/
Plugins/GameFeatures/ShooterCore/Content/
Plugins/GameFeatures/ShooterTests/Content/
Plugins/GameFeatures/TopDownArena/Content/
Plugins/GameFeatures/ShooterMaps/Content/
Plugins/GameFeatures/ShooterExplorer/Content/

# Bundle artifacts produced by Scripts/Publish-LyraAssetsRelease.ps1
lyra-assets-*.tar.zst
lyra-assets-*.tar.zst.sha256
```

- [ ] **Step 3: Verify .gitignore still tracks the two test BPs**

```bash
git check-ignore -v Content/AI/BP_TestEnemy.uasset Content/AI/BP_TestPickup.uasset
echo "exit: $?"
```

Expected: no output (files are NOT ignored), exit 1. Then:

```bash
git check-ignore -v Content/Characters/Heroes/Mannequin/Meshes/SKM_Manny.uasset
```

Expected: prints `.gitignore:<line>:Content/*.uasset    Content/Characters/Heroes/Mannequin/Meshes/SKM_Manny.uasset` or similar, exit 0.

- [ ] **Step 4: Add "After clone" section to `README.md`**

Insert after the project intro / repo-layout block, before the build instructions. The block below is the README content — note that inside the markdown sample, indent the inner `cmd` example by 4 spaces (instead of a nested code fence) so it renders cleanly:

    ## First-time setup (after clone)

    Lyra's bundled assets (~8,700 `.uasset` / `.umap` files, ~2 GB)
    are not tracked in this repo to keep clones fast. After cloning,
    run:

        setup.bat

    `setup.bat` auto-detects Lyra installed via Epic Games Launcher
    and mirrors its `Content/` and `Plugins/*/Content/` dirs into
    this repo via robocopy. If no local install is found, it
    downloads a bundle from this repo's GitHub Release
    `lyra-assets-v1` (~800 MB compressed), verifies SHA-256, and
    extracts it.

    Flags:
    - `setup.bat --Source local`   — force local Epic install path
    - `setup.bat --Source release` — force GitHub Release download
    - `setup.bat --DryRun`         — print the plan, don't touch anything
    - `setup.bat --VerifyOnly`     — re-validate current working tree against the bundled manifest

    The manifest (`Scripts/lyra-assets-manifest.json`) is the
    source of truth for what setup.bat expects to restore —
    committed in the repo, referenced by tag.

- [ ] **Step 5: Update `CLAUDE.md` Repo layout block**

In the Repo layout section, add a brief callout near `Content/AI/`:

```markdown
├── Content/AI/                                 BP_TestEnemy.uasset, BP_TestPickup.uasset
│                                               (regenerable; see "Reseed test BPs" below)
│                                               All other Lyra Content/ is restored on
│                                               demand by setup.bat — see README "First-time
│                                               setup" for the hybrid local/release flow.
```

And after the "Build invariants" or near the gotchas, add:

```markdown
## Asset restoration

`setup.bat` at the repo root restores the Lyra assets that the
filter-repo strip removed. Local Epic Games Launcher install is
preferred; GitHub Release `lyra-assets-v1` is the fallback bundle.
`Scripts/lyra-assets-manifest.json` is the committed source of truth
(8,682 entries, ~1 MB JSON) for what's expected after restore.

Re-run `setup.bat --VerifyOnly` to confirm a working tree matches
the manifest, e.g. after a destructive `git clean -fdx`.
```

- [ ] **Step 6: Commit**

```bash
git add .gitignore README.md CLAUDE.md
git commit -m "$(cat <<'EOF'
docs: setup.bat instructions + .gitignore for Lyra asset paths

.gitignore now matches every path that setup.bat restores, with
explicit !-rules to keep BP_TestEnemy/BP_TestPickup tracked. The
old per-file >50MB block is subsumed.

README gains a "First-time setup" section pointing to setup.bat
and listing the flags. CLAUDE.md notes the restoration flow in the
Repo layout + a dedicated "Asset restoration" section.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 15: STOP — Operator publish + smoke tests

**Files:** none modified — operator runs commands

This is a **hard checkpoint**. Phase B starts after the user explicitly confirms.

- [ ] **Step 1: Operator runs the publish dry-run**

```bash
pwsh -NoProfile -File Scripts/Publish-LyraAssetsRelease.ps1 -DryRun
```

Confirm: prints sensible file count, byte count, and dry-run commands.

- [ ] **Step 2: Operator runs the bundle build (no upload yet)**

```bash
pwsh -NoProfile -File Scripts/Publish-LyraAssetsRelease.ps1 -BundleOnly
ls -lh lyra-assets-lyra-assets-v1.tar.zst*
```

Confirm: bundle is somewhere in the 700-900 MB range, sha256 file present.

- [ ] **Step 3: Operator uploads to GitHub Release**

```bash
gh auth status            # confirm authenticated to the right account
pwsh -NoProfile -File Scripts/Publish-LyraAssetsRelease.ps1 -Upload
```

Confirm: release `lyra-assets-v1` exists at
`https://github.com/defessler/Unreal-Engine-5-MCP/releases/tag/lyra-assets-v1`
with three assets attached (bundle, .sha256, manifest.json).

- [ ] **Step 4: Operator smoke-tests setup.bat against the release**

```bash
# Move the working-tree assets aside (temporary, do NOT commit)
mv Content Content.bak
pwsh -NoProfile -File Scripts/setup.ps1 -Source release
# Verify the manifest comes back happy
pwsh -NoProfile -File Scripts/setup.ps1 -VerifyOnly
# Compare a few files
diff -q Content.bak/AI/BP_TestEnemy.uasset Content/AI/BP_TestEnemy.uasset || echo "(BP_TestEnemy is exempt from bundle; should still exist from git)"
diff -q Content.bak/Characters/Heroes/Mannequin/Meshes/SKM_Manny.uasset Content/Characters/Heroes/Mannequin/Meshes/SKM_Manny.uasset
```

Confirm: VerifyOnly reports `8682 files present`, SKM_Manny is byte-identical.

```bash
# Restore the original working tree (the backup) — we'll need it to do Phase B's filter-repo
rm -rf Content
mv Content.bak Content
```

- [ ] **Step 5: Operator smoke-tests setup.bat against local Lyra install**

```bash
# Repeat the above with -Source local. Hash mismatches expected if the
# local Lyra version differs from what was bundled.
mv Content Content.bak
pwsh -NoProfile -File Scripts/setup.ps1 -Source local
pwsh -NoProfile -File Scripts/setup.ps1 -VerifyOnly
# Hash warnings are OK. Missing files are not.
rm -rf Content
mv Content.bak Content
```

- [ ] **Step 6: HARD STOP — user confirms before Phase B**

> User must explicitly say "proceed with Phase B" (or equivalent) before any history-rewriting commands run. Phase B is irreversible: every commit SHA from the first Lyra import forward will change, and force-push will overwrite origin's history.

---

## Phase B — History rewrite (irreversible, operator-driven)

### Task 16: Backup + run filter-repo

**Files:** none in this repo — operates on sibling clones

- [ ] **Step 1: Tag and push backup branch**

```bash
git tag pre-asset-strip
git push origin pre-asset-strip
git push origin main:pre-asset-strip-branch
```

- [ ] **Step 2: Make a mirror clone as offline safety net**

```bash
cd ..
git clone --mirror UE5_MCP UE5_MCP-backup.git
du -sh UE5_MCP-backup.git
```

Expected: ~1.4 GB mirror. Keep this until well after force-push.

- [ ] **Step 3: Fresh-clone the working repo for the rewrite**

```bash
git clone --no-local UE5_MCP UE5_MCP-rewrite
cd UE5_MCP-rewrite
```

`--no-local` ensures filter-repo gets a fresh `objects/` dir to rewrite (it refuses to operate on a "non-fresh" clone by default).

- [ ] **Step 4: Run filter-repo**

```bash
git filter-repo --invert-paths --paths-from-file ../UE5_MCP/Scripts/strip-lyra-paths.txt
```

Expected: progress output, "New history written" message, exit 0.

- [ ] **Step 5: Garbage-collect aggressively**

```bash
git reflog expire --expire=now --all
git gc --prune=now --aggressive
```

- [ ] **Step 6: Verify pack shrinkage**

```bash
git count-objects -vH
du -sh .git
```

Expected: `size-pack` drops from ~1.36 GiB to ~150-250 MiB.

- [ ] **Step 7: Verify asset count**

```bash
git ls-tree -r HEAD | awk '$NF ~ /\.(uasset|umap)$/ {print $NF}'
```

Expected: exactly two paths:
```
Content/AI/BP_TestEnemy.uasset
Content/AI/BP_TestPickup.uasset
```

---

### Task 17: Verify the rewritten clone, force-push, finalize

**Files:** none — operator commands only

- [ ] **Step 1: Spot-build LyraEditor in the rewritten clone**

```bash
"D:/Projects/Unreal Engine 5/Engine/Build/BatchFiles/Build.bat" \
  LyraEditor Win64 Development \
  -project="$(cygpath -w "$(pwd)/LyraStarterGame.uproject")" \
  -NoUba -MaxParallelActions=4 -waitmutex
```

Expected: build succeeds (asset removal does not affect compile inputs). May need to re-apply the engine `.Build.cs` patches if they were lost in cloning — they shouldn't be, since they live in the sibling engine dir, not in this repo.

- [ ] **Step 2: Spot-run the mock doctest suite**

```bash
Binaries/Win64/BlueprintReaderMcpTests.exe
```

Expected: 441 cases pass, exit 0. (Live cases auto-skip when env vars aren't set.)

- [ ] **Step 3: Spot-run setup.bat -VerifyOnly to confirm manifest still references valid paths**

```bash
pwsh -NoProfile -File Scripts/setup.ps1 -VerifyOnly
```

Expected: `8682 files still missing after restore` warning (working tree was stripped — manifest still expects them). Exit 1 is fine here; we're checking that the manifest itself survived the rewrite and parses.

- [ ] **Step 4: Promote the rewritten clone to canonical**

```bash
cd ..
mv UE5_MCP UE5_MCP-pre-strip-backup    # do not delete; keep until force-push is confirmed
mv UE5_MCP-rewrite UE5_MCP
cd UE5_MCP
git remote set-url origin https://github.com/defessler/Unreal-Engine-5-MCP.git
```

(`git filter-repo` removes the origin remote by default — restore it.)

- [ ] **Step 5: Force-push with --force-with-lease**

```bash
git push --force-with-lease origin main
```

Expected: push succeeds. `--force-with-lease` refuses if origin advanced unexpectedly since the last fetch — safer than plain `--force`.

- [ ] **Step 6: Update the backup branch ref on origin**

```bash
git push origin --force-with-lease pre-asset-strip-branch:pre-asset-strip-branch
git push origin pre-asset-strip:pre-asset-strip   # tag was set before the rewrite
```

- [ ] **Step 7: Final verification**

```bash
git ls-tree -r HEAD --long | awk '$5 ~ /\.(uasset|umap)$/ {sum+=$4; count++} END {print count, "files,", sum/1024, "KB"}'
git count-objects -vH
pwsh -NoProfile -File Scripts/setup.ps1 -DryRun -Source release
```

Expected:
- 2 files, < 200 KB total
- size-pack < 300 MB
- DryRun prints expected URL for the lyra-assets-v1 bundle

- [ ] **Step 8: Final cleanup (after confidence holds — wait at least a day)**

```bash
# After successful smoke testing and confirmation that nothing's lost:
cd ..
rm -rf UE5_MCP-pre-strip-backup
rm -rf UE5_MCP-backup.git
```

Until then, keep both backup copies. Phase B is irreversible without them.

---

## Self-review notes

**Spec coverage check:**
- Scope of removal — Tasks 3 (Get-LyraAssetPaths), 13 (strip-paths spec). ✓
- setup.bat hybrid flow — Tasks 9 (setup.ps1), 10 (shim). ✓
- Manifest with SHA-256s — Tasks 5 (Build-Manifest), 12 (commit baked manifest). ✓
- Bundle publish — Task 11 (publisher script), Task 15 (operator runs it). ✓
- Filter-repo procedure — Tasks 13 (spec), 16 (run), 17 (verify + push). ✓
- Order of operations from spec — preserved as Task ordering. ✓
- Backup + safety net before rewrite — Task 16 Steps 1-2. ✓
- README + CLAUDE.md updates — Task 14. ✓
- Pause for explicit "go" between Phase A and B — Task 15 Step 6. ✓

**Type consistency check:**
- `Build-Manifest` returns PSCustomObject with fields `schema_version, tag, generated_at, total_files, total_bytes, files`. Same fields referenced in `Test-Manifest` and `setup.ps1` (`$manifest.total_files`, `$manifest.files`). ✓
- `Test-Manifest` returns `{Ok, Missing, Mismatch}` — same fields referenced in setup.ps1 verification block. ✓
- `Find-LyraInstallPaths` returns `string[]` — setup.ps1 calls `@(Find-LyraInstallPaths ...)[0]` and treats as path. ✓
- `Get-RestorePathMap` returns ordered hashtable with `glob` and `glob_dst` keys — setup.ps1 uses both forms. ✓

**Placeholder scan:** no TBDs, no "implement appropriately", every code block is complete code. The one place I'd flag is `.gitignore` rewrite (Task 14 Step 2): the new ignore rules are speculative — written assuming filter-repo strips on commit, but setup.bat then writes restored files into ignored paths. That works because `.gitignore` doesn't prevent writes, only `git add` — but if anyone runs `git status` post-restore they'll see no diff (good) and `git add Content/` will be a no-op (also good, by design). This is intentional, not a placeholder, but worth re-reading once during execution.
