# UE5_AI_BP — Plan

## Repo layout (current)

```
D:\Projects\UE5_AI_BP\
├── UE5_AI_BP.uproject
├── Source\                              (project runtime module)
├── Config\
├── Plugins\
│   └── BlueprintReader\                 (Phase 1 plugin)
│       └── Source\BlueprintReaderEditor\
│           ├── Public\
│           │   ├── BlueprintReaderTypes.h
│           │   ├── BlueprintIntrospector.h
│           │   ├── BlueprintReaderJson.h          (rich/legacy plugin shape)
│           │   ├── BlueprintReaderWireJson.h      (canonical MCP wire shape)
│           │   ├── BlueprintReaderCommandlet.h
│           │   └── BlueprintReaderSeedCommandlet.h
│           └── Private\ (parallel layout)
├── Shared\
│   └── BlueprintReaderTypes.h           (POD/USTRUCT dual-mode types)
├── mcp-server\                          (Phase 0 MCP server, Phase 1 commandlet backend)
│   ├── src\
│   │   ├── backends\
│   │   │   ├── IBlueprintReader.h
│   │   │   ├── MockBlueprintReader.{h,cpp}
│   │   │   ├── CommandletBlueprintReader.{h,cpp}    ★ Phase 1
│   │   │   └── BackendFactory.{h,cpp}
│   │   ├── jsonrpc\ (Server, Mcp)
│   │   ├── tools\ (BlueprintTools, ToolRegistry)
│   │   └── main.cpp
│   ├── tests\
│   │   ├── test_*.cpp
│   │   └── test_commandlet_backend.cpp              ★ Phase 1 (live, gated by env)
│   ├── scripts\
│   │   └── roundtrip.ps1                            ★ Phase 1 helper
│   ├── fixtures\
│   ├── CMakeLists.txt
│   └── vcpkg.json
├── UnrealEngine\                        (source-built engine, .gitignored)
├── PLAN.md
├── README.md
└── .gitignore
```

## Phases

- **Phase 0** — Standalone C++ MCP server at `mcp-server\`. ✔ Complete.
  - JSON-RPC 2.0 over stdio (LSP-style framing).
  - `IBlueprintReader` backend interface; mock backend with three fixtures.
  - Six MCP tools: `list_blueprints`, `read_blueprint`, `get_graph`,
    `get_function`, `list_variables`, `find_node`.

- **Phase 1** — `Plugins\BlueprintReader\` UE plugin + commandlet backend.
  ✔ Complete. Verified live end-to-end against UE 5.7.4 source build.

- **Phase 1.5 — Polish.** ✔ Complete.
  - **Daemon mode** for the commandlet backend: one editor process reused
    across calls. ~5 s cold start → ~30 ms per subsequent call (~200x).
    Now the *default* for the commandlet backend (opt out with
    `BP_READER_DAEMON=0`).
  - **`find_node` kind filter** — optional `kind` arg matches against
    K2 extras `kind` (e.g. `VariableGet`, `CallFunction`).
  - **MCP telemetry** — every `tools/call` response carries `_meta`
    `{elapsed_ms, tool}` per the MCP 2024-11-05 spec extension point.
  - **Extended K2 extras** — Branch, Sequence, Knot, Self, FormatText,
    MakeArray, MakeStruct, Composite, Tunnel, Timeline, Switch, Literal,
    CreateDelegate, CallParentFunction in addition to the original 7.
  - **AssetRegistry-backed `list_blueprints`** — replaces the previous
    disk walk; faster on large Content trees, gets `parent_class` from
    asset tags without loading every BP.
  - **Write tools (v2)** — twelve tools modifying BPs via
    `FBlueprintEditorUtils`, recompiling, and saving the `.uasset`:
    - Variables: `add_variable`, `delete_variable`, `rename_variable`,
      `set_variable_default`.
    - Functions: `add_function`, `delete_function`,
      `add_function_input`, `add_function_output`.
    - Nodes: `add_node` (Branch / Sequence / VariableGet/Set /
      CallFunction / CustomEvent / Cast / Self / MakeArray / MakeStruct /
      FormatText / Knot — 12 kinds), `set_node_position`, `delete_node`.
    - Connections: `wire_pins` (schema-validated, accepts pin GUIDs
      or names).
    Mock backend throws (read-only by design).
  - **Discoverability tools** — `list_node_kinds` and
    `list_pin_categories`. Pure metadata, no backend call. Self-describing
    surface so a model can ask "what's a valid `kind` for `add_node`?"
    or "what does a struct-ref BPPinType look like?" without scanning
    docs.
  - **README + Claude integration snippet** so the project is actually
    usable: drop-in JSON for Claude Code / Claude Desktop.
  - **Richer seed BP** — `BP_TestEnemy` event graph now has
    `BeginPlay → Branch → PrintString` with `Get bIsAlive → Branch.Condition`
    (3 connections, 6 nodes with non-trivial K2 extras).

- **Phase 2** — `live` backend (long-lived editor with non-commandlet IPC,
  parallel dispatch). Marginal value vs daemon mode; not yet scheduled.

## Phase 1 status

### What landed

- **Wire-format reconciliation.** Added `BlueprintReaderWireJson.{h,cpp}` to the
  plugin. It serializes `FBlueprintInfo` directly to the canonical wire shape
  used by the MCP server (snake_case keys; `BPNode.meta` as a real nested JSON
  object, *not* a string). The original `BlueprintReaderJson` (camelCase, with
  K2 extras) is retained as the legacy/debug format and still drives the
  existing automation tests — no introspector refactor.
- **Plugin types extended additively.** `FBPPinInfo` and `FBPVariableInfo`
  gained `StructuredType` (`FBPStructuredPinType`) populated from the raw
  `FEdGraphPinType`. Pins now carry their GUID (`PinId`). `FBPPinLinkInfo`
  carries the linked pin's GUID. `FBPGraphInfo` gained `WireType`
  (`EventGraph` / `Function` / `Macro` / `Construction` / `DelegateSignature`).
- **Commandlet ops.** `UBlueprintReaderCommandlet` now supports
  `-Op=List|Read|Graph|Function|Variables|Find` with `-Asset=`, `-Graph=`,
  `-Function=`, `-Query=`, `-Path=`, `-Out=`/`-Output=`, `-Compact`.
  Backward-compatible: `-Path=` alone (no `-Op=`) still emits the legacy
  rich JSON.
- **Seed commandlet.** New `UBlueprintReaderSeedCommandlet`
  (`-run=BlueprintReaderSeed`) synthesizes `/Game/AI/BP_TestEnemy`
  (replicated/editable variables, two custom functions, branch in event graph)
  and `/Game/AI/BP_TestPickup`. Saves the .uasset files via `UPackage::SavePackage`.
- **MCP backend.** `CommandletBlueprintReader` shells out to
  `UnrealEditor-Cmd.exe` via `CreateProcessW`, captures stdout/stderr, parses
  the wire JSON from a temp file, deletes it, returns canonical structs.
  Times out at `BP_READER_TIMEOUT_SECONDS` (default 120). Exit-code 4 from the
  commandlet (asset/graph/function not found) maps to `AssetNotFound`.
- **Backend factory.** `BP_READER_BACKEND=commandlet` now wires the live
  backend. Reads `BP_READER_ENGINE_DIR` and `BP_READER_PROJECT` (no defaults
  hard-coded inside the factory — fail-fast if missing). The `mock` path is
  unchanged.
- **`.Build.cs` is unchanged from `bc7c2b6`.** The List op walks the
  project's `Content` tree on disk (`IFileManager::FindFilesRecursive`
  + `FPackageName::TryConvertFilenameToLongPackageName`) and loads each
  `.uasset` to check whether it's a `UBlueprint`. Slower than an asset
  registry query, but avoids adding an `AssetRegistry` module dep, which
  would invalidate UBT's makefile cache and force a full rebuild.
- **Wire `asset_path` is the package path**, not the object path. A
  `ToPackagePath` helper in the wire serializer strips the `.AssetName`
  suffix from `Blueprint->GetPathName()` so the output matches the
  fixture shape (`/Game/AI/BP_Enemy`, not `/Game/AI/BP_Enemy.BP_Enemy`).
- **Integration test.** `mcp-server/tests/test_commandlet_backend.cpp` runs
  the six tools live; `doctest::skip` gates it on the env vars so a fresh
  doctest run on a clean box stays fast (5 tests are skipped without the live
  config).
- **JSON-RPC verification.** `mcp-server/scripts/roundtrip.ps1` drives a
  4-step `initialize → notifications/initialized → tools/call list_blueprints
  → tools/call read_blueprint` sequence through `bp-reader-mcp.exe`.

### Verification done in-session

- `bp-reader-tests.exe`: 44/44 cases pass with `BP_READER_BACKEND=commandlet`,
  `BP_READER_ENGINE_DIR`, `BP_READER_PROJECT` set in env (39 mock + 5 live).
  Without env vars: 39 pass, 5 skipped.
- Live `mcp-server/scripts/roundtrip.ps1` — `initialize → notifications/initialized
  → list_blueprints (path=/Game/AI) → read_blueprint (asset=/Game/AI/BP_TestEnemy)`:
  total wall-clock **11.6 s**, average **5.5 s per tool call** (cold-start spawn
  of `UnrealEditor-Cmd.exe -run=BlueprintReader` per call). Both responses
  `isError: false`.
- `list_blueprints` output (snake_case keys, package paths, ISO mtime from
  the `.uasset`):
  ```json
  [
    {"asset_path":"/Game/AI/BP_TestEnemy",  "modified_iso":"2026-05-02T01:18:14.000Z",
     "name":"BP_TestEnemy",  "parent_class":"/Script/Engine.Actor"},
    {"asset_path":"/Game/AI/BP_TestPickup", "modified_iso":"2026-05-02T01:18:14.000Z",
     "name":"BP_TestPickup", "parent_class":"/Script/Engine.Actor"}
  ]
  ```
- `read_blueprint` output for `BP_TestEnemy` (excerpted):
  ```json
  {
    "asset_path": "/Game/AI/BP_TestEnemy",
    "functions": [{"name":"TakeDamage"}, {"name":"OnDeath"}],
    "graphs":    [{"name":"EventGraph","type":"EventGraph"},
                  {"name":"UserConstructionScript","type":"Construction"}],
    "interfaces": [],
    "macros": [],
    "name": "BP_TestEnemy",
    "parent_class": "/Script/Engine.Actor",
    "variables": [
      {"name":"Health", "category":"Combat", "is_replicated":true, "is_editable":true,
       "type":{"category":"real","sub_category":"float","sub_category_object":null,
               "is_array":false,"is_set":false,"is_map":false}, "default_value":null},
      {"name":"MaxHealth", "category":"Combat", "is_replicated":false, "is_editable":true, ...},
      {"name":"AggroTarget", "category":"AI", "is_replicated":true, "is_editable":true,
       "type":{"category":"object","sub_category":null,
               "sub_category_object":"/Script/Engine.Actor", ...}}
    ]
  }
  ```
- `get_function TakeDamage`: returns inputs=`[Damage:float]`,
  outputs=`[Killed:bool]`, locals=`[NewHealth:float]`, graph type `Function`.
- Build artifacts the seed produced: `Content/AI/BP_TestEnemy.uasset` (37 KB)
  and `Content/AI/BP_TestPickup.uasset` (28 KB). Worktree `.gitignore` keeps
  them out of source control unless the user opts in (UE convention is to
  commit them; deferred for now since they're regenerable).

### Build observations

- First UE editor build from this worktree's source tree: **42 minutes**
  (2929/2932 actions; engine intermediates needed full regeneration after
  earlier aborted attempts). Subsequent incrementals: **5–10 seconds** for
  plugin-only changes — the engine never rebuilds again.
- One mid-build C1083 (`Engine/StaticMeshComponent.h` not found) from a
  stray include in the seed commandlet that dropped when AssetTools was
  removed from the deps. Fixed by trimming the include list.

### Decisions made

- **Casing strategy:** plugin emits snake_case directly via a dedicated
  serializer (`BlueprintReaderWireJson`) rather than retrofitting
  `FJsonObjectConverter` policies or post-processing keys MCP-side. The
  rich legacy serializer is kept for offline debugging.
- **`BPNode.meta`:** emitted as a real nested JSON object on the wire (the
  plugin currently only produces string→string entries; that's fine — the
  wire schema permits any value type, and consumers tolerate it).
- **List op uses the asset registry** (`SearchAllAssets(true)` + `FARFilter`)
  rather than walking the disk; this gives correct `parent_class` from asset
  tags without loading every BP.
- **Connections are synthesized from pin `LinkedTo`** in the wire serializer,
  using the linked pin's GUID for `to_pin` (matches fixture shape; falls back
  to pin name if the linked id is missing).
- **Subprocess library:** `CreateProcessW` directly. `cpp-subprocess` was
  declared in `vcpkg.json` but kept out of the link path to avoid a vcpkg
  bring-up requirement for Phase 1.

## Phase 1.5 (deferred / nice-to-have)

- Daemon mode for the commandlet backend (one editor process, multiple ops)
  to amortize the 5-7s cold start.
- Plumb `find_node` matching to optionally include kind/extras filters.
- Have the seed commandlet wire the EventGraph branch to a `Get Health` node
  to exercise the connections path more thoroughly.
