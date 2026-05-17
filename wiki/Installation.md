# Installation

There are two parts: the **MCP server** (a small C++20 binary that talks
JSON-RPC to Claude) and the **BlueprintReader UE plugin** (the editor-side
half that actually reads your `.uasset` files). The mock backend lets you
skip the plugin entirely — useful for smoke-testing the server.

## Requirements

| Component              | Required for | Notes                                                   |
|------------------------|--------------|---------------------------------------------------------|
| Windows 10/11, x64     | All          | The codebase is Windows-only today (`CreateProcessW`).  |
| Visual Studio 2022     | All          | "Game development with C++" workload + Win10/11 SDK.    |
| UE 5.7 source build    | All          | Source build, not the launcher binary build. Required to build the MCP server now that it's a UE Program target. |
| ~120 GB disk           | All          | UE source ~70 GB pulled by `Setup.bat`.                 |

**No network, no git, no vcpkg required for the MCP server.** Third-party
deps (nlohmann_json, fmt, doctest) are vendored under
`Plugins/BlueprintReader/Tests/ThirdParty/` — see [the manifest there](https://github.com/defessler/Unreal-Engine-5-MCP/tree/main/Plugins/BlueprintReader/Tests/ThirdParty).

## 1. Build the MCP server

The MCP server is now a UE Program target — UBT builds it alongside
the rest of the plugin (with UBA cache, ninja, etc.) instead of running
a separate CMake toolchain.

```powershell
git clone https://github.com/defessler/Unreal-Engine-5-MCP.git UE5_MCP
cd UE5_MCP
# Build the MCP server + the doctest suite via UBT:
.\Plugins\BlueprintReader\Scripts\Build-MCPServer.ps1 `
  -EngineDir "<Path>\UnrealEngine" `
  -ProjectFile "$PWD\UE5_MCP.uproject"
# Run the suite:
Binaries\Win64\BlueprintReaderMcpTests.exe
```

The test exe run takes ~5 s. 441 cases pass; the 12 commandlet-backed
cases skip without engine env vars set — that's expected.

The exe you'll point Claude at lives at:

```
UE5_MCP\Binaries\Win64\BlueprintReaderMcp.exe
```

If you only want the mock backend (you don't have a UE project to point at,
or you're just testing the server), **you're done — skip to step 4**.

## 2. Build the engine

The plugin requires a source-built UE 5.7 because it ships its own
`UnrealEd`-linking module. Launcher (binary) builds won't work.

```powershell
# Pick a directory OUTSIDE this repo — engine source is ~100 GB.
# Convention: a sibling of UE5_MCP.
cd "D:\Projects\Unreal Engine 5"
git clone -b 5.7 https://github.com/EpicGames/UnrealEngine.git .
.\Setup.bat
.\GenerateProjectFiles.bat
```

(You need to be in the [EpicGames GitHub org](https://www.unrealengine.com/en-US/ue-on-github)
to clone the engine source.)

Open `UE5.sln` in Visual Studio 2022 and build the **`UnrealEditor`** target
in *Development Editor / Win64*. Coffee + lunch — first build is 1–3 hours.

### Engine source patches

UE 5.7's GitHub source has three modules whose `Build.cs` declares
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
    "D:\Projects\UE5_MCP\UE5_MCP.uproject" `
    "D:\Projects\Unreal Engine 5"
```

This writes a stable GUID-style entry under
`HKCU\SOFTWARE\Epic Games\Unreal Engine\Builds` and updates the project's
`EngineAssociation` field to match. (Hand-editing the registry is brittle
— Rider / VS / UBT all want this exact format. See
[Troubleshooting](Troubleshooting#rider-says-load-failed--failed-to-locate-unreal-engine).)

## 3. Build the UE plugin

Generate project files:

```powershell
& "D:\Projects\Unreal Engine 5\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" `
    -projectfiles `
    -project="D:\Projects\UE5_MCP\UE5_MCP.uproject" `
    -game -rocket -progress
```

Then build the editor target:

```powershell
& "D:\Projects\Unreal Engine 5\Engine\Build\BatchFiles\Build.bat" `
    UE5_MCPEditor Win64 Development `
    -project="D:\Projects\UE5_MCP\UE5_MCP.uproject" `
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

The plugin's `BlueprintReader.uplugin` carries a `PreBuildSteps` hook
that invokes UBT for `BlueprintReaderMcp` before the editor build
runs, so the command above builds both halves in one invocation —
you'll see a `[BlueprintReader/PreBuild] building BlueprintReaderMcp
…` line followed by the editor compile. Drop the plugin into any
`.uproject` and you get this for free; no project-side wiring needed.

To opt out and rebuild just the editor module, set
`BP_READER_SKIP_PREBUILD=1` in the build environment. To build the
server in isolation (or to also build the test exe, which is not
pulled in automatically), invoke UBT directly per target or use the
wrapper:

```powershell
.\Plugins\BlueprintReader\Scripts\Build-MCPServer.ps1 `
  -EngineDir "D:\Path\To\UnrealEngine" `
  -ProjectFile "$PWD\UE5_MCP.uproject" `
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

The repo seeds two test BPs (`BP_TestEnemy`, `BP_TestPickup`) into
`Content/AI/`. They should already be present after cloning. To
regenerate:

```powershell
& "D:\Projects\Unreal Engine 5\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
    "D:\Projects\UE5_MCP\UE5_MCP.uproject" `
    -run=BPRSeed -nullrhi -nosplash -unattended -nopause
```

Recreates `BP_TestEnemy` (5 vars + 2 functions + event-graph topology)
and `BP_TestPickup`.
