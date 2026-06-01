# Troubleshooting

Common failure modes I've actually hit, with fixes that worked.

## `list_blueprints` returns `commandlet exit=3` with a Niagara/plugin callstack

You'll see something like:

```
commandlet exit=3; tail:
  ... UnrealEditor-Niagara.dll!FNiagaraDataChannelLayoutInfo::...
  ... UnrealEditor-NiagaraEditor.dll!FNiagaraEditorModule::OnAssetRegistryLoadComplete()
  ... UnrealEditor-AssetRegistry.dll!UAssetRegistryImpl::SearchAllAssets()
  UnrealEditor-BlueprintReaderEditor-...dll!RunListOp()
```

The bottom is us, but the crash is inside another plugin's
`OnAssetRegistryLoadComplete` handler (Niagara, Animation, Substance,
Wwise — anything that faults loading a particular asset).

**Fix**: ensure you're on a current build. `list_blueprints` now uses
`AR.ScanPathsSynchronous({pathFilter})` instead of `SearchAllAssets`, so
it only scans the path you asked about and doesn't trigger the global
broadcast that wakes up other plugins' load handlers.

**Workaround if rebuilding isn't possible**: narrow the `path` argument
to a content folder that doesn't reference the offending plugin's
assets. If the crash persists after rebuilding, it's a `LoadObject`
elsewhere in the read path — open an issue with the full stack and the
asset path that reproduces.

## "Failed to locate Unreal Engine associated with the project file"

Rider, Visual Studio, or `UnrealBuildTool.exe` reports this when opening
a `.uproject` whose `EngineAssociation` value doesn't resolve to a real
engine root.

**Fix**: re-register through `UnrealVersionSelector` instead of editing
the registry by hand. This writes the canonical GUID-style entry under
`HKCU\SOFTWARE\Epic Games\Unreal Engine\Builds` and updates the
`.uproject` to match.

```powershell
# Optional: clean any stale entries first
$key = 'HKCU:\SOFTWARE\Epic Games\Unreal Engine\Builds'
Get-Item $key | Select-Object -ExpandProperty Property | ForEach-Object {
    Remove-ItemProperty -Path $key -Name $_
}

# Then associate this project with the engine
& "D:\Projects\Unreal Engine 5\Engine\Binaries\Win64\UnrealVersionSelector-Win64-Shipping.exe" `
    /switchversionsilent `
    "D:\Projects\UE5_MCP\LyraStarterGame.uproject" `
    "D:\Projects\Unreal Engine 5"
```

Close + reopen Rider (or VS); Rider only re-reads the engine association
on solution open.

## Rider load fails after renaming the project

Rider caches its project model under `.idea/` and the previous
`.sln`/`.vcxproj` files reference the old name. After a rename:

```powershell
# Wipe stale GenerateProjectFiles intermediate
Remove-Item -Recurse -Force `
    "D:\Projects\UE5_MCP\Plugins\BlueprintReader\Intermediate\Build\Win64\x64\UnrealEditorGPF" -EA SilentlyContinue

# Wipe stale Rider cache
Remove-Item -Recurse -Force "D:\Projects\UE5_MCP\.idea"

# Regenerate project files for the new name
& "D:\Projects\Unreal Engine 5\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" `
    -projectfiles `
    -project="D:\Projects\UE5_MCP\LyraStarterGame.uproject" `
    -game -rocket -progress
```

## `fatal error C1083: cannot open include file` while building the editor target

Three engine modules in the 5.8 GitHub source declare `PrivateIncludePaths`
relative to `Engine/Source/` rather than the module directory. That
breaks project-target builds with `-DUE_TARGET_BUILD_ENVIRONMENT=Shared`.

**Fix**: patch each `Build.cs` to use `Path.Combine(ModuleDirectory, ...)`.
See the `## Engine source patches required` section in
[Installation](Installation#engine-source-patches).

## Build runs out of virtual address space mid-link

On a 64 GB RAM box with a small (~20 GB) Windows page file, UnrealBuildAccelerator
can saturate VAS even though physical RAM is fine. Symptom is usually
`fatal error LNK1102: out of memory` or hangs in the link phase.

**Fix**: build with `-NoUba -MaxParallelActions=4`. Trades wall-clock
time for predictability.

```powershell
& "D:\Projects\Unreal Engine 5\Engine\Build\BatchFiles\Build.bat" `
    LyraEditor Win64 Development `
    -project="D:\Projects\UE5_MCP\LyraStarterGame.uproject" `
    -NoUba -MaxParallelActions=4 -waitmutex
```

## Daemon hangs — "no response after sentinel"

The daemon emits `__BPR_DONE <code>__\n` after each command. If anything
in the editor logs that exact string before the real sentinel arrives,
the MCP-side scanner finds the imposter first and parses garbage as
the next call's response.

**Fix**: don't put the sentinel format in any log message. The plugin's
`-Help` output deliberately scrubs it for this reason.

If the daemon genuinely dies and the server doesn't notice, the next
call falls back to a one-shot subprocess and surfaces no error. Set
`BP_READER_DAEMON=0` to force one-shot mode while debugging.

## Every write tool returns a read-only error

The server is **read-only by default**. If every mutation tool is
rejected with a read-only error, set `BP_READER_ALLOW_WRITE=1` (or
`BP_READER_READ_ONLY=0`) in the server's `env` block and restart the
client. See [Configuration](Configuration).

## Commandlet write op failed — how do I read the diagnostic?

The commandlet backend now classifies write failures into three
named cases instead of dumping generic linker spam. Look at the tail
of the daemon stderr (the MCP server surfaces this in the error
envelope's `_meta`):

- **"file is locked (sharing violation)"** — the Unreal Editor is
  open and holds an exclusive write handle on the `.uasset`. Close
  the editor, or set `BP_READER_BACKEND=auto` so writes route through
  the editor's in-process TCP server (no file-lock conflict).
- **"asset is `<ClassName>`, not a UBlueprint"** — bp-reader handles
  Blueprint assets only. Data Assets (UPrimaryDataAsset descendants),
  DataTables, Curves, Materials, etc. aren't supported. Inspect them
  via the editor or raw asset serialization.
- **"parent class `<Path>` could not be resolved"** — the on-disk
  asset references a C++ class whose module isn't compiled in this
  build. Rebuild the project (`Build.bat <TargetName> Win64
  Development`) before reading or writing this Blueprint.
- **"file opens cleanly, so this isn't a file-lock issue"** — the
  save failed for some other reason (compile error, serialization
  bug). Check the editor log above the diagnostic line for the
  underlying error.

The probe is non-destructive — uses raw Win32 CreateFileW with
OPEN_EXISTING, never creates or truncates the asset.

## Editor doesn't show changes after a write tool call

The change *is* on disk and compiled — you just need to re-focus the
asset. Click off the asset tab, click back. UE only reloads asset state
when the asset is selected.

If you have it open in a Blueprint editor window, close + reopen the
window. UE doesn't broadcast asset change notifications across MCP-driven
modifications because the modifications happen in a separate process.

## `add_function_output` says "ok" but doesn't add the pin

Older versions of the plugin had a silent failure when a function
didn't already have a `K2Node_FunctionResult` (functions without
outputs don't auto-create one). The current plugin spawns the result
node on first `add_function_output` call, so this should no longer
reproduce. If you see it, you're on a stale build — rebuild the editor
target.

## `find_node` returns nothing for nodes I can see

`find_node` does substring matches on the **class name**, the
**rendered title**, AND a handful of meta fields:
`meta.targetFunction` / `meta.function_name` (CallFunction nodes),
`meta.variableName` / `meta.variable_name` (VariableGet/Set), and
`meta.eventName` / `meta.event_name` (Event / CustomEvent). Pin labels
are not searched.

To find "every place I read `Health`", combine `query: "Health"` with
`kind: "VariableGet"`. The query alone usually suffices because the
underlying `meta.variableName` matches the variable identifier even
when the title is humanized (e.g. "Get Health" → matches "Health"
either way).

The mapping from `kind` to the underlying K2 class set is documented
by the `list_node_kinds` meta tool. `find_node` has no `class_filter`
argument — use `kind` for narrowing.

## `list_blueprints` returns stale data after seeding

`list_blueprints` reads from the asset registry, which is process-local.
A separate editor process that writes new BPs (e.g. the seed commandlet)
won't propagate into a *running* daemon's asset registry until the
daemon restarts.

**Fix**: kill the daemon (`BP_READER_DAEMON=0` for that call, or
restart the MCP server / Claude session). The next call respawns a
fresh editor with a fresh registry.

## First diagnostic: run the verifier

Before chasing log messages, run the build-state verifier:

```
Plugins\BlueprintReader\Scripts\Verify-Build.bat
```

It checks both halves of the plugin:

- `BlueprintReaderMcp.exe` — built by UBT as a Program target
  (`Build.bat BlueprintReaderMcp ...`); output at
  `<Project>/Binaries/Win64/BlueprintReaderMcp.exe`.
- `UnrealEditor-BlueprintReaderEditor.dll` — built by UBT during an
  editor-target build

Most "daemon exited before reaching READY" cases turn out to be a
missing `UnrealEditor-BlueprintReaderEditor.dll`: UE finishes plugin
discovery, doesn't find a `BlueprintReader` commandlet class, and
exits cleanly. The verifier will spot this and print the exact UBT
command to fix it (it autodetects your `.uproject` and the editor
target name).

## Client connects but times out before any tool call ("Request timed out")

Symptom: Copilot / your client logs `MCP error -32001: Request timed out`
~60 s after starting the server. The server's stderr shows it started up
fine — banner, daemon READY — but no `framing=` line appears, meaning we
never received a request.

Cause: stdio framing mismatch. The MCP spec mandates **newline-delimited
JSON** for stdio transport, but bp-reader historically also accepted
LSP-style `Content-Length:` framing. Server versions before commit
`<dual-framing fix>` only spoke Content-Length and silently failed when a
client sent newline-delimited JSON (the spec-compliant default for the
official SDKs and JetBrains Copilot).

**Fix**: pull the latest server. The current build auto-detects which
framing the client sends on the first request and mirrors it on
responses. The first non-error log line after the request arrives is now:

```
[bp-reader-mcp] framing=newline-delimited (auto-detected from first request)
```

If you don't see that line within a few seconds of client connection,
the client is sending something neither format recognises (very rare;
usually means the input stream is corrupted somehow — open an issue).

## Plugin DLL exists but UE silently skips it (no log mention at all)

Symptom: Verify-Build says `[ OK ] UnrealEditor-BlueprintReaderEditor-Win64-DebugGame.dll`,
but the daemon log has zero mentions of `BlueprintReader` — UE neither
loads the plugin nor logs a failure for it. The daemon initialises the
engine, finishes plugin discovery, then exits without running our
commandlet.

Cause: **config-suffix mismatch**. UE only loads plugin module DLLs whose
configuration suffix matches the running editor process:

| Editor exe | Plugin DLL it loads |
|------------|--------------------|
| `UnrealEditor-Cmd.exe` (Development) | `UnrealEditor-BlueprintReaderEditor.dll` |
| `UnrealEditor-Win64-DebugGame-Cmd.exe` | `UnrealEditor-BlueprintReaderEditor-Win64-DebugGame.dll` |
| `UnrealEditor-Win64-Debug-Cmd.exe` | `UnrealEditor-BlueprintReaderEditor-Win64-Debug.dll` |

(The `-Cmd` suffix lands *after* the config decoration — see
`UEBuildBinary.cs::GetAdditionalConsoleAppPath`.)

The daemon defaults to launching `UnrealEditor-Cmd.exe` (Development). If
your editor target was built in a different config, the matching plugin
DLL has a suffix the Development exe doesn't look for — silent skip.

**Fix**: tell the daemon which config to launch:

```json
"env": {
  "BP_READER_BACKEND":       "commandlet",
  "BP_READER_ENGINE_DIR":    "...",
  "BP_READER_PROJECT":       "...",
  "BP_READER_EDITOR_CONFIG": "DebugGame"
}
```

Or just rebuild the plugin in Development (`Build.bat ProjectEditor Win64
Development ...`) so the DLL matches the suffix-less exe.

`Verify-Build.bat` lists every config variant it finds and prints this
warning when only non-Development variants are present.

## "daemon exited before reaching READY" — plugin or module failed to load

If the server's stderr says `daemon exited before reaching READY`
followed by a `(code=1)` and a tail with `LogPluginManager: Error:
Plugin 'X' failed to load because module 'Y' could not be found.`,
your project enables a plugin whose binaries aren't built in this
engine. Common offenders: DLSS, FSR, NVIDIA Reflex, Wwise — binary
marketplace plugins that come pre-built for the launcher engine but
not for your source-built one.

**Don't try `-DisablePlugin=`** — UE's CLI plugin-disable switches
(both `-DisablePlugin=` and `-DisablePlugins=`) silently no-op for
plugins that are already enabled in the `.uproject`. They only filter
plugins added by `-EnablePlugins=`. This is verifiable in
`Engine/Source/Runtime/Projects/Private/PluginManager.cpp` —
`Context.ConfiguredPluginNames.Contains(name)` is checked before
applying the disable, and `.uproject`-listed plugins are added to
that set first.

**The real escape hatch is `-EnableAllPlugins`.** Counter-intuitively
named, but it does double duty: it adds discovered plugins to the
enable set AND converts plugin-module load failures from
fatal-with-popup into warnings, so the editor commandlet can finish
starting up.

```json
"env": {
  "BP_READER_BACKEND":     "commandlet",
  "BP_READER_ENGINE_DIR":  "D:\\Projects\\Unreal Engine 5",
  "BP_READER_PROJECT":     "D:\\Path\\To\\Your.uproject",
  "BP_READER_EDITOR_ARGS": "-EnableAllPlugins"
}
```

The MCP server only needs the asset registry + Blueprint
introspection — DLSS / Wwise / etc. failing to load doesn't affect
its ability to read `.uasset` files.

If you'd rather not enable extra plugins for the commandlet, the
alternative is to **edit `.uproject`** to set `"Enabled": false` for
the broken plugins. That's invasive but it's the only way to actually
keep the plugin out of the configuration.

## "daemon timed out reaching READY" / server hits the startup timeout

Different failure mode from the one above — the daemon **didn't exit**;
it's still running but hadn't printed `__BPR_READY__` within the
startup-timeout window. Big UE projects can take **minutes** to load
all editor modules, scan the asset registry, and compile shaders
against a cold DDC.

```
[bp-reader-mcp][commandlet][daemon] daemon timed out reaching READY
  (waited 600s; bump BP_READER_STARTUP_TIMEOUT_SECONDS for slower
  projects): daemon read timeout after 600s waiting for marker
```

**Fix**: increase `BP_READER_STARTUP_TIMEOUT_SECONDS`. In `.mcp.json`:

```json
"env": {
  "BP_READER_BACKEND":               "commandlet",
  "BP_READER_ENGINE_DIR":            "D:\\Projects\\Unreal Engine 5",
  "BP_READER_PROJECT":               "D:\\Path\\To\\Your.uproject",
  "BP_READER_PREWARM":               "1",
  "BP_READER_STARTUP_TIMEOUT_SECONDS": "1800"
}
```

1800 s (30 min) is a reasonable upper bound for the worst-case first
launch (cold DDC + large content set). Once the daemon's READY, the
per-call `BP_READER_TIMEOUT_SECONDS` (default 120 s) takes over and
subsequent tool calls return in ~30 ms.

**Also helpful**: warm the DDC by opening the project in the full UE
editor once before relying on the MCP server. Shader compile is the
single biggest factor; once compiled into the DDC, commandlet startup
drops by minutes.

## `Attempted to create a package with name containing double slashes` running the commandlet from Git Bash / MSYS

Symptom: invoking `UnrealEditor-Cmd.exe ... -Asset=/Game/AI/BP_TestEnemy`
from a Git Bash / MSYS shell crashes with:

```
LogWindows: Error: appError called: Fatal error:
  Attempted to create a package with name containing double slashes.
  PackageName: //Game/AI/BP_TestEnemy
```

Cause: MSYS path translation. Git Bash sees `/Game/AI/...` and rewrites
it to `C:/Program Files/Git/Game/AI/...` because the first segment
looks like a Unix absolute path. Quoting with `'//Game/...'` makes
MSYS leave it alone but UE then sees the literal `//Game/...` (with
double slash) and rejects it.

**Fix**: set `MSYS_NO_PATHCONV=1` for the call:

```bash
MSYS_NO_PATHCONV=1 "D:/Projects/Unreal Engine 5/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" \
  "D:/Projects/UE5_MCP/LyraStarterGame.uproject" \
  -run=BPR -Op=Read -Asset=/Game/AI/BP_TestEnemy -Out=D:/temp/out.json \
  -nullrhi -nosplash -unattended -nopause
```

Or run from PowerShell / `cmd.exe` where the issue doesn't exist.
The MCP server itself isn't affected — it uses `CreateProcessW`
directly without path translation. This only bites when you invoke
the commandlet by hand from a Bash shell.

## "ImportError: Could not load module" when launching Claude with the server

You're pointing at a `Debug` build of `BlueprintReaderMcp.exe` from a
machine without the Visual C++ debug runtime. Build `Development`
(UBT's release-equivalent):

```powershell
& "<EngineDir>\Engine\Build\BatchFiles\Build.bat" `
    BlueprintReaderMcp Win64 Development `
    -project="<your.uproject>"
```

Then point Claude at `<Project>\Binaries\Win64\BlueprintReaderMcp.exe`.

## Other issues

[Open an issue](https://github.com/defessler/Unreal-Engine-5-MCP/issues)
with:
- Backend (`mock` or `commandlet`)
- The MCP request that triggered it (paste the JSON)
- Anything from the server's stderr (Claude's tool debug panel) or the
  editor's `Saved/Logs/UE5_MCP.log` if commandlet
