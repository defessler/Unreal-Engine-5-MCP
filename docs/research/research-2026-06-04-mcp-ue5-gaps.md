# Research: MCP Tech + UE5 Editor Customization Gaps
**Date:** 2026-06-04  
**Scope:** Latest MCP spec; UE5 editor customization coverage gaps; UPROPERTY/UFUNCTION specifiers + reflection; performance bottlenecks  
**Feeds into:** improvement-roadmap.md (new IDs: MCP-*, EDIT-*, REFLECT-*, PERF-*)

---

## Part 1: Latest MCP Specification Features

### Protocol versions compared to our current baseline

| Version | Status | Key additions |
|---|---|---|
| 2024-11-05 | Our old default | Baseline |
| 2025-03-26 | Stable | Streamable HTTP replaces HTTP+SSE; JSON-RPC batch still present |
| 2025-06-18 | Stable | `outputSchema`/`structuredContent`, `elicitation`, `title` field, resource links, batch **removed** |
| **2025-11-25** | **Our current max** | Tasks (experimental), `icons`, `execution.taskSupport`, JSON Schema 2020-12 default, `_meta` blessed |
| 2026-07-28 RC | Not stable yet | Tasks redesigned; Sampling deprecated; `server/discover`; don't implement yet |

### Spec features we are NOT yet using (actionable)

**Zero-risk / purely additive:**

1. **`title` field on Tool objects** (2025-06-18). A separate human-readable display name distinct from the programmatic `name`. Claude Desktop, ChatGPT both display it. Add to all 254 tools — pure metadata, zero behavioral change. Example: `name="get_graph"`, `title="Get Blueprint Graph"`.

2. **ToolAnnotations** (2024-11-05+, we have the field in `ToolAnnotations.cpp` but not all 4 hints filled). Correct values:
   - All read tools when server is in read-only mode: `{readOnlyHint:true, destructiveHint:false, idempotentHint:true, openWorldHint:false}`
   - Write tools: `{readOnlyHint:false, destructiveHint:true, idempotentHint:false, openWorldHint:false}`
   - Claude Code uses `readOnlyHint:true` for auto-approval; ChatGPT store requires correct hints for submission.

3. **`description` on `serverInfo`** (2025-11-25). Our `InitializeResult.serverInfo` currently has `name` + `version` only. Add a short description string.

4. **Input validation errors as `isError:true`** (2025-11-25 MUST). Currently bad args return JSON-RPC error `-32602`. The spec now requires returning a valid `CallToolResult` with `isError:true` and actionable text so the model can self-correct. Our `std::invalid_argument` handler should be caught and re-emitted as a tool error result, not a JSON-RPC error.

5. **`notifications/tools/list_changed`** (2024-11-05+, but we don't declare `listChanged:true` in capabilities). When `BP_READER_ALLOW_WRITE` or `BP_READER_ALLOW_TRANSPILE` is toggled at runtime, clients should be notified to re-fetch `tools/list` instead of disconnecting.

**Medium effort:**

6. **`outputSchema` + `structuredContent`** (2025-06-18). We have `output_schema` populated on ~220/254 tools. We do NOT yet emit `structuredContent` alongside `content[].text`. Gemini CLI **errors** when `outputSchema` is declared without `structuredContent`. Fix: when `outputSchema` is non-empty and the response is a JSON object, populate `structuredContent` with the same JSON. Claude Code ignores it but Gemini requires it.

7. **JSON Schema 2020-12 default** (2025-11-25). Our `inputSchema`/`outputSchema` use draft-07 conventions. Without an explicit `"$schema"` field, 2025-11-25 clients default to 2020-12. Additions: `unevaluatedProperties`, `prefixItems` (replaces `items` in tuples). Should verify our schemas don't use anything 2020-12 removed.

8. **Streamable HTTP transport update** (2025-03-26). Our existing `HttpTransport.h` implements the deprecated 2024-11-05 two-endpoint SSE pattern (`/sse` + `/message`). Newer clients probe by POSTing `InitializeRequest` to a single `/mcp` endpoint. Fix: add a `/mcp` endpoint supporting both POST (request) and GET (SSE stream opening), while keeping old endpoints for back-compat. Also required: `MCP-Protocol-Version` header on all subsequent HTTP requests after initialize (2025-06-18 requirement).

**Architectural (wait for 2026-07-28 stable before full implementation):**

9. **Tasks primitive** (2025-11-25, experimental). For long-running ops (`compile_blueprint`, `build_lighting`, `run_automation_tests`, `cook_content`, `package_project`, large `apply_ops` batches): declare `execution: {taskSupport: "optional"}` on those tools. Client sends `tools/call` with `{"task":{"ttl":60000}}`; server returns `taskId` and polls are made via `tasks/get`. The architecture will change in the 2026-07-28 stable release — implement conservatively or wait.

10. **Elicitation** (2025-06-18). When the client declares `elicitation` capability, the server can pause a destructive tool call and request confirmation: `elicitation/create` with `{message: "Enable write mode?", requestedSchema: {type:"object", properties:{confirm:{type:"boolean"}}, required:["confirm"]}}`. Implement for `delete_variable`, `delete_function`, `delete_node`, `delete_asset`, `build_lighting`. Check client capability first — silently proceed if client doesn't support it.

### Tool description quality (arXiv 2025 study findings)

97.1% of MCP tool descriptions had quality defects. For our 254 tools:
- 56% had "unclear purpose" — missing **when to use this** guidance.
- Improvements yielded +5.85% task success rate, +15.12% evaluator accuracy.
- **What to add per tool**: (a) explicit purpose statement, (b) activation criteria (when to use this vs. a similar tool), (c) parameter constraints and failure modes.
- **Examples are optional** — removing them had no significant degradation. Keep descriptions tight.

Claude Code uses BM25 search over `name + description + parameter names` for tool selection when the context is large. Our current descriptions often describe WHAT but not WHEN — "Get a Blueprint graph" vs. "Fetch the full K2 node graph of a Blueprint function or event graph by name. Use when you need to see specific node wiring, not just the function list."

### `tools/list` pagination: do NOT paginate

The spec defines cursor-based `nextCursor` pagination for `tools/list`. **Claude Code does not follow it** (confirmed GitHub issue #24785, closed "not planned"). Any tools on page 2+ are silently invisible and uncallable. Return all 254 tools on the first page — this is already our behavior.

---

## Part 2: UE5 Editor Customization Gaps

### Top 5 gaps ranked by (frequency × MCP leverage)

#### Gap 1: AnimBlueprint state machine + AnimGraph authoring ★★★★★

**Why it matters:** Every character game has AnimBlueprints. In Lyra alone there are 40+ ABPs. State machines, transition conditions, and blend space wiring are done entirely by hand today.

**Current state:** `add_anim_state` always returns `{added:false}` (explicit stub). `read_anim_blueprint` returns parent class only. The `AnimGraph` module is NOT in `BlueprintReaderEditor.Build.cs` private deps — state machine walks require `UAnimStateMachineGraph` from that module.

**What's needed:**
- Add `AnimGraph` to private deps in `BlueprintReaderEditor.Build.cs`
- `read_anim_blueprint` → walk `UAnimBlueprint::AnimationGraphs` + `IAnimationGraph`-typed `UEdGraph`s, enumerate `UAnimStateNode`, `UAnimStateTransitionNode`, transition condition graphs
- Write: `add_anim_state(asset, state_machine_name, state_name)` via `FBlueprintEditorUtils::AddStateNode`; `add_anim_transition(asset, from, to, condition_func)`; `set_anim_state_animation(asset, state, anim_sequence_path)`
- Key headers: `AnimGraph/Classes/AnimGraphNode_StateMachine.h`, `AnimGraph/Classes/AnimStateNode.h`, `AnimGraph/Classes/AnimStateTransitionNode.h`, `Animation/AnimBlueprint.h`

#### Gap 2: Timeline read/write (UTimelineTemplate + UCurveFloat) ★★★★★

**Why it matters:** Timelines are in virtually every BP-heavy project — door animations, weapon recoil, camera shake drivers, UI transitions. Every BP with a `TimelineComponent` has one or more `UTimelineTemplate` entries.

**Current state:** `K2Node_Timeline` surfaces in graph reads with `kind=Timeline` + `timelineName`, but that's it. Zero tools for the underlying timeline data. The BPIR transpiler emits `// TODO[bpr-unsupported]: manually add a UTimelineComponent member`.

**Structure:** `UBlueprint::Timelines` is a `TArray<UTimelineTemplate*>` directly on the BP object — readable without any special module. Each `UTimelineTemplate` has:
- `TArray<FTTFloatTrack> FloatTracks` — `{TrackName, CurveFloat: UCurveFloat}`
- `TArray<FTTVectorTrack> VectorTracks` — `{TrackName, CurveVector}`
- `TArray<FTTEventTrack> EventTracks` — event-driven execution pins
- `float TimelineLength`, `bool bAutoPlay`, `bool bLoop`, `bool bReplicated`
- UCurveFloat → `FRichCurve FloatCurve` → `TArray<FRichCurveKey> Keys`

**What's needed:** `read_timeline(asset, timeline_name)` → tracks + keys; `add_timeline_track(asset, timeline_name, track_type, track_name)`; `set_curve_key(asset, timeline_name, track_name, time, value, interp_mode)`; `read_curve(curve_asset_path)` for standalone UCurveFloat assets.

Key headers: `Engine/Classes/Engine/TimelineTemplate.h`, `Engine/Classes/Curves/CurveFloat.h`, `K2Node_Timeline.h`

#### Gap 3: UPROPERTY metadata specifiers in class introspection ★★★★☆

**Why it matters:** Every team with custom C++ types needs their property metadata to be visible. `EditCondition`, `AllowPrivateAccess`, `ClampMin/Max`, `GetOptions`, `DeterminesOutputType` — all invisible to the MCP server today.

**Current state:** `get_class_info` returns `{name, typeName, category, declaredOn}` per property — **zero metadata specifiers**. An AI generating a Details customization class needs the full `FField::GetMetaData` map for the target class.

**What's needed:**
- Extend the `IntrospectClass` result to include per-property `metadata` map: `Property->GetMetaData("EditCondition")`, `->GetMetaData("Category")`, etc. Use `TMap<FName,FString>` from `Property->GetMetaDataMap()`.
- Decode `PropertyFlags` (EPropertyFlags bitmask) as named boolean fields: `{blueprint_read_write, blueprint_read_only, replicated, transient, edit_anywhere, edit_defaults_only, ...}` rather than raw hex.
- Surface `RepNotifyFunc` name for `CPF_RepNotify` properties.
- Surface `GetCPPType()` per property (the exact C++ typename — `TObjectPtr<UClass>`, `TArray<FVector>`, etc.).
- New tool: `get_registered_customizations()` — lists all registered `IDetailCustomization` and `IPropertyTypeCustomization` implementations from `FPropertyEditorModule` so AI knows what's already customized.

Key APIs: `FField::GetMetaData(FName Key)`, `FField::GetMetaDataMap()`, `FProperty::PropertyFlags`, `FProperty::RepNotifyFunc`, `FProperty::GetCPPType()`

#### Gap 4: AnimMontage read/write ★★★★☆

**Why it matters:** All GAS-based projects (Lyra, most UE5 action games) drive character actions through Montages. Teams need to add sections, anim notifies, and slot assignments.

**Current state:** Zero tools. Asset can be found via `find_asset`/`list_assets` but cannot be read or mutated.

**Structure:** `UAnimMontage` contains:
- `TArray<FCompositeSection>` — named sections with start times
- `FAnimTrack` per `FSlotAnimationTrack` (slot name + `TArray<FAnimSegment>` with anim sequence refs)
- `TArray<FAnimNotifyEvent>` — per-notify: name, trigger time, duration, `UAnimNotify*` / `UAnimNotifyState*`
- Blend in/out settings

Key headers: `Animation/AnimMontage.h`, `Animation/AnimNotify.h`

**What's needed:** `read_anim_montage(asset)` → sections + notifies + slots; `add_montage_section(asset, name, start_time)`; `add_montage_notify(asset, notify_class, trigger_time)`; `set_montage_slot(asset, slot_name, anim_sequence_path, start_time, length)`.

#### Gap 5: Custom K2Node skeleton generation ★★★☆☆

**Why it matters:** Plugin authors and framework teams building custom ability/dispatch nodes need UK2Node subclasses. Currently: `transpile_blueprint` emits `// TODO[bpr-unsupported]` for any custom node class.

**What's needed (two parts):**
- `describe_k2node(class_path)` — reads a loaded custom UK2Node's pin layout (`AllocateDefaultPins` output), `GetMenuActions` category/tooltip, `IsNodePure`, `ExpandNode` result (what real nodes replaced it after compilation). Works against a live editor that has the custom node loaded.
- `generate_k2node_skeleton(pin_spec, target_function)` — given an array of `{name, direction, category, subCategory}` plus a static library function to call, emit compilable `.h`/`.cpp` pair implementing `AllocateDefaultPins`, `GetMenuActions`, and a canonical `ExpandNode` calling that function. Builds on the `transpile_blueprint` C++ emit infrastructure but targets the `UK2Node` API surface.

Key headers: `BlueprintGraph/Classes/K2Node.h`, `BlueprintGraph/Classes/K2Node_CallFunction.h`, `Kismet2/BlueprintEditorUtils.h`, `BlueprintActionDatabaseRegistrar.h`

---

## Part 3: UPROPERTY / UFUNCTION / Reflection Architecture

### Key specifier groups for the MCP server to model

**UPROPERTY: access tier (most important for BP generation)**

| Specifier | CPF bit | Blueprint can | Editor shows |
|---|---|---|---|
| `BlueprintReadWrite` | `CPF_BlueprintVisible` | Read + write | No |
| `BlueprintReadOnly` | `CPF_BlueprintVisible \| CPF_BlueprintReadOnly` | Read only | No |
| `EditAnywhere + BlueprintReadWrite` | both set | Read + write | Yes (all) |
| `EditDefaultsOnly + BlueprintReadOnly` | combined | Read only | CDO only |
| *(none)* | — | No access | No |

**Illegal combinations that AI models often generate:**
- `BlueprintReadWrite` on `private:` without `meta=(AllowPrivateAccess="true")` → UHT error
- `Server` + `Client`/`NetMulticast` without `Reliable` or `Unreliable` → UHT error
- `BlueprintNativeEvent` + `BlueprintImplementableEvent` on the same function → illegal
- `Config` on a struct member inside a non-Config UCLASS → silently ignored

**UFUNCTION: wildcard + CustomThunk patterns**

`CustomThunk` tells UHT to skip `execFoo` generation. Developer writes `IMPLEMENT_FUNCTION(ClassName, execFunctionName)` that uses:
- `P_GET_PROPERTY(FProperty*, PropName)` — extract a typed property from the VM frame
- `P_GET_STRUCT(StructType, VarName)` — extract a struct parameter
- `P_FINISH` — step past all params
- `P_NATIVE_BEGIN` / `P_NATIVE_END` — wraps native call

Combined `meta=(ArrayParm="ParamA,ParamB")` marks parameters as wildcard arrays — the K2 node (`UK2Node_CallArrayFunction`) resolves their types at expansion time from connected pin types.

### Reflection data the server should expose better

The UHT-generated `*.gen.cpp` stores per-property and per-parameter metadata arrays (`FMetaDataPairParam[]`) that are fully accessible at editor runtime via:
- `Property->GetMetaData(TEXT("EditCondition"))` 
- `Property->GetMetaDataMap()` → full TMap<FName,FString>
- `Property->PropertyFlags` → EPropertyFlags bitmask (decode as named booleans)
- `Property->RepNotifyFunc` → FName of repnotify function
- `Property->GetCPPType()` → exact C++ typename string
- CDO default: `Property->ExportTextItem_InContainer(ValueStr, CDO, nullptr, nullptr, PPF_None)`
- For complex types (FVector etc.): parse `"(X=0,Y=0,Z=0)"` text → `{"X":0,"Y":0,"Z":0}` JSON

**UClass registry population sequence** (relevant for performance):
1. Static initializers: `FRegisterCompiledInInfo` queues constructor pointers
2. `ProcessNewlyLoadedUObjects()`: drains queue, two-phase `Z_Construct_UClass_*` (Inner = shell, Outer = properties+functions)
3. `StaticRegisterNativesClassName()`: binds `execFoo` pointers to UFunction objects
4. `UStruct::Link()`: computes `Offset_Internal` for all properties, builds property chains

**Asset registry searchable fields** (free — no asset load needed):
- `BlueprintType`, `BlueprintNamespace`, `ImplementedInterfaces`, `NumReplicatedProperties` (from `AssetRegistrySearchable` UPROPERTY)
- `ParentClass`, `NativeParentClass` (stored as asset tags in generated class)
- Variable names, full signatures, CDO values: **require full asset load**

---

## Part 4: Performance Bottlenecks

### Bottleneck 1: 50 ms minimum per daemon call

**Root cause:** `BlueprintReaderCmdletServer.cpp:241` — `while (!DoneEvent->Wait(50))` polls the game-thread dispatch in 50 ms intervals. Every call spins at least once even for sub-millisecond ops (e.g. `get_stats`, `tools/list`).

**Impact:** 20 calls/second maximum throughput on the daemon path regardless of op cost. An `apply_ops` with 50 nodes would take ≥ 2.5 seconds (50 × 50ms worst case).

**Fix:** Reduce the poll interval to 5 ms for read ops; better yet, use a proper `FEvent::Wait(0)` + short-sleep loop that tightens after first yield. Alternative: move the wait off the connection thread using an async callback on completion.

### Bottleneck 2: O(N) filesystem stats in `list_blueprints`

**Root cause:** `BlueprintReaderCommandlet.cpp:11837` — `IsoDateForFile(FileOnDisk)` called per BP asset inside `RunListOp`. On a 1000-BP project this is 1000 `IFileManager::GetTimeStamp` syscalls serialized on the game thread.

**Fix:** `FAssetData` from the asset registry already has a `PackageSaveHash` tag in 5.0+ and the `FAssetPackageData` struct includes `DiskSize` and `FileVersionUE`. The `modified_iso` field could be derived from `FAssetData::PackageFileSummary.SavedByEngineVersion` without touching the filesystem. Alternatively: batch the stat calls into a single async filesystem pass before the game thread returns.

### Bottleneck 3: Temp-file I/O per daemon call

**Root cause:** `BlueprintReaderCmdletServer.cpp:197-251` — every call writes JSON to `<Intermediate>/bpr-cmdlet-<guid>.json`, the connection thread reads it back, then deletes it. For a large graph this adds 5–20 ms of filesystem I/O per call.

**Fix:** Pass the result back directly over the TCP connection instead of via temp file. The result JSON is already in memory after `EmitJson`; write it to the socket instead of to disk. Requires refactoring `EmitJson` to accept a write target (file path OR socket buffer). This is the single highest-value latency improvement for the warm-daemon path.

### Bottleneck 4: Uncached read tools

**Current cache coverage:** `ListBlueprints`, `ReadBlueprint`, `GetGraph`, `GetFunction`, `ListVariables`, `GetComponents`, `FindNode`.

**Not cached (worth adding the same TTL+mtime pattern):**
- `GetReferencers` / `GetDependencies` — asset-registry graph, stable until assets change
- `ReadDataTable`, `ReadDataAsset`, `ReadMaterial`, `ReadWidgetBlueprint`, `ReadBehaviorTree`, `ReadStateTree`, `ReadNiagaraSystem`, `ReadLevelSequence`, `ReadAnimBlueprint` — all pass-through today; all are `.uasset` files where mtime-based invalidation works identically to the BP cache
- `ListAssets`, `FindAsset` — pure asset-registry queries; completely stable between write ops

**Cache gap: TOCTOU window** — `SafeMtime` is called outside the cache lock (`CachingBlueprintReader.cpp:83-84`). Low risk but worth noting.

### Bottleneck 5: Game-thread serialization (fundamental, not fixable)

All `AsyncTask(GameThread)` dispatches serialize all ops. Independent `ReadBlueprint` calls for different assets cannot run concurrently. This is fundamental to UE's UObject model. The only mitigation is keeping the per-call cost as low as possible (Bottlenecks 1–3 above).

---

## Summary: Prioritized new roadmap items

### MCP spec compliance (standalone, CI-safe)

| ID | Item | Effort | Why |
|---|---|---|---|
| MCP-1 | `title` on all 254 tools | S | Display names in all clients; zero behavioral change |
| MCP-2 | Complete `readOnlyHint`/`destructiveHint`/`idempotentHint`/`openWorldHint` on all tools | S | Claude Code auto-approval; ChatGPT store compliance |
| MCP-3 | Input validation errors as `isError:true` results (not -32602) | S | Spec MUST; lets models self-correct |
| MCP-4 | `structuredContent` alongside `content[].text` for all tools that declare `outputSchema` | S | Gemini CLI requires it; currently errors on our tools |
| MCP-5 | `description` on `serverInfo`; `listChanged:true` in capabilities | S | Best practice; enables dynamic mode toggle without reconnect |
| MCP-6 | Streamable HTTP: add `/mcp` POST+GET endpoint; `MCP-Protocol-Version` header | M | Unblocks newer client versions; old `/sse` + `/message` deprecated since 2025-03-26 |
| MCP-7 | Tool description quality pass — add "when to use" + activation criteria to ~50 highest-traffic tools | M | +5.85% task success per arXiv study; high BM25 search impact |
| MCP-8 | `execution.taskSupport:"optional"` on long-running ops + basic `tasks/get` polling | L | Async compile/recreation; wait for 2026-07-28 stable before full impl |
| MCP-9 | `elicitation/create` for destructive write confirmation | M | Better UX; check client capability first |

### UE5 editor customization (LIVE, editor-module)

| ID | Item | Effort | Why |
|---|---|---|---|
| EDIT-1 | AnimBlueprint state machine read/write | L | Biggest gap; add `AnimGraph` dep; implement `add_anim_state`/`add_anim_transition` |
| EDIT-2 | Timeline read/write (UTimelineTemplate + UCurveFloat) | M | Very high frequency; no special module needed; zero write tools today |
| EDIT-3 | UPROPERTY metadata specifiers in class introspection | M | `FField::GetMetaData` already accessible; pure read extension |
| EDIT-4 | AnimMontage read/write | M | All GAS projects; `UAnimMontage` is UPROPERTY arrays; standard mutation pattern |
| EDIT-5 | Custom K2Node: `describe_k2node` (read) + `generate_k2node_skeleton` (emit) | L | Plugin authors; builds on transpiler C++ emit; lower frequency |

### Reflection enrichment (editor-module + standalone)

| ID | Item | Effort | Why |
|---|---|---|---|
| REFLECT-1 | Surface decoded property flags as named booleans (not raw hex) | S | AI can reason about BP/editor access without knowing bit positions |
| REFLECT-2 | Surface `RepNotifyFunc`, per-property `GetMetaDataMap()`, `GetCPPType()` in class introspection | S | Completes the reflection read surface for BP→C++ transpile accuracy |
| REFLECT-3 | CDO complex-type defaults as parsed JSON (FVector → `{X,Y,Z}` not `"(X=0,Y=0,Z=0)"`) | S | AI-friendly property defaults without string parsing |
| REFLECT-4 | Parameter-level metadata (`HidePin`, `DefaultToSelf`, `AutoCreateRefTerm`) in function introspection | S | Required for accurate `generate_k2node_skeleton` and function call node generation |

### Performance (mixed)

| ID | Item | Effort | Why |
|---|---|---|---|
| PERF-1 | Result over TCP not temp file — eliminate per-call disk I/O in daemon | M | Highest single-call latency win; need to refactor `EmitJson` to take a write target |
| PERF-2 | Reduce poll interval 50ms → 5ms in `DoneEvent->Wait` | S | 10× throughput improvement; trivial change |
| PERF-3 | Cache `GetReferencers`, `GetDependencies`, `ListAssets`, `FindAsset` | S | Same TTL+mtime pattern already implemented for BP reads |
| PERF-4 | Cache `ReadDataTable`, `ReadMaterial`, `ReadWidgetBlueprint`, `ReadBehaviorTree`, etc. | S | Same pattern; high-frequency tools currently uncached |
| PERF-5 | Replace `IsoDateForFile` per-BP with batch or asset-registry-based mtime in `list_blueprints` | M | Eliminates O(N) filesystem stats; 1000 BPs → 0 stat syscalls |
