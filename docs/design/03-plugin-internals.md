# 03 — Plugin internals (editor side)

This file documents the UE-side half — what lives under
`Plugins/BlueprintReader/Source/`. The standalone MCP server is
covered in [04-mcp-server.md](04-mcp-server.md); how the halves
communicate is in [02-architecture.md](02-architecture.md).

## Module layout

```
Plugins/BlueprintReader/
├── BlueprintReader.uplugin                  plugin manifest (no PreBuildStep; MCP server is its own UBT Program target)
└── Source/
    ├── BlueprintReaderEditor/               UnrealEd module — does all the work
    │   ├── Public/
    │   │   ├── BlueprintReaderTypes.h        FBP*Info structs (wire shapes)
    │   │   ├── BlueprintIntrospector.h       static reader API + DiagnoseFailedBlueprintLoad
    │   │   ├── BlueprintReaderCommandlet.h   UBlueprintReaderCommandlet declaration
    │   │   ├── BlueprintReaderSeedCommandlet.h
    │   │   ├── BlueprintReaderLiveServer.h   FLiveServer + start/stop wiring
    │   │   ├── BlueprintReaderLogSink.h      captures UE_LOG into a per-op buffer
    │   │   ├── BlueprintReaderJson.h         legacy rich JSON (camelCase) — backwards compat
    │   │   └── BlueprintReaderWireJson.h     snake_case wire JSON (current)
    │   └── Private/                           impls of all of the above
    └── BlueprintReaderRuntime/                non-editor sibling, opt-in via CVar
        ├── BlueprintReaderRuntime.Build.cs    no editor deps; cook-safe
        ├── Public/                            BlueprintRuntimeIntrospector etc.
        └── Private/                            includes RuntimeServer (TCP) and Console (CVars)
```

Three editor-side entry points:

- **`UBlueprintReaderCommandlet`** — invoked via
  `UnrealEditor-Cmd.exe ... -run=BlueprintReader -Op=<Verb> ...`. One
  giant `RunOneOp(Params)` dispatch that handles every read + every
  write tool. Also the only entry point exposed by `-Daemon` and by
  the live TCP server.
- **`UBlueprintReaderSeedCommandlet`** — invoked via
  `-run=BlueprintReaderSeed`. Synthesizes the test BPs (`BP_TestEnemy`,
  `BP_TestPickup`) so the live integration tests have something to
  read. Re-runnable; safe to commit the output.
- **`FLiveServer`** — TCP listener bound at module startup if the
  environment doesn't disable it. Sits inside the editor process.

The runtime module (`BlueprintReaderRuntime`) exists only so the
listener can also run in a packaged game. Its `StartupModule` defers
the listener-start check to `OnPostEngineInit`
(`BlueprintReaderRuntime.cpp:24-27`) and the listener itself is gated
on the `bp.reader.listen` CVar.

## Op dispatch (the spine of the plugin)

Every tool — read or write — funnels through one `RunOneOp(Params)`
function in `BlueprintReaderCommandlet.cpp`. Three things make it
work: the `EOp` enum, the `ParseOp` resolver, and a flat if-ladder of
per-verb function calls.

### `EOp` enum

Defined in an anonymous namespace at
`BlueprintReaderCommandlet.cpp:115-244`. One enumerator per tool. The
shape:

```cpp
enum class EOp : uint8
{
    Legacy,        // No -Op specified — emit the rich plugin shape
    List,
    Read,
    Graph,
    Function,
    Variables,
    Components,
    Find,
    // Write ops:
    AddVariable,
    SetNodePosition,
    DeleteNode,
    AddNode,
    WirePins,
    // ... ~110 more, grouped by feature area with section comments
};
```

The order matches no particular external API — new ops just append.
The order in `BlueprintTools.cpp` (the MCP-side tool registration) is
independent. The shared contract is the *name*: the MCP server passes
`-Op=AddVariable` and the plugin's `ParseOp` resolves the string back
to the enumerator.

### `ParseOp`

A linear `Equals(..., ESearchCase::IgnoreCase)` chain
(`BlueprintReaderCommandlet.cpp:246-369`). Reads `-Op=<name>` from the
commandlet args via `FParse::Value`, walks the chain, returns the
matching enum or logs `Unknown -Op=<name>` and fails.

Three reasons it's a chain instead of a map:

- `FParse` is the canonical UE arg parser; using it keeps the
  commandlet's arg surface consistent.
- The chain is short enough (~120 entries) that the cost is
  immaterial — every call already pays the cost of spawning a
  process or game-thread-bouncing a TCP message.
- Compile-time error if you add an enum entry and forget to add a
  parse entry: nothing routes to it.

### `RunOneOp`

The dispatch table itself. Lives at
`BlueprintReaderCommandlet.cpp:5092-5301`. Three sections:

1. **Top of function**: resolves `bPretty` from `-Compact`, resolves
   `OutputPath` from `-Out=`.
2. **Write op fan-out**: a flat if-ladder. Each line is roughly:
   ```cpp
   if (Op == EOp::AddVariable) return RunAddVariableOp(Params, OutputPath, bPretty);
   ```
   Every write op has its own `Run<Verb>Op` function defined earlier
   in the same translation unit.
3. **Read op switch**: for ops that need a loaded `UBlueprint`, the
   function calls `FBlueprintIntrospector::Read(AssetPath)` once and
   then `switch`es on `Op` to project the result into the appropriate
   wire shape via `FBlueprintReaderWireJson::*ToJson`.

The `RunOneOpFromLiveServer` shim
(`BlueprintReaderCommandlet.cpp:5319-5322`) is the only externally-
visible symbol — `BlueprintReaderLiveServer.cpp` calls it via `extern`
because `RunOneOp` itself lives in an anonymous namespace.

### Per-verb function shape

Most write ops follow a stereotyped 5-step pattern. Example, the
`WirePins` op
(`BlueprintReaderCommandlet.cpp:4640-4698`, condensed):

```cpp
int32 RunWirePinsOp(const FString& Params, ...)
{
    // 1. Parse args via FParse::Value
    const FString AssetPath = ResolveAssetPath(Params);
    FString GraphName; FParse::Value(*Params, TEXT("Graph="), GraphName);
    /* ... more args ... */

    // 2. Load the BP (with GC anchor logic in CompileAndSaveBlueprint)
    UBlueprint* BP = LoadMutableBlueprint(AssetPath);
    if (!BP) return 4;

    // 3. Resolve subgraph / node / pin
    UEdGraph* Graph = FindGraphByName(BP, GraphName);
    UEdGraphNode* FromNode = FindNodeByGuid(Graph, FromNodeId);
    UEdGraphPin* FromPin = FindPinByIdOrName(FromNode, FromPinSpec);
    /* ... */

    // 4. Mutate via the K2 schema (NOT direct MakeLinkTo)
    const UEdGraphSchema* Schema = Graph->GetSchema();
    const bool bMade = Schema->TryCreateConnection(FromPin, ToPin);
    if (!bMade) { /* log + return code */ }

    // 5. Compile + save (or defer to batch end)
    if (!MaybeCompileAndSave(BP)) return 5;
    return EmitOk(OutputPath, bPretty);
}
```

The shared helpers are defined once at
`BlueprintReaderCommandlet.cpp:414-624`:

- `LoadMutableBlueprint` — `LoadObject<UBlueprint>` with
  `LOAD_NoWarn | LOAD_Quiet`, falls through to
  `DiagnoseFailedBlueprintLoad` on failure
  (`BlueprintReaderCommandlet.cpp:416-444`).
- `FindGraphByName` — case-insensitive scan of
  `BP->UbergraphPages`, `BP->FunctionGraphs`, `BP->MacroGraphs`.
- `FindNodeByGuid` / `FindPinByIdOrName` — straight loops.
- `CompileAndSaveBlueprint` — anchors the BP against GC with
  `TStrongObjectPtr`, marks structurally modified, runs
  `FKismetEditorUtilities::CompileBlueprint`, writes via
  `UPackage::SavePackage`, and on failure probes for a Windows
  sharing violation
  (`BlueprintReaderCommandlet.cpp:535-624`).
- `EmitOk` — emits `{"ok":true}` to the output path.

## Batch mode (apply_ops / BeginBatch / EndBatch)

The naive case-per-op pattern would compile + save N times for an
N-op batch. `BeginBatch` flips a daemon-scoped flag
(`BatchDeferFlag` —
`BlueprintReaderCommandlet.cpp:635-644`), and the per-op call sites
use `MaybeCompileAndSave` instead of `CompileAndSaveBlueprint`
directly. When the flag is set, `MaybeCompileAndSave` records the BP
in a `TArray<TWeakObjectPtr<UBlueprint>>` and marks it structurally
modified, but does *not* compile. `EndBatch` flushes the array with
one compile + save per unique BP and returns aggregated diagnostics
(`BlueprintReaderCommandlet.cpp:676-755`).

Two notable behaviors:

- `BeginBatch` is idempotent. Re-issuing it without an `EndBatch` —
  which happens after a daemon crash recovery — just resets state.
- `EndBatch -Skip` (set when the MCP-side `apply_ops` honors
  `on_failure="skip"`) discards the pending compile + save. The
  in-memory `UBlueprint`s stay dirty until the daemon restarts; this
  is documented as the cost of strict-atomic mode.

## Daemon mode

`RunDaemon` (`BlueprintReaderCommandlet.cpp:5326-5393`) is what the
commandlet runs when invoked with `-Daemon`. Its job is to keep one
editor process alive across many tool calls. Two non-obvious
pieces:

### Raw Win32 stdio (not UE-mediated)

```cpp
HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
HANDLE hIn  = GetStdHandle(STD_INPUT_HANDLE);
```

We can't use `fputs(stdout, …)`, `FPlatformMisc::LocalPrint`, or
`UE_LOG` for daemon I/O. UE 5.7 redirects the C stdio streams through
its log device in some configurations, so a `fputs` to "stdout" can
end up in the log file instead of the pipe. The MCP-side scanner
reading the pipe would never see the `__BPR_DONE` sentinel and would
hit its timeout. `WriteFile(hOut, ...)` writes raw bytes to the
actual pipe handle.

### Sentinel framing

After each op:

```cpp
const FString DoneStr = FString::Printf(TEXT("__BPR_DONE %d__\n"), Code);
```

The MCP server scans the daemon's stdout for the *first* occurrence
of the literal `__BPR_DONE ` and parses the trailing exit code. Any
other appearance of that string anywhere — including inside log
chatter, error messages, or even a help line documenting the
protocol — breaks the next call. Sentence quoted verbatim in
[CLAUDE.md → "Sentinel format"](../../CLAUDE.md).

A `__BPR_READY__\n` sentinel is emitted before the first op so the
MCP server knows the daemon finished initializing.

The daemon also handles a `QUIT` line as a clean shutdown signal,
distinct from "stdin closed" (which exits the same way).

## Live TCP server

`FLiveServer` (`BlueprintReaderLiveServer.cpp`) is the in-editor
counterpart of the commandlet daemon — same op surface, talking to
clients over TCP instead of stdin pipes. Key facts:

### Loopback-only, token-authenticated

`BindAddr->SetIp(0x7F000001);` — explicit 127.0.0.1 bind
(`BlueprintReaderLiveServer.cpp:307-308`). A two-GUID-concatenated
256-bit token gates every connection
(`BlueprintReaderLiveServer.cpp:391-399`). Token can be overridden
via `BP_READER_LIVE_TOKEN`.

### Auto-published handshake

Port + token are written to
`<ProjectDir>/Saved/bp-reader-live.json`
(`BlueprintReaderLiveServer.cpp:501-541`) on startup, deleted on
shutdown. The MCP server's `AutoBlueprintReader` and `BackendFactory`
both watch this file — if it disappears, the live backend is
considered down.

### Per-connection thread; game-thread dispatch

`FLiveConnectionRunnable::Run`
(`BlueprintReaderLiveServer.cpp:70-179`) owns the protocol I/O —
hello, auth, frame loop. Every op message it receives is dispatched
to the game thread via:

```cpp
AsyncTask(ENamedThreads::GameThread, [&Code, Params, DoneEvent]()
{
    Code = RunOneOpFromLiveServer(Params);
    DoneEvent->Trigger();
});
DoneEvent->Wait();
```

Same `RunOneOp` dispatch the commandlet uses; same temp-file-based
JSON return. The connection thread blocks until the game thread
finishes, then writes the result frame.

### Port cache for reconnect after editor restart

If the bind succeeds, the port is cached to
`<ProjectDir>/Saved/bp-reader-live-port.json` (separate file from
the handshake, because the handshake gets deleted on shutdown)
(`BlueprintReaderLiveServer.cpp:543-548`). On next startup the cache
is consulted first; if the cached port is occupied, the listener
falls back to an ephemeral port and overwrites the cache.

## K2 schema connections (why we use `TryCreateConnection`)

The naive way to wire two pins is `UEdGraphPin::MakeLinkTo`. We
don't. We route through `Schema->TryCreateConnection` instead
(`BlueprintReaderCommandlet.cpp:4660-4694`):

```cpp
const UEdGraphSchema* Schema = Graph->GetSchema();
const FPinConnectionResponse Resp = Schema->CanCreateConnection(FromPin, ToPin);
if (Resp.Response == CONNECT_RESPONSE_DISALLOW) { /* error */ }
const bool bMade = Schema->TryCreateConnection(FromPin, ToPin);
```

Two reasons:

1. **Connection conversions.** The schema honors response codes
   `MAKE_WITH_CONVERSION_NODE` (insert a cast), `MAKE_WITH_PROMOTION`
   (widen int → float), `BREAK_OTHERS_A/B/AB` (replace existing
   inputs). `MakeLinkTo` would silently create an invalid graph; the
   schema does what the editor's drag-drop handler does.
2. **`PinConnectionListChanged` callback.** This is what UE 5's
   wildcard-pin propagation hooks into. `UK2Node_CallArrayFunction`,
   `UK2Node_Select`, and friends override this method to walk their
   wildcard slots and bind the concrete type from the newly-connected
   pin. Without this, wiring a typed pin into an array library node's
   `TargetArray` wildcard leaves the wildcard as `wildcard` and the
   blueprint fails to compile with "The type of Target Array is
   undetermined" (issue #11, callsite comment).

## LoadMutableBlueprint and the GC anchor

`LoadMutableBlueprint`
(`BlueprintReaderCommandlet.cpp:416-444`) wraps `LoadObject<UBlueprint>`
with two non-obvious choices:

- `LOAD_NoWarn | LOAD_Quiet` suppresses UE's default LogLinker
  "Failed to load X" warning, so on the failure path the
  agent-facing diagnostic from `DiagnoseFailedBlueprintLoad` is the
  only error in the daemon tail. Otherwise the MCP scanner picks up
  two error lines and the second one is uninformative noise.
- The object path is normalized: callers pass package paths
  (`/Game/AI/BP_Foo`), `LoadObject` wants object paths
  (`/Game/AI/BP_Foo.BP_Foo`). The leading section of
  `LoadMutableBlueprint` appends `.<leaf>` if missing.

On failure we delegate to
`FBlueprintIntrospector::DiagnoseFailedBlueprintLoad`
(`BlueprintIntrospector.cpp:352-410`), which classifies the failure
into one of three buckets:

1. **Not in registry**: caller typed the path wrong.
2. **Wrong asset class**: the asset exists but isn't a `UBlueprint`
   subclass — DataAssets, DataTables, Materials etc. Surfaces a
   clear "bp-reader doesn't handle this asset type" message
   (issue #4).
3. **Parent class missing**: the parent C++ class can't be resolved.
   This usually means the project's editor target isn't built.
   Surfaces a "Rebuild the project (Build.bat ...)" message
   (issue #3).

The same classifier is called from
`FBlueprintIntrospector::Read(const FString&)` so read-path failures
get the same diagnostic — initially only the write path called it
(PR #58 caught the gap).

The GC anchor pattern lives in `CompileAndSaveBlueprint`
(`BlueprintReaderCommandlet.cpp:546`):

```cpp
TStrongObjectPtr<UBlueprint> Anchor(BP);
```

`FKismetEditorUtilities::CompileBlueprint` constructs and destroys
intermediate `UObject`s during recompilation; some of those calls can
mark child `UK2Node` instances as garbage. A GC pass fired by the
compiler could (rarely) collect the BP itself if its outer package's
reference graph has weak links to the world. In daemon mode this is
hard to trigger but real; the anchor is a one-line defense.

## Asset Registry usage

The list path (`RunListOp` —
`BlueprintReaderCommandlet.cpp:4986-5066`) uses
`AR.ScanPathsSynchronous({PathFilter}, /*bForceRescan=*/false)`,
*not* `SearchAllAssets`. This matters:

```cpp
// The previous code did SearchAllAssets(bSync=true) which forces the
// entire project's asset registry to finalize and fires the global
// OnAssetRegistryLoadComplete broadcast. That broadcast invokes
// every plugin's load handler — and one bad handler (e.g. Niagara
// loading a NiagaraDataChannel asset whose post-load constructor
// crashes in some projects) takes the whole commandlet down with
// exit=3 and a long callstack ending in our RunListOp.
```

`ScanPathsSynchronous` only populates the requested path's metadata,
doesn't invoke the global completion broadcast, and doesn't load
asset payloads (only header tags). `bForceRescan=false` makes it a
cheap no-op when the path is already registered from earlier
startup. Same pattern is repeated for the other list-style ops
(DataTables, BTs, Materials, etc. — see references at lines 1275,
2209, 2830, 3046, 3191, 3675).

`ParentClass` for each asset is read from the asset registry tags
without ever loading the BP. The tag value is wrapped in
`Class'/Script/Engine.Actor'` syntax that we unwrap to bare
`/Script/Engine.Actor` so the wire shape matches what
`Blueprint->ParentClass->GetPathName()` would emit
(`BlueprintReaderCommandlet.cpp:5037-5045`).

## File-lock probe via raw `CreateFileW`

When `UPackage::SavePackage` fails, the most common cause is that the
editor is open and holds an exclusive write handle on the `.uasset`
(issue #2). We surface a clear "close the editor or use the live
backend" message instead of an opaque "SavePackage failed". The
probe (`BlueprintReaderCommandlet.cpp:580-617`):

```cpp
HANDLE Probe = ::CreateFileW(*FileName,
    GENERIC_READ | GENERIC_WRITE,
    0,                      // no share — fail if anyone has it open
    nullptr,
    OPEN_EXISTING,          // never create or truncate
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
if (Probe == INVALID_HANDLE_VALUE)
{
    const DWORD Err = ::GetLastError();
    if (Err == ERROR_SHARING_VIOLATION) { /* clear hint */ }
}
```

`OPEN_EXISTING` + `dwShareMode=0` is the non-destructive way to test
"is this file locked by someone else?". The PR's initial draft used
`IPlatformFile::OpenWrite` which truncates the file on success —
turning a non-lock save failure into actual asset corruption.
Codex caught that on PR #59.

## Wire JSON

The plugin emits all wire-shape JSON through one helper module,
`BlueprintReaderWireJson.cpp`. The two non-obvious conventions:

- **`SetStringOrNull`** for optional strings
  (`BlueprintReaderWireJson.cpp:32-42`): empty `FString` becomes JSON
  `null`, non-empty becomes a JSON string. This distinguishes "not
  applicable" from "the empty string", which the MCP server's
  `BPROptionalString` (`std::optional<std::string>`) preserves.
- **`ToPackagePath`** for asset paths
  (`BlueprintReaderWireJson.cpp:14-24`): strip the `.<leaf>` object
  suffix consistently so every asset path on the wire is a package
  path. New wire shapes that reference assets must call this helper
  or recreate it.

## Construction script naming

UE 5.7's actual graph name for the construction script is
`UserConstructionScript`, not `ConstructionScript`. The introspector
(`BlueprintIntrospector.cpp:464-468`) classifies both names as
`WireType="Construction"` so callers don't have to know which
internal name the engine version uses:

```cpp
const bool bIsConstruction =
    G.Name.Equals(TEXT("ConstructionScript"), ESearchCase::IgnoreCase) ||
    G.Name.Equals(TEXT("UserConstructionScript"), ESearchCase::IgnoreCase);
G.WireType = bIsConstruction ? TEXT("Construction") : TEXT("Function");
```

## See also

- [02-architecture.md](02-architecture.md) — how the MCP server
  reaches `RunOneOp`.
- [04-mcp-server.md](04-mcp-server.md) — the MCP server's side of
  every protocol described here (daemon spawn, live socket, file
  framing).
- [CLAUDE.md → "Common gotchas"](../../CLAUDE.md) — the history
  behind half of these design choices.
- [`BlueprintReaderCommandlet.cpp`](../../Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintReaderCommandlet.cpp)
  — 5400-line single TU. Don't be intimidated; it's organized by
  feature area with section comments.
