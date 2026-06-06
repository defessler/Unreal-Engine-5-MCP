# Installation

There are two parts: the **MCP server** (a small C++20 binary that talks
JSON-RPC to Claude) and the **BlueprintReader UE plugin** (the editor-side
half that actually reads your `.uasset` files). The mock backend lets you
skip the plugin entirely — useful for smoke-testing the server.

> **Bring your own UE project.** This repo tracks the
> `BlueprintReader` plugin + docs, not a full game project — a fresh
> clone has no `LyraStarterGame.uproject`. Mount
> `Plugins/BlueprintReader/` into your own UE 5.8 project's `Plugins/`
> folder (any UE 5.8 project works; Lyra is a convenient host), then
> build that project's editor target. The commands below reference
> `LyraStarterGame.uproject` / `LyraEditor` because that's the
> maintainer's local build host — substitute `<your-project>.uproject`
> and `<YourEditor>` throughout.

## Requirements

| Component              | Required for | Notes                                                   |
|------------------------|--------------|---------------------------------------------------------|
| Windows 10/11, x64     | Everything   | Windows-only today (`CreateProcessW`).                  |
| Visual Studio 2022 (MSVC) | **Re**building the server or editor module (skip if using the precompiled release exe) | "Desktop development with C++" (for the CMake build) **or** "Game development with C++" (for the UBT build), plus a Win10/11 SDK. |
| A UE 5.8 install — **source _or_ Launcher/installed** | The `commandlet` + `live` backends (working real `.uasset` files) | **Not needed for the mock backend** (fixture-only server testing). A *source* engine builds everything through UBT (editor module + MCP server). A *Launcher / installed* engine works too: the editor module still builds via UBT, and the MCP server builds engine-free via the [CMake fallback](#installed--launcher-engine-build-the-mcp-server-with-cmake). |
| Disk: ~120 GB *(source engine)* / a few GB *(installed engine)* | Per engine choice | A source build pulls ~70 GB of UE source via `Setup.bat`; an installed engine needs only the Launcher download + the build output. |

**No network, no git, no vcpkg required for the MCP server.** Third-party
deps (nlohmann_json, fmt, doctest) are vendored under
`Plugins/BlueprintReader/Tests/ThirdParty/` — see [the manifest there](https://github.com/defessler/Unreal-Engine-5-MCP/tree/main/Plugins/BlueprintReader/Tests/ThirdParty).

## 1. Build the MCP server

> **Prefer not to build?** Release plugin bundles ship `BlueprintReaderMcp.exe`
> precompiled under `Binaries/Win64/` — the server is pure C++20 and
> engine-version-independent, so the prebuilt exe drives any UE 5.x editor the
> same as a locally-built one. Unzip the plugin into your project's `Plugins/`
> and **skip to [step 4](#4-wire-it-into-your-ai-client)** (the bundled
> `fixtures/` also give you the mock backend with no UE install). Build from
> source only if you're working from a clone or want the test suite — the rest
> of this section covers that.

The MCP server is a UE Program target. **On a source-built engine**, UBT
builds it alongside the rest of the plugin (UBA cache, ninja, etc.) — use
`Build-MCPServer.ps1` below. **On a Launcher / installed engine**, UBT
refuses Program targets, so build the server with CMake instead — jump to
[the CMake fallback](#installed--launcher-engine-build-the-mcp-server-with-cmake).

```powershell
git clone https://github.com/defessler/Unreal-Engine-5-MCP.git UE5_MCP
# Copy the plugin into your UE 5.8 project, then build from there:
Copy-Item -Recurse UE5_MCP\Plugins\BlueprintReader <YourProject>\Plugins\
cd <YourProject>
# Build the MCP server + the doctest suite via UBT:
.\Plugins\BlueprintReader\Scripts\Build-MCPServer.ps1 `
  -EngineDir "<Path>\UnrealEngine" `
  -ProjectFile "$PWD\<your-project>.uproject"
# Run the suite:
Binaries\Win64\BlueprintReaderMcpTests.exe
```

The test exe run takes ~5 s. 800+ cases pass; the live-only
cases skip without engine env vars set — that's expected.

The exe you'll point Claude at lives at:

```
<YourProject>\Binaries\Win64\BlueprintReaderMcp.exe
```

### Installed / Launcher engine? Build the MCP server with CMake

A Launcher / installed engine (e.g. `…\Epic Games\UE_5.8`) **refuses to
build UE Program targets** via UBT (`Program targets are not currently
supported from this engine distribution`), so `Build-MCPServer.ps1` won't
work there. But the MCP server + test suite are pure standalone C++20
(`bCompileAgainstEngine=false`, vendored `json`/`fmt`/`doctest`) and link
against **no engine at all** — so build them directly with CMake + Ninja +
the system MSVC toolchain:

```powershell
# From an "x64 Native Tools for VS 2022" shell (so cl + ninja are on PATH),
# at the repo root:
cmake -S Plugins/BlueprintReader/Tests -B Saved/mcp-build -G Ninja
cmake --build Saved/mcp-build
```

This emits the same `Binaries\Win64\BlueprintReaderMcp.exe` +
`BlueprintReaderMcpTests.exe` as the UBT path. The binary is
**engine-version-independent** — it drives a UE 5.8 editor/daemon exactly
like a UBT-built one. (The one non-obvious flag, `/Zc:preprocessor`, is
already set in `Tests/CMakeLists.txt`.) You still build the **editor plugin
module** with your installed engine's UBT in [step 3](#3-build-the-ue-plugin);
only the standalone Program targets need this fallback.

If you only want the mock backend (you don't have a UE project to point at,
or you're just testing the server), **you're done — skip to step 4**.

## 2. Build the engine

> **On a Launcher / installed engine? Skip this entire section.** You only
> need a *source* engine if you want UBT to build the MCP server's Program
> targets. With an installed engine, build the server via the
> [CMake fallback](#installed--launcher-engine-build-the-mcp-server-with-cmake)
> (step 1) and the editor module with your installed engine's UBT (step 3) —
> the plugin's editor module builds fine against a binary engine.

This section is the **source-engine** path: it gives you the full UBT
pipeline (one build covers the editor module *and* the MCP server's Program
targets). A source build is required only for that UBT server path — not for
the plugin's editor module, and not at all if you use the CMake fallback
above.

```powershell
# Pick a directory OUTSIDE this repo — engine source is ~100 GB.
# Convention: a sibling of UE5_MCP.
cd "D:\Projects\Unreal Engine 5"
git clone -b 5.8 https://github.com/EpicGames/UnrealEngine.git .
.\Setup.bat
.\GenerateProjectFiles.bat
```

(You need to be in the [EpicGames GitHub org](https://www.unrealengine.com/en-US/ue-on-github)
to clone the engine source.)

Open `UE5.sln` in Visual Studio 2022 and build the **`UnrealEditor`** target
in *Development Editor / Win64*. Coffee + lunch — first build is 1–3 hours.

### Engine source patches

UE 5.8's GitHub source has three modules whose `Build.cs` declares
`PrivateIncludePaths` relative to `Engine/Source/` instead of the module
directory. That breaks project-target builds with `fatal error C1083`.

**Easy path:** the plugin ships a script that applies the patches
idempotently.

```powershell
# Dry-run (report what would change):
.\Plugins\BlueprintReader\Scripts\Patch-Engine.ps1 `
    -EngineDir "D:\Projects\Unreal Engine 5"

# Apply:
.\Plugins\BlueprintReader\Scripts\Patch-Engine.ps1 `
    -EngineDir "D:\Projects\Unreal Engine 5" `
    -Apply
```

The script:
- Patches three files: `LiveCoding.Build.cs`, `TVOSTargetPlatformSettings.Build.cs`, `VisionOSTargetPlatformSettings.Build.cs`.
- Adds `using System.IO;` near the top.
- Replaces the relative `PrivateIncludePaths` entry with
  `Path.Combine(ModuleDirectory, ...)`.
- Idempotent — re-running on already-patched files is a no-op.

After patching, rebuild the affected modules — you don't need a full
engine rebuild.

### Register the engine

```powershell
& "D:\Projects\Unreal Engine 5\Engine\Binaries\Win64\UnrealVersionSelector-Win64-Shipping.exe" `
    /switchversionsilent `
    "D:\Projects\UE5_MCP\LyraStarterGame.uproject" `
    "D:\Projects\Unreal Engine 5"
```

This writes a stable GUID-style entry under
`HKCU\SOFTWARE\Epic Games\Unreal Engine\Builds` and updates the project's
`EngineAssociation` field to match. (Hand-editing the registry is brittle
— Rider / VS / UBT all want this exact format. See
[Troubleshooting](Troubleshooting#failed-to-locate-unreal-engine-associated-with-the-project-file).)

## 3. Build the UE plugin

Generate project files:

```powershell
& "D:\Projects\Unreal Engine 5\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" `
    -projectfiles `
    -project="D:\Projects\UE5_MCP\LyraStarterGame.uproject" `
    -game -rocket -progress
```

Then build the editor target (substitute your own editor-target name +
`.uproject`; the `LyraEditor` / `LyraStarterGame.uproject` values below
are the maintainer's local build host):

```powershell
& "D:\Projects\Unreal Engine 5\Engine\Build\BatchFiles\Build.bat" `
    <YourEditor> Win64 Development `
    -project="<Absolute>\<your-project>.uproject" `
    -waitmutex
```

If you have a small Windows page file (~20 GB or less), add
`-NoUba -MaxParallelActions=4` — UBA allocates large VAS chunks per worker
and can saturate small page files even on 64 GB RAM machines.

After the first full build (10–30 min), incremental rebuilds are 5–10 s
for plugin-only changes.

### MCP server is its own UBT target

The MCP server lives as two independent UE Program targets:

- `BlueprintReaderMcp` → `Binaries\Win64\BlueprintReaderMcp.exe`
- `BlueprintReaderMcpTests` → `Binaries\Win64\BlueprintReaderMcpTests.exe`

The MCP server is engine-independent and is **not** built by the editor
target — building the editor compiles only the editor module. Release
plugin bundles ship `BlueprintReaderMcp.exe` precompiled, so usually there's
nothing to build. Otherwise build it on demand with the wrapper below (it
auto-picks UBT on a source engine or the engine-free CMake fallback on an
installed/Launcher engine), or use the Toolbox's "Rebuild MCP server"
option. (Earlier versions auto-built the server via a `.uplugin`
`PreBuildSteps` hook on every editor compile; that hook was removed.)

```powershell
.\Plugins\BlueprintReader\Scripts\Build-MCPServer.ps1 `
  -EngineDir "D:\Path\To\UnrealEngine" `
  -ProjectFile "$PWD\LyraStarterGame.uproject" `
  [-Targets All|Mcp|Tests] [-Config Development] [-ExtraArgs "-NoUba -MaxParallelActions=4"]
```

Incremental rebuilds: changing one `.cpp` under `Plugins/BlueprintReader/Tests/`
rebuilds in ~10 s.

### Drop the plugin into your own project

The `BlueprintReader` plugin lives at `Plugins/BlueprintReader/` in this
repo. To use it in another UE project, copy that whole directory into
your project's `Plugins/` folder, then regenerate project files. The
plugin only adds an editor module — no runtime impact, no game-thread
cost.

## 4. Wire it into your AI client

The fastest path is to ask the server for a ready-to-paste config snippet:

```powershell
# Auto-discovers your engine + project from the exe location
Binaries\Win64\BlueprintReaderMcp.exe config --client=claude-code
```

That prints a complete `.mcp.json` block with absolute paths filled in.
Other client formats:

- `--client=claude-desktop` (same JSON shape, used in `claude_desktop_config.json`)
- `--client=copilot` (uses the `"servers"` key VS Code expects)

> The server is **read-only by default** — reads work immediately but
> every write tool is rejected until you add `BP_READER_ALLOW_WRITE=1`
> (or `BP_READER_READ_ONLY=0`) to the server's `env` block.

If the auto-discovered values look wrong (or you need to override), the
manual path + per-client conventions are documented below.

### Sanity-check the install before wiring

```powershell
Binaries\Win64\BlueprintReaderMcp.exe doctor
```

Walks the same checks the server runs at startup and prints what's
missing with concrete fix commands. Exits non-zero if anything's broken,
so it's CI-friendly. Replaces the `Verify-Build.bat` flow for a more
detailed picture (the .bat still works but only checks file presence).

---

Wiring is per-client and lives on its own page. Pick yours:

- **[Claude Code / Claude Desktop](Clients#claude-code-recommended)** —
  drop `.mcp.json` (or run `claude mcp add`); auto-loads at session
  start. Recommended for Blueprint work.
- **[GitHub Copilot in VS Code](Clients#github-copilot-vs-code)** —
  workspace `.vscode/mcp.json`; symmetric setup to Claude. Tools only
  surface in Copilot Chat's **Agent mode**.
- **[ChatGPT](Clients#chatgpt-requires-a-bridge)** — does **not**
  support local stdio MCP. Requires bridging the exe to HTTPS via
  `mcp-remote` + a tunnel, then registering the public URL via
  Settings → Connectors with Developer mode on.

Mock-backend-only setup (no UE, no env vars) is also covered on the
[Clients](Clients) page.

## 5. Verify

In your client, ask:

> What variables are on `/Game/AI/BP_TestEnemy`?

The AI should call `read_blueprint`, get back JSON, and summarize.

You can also drive the doctest suite directly (no client needed) to
sanity-check the whole stack:

```powershell
Binaries\Win64\BlueprintReaderMcpTests.exe
```

Or run the server interactively against an MCP client:

```powershell
pwsh -File Plugins\BlueprintReader\Scripts\Start-MCPServer.ps1
```

See [Clients → Starting the server](Clients#starting-the-server) for
manual-launch details.

## Test blueprints

The repo tracks two test BPs (`BP_TestEnemy`, `BP_TestPickup`) under
`Content/AI/` — they're present after cloning. To regenerate (run
against whatever project hosts the plugin; substitute your own
`.uproject`):

```powershell
& "D:\Projects\Unreal Engine 5\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
    "<Absolute>\<your-project>.uproject" `
    -run=BPRSeed -nullrhi -nosplash -unattended -nopause
```

Recreates `BP_TestEnemy` (5 vars + 2 functions + event-graph topology)
and `BP_TestPickup`.
