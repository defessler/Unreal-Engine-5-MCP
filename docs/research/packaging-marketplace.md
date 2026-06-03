# Packaging — Marketplace/Fab plugin + standalone server (INSTALL-PKG)

Executable plan for distributing BlueprintReader as **two deliverables**, and
why a full one-shot restructure is deliberately *not* attempted in-place (it
would destabilize the verified, working dev build).

## The two deliverables

1. **The plugin proper** — `BlueprintReaderRuntime` (Type=Runtime) +
   `BlueprintReaderEditor` (Type=Editor). This is what a UE project consumes:
   in-editor BP introspection/mutation + cooked-runtime read. Candidate for a
   Fab/Marketplace listing or a versioned `.zip`.
2. **The MCP server** — `BlueprintReaderMcp.exe`, a standalone C++20 program.
   It is NOT a UE module and does not belong in an engine plugin tree. It ships
   as the **prebuilt-binary GitHub Release** (INSTALL-M3 / `release.yml`,
   shipped Batch 9): engine-version-independent, downloaded + pointed at by the
   MCP client. The plugin and the server are versioned together (both stamped
   from the same `VersionName` + git hash, INSTALL-1).

This split is the key insight: the server doesn't need to be *inside* the
distributed plugin at all — decoupling it removes most Marketplace blockers.

## What actually ships in a packaged plugin (and what doesn't)

`RunUAT BuildPlugin`/`PackagePlugin` packages, by default, only:
`Source/`, `Config/`, `Content/`, `Resources/`, and the `.uplugin`.

So the often-cited blocker "the `Tests/` Program targets + vendored
`ThirdParty/` ship with the plugin" is **false** — `Tests/` is not a
default-packaged directory, so `BlueprintReaderMcp` / `BlueprintReaderMcpTests`
and `Tests/ThirdParty/` are **already excluded** from a packaged plugin. No
tree move is required to keep them out of the package.

`Binaries/` and `Intermediate/` are build artifacts (also not packaged from
source). `Scripts/`, `Claude/`, and `AGENTS.md` are dev-time assets — harmless
if excluded; include selectively via `Config/FilterPlugin.ini` only if a
consumer benefit is identified.

## Real remaining blockers (for an actual Fab submission)

| Blocker | Status / fix |
|---|---|
| **`.uplugin` `PreBuildSteps`** (Fab rejects shell-outs) | The dev `.uplugin` declares a `PreBuildSteps` hook (`PreBuildHook.ps1`) to build the server alongside the editor. Fab validation rejects PreBuildSteps. **A Marketplace-variant `.uplugin` must omit the `PreBuildSteps` block** (the server is deliverable #2, built separately). Do NOT strip it from the dev `.uplugin` — the working build relies on it. The hook is already a no-op under `BP_READER_SKIP_PREBUILD=1`, but Fab checks *presence*, not whether it runs, so a variant `.uplugin` (or a build-time strip in the package step) is required. |
| **Marketplace icon** | Fab requires `Resources/Icon128.png`. Not present yet — add a 128×128 plugin icon before submission. |
| **3 engine `.Build.cs` patches** | Documented as patches to the *sibling engine checkout* (README). They resolve `PrivateIncludePaths` for **project-target** builds; verify whether the `Runtime`/`Editor` *modules* build in a clean consumer engine without them. If the modules need them, that's a hard Fab blocker (you can't patch a consumer's engine) and must be refactored out of the modules' include resolution. **Action: build the plugin into a vanilla consumer project on an unpatched engine and see if the modules compile.** |
| **Multi-engine `EngineVersion`** | The plugin targets multiple UE versions (5.7–5.8). A Marketplace listing pins one engine version per upload; ship one package per supported version (the `doctor` compat-range check, INSTALL-3, already warns at runtime). |

## Migration steps (focused follow-up, verify each on a real editor build)

1. Add `Resources/Icon128.png` (+ `FeaturedIconSettings` if desired).
2. Produce a **Marketplace `.uplugin` variant** with no `PreBuildSteps` — either
   a separate `BlueprintReader.Marketplace.uplugin` swapped in by the package
   script, or a `RunUAT BuildPlugin` wrapper that strips the block from a temp
   copy. Verify the packaged plugin loads in a clean project.
3. Build the plugin into a **vanilla consumer project on an unpatched engine**;
   if the modules need the 3 `.Build.cs` patches, refactor their include
   resolution so they don't.
4. `RunUAT BuildPlugin -Plugin=…/BlueprintReader.uplugin -Package=…` → confirm
   the output contains only `Source/` + `.uplugin` + `Resources/` (no `Tests/`,
   `Binaries/`, `Intermediate/`).
5. Keep the server on the **binary-release** track (`release.yml`); document
   the pairing (plugin vN ↔ server vN).

## Why this isn't a single in-place PR

The dev build is verified + working (editor module + server via UBT/CMake; live
daemon confirmed). The Fab-valid changes (a `PreBuildSteps`-free `.uplugin`,
unpatched-engine module builds) each need a **clean-consumer editor build** to
verify — which CI can't do (no engine) and which risks the working build if
rushed. So INSTALL-PKG ships as: deliverable #2 (binary release) **done**, the
server **already package-excluded**, and this **executable plan** for the
Fab-submission steps — to be run as a focused effort with per-step editor-build
verification, not a hasty restructure.
