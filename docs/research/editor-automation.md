# Editor automation levers in Unreal Engine 5.7

Audience: a maintainer of `bp-reader` (or a similar UE5 tooling MCP
server) deciding whether to grow new automation backends past the
commandlet-daemon + in-editor TCP socket combo that ships today.

Scope: every general-purpose lever the engine offers to drive the
editor without a human hands on the keyboard. Trade-offs at the end
(startup cost, parallelism, editor-required, scripting language, AI-
agent fit).

Notation: file paths are absolute under either the engine checkout
(`D:/Projects/Unreal Engine 5/Engine/...`) or the project
(`D:/Projects/UE5_MCP/...`). When citing engine API names I prefix
the originating module so a reader can grep from the engine root.

WebFetch / WebSearch were not available during the writing of this
note. Doc-page details that depend on Epic's web docs are marked
"Not verified — see TODO" inline where they apply.

---

## 1. Commandlets (`UCommandlet`)

### What it is

A commandlet is a UObject-derived class with a single `Main(const
FString& Params)` entry point. The engine launches it via
`UnrealEditor-Cmd.exe <Project> -run=<CommandletName> [args...]` and
exits when `Main` returns. The base class lives at
`D:/Projects/Unreal Engine 5/Engine/Source/Runtime/Engine/Classes/Commandlets/Commandlet.h`
(`UCommandlet`). The CDO holds boolean flags that gate engine
subsystem startup: `IsServer`, `IsClient`, `IsEditor`, `LogToConsole`,
`ShowErrorCount`, `FastExit`, etc.

The launcher resolves `-run=Foo` by matching against `UClass` names
with or without the `U` prefix and the `Commandlet` suffix. Built-in
commandlets (cook, derived-data, asset-registry, world-partition
helpers) live at
`D:/Projects/Unreal Engine 5/Engine/Source/Editor/UnrealEd/Public/Commandlets/`
(`CompileAllBlueprintsCommandlet`, `ImportAssetsCommandlet`,
`AssetRegistryGenerator`, etc.).

### Invocation patterns

- One-shot, headless: `UnrealEditor-Cmd.exe <uproject>
  -run=BPRSeed -nullrhi -nosplash -unattended -nopause`.
  `-nullrhi` skips RHI init (no GPU, no Slate); `-nosplash` skips the
  splash window; `-unattended` disables modal prompts; `-nopause`
  prevents the "press any key to exit" stall.
- "Driven from an external process": same launcher, parse `Main`'s
  `Params` via `FParse::Value` / `FParse::Bool` / `FParse::Param`,
  emit output to a file or stdout, exit. This is what
  `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintReaderCommandlet.cpp`
  does in single-op mode.
- Daemon mode: one persistent commandlet process services many
  requests so the per-call cost of engine startup is paid once. The
  loop reads work items, dispatches to the same shared handler the
  one-shot path uses, and writes results back.

### `FParse` traps

`FParse::Value(*Params, TEXT("Foo="), Out)` is the standard way to
pull a string flag from the commandline. Two well-known caveats:

- Embedded double-quotes in the value terminate the parser early —
  passing structured JSON as `-Args=<json>` mangles it. Pass each
  field as its own flag (`-TypeCategory=`, `-TypeSubCategory=`, ...)
  or base64-encode the payload.
- An empty value on a bare `-Foo=` followed by `-Bar=value` makes
  `FParse` swallow the next token as `Foo`'s value. The
  `CommandletBlueprintReader` client in
  `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/backends/CommandletBlueprintReader.cpp`
  skips empty optional flags to avoid this.

### How `bp-reader` uses commandlets today

Two commandlets ship in the plugin:

- `UBPRCommandlet` (`-run=BPR`), at
  `D:/Projects/UE5_MCP/Plugins/BlueprintReader/Source/BlueprintReaderEditor/Public/BlueprintReaderCommandlet.h`.
  Sets `IsEditor=true`, `LogToConsole=true`, `ShowErrorCount=false`
  in its CDO. `Main(Params)` dispatches: if the params contain
  `-Daemon`, it calls `RunDaemon()`; otherwise it calls `RunOneOp`.
  `RunOneOp` parses `-Op=Read|Graph|Function|AddVariable|...`
  (90+ enum values; see `EOp` in `BlueprintReaderCommandlet.cpp`)
  and dispatches to per-op handlers via a static table.
- `UBPRSeedCommandlet` (`-run=BPRSeed`), at
  `BlueprintReaderSeedCommandlet.h`. Generates the two test
  blueprints (`Content/AI/BP_TestEnemy.uasset`,
  `BP_TestPickup.uasset`) the live integration tests depend on.

The daemon path in `RunDaemon` used to read newline-delimited
commandlet-arg lines from stdin and write `__BPR_DONE <code>__` as
a sentinel back to stdout. The current implementation replaces
stdin/stdout with `FCmdletServer` — a TCP listener that speaks the
exact same JSON wire protocol the in-editor `FLiveServer` does (see
`BlueprintReaderCmdletServer.h` and `.cpp` for the full protocol /
auth / handshake-file design). The dispatcher
(`RunOneOpFromLiveServer`) is shared between both server types and
dispatches all UObject mutation via
`AsyncTask(ENamedThreads::GameThread, ...)` so the per-connection
worker threads stay off the game thread.

The `RunDaemon` body in `BlueprintReaderCommandlet.cpp:6626` also
hand-pumps the game-thread task queue (`ProcessThreadUntilIdle`) and
the core ticker — a normal editor session pumps these for you, a
commandlet does not. Missing this dispatch step manifests as every
op call hanging until the client times out.

### Limitations

- No Slate UI. The renderer is not initialized under `-nullrhi`;
  even without `-nullrhi`, no editor window is created. Anything that
  reaches `FSlateApplication::Get()` and expects an active main
  window will assert or no-op. Asset editors (`UAssetEditorSubsystem`)
  technically exist as UObjects but their toolkits are headless and
  must be driven entirely via API, not via menu actions.
- No PIE worldscape. Commandlets do not bring up a `UWorld` with a
  game mode unless you build one yourself (`UEditorEngine::PlayWorld`
  is editor-Engine state, not commandlet state).
- Cold start is expensive. A fresh commandlet launch pays full
  engine-DLL load + module init + asset-registry mount + plugin load.
  On this machine that is ~15–30 s before any op runs. Daemon mode
  amortizes this across N requests.
- One commandlet process per project. `bp-reader`'s
  `FCmdletServer::AcquireLifetimeLock` is the explicit guard:
  competing daemons against the same `<Project>/Saved/` directory
  collide on asset registry / DDC / `.uasset` files. The lock is
  Windows-only today; on POSIX it falls back to a presence check.
- The launcher is single-threaded — `Main` runs on the main thread.
  Parallelism within a commandlet is "use the task graph"; you do
  not get parallelism by spawning multiple commandlet processes.

### Console-control + signal handling

Win32's console-control handler runs on a dedicated handler thread
the OS injects into the process. Calling `FCmdletServer::Stop()` from
there risks deadlock if any of the listener/connection threads is
blocked on a syscall the handler is racing. The daemon installs a
handler that only sets `bShuttingDown` and lets the main-thread
polling loop notice and drive teardown. There's a documented
non-shipping-x86 wrinkle in `BlueprintReaderCommandlet.cpp:6661`:
UE patches `SetConsoleCtrlHandler` to prevent third-party handlers
from registering, so the user handler never fires; the OS reaps
handles on `TerminateProcess` but the handshake / lifetime-lock files
leak.

---

## 2. Python (`PythonScriptPlugin`)

### What it is

An embedded CPython interpreter that exposes the entire reflected UE
API as a `unreal` Python module. The plugin lives at
`D:/Projects/Unreal Engine 5/Engine/Plugins/Experimental/PythonScriptPlugin/`;
the public API surface is at
`D:/Projects/Unreal Engine 5/Engine/Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Public/`:

- `IPythonScriptPlugin.h` — module interface; the only entry point
  C++ callers need. Exposes `ExecPythonCommand`,
  `ExecPythonCommandEx`, `IsPythonAvailable`.
- `PythonScriptTypes.h` — `FPythonCommandEx` (the structured-input
  type the `Ex` variant takes), `EPythonCommandExecutionMode`
  (`ExecuteFile` vs `ExecuteStatement` vs `EvaluateStatement`),
  `EPythonCommandFlags` (`Unattended`, `NoLogging`, `CaptureOutput`).
- `IPipInstall.h` — pip-install hook for third-party Python deps.
- `PipInstallHelpers.h` — wrappers around the pip-install machinery.

### Entry points

- Editor menu: `Tools → Run Python Script` (file picker).
- Console: `py <expr-or-statement>` in the output log.
- Commandline: `UnrealEditor.exe <Project>
  -ExecutePythonScript=<absolute-path-to-py-file>`. Triggers
  `IPythonScriptPlugin::Get().ExecPythonCommand` after engine init,
  then exits.
- Editor Utility Widget: a Blueprint event can call
  `ExecuteEditorUtilityWidgetScript` or use the `Python Script`
  function library. (Not verified — see TODO; needs docs round-trip.)
- C++: `IPythonScriptPlugin::Get().ExecPythonCommandEx(FPythonCommandEx&)`.
  Returns a bool for success; `LogOutput` array on the input struct
  captures stdout/stderr lines when `CaptureOutput` flag is set.

### How `bp-reader` uses it

The `run_python_script` MCP tool is gated by env var
`BP_READER_ALLOW_PYTHON=1` (default off). When enabled, it routes
through `RunRunPythonScriptOp` in
`BlueprintReaderCommandlet.cpp:5957`. The handler:

1. Reads `Code` from `-Code=` flag.
2. Wraps execution in a `GEditor->BeginTransaction` /
   `EndTransaction` RAII scope so all mutations land as one undo
   unit. Mirror of Epic AIAssistant's pattern.
3. Builds an `FPythonCommandEx` with
   `ExecutionMode=ExecuteFile` and `Flags |= Unattended`.
4. Calls `IPythonScriptPlugin::Get().ExecPythonCommandEx(PyCmd)`.
5. Serializes `PyCmd.LogOutput` (per-entry: type, output text) to
   JSON and returns it.

The Build.cs entry at
`D:/Projects/UE5_MCP/Plugins/BlueprintReader/Source/BlueprintReaderEditor/BlueprintReaderEditor.Build.cs:98`
adds `PythonScriptPlugin` as a private dependency.

### Pros

- Rapid iteration. No C++ recompile, no editor restart — paste a
  Python snippet, watch it run against the live `unreal.*` graph.
- Largest pre-built API surface of any scripting lever — every
  reflected `UFUNCTION` / `UPROPERTY` is reachable.
- First-class library ecosystem via pip (with the caveats below).
- Same execution context as the editor: full access to the asset
  editor subsystem, content browser, asset tools, undo stack.
- Good fit for "scripted multi-step refactor" workflows where the
  exact sequence is computed at runtime from inspection.

### Cons

- Interpreter cost. Each `ExecPythonCommandEx` call re-parses the
  passed source. There is no compile-cache; long scripts pay
  per-call. (Module imports do cache between calls within one editor
  session.)
- Single interpreter, single GIL. There is one CPython interpreter
  per editor process, on the game thread. Scripts cannot fan out;
  any "parallelism" in a Python script is cooperative via UE async
  tasks, not free-threading.
- Distribution requires PythonScriptPlugin enabled. Marked
  Experimental in mainline UE5 — does ship with the engine but is
  not on by default in some `.uproject` configurations.
- Pip install path is per-project and writes to
  `<Project>/Saved/Python/`. Cross-machine reproducibility is the
  user's problem.
- Security. Arbitrary Python in the editor = arbitrary editor
  mutation. The `bp-reader` default-off gate (`BP_READER_ALLOW_PYTHON`)
  is the right shape — the curated MCP tool surface offers a
  reviewable boundary; opening Python re-exposes everything.

### Variants

`-ExecutePythonScript=<path>` is a launcher convenience that runs a
single `.py` file at startup and exits. Same engine startup cost as
any commandlet; no daemon mode (you would have to write the Python
side of a daemon yourself using `tcp` from inside the script).

---

## 3. Editor Utility Widgets / Blueprints / Actors

The Blutility module (`D:/Projects/Unreal Engine 5/Engine/Source/Editor/Blutility/`)
ships three Blueprint-driven authoring surfaces for editor automation
that designers and TDs (not just programmers) can build.

### Editor Utility Widget

Parent class: `UEditorUtilityWidget` in
`D:/Projects/Unreal Engine 5/Engine/Source/Editor/Blutility/Classes/EditorUtilityWidget.h`
(Read denied during research — class derived from `UUserWidget` per
the standard pattern; not verified — see TODO for surface details).

UI is authored in UMG inside the editor itself. The widget is a
`.uasset` (`UEditorUtilityWidgetBlueprint`); the workflow is:

1. Content Browser → right-click → Editor Utilities → Editor Utility
   Widget. Pick a parent class (`EditorUtilityWidget`).
2. Open in the UMG designer; assemble buttons, lists, text inputs.
3. Wire `OnClicked` events to Blueprint logic that calls Editor
   Scripting libraries (`UEditorAssetLibrary`,
   `UEditorLevelLibrary`, `UEditorAssetSubsystem`, etc.).
4. Run from the content browser → docks as an editor tab.

`Run On Startup` flag is honored by the editor utility subsystem on
project load — handy for "always show this panel."

### Editor Utility Blueprint (EUB)

Parent class: `UEditorUtilityBlueprint`. No UI — a function library
that the editor surfaces in `Tools → Editor Utilities` and that you
invoke from the content browser via `Run Editor Utility Blueprint`.
Useful for "one-shot batch script with no parameters" — the UMG
overhead of an EUW is unnecessary when the operation is just
"button-press → execute."

### Editor Utility Actor

Parent class: `AEditorUtilityActor`. A scene-resident actor that
ticks in the editor (`bTickInEditor=true`) and can react to
selection / placement events. Use case: viewport overlays, gizmo
extensions, level-design helpers.

### When to prefer over commandlets

- The work is interactive and designer-driven. Commandlets do not
  ship UI; EUW does.
- The work runs inside an already-open editor session. No cold-start
  cost — same process, same loaded assets.
- The author is a designer / TD comfortable in Blueprint but not in
  C++ or Python.
- The user wants the operation persistently available in the
  editor's chrome (docked tab, menu entry).

### When commandlets win

- Headless CI. EUW requires editor + Slate + UMG runtime.
- The work needs to fan out across many isolated processes (cook,
  bulk-asset-import).
- The work is invoked from outside the editor (CI pipeline, MCP
  server, shell script).

### Relationship to `bp-reader`

`bp-reader` is process-external and does not author EUWs. It could
co-exist with one: a user could build an EUW that triggers
`bp-reader` MCP calls over the live TCP socket (using the same wire
protocol the MCP server uses, since the in-editor `FLiveServer`
is just a TCP listener). Not implemented; not planned.

---

## 4. ScriptableTool / InteractiveToolsFramework (ITF)

### What it is

The InteractiveToolsFramework is the C++-level base layer for
modal editor tools — the framework that backs Modeling Mode, Mesh
Editing, the landscape sculpt tools, etc. It's an `EdMode` + `Tool`
+ `Builder` pattern: an `UEdMode` hosts a set of tool builders;
selecting one activates a tool instance (`UInteractiveTool`) that
intercepts viewport input, draws preview gizmos, and writes its
result on `Shutdown(EToolShutdownType::Accept)`.

ScriptableTool (plugin at
`D:/Projects/Unreal Engine 5/Engine/Plugins/Runtime/ScriptableToolsFramework/`
plus editor-mode wrapper at
`D:/Projects/Unreal Engine 5/Engine/Plugins/Editor/ScriptableToolsEditorMode/`)
is a Blueprint/Python wrapper around ITF: write a `UScriptableTool`
subclass in Blueprint, register it with the Scriptable Tools editor
mode, get the same modal + gizmo + click-target lifecycle the
native C++ tools have.

### When ITF fits

- The tool needs a custom viewport interaction model (drag to size,
  click to place, hover-preview before commit).
- The tool wants to participate in the standard editor undo /
  transaction / preview-actor lifecycle.
- The tool is genuinely modal — it owns the viewport input until
  the user accepts or cancels.

### When EUW fits better

- The interaction is "open a panel, click buttons, run scripts" with
  no viewport drag/click.
- The tool is non-modal (user keeps editing while it runs).

### Relationship to `bp-reader`

ITF is in a completely different layer — it's for interactive human
tools, not for headless agent-driven automation. `bp-reader` does
not interact with it; an MCP-driven workflow does not need viewport
gizmos. Worth noting only because someone evaluating "which
automation surface should I extend" will see it in the docs.

---

## 5. RemoteControl plugin

### What it is

A network-API layer for the running editor, originally built for
virtual-production live-event setups. Lets external clients (web
dashboards, OSC controllers, custom hardware) read and write
exposed properties on UObjects without going through the editor UI.

Plugin: `D:/Projects/Unreal Engine 5/Engine/Plugins/VirtualProduction/RemoteControl/`.
The core module is `RemoteControl/` (the data model — `URemoteControlPreset`,
exposed fields, bindings). The transport modules are `WebRemoteControl/`
(HTTP + WebSocket server) and the various `RemoteControlProtocol*/` modules
(OSC, MIDI, DMX).

### Model

A `URemoteControlPreset` asset is a curated set of "exposed" fields
and functions on specific UObjects. The user picks (in the editor
UI) which properties of which actors they want exposed; the preset
stores stable IDs (so renames don't break clients) and a binding
table that resolves the IDs back to UObject paths at runtime.

External clients then query the preset by name or ID and read/write
the exposed fields. The transport is HTTP REST for one-shot
read/write and WebSocket for subscriptions (push notifications when
exposed values change).

### Endpoints

(Not verified — see TODO; Epic's overview page was not fetched
during this research pass. Names below are from class-name evidence
in `Source/WebRemoteControl/Private/`):

- `RemoteControlRoute.h` defines route handlers for object-property
  GET/PUT and function invocation POST.
- `RemoteControlWebSocketServer.h` defines the WS endpoint for
  subscribe / unsubscribe / push frames.
- Settings (port, auth) in
  `Source/RemoteControlCommon/Public/RemoteControlSettings.h` —
  user-editable in Project Settings.

Default port and authentication scheme: not verified — Epic's docs
list a default HTTP port (typically 30010 in older 5.x; confirm
against the project's `RemoteControlSettings` for this engine
build). Authentication is opt-in shared-token or disabled in the
default config — treat the server as no-auth on a trusted network
until verified otherwise.

### Lifecycle

- Editor must be running. The HTTP/WS server is hosted inside the
  editor process; there is no commandlet equivalent.
- Available in cooked builds too (the runtime module loads in
  non-editor targets), but the editor-side UI for authoring presets
  is editor-only.

### Overlap with `bp-reader`'s live backend

Both are "talk to a running editor over a socket." Differences:

- RemoteControl exposes a fixed shape per preset (user picks
  properties up front). `bp-reader`'s live backend exposes a
  dynamic op dispatch (any `-Op=` the commandlet supports).
- RemoteControl is HTTP/WS; `bp-reader` is JSON-framed TCP. HTTP
  pays a parser tax per call; JSON-framed TCP is leaner but does
  not get any of the standard HTTP tooling for free (no proxy
  support, no browser-direct calls, no Postman testing).
- RemoteControl has property-change subscriptions (push). `bp-reader`
  is request/response only — no push notifications.
- RemoteControl ships with the engine; `bp-reader` is a custom
  plugin that has to be installed per project.

### When `bp-reader` could lean on RemoteControl

- "Watch this property and notify me when it changes" — would need
  WebSocket subscription, which `bp-reader`'s live backend does not
  do. Could add the same primitive directly; could also delegate
  to RemoteControl for that single use case.
- "Drive the editor from a browser tab" — RemoteControl's HTTP
  surface is the answer. `bp-reader`'s TCP framing is not
  browser-reachable.
- "Standard auth + multi-protocol" — RemoteControl has DMX, OSC,
  MIDI protocol bindings out of the box. None of which an AI agent
  cares about, but worth knowing.

### Why `bp-reader` does not use it today

The op surface is dynamic (126 tools, schema-driven) and the wire
shape is JSON-RPC 2.0 (the MCP standard). Mapping that to a fixed
preset of exposed properties would force a "every op is a preset
function" indirection — and the preset itself would need to be
authored in the editor UI, defeating the whole "external agent
discovers and calls tools" model. The bespoke TCP listener is the
right shape for the requirement.

---

## 6. AutomationTestFramework (`FAutomationTestFramework`)

### What it is

UE's built-in test runner. Two flavors:

- C++ unit/integration tests via macros in
  `Runtime/Core/Public/Misc/AutomationTest.h`:
  `IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMyTest,
  "Category.SubCategory.Name", EAutomationTestFlags::...)`. Body in
  `FMyTest::RunTest`. The framework discovers them at engine init
  by walking the registered classes.
- Functional tests (`AFunctionalTest` actors placed in test maps).
  Used for in-world gameplay tests; out of scope for headless BP
  tooling.

### Invocation

- From the editor: `Tools → Test Automation` opens a tree of all
  registered tests; check + Run.
- From console: `Automation RunTests <pattern>` (wildcard supported).
- From CLI: `UnrealEditor-Cmd.exe <Project> -ExecCmds="Automation
  RunTests <pattern>; Quit"` for batch runs.
- Programmatic: `FAutomationTestFramework::Get().StartTestByName`,
  `LoadTestModules`, etc.

### Result reporting

- Each test reports pass/fail + a list of messages.
- Output lands in:
  - The output log during the run (`LogAutomation` category).
  - JSON / XML reports under `<Project>/Saved/Automation/` (the
    schema is the long-form Epic automation report).
- The framework integrates with the Session Frontend for live status
  display when the editor is open.

### How `bp-reader` uses it

The `run_automation_tests` MCP tool exists at the registry level
(`D:/Projects/UE5_MCP/Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/BlueprintTools.cpp:2865`)
and forwards to `IBlueprintReader::RunAutomationTests(pattern)`. The
commandlet implementation at
`BlueprintReaderCommandlet.cpp:4999` (`RunRunAutomationTestsOp`)
just shells out to the console:

```
GEngine->Exec(GetEditorWorldOrNull(),
              *FString::Printf(TEXT("Automation RunTests %s"), pattern),
              Capture);
```

This is fire-and-forget — the framework runs async on the game
thread and the `Capture` string is the dispatch confirmation, not
the test result. The tool returns `{ok: true, started: bool,
message: <log capture>}`. A caller that wants results has to read
`<Project>/Saved/Automation/` directly.

Plugin-side tests live alongside the plugin: smoke test at
`Plugins/BlueprintReader/Source/BlueprintReaderEditor/Tests/BlueprintReaderEditorSmokeTest.cpp`,
runtime introspector tests at
`Plugins/BlueprintReader/Source/BlueprintReaderRuntime/Private/BlueprintReaderRuntimeTests.cpp`.

### When it fits

- Repeatable verification of editor / engine state changes ("after
  this commit, do all 126 MCP tools still emit valid JSON against
  the seeded test BPs?").
- Nightly CI runs against the cooked build to catch regressions in
  the runtime module.
- Cross-platform smoke (the same `IMPLEMENT_*_AUTOMATION_TEST` runs
  on any target).

### Limitations

- The fire-and-forget result model is awkward for an agent — there's
  no "block until done, return the JSON result inline" entrypoint.
  An MCP tool that wraps this nicely would need to poll
  `Saved/Automation/` or hook the `FAutomationTestFramework`
  completion delegate directly.
- Heavyweight: needs full engine boot. Even with `-nullrhi` you pay
  module init + asset registry mount.
- Test parallelism is coarse — tests run serially within one
  process; multiple processes need separate seed data.

---

## 7. Console-command-from-CLI (`-ExecCmds`)

### What it is

The cheapest lever: launch the editor (or commandlet) with
`-ExecCmds="<cmd1>; <cmd2>; <cmd3>"` and the engine routes each
semicolon-delimited string through `GEngine->Exec` after init. Any
console command works — `stat unit`, `obj list`, `Automation
RunTests`, `BugItGo`, `r.<cvar> <value>`, etc.

### Form

```
UnrealEditor-Cmd.exe <uproject> -ExecCmds="<cmd>; Quit" \
    -nullrhi -nosplash -unattended -nopause
```

Always include `Quit` (or `EXIT`) as the last command — without it
the editor stays running. `-unattended` and `-nopause` keep modal
prompts from blocking the exec.

### Pros

- Zero authoring. No C++, no Blueprint, no Python.
- Discoverability: every console command in the editor is reachable.
- Works against either `UnrealEditor.exe` (full editor) or
  `UnrealEditor-Cmd.exe` (headless).

### Cons

- One-shot. Pay full engine startup per call; no daemon.
- Result capture is via the output log only — no structured JSON
  return. Parsing the log is brittle.
- Console commands operate on the live editor world / engine state;
  the surface is wide but unstructured.

### Where it sits relative to `bp-reader`

Two of `bp-reader`'s ops are essentially "shell out to a console
command" with a structured return shape: `console_command` (general
cvar/exec) and `live_coding_compile` (Live Coding rebuild). Both run
inside the daemon's persistent process, so they pay zero startup
cost — `-ExecCmds` would be strictly worse for the same calls.

`-ExecCmds` is the right tool for "I want to drive the editor from a
shell script once and don't want to install a plugin." Not the right
tool when you're already going to install something — anything you
build will be cheaper per call.

---

## 8. MCP integration angle

### Today

`bp-reader` runs two server roles:

- Commandlet daemon (`UBPRCommandlet -Daemon`). One per project.
  Hosts `FCmdletServer` (TCP listener, JSON-framed wire protocol,
  handshake-file discovery at `<Project>/Saved/bp-reader-cmdlet.json`,
  GUID-based auth token, lifetime lock). All UObject mutation
  marshalled to the game thread via `AsyncTask`. Pumps the task
  graph + ticker hand because there is no editor to do it.
- In-editor live server (`FLiveServer`). Started on editor module
  init unless `BP_READER_LIVE_DISABLED=1`. Same wire protocol;
  handshake file at `<Project>/Saved/bp-reader-live.json`. Sees
  the editor's live in-memory state (including unsaved changes).

The MCP server (`BlueprintReaderMcp.exe`) picks the backend per
call via the `auto` policy: probe both handshake files, prefer live
when the editor is up. Cache the probe for 2 s to avoid hammering
the filesystem on bursty workloads. Mock backend (`mock`) for tests
that should not touch UE at all.

This pair covers the "fast headless ops" + "live editor ops" axes
adequately. The remaining levers above are not in the production
path.

### What it could add

- **Python (`run_python_script`)**. Already implemented behind
  `BP_READER_ALLOW_PYTHON=1`. Not enabled by default because
  arbitrary Python bypasses the curated tool boundary. Useful when
  the agent needs "something the 126 curated tools don't cover" —
  the escape hatch. Per-call cost is the script-source parse;
  module imports cache across calls within the daemon's lifetime.
- **RemoteControl mirror**. Could re-host the same dispatch on
  RemoteControl's HTTP/WS surface for browser-direct calls or for
  webhook-style "notify on property change" subscriptions. Cost:
  another module dependency, another preset to maintain. Benefit:
  HTTP tooling is more universal than custom TCP framing. Likely
  not worth it unless someone explicitly needs HTTP — the auto
  backend already covers the parallelism story by routing through
  the live editor when one is open.
- **AutomationTestFramework integration**. The `run_automation_tests`
  tool today is fire-and-forget. A "wait for completion + return
  the JSON result inline" variant would let an agent run a nightly
  verification suite and read the verdict in one round-trip. Path:
  hook `FAutomationTestFramework`'s `OnTestEndEvent` delegate from
  inside the commandlet daemon, accumulate results, return on a
  bounded timeout.
- **Editor Utility Widget shell**. If a user wants the agent's
  outputs visible inside the editor (a tab showing the last 50 MCP
  calls and their results), an EUW reading the same handshake file
  is the path. Not a "new automation lever" — it's a UI front-end
  for the existing socket.

### What it should not add

- Custom Slate UI from C++. `bp-reader` is a service, not an editor
  feature; the surface is JSON-RPC, not menus.
- A second commandlet daemon variant. The lifetime-lock already
  forces single-process; adding more would multiply asset-registry
  contention.
- An ITF/InteractiveTool wrapper. Modal viewport tools are for
  humans; an agent does not click-and-drag.

---

## 9. Trade-off matrix

Columns:

- **Startup**: cold-start cost per call (assumes no warmup).
- **Parallelism**: per-process concurrency story.
- **Editor req**: does it require a running editor process (Y),
  a headless commandlet/launcher (N), or either (E)?
- **Lang**: scripting language(s) the lever uses.
- **AI fit**: how well-suited for an autonomous agent driving the
  engine: ★ (poor) → ★★★★★ (excellent).

| Lever                              | Startup       | Parallelism                          | Editor req | Lang                  | AI fit   |
| ---------------------------------- | ------------- | ------------------------------------ | ---------- | --------------------- | -------- |
| Commandlet (`-run=Foo`) one-shot  | 15–30 s       | None (1 proc, 1 main thread)         | N          | C++                   | ★★       |
| Commandlet daemon (persistent)     | Once, ~20 s   | TCP multiplex; ops marshal to GT     | N          | C++                   | ★★★★★    |
| Python (`-ExecutePythonScript=`)   | 15–30 s       | None; single CPython, GIL            | N          | Python 3              | ★★       |
| Python (`py` in running editor)    | ~ms (cached)  | None; single CPython, GIL            | Y          | Python 3              | ★★★★     |
| Editor Utility Widget              | n/a (in editor) | Game thread only                    | Y          | Blueprint (+ Python)  | ★★       |
| Editor Utility Blueprint           | n/a (in editor) | Game thread only                    | Y          | Blueprint             | ★        |
| ScriptableTool / ITF               | n/a (in editor) | Game thread only                    | Y          | C++ or BP (modal)     | ★        |
| RemoteControl HTTP / WebSocket     | n/a (in editor) | Per-request HTTP; WS push           | Y          | (transport only)      | ★★★      |
| AutomationTestFramework            | 15–30 s       | Tests serial in proc; multi-proc ok  | E          | C++ (BP for Funcl.)   | ★★★      |
| `UnrealEditor-Cmd -ExecCmds="..."` | 15–30 s       | None; single shot                    | N          | Console grammar       | ★        |
| `bp-reader` (commandlet + live)    | Once, ~20 s   | TCP multiplex; ops marshal to GT     | E (auto)   | JSON-RPC over TCP     | ★★★★★    |

Notes:

- The 7 numbered levers in the prompt are: Commandlet (rows 1+2),
  Python (rows 3+4), Editor Utility Widgets/BPs/Actors (rows 5+6),
  ScriptableTool/ITF (row 7), RemoteControl (row 8),
  AutomationTestFramework (row 9), Console-from-CLI (row 10). The
  final `bp-reader` row is the integration baseline for comparison.
- "Parallelism" is per-process; you can always spawn N processes —
  the question is whether they fight over the asset registry / DDC /
  `.uasset` files (commandlets do; that's why the lifetime lock
  exists in `bp-reader`).
- Cold-start numbers are this machine, this engine build, this
  project. Smaller projects start faster; HDD-bound machines slower.
- AI-fit is qualitative: structured input + structured output + low
  per-call latency + multi-call coherence = ★★★★★; one-shot stdout
  with no schema = ★.

---

## TODOs (not verified in this pass)

- RemoteControl default HTTP port — confirm against
  `Engine/Plugins/VirtualProduction/RemoteControl/Source/RemoteControlCommon/Public/RemoteControlSettings.h`
  defaults for UE 5.7.4.
- RemoteControl auth scheme — read the same settings header for the
  shared-token / disabled toggle.
- Editor Utility Widget Python-script-action surface — exact API
  name for "Blueprint event calls Python from inside an EUW."
- ScriptableTool: where Python integration sits relative to the
  Blueprint authoring path.
- Epic doc URLs in the source list (Python, RemoteControl, EUW)
  were not fetched. Re-verify once WebFetch is available.
