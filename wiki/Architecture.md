# Architecture (UML)

A structural map of the **whole system** — both halves of the plugin:

1. **`bp-reader-mcp.exe`** — a standalone, out-of-process C++20 MCP
   server (a UE *Program* target, but it links no engine runtime). Speaks
   JSON-RPC 2.0 to MCP clients and drives the editor over a subprocess or
   socket.
2. **`BlueprintReader` UE plugin** — two modules loaded *inside* the UE
   process: `BlueprintReaderEditor` (full introspection + mutation, editor
   only) and `BlueprintReaderRuntime` (read-only reflection, ships in
   cooked builds).

The hard line between them is a **process boundary**: the server never
links UE; the plugin never links nlohmann/json. They meet over a wire
protocol — CreateProcessW + stdin/stdout for `commandlet`, newline-JSON
over loopback TCP for `live`.

> These diagrams are [Mermaid](https://mermaid.js.org/); GitHub renders
> them inline. Source of truth is the headers under
> `Plugins/BlueprintReader/` — see the [file map](#source-map) at the end.

---

## 1. System component view

How a request flows from an MCP client to UE reflection and back, across
the process boundary, for each backend.

```mermaid
flowchart LR
    subgraph clients["MCP clients"]
        direction TB
        CC["Claude Code / Desktop"]
        CP["GitHub Copilot (VS Code)"]
        GP["ChatGPT (HTTPS bridge)"]
    end

    subgraph proc_server["Process A - bp-reader-mcp.exe (no UE linkage)"]
        direction TB
        MAIN["main.cpp<br/>transport + backend select"]
        TRANSPORT{"transport"}
        STDIO["Server.Run<br/>stdio framing"]
        HTTP["HttpServerMain<br/>HTTP + chunked SSE"]
        MCP["mcp::RegisterHandlers<br/>initialize / tools.* / ping"]
        REG["ToolRegistry<br/>~249 ToolDescriptors"]
        BTOOLS["RegisterBlueprintTools<br/>handlers"]
        CHAIN["IBlueprintReader chain<br/>ReadOnly to Caching to Auto"]
        MAIN --> TRANSPORT
        TRANSPORT -->|default| STDIO
        TRANSPORT -->|BP_READER_HTTP_PORT| HTTP
        STDIO --> MCP
        HTTP --> MCP
        MCP --> REG
        BTOOLS --> REG
        BTOOLS --> CHAIN
    end

    subgraph proc_editor["Process B - UnrealEditor-Cmd.exe + BlueprintReaderEditor.dll"]
        direction TB
        DAEMON["UBPRCommandlet<br/>-run=BPR (-Daemon)"]
        LIVE["FLiveServer<br/>loopback TCP listener"]
        RUNONE["RunOneOp dispatch<br/>EOp table"]
        INTRO["FBlueprintIntrospector"]
        WIRE["FBlueprintReaderWireJson"]
        DAEMON --> RUNONE
        LIVE --> RUNONE
        RUNONE --> INTRO
        RUNONE --> WIRE
    end

    subgraph proc_cooked["Process C - packaged game + BlueprintReaderRuntime.dll (opt-in)"]
        direction TB
        RTSRV["FRuntimeServer<br/>loopback TCP (off by default)"]
        RTINTRO["FBlueprintRuntimeIntrospector"]
        RTSRV --> RTINTRO
    end

    UEREFL["UE - UClass reflection, Asset Registry, K2 graphs, CDO"]

    clients -->|"JSON-RPC 2.0 over stdio or HTTP"| MAIN
    CHAIN -->|"CreateProcessW + stdin pipe (commandlet)"| DAEMON
    CHAIN -->|"newline-JSON over TCP (live)"| LIVE
    CHAIN -.->|"newline-JSON over TCP (live to cooked)"| RTSRV
    INTRO --> UEREFL
    RTINTRO --> UEREFL
```

**Auto** (the default) re-probes on every call with a 2 s cache: if the
editor's `FLiveServer` handshake file is present it routes to `live`,
otherwise it spawns/feeds the `commandlet` daemon.

---

## 2. MCP server core (JSON-RPC + tools)

The transport-agnostic core. `Server` knows only JSON-RPC framing +
method handlers; `mcp::RegisterHandlers` layers the MCP protocol on top;
`ToolRegistry` holds the tool surface. The MCP layer never interprets
tool semantics — it only knows the registry.

```mermaid
classDiagram
    class Server {
        +RegisterMethod(name, handler)
        +Run(in, out)
        +Dispatch(request) json
        +QueueNotification(method, params)
        +TakePendingNotifications() vector
        +RegisterSseSession() SseSessionId
        +UnregisterSseSession(id)
        +TakeSseSessionNotifications(id) vector
        +RegisterInFlight(ctx)
        +FindInFlight(requestId) CallContext
        -handlers_
        -pendingNotifications_
        -sseSessions_
    }
    class Mcp {
        <<namespace>>
        +RegisterHandlers(server, registry, prompts, logger, resources, info, editorSubs)$
        +DefaultInstructions() string$
    }
    class ServerInfo {
        +string name
        +string protocolVersion
        +string instructions
    }
    class ToolRegistry {
        +Add(desc, fn)
        +ListSpec() json
        +Find(name) ToolFn
        +FindAny(name) ToolFn
        +ApplyFilter(allow, deny)
        +ActivateToken(token) vector
        +TakeListChangedFlag() bool
        -descriptors_
        -fns_
        -active_
    }
    class ToolDescriptor {
        +string name
        +string description
        +json input_schema
        +json output_schema
        +ToolAnnotations annotations
    }
    class ToolAnnotations {
        +read_only_hint
        +destructive_hint
        +idempotent_hint
        +open_world_hint
    }
    class CallContext {
        +EmitProgress(progress, total, msg)
        +IsCancelled() bool
        +MarkCancelled()
        +Current() CallContext$
        +Scope
    }
    class EditorSubscriptions {
        +Subscribe(types) id
        +Unsubscribe(id) bool
        +IsSubscribed(type) bool
        +Count() size_t
    }
    class HttpTransport {
        <<namespace>>
        +ParseRequest(raw) HttpRequest
        +FormatResponse(resp) string
        +IsOriginAllowed(origin) bool
        +Handle(req, server, mcpPath) HttpResponse
    }
    class HttpServerMain {
        <<namespace>>
        +RunHttpServer(server, port, mcpPath, log, reader, editorSubs)$
    }
    class PromptRegistry
    class Logger
    class ResourceRegistry

    Mcp ..> Server : registers handlers on
    Mcp ..> ToolRegistry : binds tools/list, tools/call
    Mcp ..> EditorSubscriptions : editor/subscribe (Phase 10)
    Mcp ..> PromptRegistry
    Mcp ..> Logger
    Mcp ..> ResourceRegistry
    Mcp ..> ServerInfo
    ToolRegistry "1" *-- "many" ToolDescriptor
    ToolDescriptor *-- ToolAnnotations
    CallContext ..> Server : queues progress
    HttpServerMain ..> Server : per-session SSE drain
    HttpServerMain ..> HttpTransport
    HttpServerMain ..> EditorSubscriptions
```

`CallContext` is thread-local ambient state set around each `tools/call`
so long-running tools (cook, package, automation) can `EmitProgress()`
and poll `IsCancelled()` without changing the `ToolFn` signature.
`ToolRegistry` supports both a static allow/deny filter and runtime
progressive disclosure (the `enable_tool_category` meta-tool sets
`listChanged_`, which the dispatcher turns into
`notifications/tools/list_changed`).

---

## 3. Backend chain (the decorator stack)

Every tool handler calls one `IBlueprintReader`. The concrete object is a
**decorator stack** assembled by `BackendFactory::Create`:

`ReadOnly` → `Caching` → `Auto` → ( `Socket`(live) | `Commandlet` | `Mock` )

Each layer adds one concern: `ReadOnly` rejects write tools, `Caching`
memoizes reads on a TTL, `Auto` probes per call and forwards to the live
or commandlet leaf.

```mermaid
classDiagram
    class IBlueprintReader {
        <<interface>>
        +ListBlueprints(path) ListResult
        +ReadBlueprint(path) BlueprintInfo
        +AddVariable(args) Result
        +AddNode(args) Result
        +DecompileFunction(args) Bpir
        +TranspileFunction(args) Source
        +ParseCppFunction(args) Bpir
        +GetEditorEvents() EditorEventsResult
        +GetUiStateStub(feature) UiStateStubResult
        +GetActiveCookTarget() CookTargetResult
        +GetTraceState() TraceStateResult
        +GetWorkspaceLayout() WorkspaceLayoutResult
    }
    class ReadOnlyBlueprintReader {
        -IBlueprintReader inner_
    }
    class CachingBlueprintReader {
        -IBlueprintReader inner_
        -int ttlSeconds
    }
    class AutoBlueprintReader {
        -SocketBlueprintReader live_
        -CommandletBlueprintReader commandlet_
        +probe per call, 2s cache
    }
    class MockBlueprintReader {
        +fixtures only, no UE
    }
    class CommandletBlueprintReader {
        +RunOp(wargs) json
    }
    class SocketBlueprintReader {
        +RunOp(args) json
    }

    IBlueprintReader <|.. ReadOnlyBlueprintReader
    IBlueprintReader <|.. CachingBlueprintReader
    IBlueprintReader <|.. AutoBlueprintReader
    IBlueprintReader <|.. MockBlueprintReader
    IBlueprintReader <|.. CommandletBlueprintReader
    IBlueprintReader <|.. SocketBlueprintReader

    ReadOnlyBlueprintReader o-- IBlueprintReader : wraps
    CachingBlueprintReader o-- IBlueprintReader : wraps
    AutoBlueprintReader o-- SocketBlueprintReader : live route
    AutoBlueprintReader o-- CommandletBlueprintReader : commandlet route
```

```mermaid
classDiagram
    class BackendFactory {
        <<namespace>>
        +ConfigFromEnv(exeDir, log) BackendConfig$
        +Create(cfg) IBlueprintReader$
    }
    class BackendConfig {
        +string backend
        +path engineDir
        +path uproject
        +bool useDaemon
        +bool prewarm
        +int cacheTtlSeconds
        +bool readOnly
        +string liveHost
        +int liveProcPort
        +string liveToken
    }
    BackendFactory ..> BackendConfig : reads BP_READER_*
    BackendFactory ..> IBlueprintReader : assembles stack
```

`IBlueprintReader` carries roughly one virtual per tool family (~240).
`AutoBlueprintReader` forwards every op through a `FORWARD` macro, so new
tools are picked up automatically. Adding one tool touches all of:
`IBlueprintReader` (virtual) → Mock/Commandlet/Socket/Caching/ReadOnly/Auto
(impl) → `BlueprintTools.cpp` (descriptor + handler) → plugin `RunXxxOp`.

---

## 4. BPIR — the BP to source pivot

`decompile_function`, `transpile_function`, and `parse_cpp_function` all
operate on **BPIR**, a versioned JSON AST. Adding Lua/Python/JS later is
another `codegen + parse` pair against the same IR.

```mermaid
flowchart LR
    BP["Blueprint K2 graph"]
    BPIR["BPIR (tools/Bpir.h)<br/>versioned JSON AST"]
    CPP["C++ .h / .cpp"]

    BP -->|"decompile_function (plugin reads graph)"| BPIR
    BPIR -->|"transpile_function (codegen CppEmit / CppClassEmit)"| CPP
    CPP -->|"parse_cpp_function (parse CppLex to CppParse)"| BPIR
    BPIR -->|"compile_function (plugin materializes graph)"| BP
```

```mermaid
classDiagram
    class Bpir {
        <<JSON AST>>
        +version
        +nodes
        +pins
        +links
        +types
    }
    class CppEmit {
        +emit(bpir) cpp
    }
    class CppClassEmit {
        +emitClass(bpir) cpp
    }
    class UnsupportedTreatment {
        +degrade non-representable nodes
    }
    class CppLex {
        +tokenize(cpp) tokens
    }
    class CppParse {
        +parse(tokens) bpir
    }
    class Decompile {
        +decompile(graphJson) bpir
    }
    Decompile ..> Bpir
    CppEmit ..> Bpir
    CppClassEmit ..> CppEmit
    CppEmit ..> UnsupportedTreatment
    CppParse ..> CppLex
    CppParse ..> Bpir
```

---

## 5. UE plugin — editor module (`BlueprintReaderEditor`)

Editor-only (`Type=Editor`, stripped from non-editor targets by UBT).
Full introspection + mutation. Two entry points share one dispatch
(`RunOneOp` over the `EOp` table): the commandlet daemon (stdin lines)
and `FLiveServer` (loopback TCP frames, dispatched on the game thread).

```mermaid
classDiagram
    class UBPRCommandlet {
        <<UCommandlet>>
        +Main(params) int32
    }
    class FLiveServer {
        +Start(port) bool
        +Stop()
        +GetListenPort() int32
        -OnIncomingConnection(socket, endpoint) bool
        -WriteHandshakeFile() bool
    }
    class UBPRSeedCommandlet {
        <<UCommandlet>>
        +Main(params) int32
    }
    class FBlueprintIntrospector {
        +Read(assetPath) FBlueprintInfo$
        +Read(blueprint) FBlueprintInfo$
        +FormatPinType(type) string$
        +DiagnoseFailedBlueprintLoad(path)$
        -ReadGraph(graph) FBPGraphInfo$
    }
    class FBlueprintReaderWireJson {
        +ToPackagePath(objectPath) string$
        +WriteString(json, pretty) string$
    }
    class FBlueprintStructuralDiff {
        +structural_diff support
    }
    class UBlueprintReaderSettings {
        <<UDeveloperSettings>>
        +port
        +autostart
        +allow / block patterns
    }
    class FBlueprintReaderLogSink {
        +capture editor log
    }

    UBPRCommandlet ..> FBlueprintIntrospector : reads
    UBPRCommandlet ..> FBlueprintReaderWireJson : serializes
    UBPRCommandlet ..> FBlueprintStructuralDiff
    FLiveServer ..> UBPRCommandlet : dispatches RunOneOp
    UBPRSeedCommandlet ..> FBlueprintReaderWireJson
```

`RunOneOp` is a free-function dispatch table (`EOp` enum →
`RunReadOp` / `RunAddVariableOp` / `RunGetEditorEventsOp` / …). Lazy
editor-delegate subscriptions (`EnsureEditorEventsSubscribed`) push 13
Tier-A + Tier-B/C debounced events into a ring buffer drained by
`get_editor_events` — see [§8](#8-sequence--ea-push-events-phase-1015).

---

## 6. UE plugin — runtime module (`BlueprintReaderRuntime`)

`Type=Runtime` — loads in editor **and** packaged builds. Read-only
`UClass` reflection (asset registry, parent chain, UPROPERTY vars with
CDO defaults, UFUNCTION signatures, SCS/CDO components). Cannot read K2
graphs (stripped during cook → `graphs[]` empty).

```mermaid
classDiagram
    class FBlueprintRuntimeIntrospector {
        +ListBlueprints(pathFilter) vector~FBPRRAssetSummary~$
        +Read(assetPath) FBPRRBlueprint$
        +ResolveClass(assetPath) UClass$
        +PropertyTypeShorthand(prop) string$
    }
    class FRuntimeServer {
        +Start(port) bool
        +Stop()
        +IsListening() bool
    }
    class FBPRRBlueprint {
        +string AssetPath
        +string Name
        +string ParentClassPath
        +Interfaces
        +Variables
        +Components
        +Functions
    }
    class FBPRRVariable {
        +string Name
        +string TypeShorthand
        +string DefaultValue
        +bool bIsReplicated
    }
    class FBPRRFunction {
        +string Name
        +Inputs
        +Outputs
        +bool bIsBlueprintCallable
    }
    class FBPRRComponent {
        +string Name
        +string ClassPath
        +bool bIsRoot
    }

    FRuntimeServer ..> FBlueprintRuntimeIntrospector : dispatches List/Read
    FBlueprintRuntimeIntrospector ..> FBPRRBlueprint : produces
    FBPRRBlueprint *-- FBPRRVariable
    FBPRRBlueprint *-- FBPRRFunction
    FBPRRBlueprint *-- FBPRRComponent
```

`FRuntimeServer` is **off by default** (a shipping game should not open a
port silently); opt in with the `bp.reader.listen` CVar or
`BP_READER_RUNTIME_LISTEN=1`. It speaks the same wire protocol as
`FLiveServer`, so the MCP server's `live` backend works against either.
Two console commands give in-game triage: `bp_reader.list <Path>` and
`bp_reader.read <AssetPath>`.

---

## 7. Sequence — a `tools/call` end-to-end

`read_blueprint` through the default `auto` backend, showing both routes.

```mermaid
sequenceDiagram
    autonumber
    participant Client as MCP client
    participant Srv as Server + Mcp
    participant Reg as ToolRegistry
    participant Chain as ReadOnly-Caching-Auto
    participant Live as FLiveServer (editor)
    participant Cmd as UBPRCommandlet daemon
    participant UE as UE reflection

    Client->>Srv: tools/call read_blueprint {path}
    Srv->>Reg: Find("read_blueprint")
    Reg-->>Srv: ToolFn
    Srv->>Chain: ReadBlueprint(path)
    Note over Chain: ReadOnly passes read<br/>Caching TTL miss to inner<br/>Auto probes handshake (2s cache)
    alt editor open (handshake present)
        Chain->>Live: TCP op -Op=Read
        Live->>UE: RunOneOp to FBlueprintIntrospector.Read
        UE-->>Live: FBlueprintInfo
        Live-->>Chain: result json
    else editor closed
        Chain->>Cmd: stdin -Op=Read -Asset=... -Out=...
        Cmd->>UE: RunOneOp to FBlueprintIntrospector.Read
        UE-->>Cmd: FBlueprintInfo
        Cmd-->>Chain: stdout json + done sentinel
    end
    Chain-->>Srv: BlueprintInfo (snake_case wire JSON)
    Srv-->>Client: content text json, isError false, _meta elapsed_ms
```

---

## 8. Sequence — EA-push events (Phase 10/15)

User actions in a live editor become `notifications/editor/*` over the
HTTP SSE stream, fanned out per session.

```mermaid
sequenceDiagram
    autonumber
    participant UE as UE editor delegates
    participant Buf as Editor event ring buffer
    participant Cmd as RunGetEditorEventsOp
    participant SSE as HttpServerMain StreamSse
    participant Srv as Server (per-session queues)
    participant Client as SSE client(s)

    Note over UE,Buf: EnsureEditorEventsSubscribed (lazy, once)
    UE->>Buf: actor_added / blueprint_compiled / camera_moved (debounced)
    loop every ~2s under server mutex
        SSE->>Cmd: GetEditorEvents drains buffer once
        Cmd-->>SSE: events
        SSE->>Srv: QueueNotification notifications/editor/<name>
        Note over Srv: fan-out copies to every registered SSE session
    end
    loop SSE poll 250ms
        SSE->>Srv: TakeSseSessionNotifications(sessionId)
        Srv-->>SSE: this session notifications
        SSE-->>Client: chunked SSE frame
    end
```

`editor/subscribe` (advertised only when `BP_READER_PUSH_EVENTS=1`)
registers interest in event types via `EditorSubscriptions`; the poll
skips unsubscribed events. The per-session fan-out means two connected
clients each get their own copy — neither steals the other's.

---

## Source map

| Diagram | Source of truth |
|---|---|
| Server core (§2) | `Tests/BlueprintReaderMcpCore/Private/jsonrpc/{Server,Mcp,CallContext,HttpServerMain,HttpTransport,SseFrame}.h` |
| Tools (§2) | `…/Private/tools/{ToolRegistry,BlueprintTools,EditorSubscriptions,ToolAnnotations}.h` |
| Backends (§3) | `…/Private/backends/{IBlueprintReader,ReadOnly,Caching,Auto,Commandlet,Socket,Mock}*.h`, `BackendFactory.h` |
| BPIR (§4) | `…/Private/tools/{Bpir,Decompile}.h`, `…/tools/codegen/*`, `…/tools/parse/*` |
| Editor module (§5) | `Source/BlueprintReaderEditor/Public/*.h` + `Private/BlueprintReaderCommandlet.cpp` (`RunOneOp` / `EOp`) |
| Runtime module (§6) | `Source/BlueprintReaderRuntime/Public/{BlueprintRuntimeIntrospector,BlueprintReaderRuntimeServer}.h` |

The 7-file pattern for adding a tool, build/test commands, and gotchas
live in [`CLAUDE.md`](https://github.com/defessler/Unreal-Engine-5-MCP/blob/main/CLAUDE.md);
tool usage is in [Tool Reference](Tool-Reference); the BP-to-C++ IR is in
[BPIR](BPIR).
