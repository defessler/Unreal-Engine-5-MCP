# Strip Lyra assets, restore via `setup.bat`

**Date:** 2026-05-19
**Status:** Approved (brainstorming) — pending implementation plan
**Author:** Doug Fessler (via Claude brainstorm)

## Problem

The repo ships 8,682 Lyra `.uasset` + `.umap` files totalling
~2.05 GB on disk and ~1.36 GB in the git pack. Clones are slow,
pushes are large, and the working tree dwarfs the code that actually
matters (the BlueprintReader plugin and MCP server). None of those
assets are required for:

- Building the editor target — C++ inputs only.
- Building and running the MCP server (`BlueprintReaderMcp`).
- Running the doctest suite (mock-backend cases are file-driven;
  live-backend cases depend on two seed BPs that are not Lyra).

The two test BPs (`Content/AI/BP_TestEnemy.uasset`,
`Content/AI/BP_TestPickup.uasset`) are produced by
`UBPRSeedCommandlet` and are not derived from Lyra. They stay
tracked.

## Goal

1. Remove every Lyra-origin `.uasset` / `.umap` from the working
   tree.
2. Rewrite git history so the pack actually shrinks (current pack
   stays ~1.36 GB if we only delete from HEAD).
3. Ship a `setup.bat` so anyone (the author included, on a fresh
   clone) can restore those assets — either from a local Epic Games
   Launcher Lyra install or from a published GitHub Release.

## Non-goals

- Modifying any Lyra C++ source (`Source/LyraGame`,
  `Source/LyraEditor`) or `Plugins/LyraGenerated/`.
- Touching `.uplugin`, `Config/`, `*.uproject`, `*.Build.cs`, or
  the BlueprintReader plugin sources.
- CI for republishing the asset bundle. Manual publish only.
- Migrating to Git LFS.

## Scope of removal

### Removed

Every `.uasset` and `.umap` under the following globs **except**
the two test-BP exemptions:

| Glob                                                         | Files (HEAD) |
|--------------------------------------------------------------|-------------:|
| `Content/**` (except `Content/AI/BP_Test{Enemy,Pickup}.uasset`) | 2840 |
| `Plugins/LyraExampleContent/Content/**`                      | 75   |
| `Plugins/LyraExtTool/Content/**`                             | 1    |
| `Plugins/PocketWorlds/Content/**`                            | 3    |
| `Plugins/GameFeatures/ShooterCore/Content/**`                | 260  |
| `Plugins/GameFeatures/ShooterTests/Content/**`               | 35   |
| `Plugins/GameFeatures/TopDownArena/Content/**`               | 87   |
| `Plugins/GameFeatures/ShooterMaps/Content/**`                | 5330 |
| `Plugins/GameFeatures/ShooterExplorer/Content/**`            | 51   |

Total: **8,682 files, ~2.05 GB working tree, ~1.36 GB pack**.
`ShooterMaps` dominates the count — its `BuiltData.uasset` lighting
bakes and per-tile streaming chunks are the bulk.

### Kept

- `Content/AI/BP_TestEnemy.uasset`, `Content/AI/BP_TestPickup.uasset`
  — produced by `BPRSeed`, asserted by live tests against the literal
  paths `/Game/AI/BP_TestEnemy` and `/Game/AI/BP_TestPickup`. Tiny,
  not Lyra-sourced. Stay tracked.
- All `.uplugin` files (including the content-only ones for
  `LyraExampleContent`, `ShooterMaps`, `ShooterExplorer`). Removing
  Content but keeping `.uplugin` leaves the plugins as empty
  modules that load cleanly rather than missing modules that warn.
- All plugin Source/, all engine config under `Config/`, the
  `.uproject`, and the entire `Plugins/BlueprintReader/` and
  `Plugins/LyraGenerated/` trees.

### Why these are safe to remove

- Editor build is C++-only; assets are not compile inputs.
- MCP integration tests rely on the two seed BPs (regenerable via
  `LyraEditor-Cmd.exe -run=BPRSeed`).
- `git log` confirms every Content commit in this repo is a
  pristine Lyra import — there are no local modifications worth
  preserving (single author, only chore commits touching content).

## `setup.bat` behavior

`setup.bat` at the repo root is a thin shim that re-invokes
PowerShell with `Scripts/setup.ps1` (real logic lives in
PowerShell — testable, readable, callable from non-Windows hosts
later if needed).

### Flags

```
setup.bat [--source=auto|local|release]
          [--release-tag=lyra-assets-v1]
          [--force]
          [--dry-run]
          [--verify-only]
```

Default `--source=auto` = local first, GitHub Release on fallback.

### Flow

1. **Pre-flight.** Print plan + target paths. Refuse to run if the
   working tree has uncommitted asset changes under any of the
   restore-target globs (avoid overwriting local work). `--force`
   bypasses.
2. **Source resolution.**
   - **Local Lyra install.** Read
     `%PROGRAMDATA%\Epic\UnrealEngineLauncher\LauncherInstalled.dat`
     (JSON list of EGL-managed installs). For each entry, check
     whether `<InstallLocation>\LyraStarterGame.uproject` exists
     (tolerant matcher — Epic has shifted Lyra's `AppName` over the
     years). If at least one match, mirror each restore-target
     path with `robocopy /MIR /XO <src> <dst>` (multi-thread,
     resumable). Done.
   - **GitHub Release fallback.** If no local install (or
     `--source=release`), download the bundle:
     `https://github.com/defessler/Unreal-Engine-5-MCP/releases/download/<tag>/lyra-assets-<tag>.tar.zst`
     plus its `.sha256`. Verify with `certutil -hashfile`. Extract
     with `tar -x --zstd` (built into Windows 10 1803+).
3. **Post-fetch verification.** Validate every restored file
   against `Scripts/lyra-assets-manifest.json` (path + size + SHA-256).
   In local-install mode, version skew between the user's Lyra
   install and the bundle baseline shows up here — print a warning
   listing mismatched files but don't fail (UE auto-upgrades older
   asset versions on load).
4. **Test-BP defense.** If `Content/AI/BP_TestEnemy.uasset` or
   `BP_TestPickup.uasset` is missing after restore (shouldn't
   happen — they're tracked), offer to run BPRSeed.
5. **Summary.** Print `Restored N assets, X.X MB`. Exit 0.

### What `setup.bat` deliberately does NOT do

- Build the engine, plugin, or MCP server.
- Run tests.
- Modify `.uproject`, `.uplugin`, or any tracked file outside the
  restore-target globs.
- Touch git history.

Idempotent. Re-running just re-mirrors / re-extracts.

## Bundle publish (`Scripts/Publish-LyraAssetsRelease.ps1`)

One-shot operator script. Must run on a checkout that *still has*
the Lyra assets — i.e., before the history rewrite. Steps:

1. **Build the manifest.** Walk every removal-target glob, record
   `{path, size, sha256}`. Sort by path. Write
   `Scripts/lyra-assets-manifest.json` (committed to the repo, ~1
   MB, the source of truth for what setup.bat expects).
2. **Pack.** `tar --zstd -19 -cf lyra-assets-<tag>.tar.zst @<paths>`.
   Expected output ~700-900 MB compressed (DXT-compressed textures
   limit compressibility, but zstd-19 still beats raw by ~30-40%).
   Comfortably under GitHub's 2 GB per-file limit.
3. **Hash the bundle.** Write `lyra-assets-<tag>.tar.zst.sha256`.
4. **Publish.** `gh release create <tag> --notes-file release-notes.md
   lyra-assets-<tag>.tar.zst lyra-assets-<tag>.tar.zst.sha256
   Scripts/lyra-assets-manifest.json`. The manifest is uploaded as
   a release asset for visibility / out-of-band consumers, but
   `setup.bat` itself only ever reads the committed
   `Scripts/lyra-assets-manifest.json` — that's the source of truth
   bundled with every clone, so no network round-trip is needed
   for validation.
5. **Tag scheme.** `lyra-assets-v<N>`. Bump N any time the asset
   set changes (e.g., upstream Lyra updates). Initial = `v1`.
   `setup.bat`'s `--release-tag` default and `setup.ps1`'s constant
   both reference the current version.

## Git history rewrite

### Tool

`git-filter-repo`. Officially the recommended replacement for the
archived `git filter-branch`, 10-100× faster, native path-glob /
regex support. Not BFG — `filter-repo` is more flexible and
modern.

### Procedure

1. **Safety net.**
   - Push `main` to a `pre-asset-strip` backup branch on origin.
   - Make a local mirror clone: `git clone --mirror . ../UE5_MCP-backup.git`.
   This is the real rollback. A single branch on origin is not
   sufficient.
2. **Path-rewrite spec** at `Scripts/strip-lyra-paths.txt`:
   ```
   regex:^Content/(?!AI/BP_TestEnemy\.uasset$|AI/BP_TestPickup\.uasset$).*\.(uasset|umap)$
   regex:^Plugins/LyraExampleContent/Content/.*
   regex:^Plugins/PocketWorlds/Content/.*
   regex:^Plugins/LyraExtTool/Content/.*
   regex:^Plugins/GameFeatures/(ShooterCore|ShooterTests|TopDownArena|ShooterMaps|ShooterExplorer)/Content/.*
   ```
3. **Dry-run** with `git filter-repo --analyze` to see what would
   be touched. Inspect the report under `.git/filter-repo/analysis/`.
4. **Fresh-clone workflow** (required — `filter-repo` refuses to
   rewrite an "unfresh" working clone by default):
   - `git clone --no-local . ../UE5_MCP-rewrite`
   - `cd ../UE5_MCP-rewrite`
   - `git filter-repo --invert-paths --paths-from-file ../UE5_MCP/Scripts/strip-lyra-paths.txt`
5. **Garbage-collect.**
   `git reflog expire --expire=now --all && git gc --prune=now --aggressive`
6. **Verify.**
   - `git ls-tree -r HEAD --long` reports 2 `.uasset` files under
     `Content/AI/` and zero `.uasset` / `.umap` anywhere else.
   - Pack size: expect ~150-250 MB (down from ~1.36 GB).
   - Spot-build the editor target from the rewritten clone — must
     succeed (asset removal doesn't affect compile inputs).
   - Spot-run the doctest mock suite — must remain 441 passing.
7. **Promote the rewritten clone to canonical.** Move the original
   working copy aside (don't delete — keep as a second backup until
   confidence holds), and move `../UE5_MCP-rewrite` into its place.
8. **Force-push.** `git push --force-with-lease origin main`.
   `--force-with-lease` (not `--force`) refuses if origin advanced
   unexpectedly. Update the `pre-asset-strip` backup ref on origin
   afterwards.

### Risks of the rewrite

- Every commit SHA from the first Lyra import forward changes
  (~hundreds of commits). Any external reference to those SHAs
  (wiki pages, README badges, issue threads, external doc links)
  becomes a dead pointer. *Mitigation: pre-scan the wiki + README
  for hardcoded SHAs.*
- Anyone with an existing clone or fork must re-clone. Single
  author per `git log`, so impact is limited, but worth noting in
  the commit message and a `git tag` on the pre-rewrite state.

## Order of operations (the runbook)

1. Write `setup.bat`, `Scripts/setup.ps1`,
   `Scripts/Publish-LyraAssetsRelease.ps1`,
   `Scripts/strip-lyra-paths.txt`. Build
   `Scripts/lyra-assets-manifest.json` from current HEAD.
2. Update `.gitignore` (the four manually-ignored large files
   block becomes redundant — leave it or simplify, but don't
   accidentally reinstate the deleted assets).
3. Update `README.md` and `CLAUDE.md` with the new setup
   instructions (run `setup.bat` after clone) and the asset
   bundle's version/tag.
4. Commit everything from steps 1-3 — this is the "setup before
   strip" commit. The new files will survive the history rewrite
   because they're already present from before the strip's
   earliest changed commit.
5. Run `Publish-LyraAssetsRelease.ps1` → bundle + sha256 +
   manifest pushed to GitHub Release `lyra-assets-v1`.
6. Smoke tests:
   - `setup.bat --source=release --dry-run` (validates the release
     artifact is reachable and the manifest is consistent).
   - `setup.bat --source=local` against the author's Lyra install.
7. **Pause for explicit "go".** Steps 1-6 are fully reversible
   (just `git revert` the setup commit and delete the release).
   Step 8 is not.
8. Backup branch + mirror clone (per "Safety net" above).
9. Fresh-clone, `git filter-repo`, `gc --aggressive`.
10. Verify the rewritten clone (working tree count, pack size,
    spot-build, spot-test).
11. Force-push, update backup ref on origin.

## Success criteria

- `git ls-tree -r HEAD --long` shows 2 `.uasset` / `.umap` files
  total (the two test BPs).
- Pack size below 300 MB (target ~150-250 MB).
- Fresh `git clone` followed by `setup.bat` produces a working
  tree byte-identical (mod re-import timestamps embedded in
  some `.uasset` headers) to the pre-strip working tree.
- `Build.bat LyraEditor` succeeds on the rewritten clone.
- `BlueprintReaderMcpTests.exe` (mock backend) passes 441 cases.

## Open questions

None currently — all four design sections were approved without
modification during the brainstorm.

## See also

- [`CLAUDE.md`](../../../CLAUDE.md) — project guidance, build
  invariants, gotchas.
- [`README.md`](../../../README.md) — user-facing setup that
  `setup.bat` integrates with.
- `Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BPRSeedCommandlet.cpp`
  — produces the two test BPs that survive the strip.
