# UE5.7 — what affects Blueprint tooling (bp-reader)

Research note for the bp-reader MCP server. Scope is deliberately
narrow: the surface area this server reads, writes, and transpiles —
not a general UE5.7 release-notes rehash.

**Methodology note.** The Anthropic-side `WebFetch` and `WebSearch`
tools were denied in this session, so anything that would normally
come from Epic's release notes / Epic Dev Community is flagged with
**"Not verified — see TODO"** below. Local-source claims (engine
checkout at `D:/Projects/Unreal Engine 5/` and the project at
`D:/Projects/UE5_MCP/`) are first-hand and were Glob/Grep-verified.

The local engine version is **UE 5.7.4** (verified by `CLAUDE.md:3`,
`README.md:424`, `wiki/Installation.md:14`,
`.claude/skills/bp-reader/SKILL.md:9`). The project's `.uproject`
pins `EngineAssociation: {8C2F4F06-47C3-B6B7-7D7F-5AB83BABA7D3}` —
the GUID-style binding to that source-built engine.

---

## 1. UE5.7 release timeline & supported toolchain

### Release timeline

UE5.7 GA shipped in late 2025. The local checkout is at patch
`5.7.4`. **Not verified — see TODO:** exact GA date and patch
cadence (would normally come from
`https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5.7-release-notes`,
which `WebFetch` was denied for).

### Windows toolchain

Confirmed from the project's `README.md` and the engine source-build
flow we use:

| Component        | Required version                                           | Verified from                          |
|------------------|------------------------------------------------------------|----------------------------------------|
| Visual Studio    | **VS 2022** (Game Development with C++ workload)           | `README.md:439`                        |
| Windows SDK      | Windows 10/11 SDK (bundled with VS workload)               | `README.md:439-442`                    |
| MSVC toolset     | v143 (VS 2022 default)                                     | Implied by VS 2022 requirement          |
| Engine target    | Source build, **not** the launcher binary build            | `wiki/Installation.md:14`              |
| Project target   | `BuildSettingsVersion.V6`, `TargetBuildEnvironment.Shared` | `CLAUDE.md:177-181`                    |

The MCP server (UE Program target, separate exe) builds via the same
UBT pipeline as the editor — there is no longer a CMake fork to keep
in sync.

**Not verified — see TODO:**

- Minimum MSVC patch level (Epic typically publishes a specific minor
  version per engine release; UE5.7 release notes likely call this
  out under "Build & Tools").
- `.NET SDK` version for UBT in UE5.7. UE5.4 baseline was .NET 6;
  5.5+ moved to .NET 8 (.NET 6 entered LTS-end in 11/2024). UBT in
  this repo runs as the engine ships it, so this is "whatever
  `Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe` was
  built against." Confirm against the release notes.
- Rider for Unreal Engine version. Rider has supported UE 5.x since
  2022; specific UE5.7 Rider-side support typically lands in a Rider
  point release within 1–2 months of UE GA.
- Mac / Xcode requirements, Linux clang version. Out of scope for
  this server (Windows-only build flow) but worth noting in the
  release notes.

### Engine source patches the bp-reader plugin depends on

These persist into 5.7.4 and must be re-applied on a fresh engine
clone. They are not committed engine files. Source:
`Plugins/BlueprintReader/Scripts/Patch-Engine.ps1`,
`README.md:475-499`.

- `Engine/Source/Developer/Windows/LiveCoding/LiveCoding.Build.cs`
- `Engine/Source/Developer/IOS/TVOSTargetPlatformSettings/TVOSTargetPlatformSettings.Build.cs`
- `Engine/Platforms/VisionOS/Source/Developer/VisionOSTargetPlatformSettings/VisionOSTargetPlatformSettings.Build.cs`

All three have the same shape: `PrivateIncludePaths.Add("Developer/IOS/...")`
declared as a string relative to `Engine/Source/` instead of the
module dir. Project-target builds resolve include paths from the
module dir, so the string lookups fail with `fatal error C1083`. The
patch rewrites each to `Path.Combine(ModuleDirectory, ...)` and is
idempotent.

If Epic fixes these upstream in 5.8, the patches become no-ops; the
script already detects the patched form and skips.

---

## 2. K2 / BlueprintGraph subsystem changes since UE5.4 baseline

### Current K2Node_* inventory (from the local 5.7.4 source)

The engine checkout under
`D:/Projects/Unreal Engine 5/Engine/Source/Editor/BlueprintGraph/Classes/`
contains **~100 `K2Node_*.h`** classes. Globbed inventory (the bp-reader
already handles the ones marked elsewhere; this is the raw set):

```
K2Node_ActorBoundEvent           K2Node_AddComponent             K2Node_AddComponentByClass
K2Node_AddDelegate               K2Node_AddPinInterface          K2Node_AssignDelegate
K2Node_AssignmentStatement       K2Node_AsyncAction              K2Node_BaseAsyncTask
K2Node_BaseMCDelegate            K2Node_BitmaskLiteral           K2Node_BreakStruct
K2Node_CallArrayFunction         K2Node_CallDataTableFunction    K2Node_CallDelegate
K2Node_CallFunction              K2Node_CallFunctionOnMember     K2Node_CallMaterialParameterCollectionFunction
K2Node_CallParentFunction        K2Node_CastByteToEnum           K2Node_ClassDynamicCast
K2Node_ClearDelegate             K2Node_CommutativeAssociativeBinaryOperator
K2Node_ComponentBoundEvent       K2Node_Composite                K2Node_ConstructObjectFromClass
K2Node_ConvertAsset              K2Node_Copy                     K2Node_CreateDelegate
K2Node_CustomEvent               K2Node_DeadClass                K2Node_DelegateSet
K2Node_DoOnceMultiInput          K2Node_DynamicCast              K2Node_EaseFunction
K2Node_EditablePinBase           K2Node_EnumEquality             K2Node_EnumInequality
K2Node_EnumLiteral               K2Node_Event                    K2Node_EventNodeInterface
K2Node_ExecutionSequence         K2Node_ExternalGraphInterface   K2Node_ForEachElementInEnum
K2Node_FormatText                K2Node_FunctionEntry            K2Node_FunctionResult
K2Node_FunctionTerminator        K2Node_GeneratedBoundEvent      K2Node_GenericCreateObject
K2Node_GenericToText             K2Node_GetArrayItem             K2Node_GetClassDefaults
K2Node_GetDataTableRow           K2Node_GetEnumeratorName        K2Node_GetEnumeratorNameAsString
K2Node_GetInputAxisKeyValue      K2Node_GetInputAxisValue        K2Node_GetInputVectorAxisValue
K2Node_GetNumEnumEntries         K2Node_GetSubsystem             K2Node_IfThenElse
K2Node_InputAction               K2Node_InputActionEvent         K2Node_InputAxisEvent
K2Node_InputAxisKeyEvent         K2Node_InputKey                 K2Node_InputKeyEvent
K2Node_InputTouch                K2Node_InputTouchEvent          K2Node_InputVectorAxisEvent
K2Node_InstancedStruct           K2Node_Knot                     K2Node_Literal
K2Node_LoadAsset                 K2Node_LocalVariable            K2Node_MacroInstance
K2Node_MakeArray                 K2Node_MakeContainer            K2Node_MakeMap
K2Node_MakeSet                   K2Node_MakeStruct               K2Node_MakeVariable
K2Node_MapForEach                K2Node_MathExpression           K2Node_Message
K2Node_MultiGate                 K2Node_PromotableOperator       K2Node_PureAssignmentStatement
K2Node_RemoveDelegate            K2Node_Select                   K2Node_Self
K2Node_SetFieldsInStruct         K2Node_SetForEach               K2Node_SetVariableOnPersistentFrame
K2Node_SpawnActor                K2Node_SpawnActorFromClass      K2Node_StructMemberGet
K2Node_StructMemberSet           K2Node_StructOperation          K2Node_Switch
K2Node_SwitchEnum                K2Node_SwitchInteger            K2Node_SwitchName
K2Node_SwitchString              K2Node_TemporaryVariable        K2Node_Timeline
K2Node_Tunnel                    K2Node_TunnelBoundary           K2Node_Variable
K2Node_VariableGet               K2Node_VariableSet              K2Node_VariableSetRef
```

Source: `Glob D:/Projects/Unreal Engine 5/Engine/Source/Editor/BlueprintGraph/Classes/K2Node_*.h`.

### Container-iteration nodes

`K2Node_MapForEach`, `K2Node_SetForEach`, `K2Node_InstancedStruct`
exist in this 5.7.4 checkout. Cross-referenced with
`docs/design/10-bp-to-cpp-node-coverage.md`:

- `K2Node_MapForEach`, `K2Node_SetForEach` — **not yet in the
  coverage chart**. These are the loop-over-map and loop-over-set
  variants that ship with the BlueprintGraph module (not as macros
  inside `EngineMacros`). bp-reader currently lowers `ForEachLoop`
  via the macro path; for native Map/Set iteration nodes, the BPIR
  walker will hit "unsupported" until handlers are added.
- `K2Node_InstancedStruct` — UE5.5+ shipping support for
  `FInstancedStruct`. bp-reader's BPIR has no specific lowering;
  decompile would emit `{unsupported}`. C++ idiom is straightforward
  (`InstancedStruct.GetMutable<T>()`), so adding it is a small task.

**Not verified — see TODO:** whether any of these landed *in* 5.7
specifically vs. earlier 5.5/5.6. The 5.4 baseline (per the doc
title's wording) did not include `K2Node_MapForEach`, but I cannot
verify the exact intermediate release without diffing against a 5.4
checkout (none available locally).

### `FBPVariableDescription` flag changes

The struct is the wire-level representation of a BP member variable
that bp-reader reads in `BlueprintIntrospector.cpp:64-79`. Fields it
currently reads:

- `VarName` (FName)
- `FriendlyName` (FString)
- `Category` (FName)
- `VarType` (FEdGraphPinType)
- `DefaultValue` (FString)
- `PropertyFlags` (uint64) — checked bits: `CPF_Net`,
  `CPF_Transient`, `CPF_Edit`, `CPF_BlueprintReadOnly`
- `HasMetaData("ExposeOnSpawn")`

**Not verified — see TODO:** any new fields or `CPF_*` flags
introduced between 5.4 → 5.7. The local `FBPVariableDescription`
header is in
`D:/Projects/Unreal Engine 5/Engine/Source/Editor/UnrealEd/Classes/Kismet2/BlueprintEditorUtils.h`
(or `Engine/Source/Editor/Kismet/...`); read access to engine source
was denied this session, so I couldn't diff. If any new
`PropertyFlags` matter to introspection (e.g. a new `CPF_*` for
push-model replication classification), update
`VariableDescToInfo` and the `FBPVariableInfo` wire struct.

### Deprecated `K2Node_Tunnel` / macro-graph idioms

The base `K2Node_Tunnel` class is **still alive** in 5.7.4 — it's
used internally as the base for `K2Node_MacroInstance`, the
collapsed-graph entry/exit pair, and the function entry/result
pair's parent boundary. bp-reader treats `K2Node_Tunnel` as
passthrough (#112 in the coverage chart). No deprecation alarm
expected.

**Not verified — see TODO:** Epic has been gradually pushing users
away from macro graphs (in favor of function libraries + interface
calls) for several releases; 5.7 release notes likely contain
deprecation language for at least the `K2Node_DoOnceMultiInput` /
`K2Node_MultiGate` flavors. bp-reader already classifies these as
unsupported / stateful (chart: ❌ for `K2Node_MultiGate`,
`K2Node_DoOnceMultiInput`), so no immediate write-side action.

### New / changed K2Node_* classes worth tracking

These show up in the 5.7.4 source and may not be in older bp-reader
coverage:

| Class                              | Status in bp-reader                                   |
|------------------------------------|-------------------------------------------------------|
| `K2Node_MapForEach`                | Unsupported (no entry in coverage chart)              |
| `K2Node_SetForEach`                | Unsupported (no entry in coverage chart)              |
| `K2Node_InstancedStruct`           | Unsupported (no entry in coverage chart)              |
| `K2Node_AddComponentByClass`       | Unsupported (no entry in coverage chart)              |
| `K2Node_ExternalGraphInterface`    | Unsupported — likely Composite-graph internal         |
| `K2Node_GeneratedBoundEvent`       | Unsupported — likely BlueprintGeneratedClass-internal |
| `K2Node_PromotableOperator`        | Unsupported — newer polymorphic math op (5.x feature) |
| `K2Node_Copy`                      | ❌ in chart (Specialized; falls through)              |
| `K2Node_DeadClass`                 | Unsupported (asset-stale node)                        |
| `K2Node_GetSubsystem`              | Generic CallFunction handles common cases             |

`K2Node_AddComponentByClass` (vs the older `K2Node_AddComponent`
that takes a CDO template) is the runtime-class-only variant. C++
lowering is `NewObject<UActorComponent>(this, ClassExpr)` followed
by `RegisterComponent()`. bp-reader's existing
`__bpr_add_component` sentinel could be extended to take the class
from a pin expression instead of a literal type, but the BPIR shape
would change.

---

## 3. Asset registry changes

bp-reader's `list` and `find` tools use `IAssetRegistry::GetAssets`
(`BlueprintIntrospector.cpp:582-584`,
`BlueprintReaderCommandlet.cpp:1016, 1314-1317`). Tag-based reads
(parent class, blueprint type, etc.) come from asset tags written
during cook / save.

### Key surface area for bp-reader

- `FAssetData::GetAssetByObjectPath(FSoftObjectPath)` — single-asset
  lookup, used everywhere we resolve a wire-format path to an asset.
- Asset tags read in `list_blueprints` (via the commandlet's
  `RunListOp`) — `ParentClass`, `BlueprintType`, `NumNativeClasses`,
  `NumNativeFunctions`, `NumNativeProperties`, etc.
- Tag cache invalidation: rebuild of the asset registry happens
  when an asset is saved.

**Not verified — see TODO:** UE5.7-specific asset-tag changes. The
release notes typically list new `UClass`-level tags exposed via
`UObject::GetAssetRegistryTags` (deprecated path) vs.
`GetExternalActorPath` and the `FAssetRegistryTagsContext` API
introduced in 5.4. Per the
`FAssetRegistryTagsContext` migration, `GetAssetRegistryTags` is
*deprecated but functional*; bp-reader still calls into the
`FAssetData::GetTagValue` API on the read side, which is unchanged.

If 5.7 adds new tags relevant to BP introspection (e.g. a
`UFunctionCount` or `bHasReplicatedProperties`-style tag),
`list_blueprints` could surface them without a full asset load.

### Asset registry tag-cache invalidation

The bp-reader project's two test BPs at `Content/AI/BP_TestEnemy.uasset`
and `BP_TestPickup.uasset` are reseeded via
`Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintReaderSeedCommandlet.cpp`.
After a reseed, the asset registry needs a `ScanModifiedAssetFiles`
or full rescan before tag-based lookups will see the new content;
the commandlet's seed path triggers a save which writes through to
the registry on the next `WaitForCompletion` call.

**Not verified — see TODO:** whether 5.7 changed the
`AssetRegistry`'s tag-cache invalidation triggers (e.g. new
`OnAssetTagsUpdated` events that the live backend could subscribe
to for cache invalidation).

---

## 4. EnhancedInput

### Default status

`Config/DefaultInput.ini:81-82` in this project pins:

```
DefaultPlayerInputClass=/Script/EnhancedInput.EnhancedPlayerInput
DefaultInputComponentClass=/Script/EnhancedInput.EnhancedInputComponent
```

So this project — and per the spec, the UE5.7 TPC template — uses
EnhancedInput by default. Legacy input nodes (`K2Node_InputAction`,
`K2Node_InputKey`, `K2Node_InputAxisEvent` etc.) are still
*available* but new projects start with EnhancedInput.

**Not verified — see TODO (TPC template inspection):** The local
TPC template files at
`D:/Projects/Unreal Engine 5/Templates/TP_ThirdPersonBP/` and
`D:/Projects/Unreal Engine 5/Templates/TP_ThirdPerson/` exist; the
TPC's `BP_ThirdPersonCharacter.uasset` should contain
`K2Node_EnhancedInputAction` nodes wired to the included
`IA_Move` / `IA_Jump` / `IA_Look` Input Actions. Confirming this
requires loading the uasset — out of scope for a research note,
but a follow-up bp-reader live-mode test
(`bp_reader.read /Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter`)
on a TPC import would confirm it.

The seed commandlet
`Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BPRoundtripSeedCommandlet.cpp`
exists to import the TPC template into `/Game/Imported/ThirdPerson/`;
that's the roundtrip plumbing for testing.

### EnhancedInput plugin location & K2Nodes

Confirmed from the local engine checkout:

| File                                                                                                       | Notes                                |
|------------------------------------------------------------------------------------------------------------|--------------------------------------|
| `Engine/Plugins/EnhancedInput/Source/EnhancedInput/Public/EnhancedInputComponent.h`                        | The `UEnhancedInputComponent` class  |
| `Engine/Plugins/EnhancedInput/Source/EnhancedInput/Public/InputAction.h`                                   | `UInputAction` asset class           |
| `Engine/Plugins/EnhancedInput/Source/EnhancedInput/Public/InputActionValue.h`                              | `FInputActionValue`                  |
| `Engine/Plugins/EnhancedInput/Source/InputBlueprintNodes/Public/K2Node_EnhancedInputAction.h`              | Public BP node                       |
| `Engine/Plugins/EnhancedInput/Source/InputBlueprintNodes/Private/K2Node_EnhancedInputActionEvent.h`        | Private event-shape variant          |
| `Engine/Plugins/EnhancedInput/Source/InputBlueprintNodes/Public/K2Node_GetInputActionValue.h`              | Pure read of the value pin           |
| `Engine/Plugins/EnhancedInput/Source/InputBlueprintNodes/Private/K2Node_InputActionValueAccessor.h`        | Internal helper                      |
| `Engine/Plugins/EnhancedInput/Source/InputBlueprintNodes/Private/K2Node_InputDebugKey{,Event}.h`           | Debug-only                           |
| `Engine/Plugins/EnhancedInput/Source/InputEditor/Public/EnhancedInputEditorSubsystem.h`                    | Editor-side input editor             |
| `Engine/Plugins/EnhancedInput/Source/EnhancedInput/Public/UserSettings/EnhancedInputUserSettings.h`        | Per-user input settings (5.x)        |

### What bp-reader surfaces today

From `Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/Decompile.cpp:2483-2790`:

The decompile pipeline has a full **EnhancedInput auto-lowering
pass**. It scans event graphs for `K2Node_EnhancedInputAction`
nodes, then generates:

1. One `UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Input")
   TObjectPtr<UInputAction> IA_<Name>;` per referenced action asset
   (deduplicated).
2. One `UFUNCTION() void On<Name><Trigger>(FInputActionValue Value);`
   per wired output exec pin (`Started`, `Triggered`, `Ongoing`,
   `Canceled`, `Completed`).
3. One `virtual void SetupPlayerInputComponent(UInputComponent*
   PlayerInputComponent) override;` per class, body wrapping
   `EIC->BindAction(...)` calls in a `Cast<UEnhancedInputComponent>`
   success block.

Per-class aggregation: bindings from all event graphs route into a
single `SetupPlayerInputComponent` override.

### What bp-reader does NOT yet surface

- **`K2Node_GetInputActionValue` standalone reads.** When a graph
  reads `InputActionValue` outside of a triggered event handler (e.g.
  on Tick), bp-reader's coverage chart has no entry for it. Likely
  falls through to generic `K2Node_CallFunction` handling, but the
  type punning back from `FInputActionValue` may not be smooth.
- **`InputMappingContext` asset references.** The decompile output
  does not auto-add `AddMappingContext(IMC_Default, 0)` to
  `BeginPlay`. The agent has to wire the IMC manually, then assign
  it via Enhanced Input Subsystem.
- **`UInputModifier` / `UInputTrigger` overrides** on InputActions.
  bp-reader treats the InputAction as an opaque asset reference; per-
  modifier / per-trigger settings live in the InputAction asset's
  serialized data, not surfaced.
- **Legacy `K2Node_InputAction` / `K2Node_InputKey` / `K2Node_InputAxisEvent`**
  — explicitly ❌ in the coverage chart with a "manual port to
  EnhancedInput" guidance.

**Not verified — see TODO:** UE5.7-specific EnhancedInput changes
(e.g. new `UInputTrigger` subclasses, new `ETriggerEvent` values
beyond `Started/Triggered/Ongoing/Canceled/Completed`,
`PlayerMappableInputConfig` → `PlayerMappableKeySettings` migration
status). The 5.7 release notes likely cover these in an "Input"
section.

---

## 5. Async actions / `UBlueprintAsyncActionBase` changes

### Current bp-reader handling

`Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintIntrospector.cpp:131-237`
implements `ExtractAsyncTaskDelegateParams`. For each
`K2Node_BaseAsyncTask` node, it:

- Resolves `UFunction* Factory = AsyncNode->GetFactoryFunction()`.
- Casts the return type to `FObjectProperty` → gets `UClass* ProxyClass`.
- Iterates `TFieldIterator<FMulticastDelegateProperty>` over
  `ProxyClass`.
- For each delegate, walks `SignatureFunction`'s `CPF_Parm`
  properties (skipping return), runs `K2Schema->ConvertPropertyToPinType`,
  serializes `{name, type}` entries.
- Writes the whole nested structure as a JSON-stringified `delegate_params`
  entry on the node's `Extras` map.

The MCP-side decompile (`Decompile.cpp:1298-1430`) reads this and
emits the standard async-task 3-sentinel sequence:

- `__bpr_async_factory` — `auto* Action = FactoryClass::Create(args);`
- `__bpr_async_bind` — `Action->OnPin.AddDynamic(this, &ThisClass::Cb);`
  (one per wired output exec pin)
- `__bpr_async_activate` — `Action->Activate();`

Each wired output exec generates a `UFUNCTION()` continuation method
whose `inputs[]` mirror the per-delegate param signature. This was
merged at commit `dfefc71` ("feat(introspector): surface async-task
per-delegate parameter signatures"). The walker change to chain
ExecTarget destination pins for auto-lowering was `f0a412e`.

### What's still ❌

From `docs/design/10-bp-to-cpp-node-coverage.md:251`:

> `K2Node_LatentGameplayTaskCall`, `K2Node_LatentAbilityCall` — same
> continuation-passing shape with multicast-delegate exec pins;
> auto-lowered the same way **when surfaced via `K2Node_BaseAsyncTask`
> introspection**. Some GAS-specific nodes may not derive from
> `BaseAsyncTask` directly and will need their own probe.

### UE5.7 changes

**Not verified — see TODO:** Whether 5.7 added any new
`UBlueprintAsyncActionBase` subclasses to engine modules that
bp-reader should detect. Common candidates worth checking:

- **Online subsystem** (now mostly OSSv2-era) — async login,
  async session find/join. These typically derive from
  `UBlueprintAsyncActionBase` via a factory like
  `UAsyncAction_LoadObject`.
- **`UAsyncAction_LoadGameFromSlot`** — save/load API.
- **HTTP / WebSocket** async helpers in the new OnlineFramework v2.
- **Iris replication** — likely no new BP-visible async actions
  (Iris is mostly under-the-hood), but worth confirming.

If 5.7 introduced new `K2Node_BaseAsyncTask`-derived spawn points
beyond what bp-reader probes, the existing
`ExtractAsyncTaskDelegateParams` will *still* handle them
generically — the introspection walks the proxy class reflection,
not a hardcoded list. The only change needed would be if a 5.7
introduces a new `K2Node_*` async variant that does *not* derive
from `K2Node_BaseAsyncTask`.

---

## 6. Gameplay Ability System (GAS) tagged-event changes

GAS is out of scope for bp-reader today — there's no
`gas-*` tool. The introspector reads `K2Node_LatentAbilityCall`
generically via `K2Node_BaseAsyncTask` reflection, which covers
basic ability tasks.

**Not verified — see TODO:** UE5.7 GAS changes typically center on:

- **Tag-table refactors** — `FGameplayTag` cooking, container
  performance.
- **New `UGameplayTask` subclasses** — handled the same as any
  `BaseAsyncTask` (proxy-class reflection), no bp-reader-specific
  work needed.
- **`FGameplayCue` event routing** — purely runtime, no BP node
  changes.

If full GAS support becomes a goal, a dedicated `gas_list_abilities`
/ `gas_read_ability` tool family would belong in a new module
analogous to the EnhancedInput auto-lowering pass. For now, the
generic async-task lowering covers the common `K2Node_LatentAbilityCall`
patterns.

---

## 7. CommonUI status

### Plugin presence in the engine

`D:/Projects/Unreal Engine 5/Engine/Plugins/Runtime/CommonUI/CommonUI.uplugin`
exists in the 5.7.4 checkout, with a fully populated `Source/` tree
(CommonInput, CommonUI, CommonUIEditor modules). Standard widget
classes confirmed: `CommonActivatableWidget`, `CommonButtonBase`,
`CommonRichTextBlock`, `CommonListView`, `CommonTileView`,
`CommonTreeView`, `CommonNumericTextBlock`, etc.

### Production-ready in 5.7?

**Not verified — see TODO.** CommonUI has been *technically* shipped
as a Runtime plugin since 4.x, was promoted out of `Experimental`
in 5.0, and Epic has used it in shipping titles since
Lyra/Fortnite. By 5.7 it's safely "production-ready" by any
practical measure, but a definitive Epic statement in the 5.7
release notes (e.g. "removed from beta") wasn't accessible this
session.

### Effect on bp-reader's `read_widget_blueprint`

Confirmed wire shape from `BlueprintReaderCommandlet.cpp:3382-3404`
and the tool registration in `BlueprintTools.cpp:3138+`:

`read_widget_blueprint` loads a `UWidgetBlueprint`, walks
`WidgetTree`, and emits a JSON tree of widget instances. The tool
is class-agnostic — any `UWidget` subclass is dumped via its
`UPROPERTY` reflection.

So `UCommonUserWidget`, `UCommonButtonBase`,
`UCommonActivatableWidget` etc. all work *as widgets* under
`read_widget_blueprint`. What bp-reader does **not** know:

- **Activation state** — `UCommonActivatableWidget` has
  `BP_OnActivated`, `BP_OnDeactivated`, `BP_OnHandleBackAction`
  virtuals that drive the activation lifecycle. These are
  `K2Node_Event` overrides and show up in event graphs as usual,
  but bp-reader's coverage chart treats `K2Node_Event (override of
  parent UFUNCTION)` correctly (`✅`) so transpile handles them.
- **`UCommonInputActionDataTable`** rows — bp-reader's
  `read_data_table` covers any `UDataTable`, no CommonUI-specific
  handling needed.
- **Style assets** (`UCommonButtonStyle`, etc.) — surfaced as
  asset references on the relevant widget property, same as
  any other `TObjectPtr<U...>`.

No bp-reader changes needed for CommonUI specifically. The 5
`UMG widgets` tools (`read_widget_blueprint`, `add_widget`,
`set_widget_property`, `bind_widget_event`,
`compile_widget_blueprint` per `README.md:54`) work uniformly.

---

## 8. Editor extensibility API changes

### `FUICommandList`, `FSlateApplication`

bp-reader has **no direct usage** of these (grep result: only
mention is in a planning doc, not a source file). The plugin's
editor extensions go through:

- Commandlets (`UCommandlet` subclasses) — invoked headless via
  `-run=BPR`.
- Live-mode TCP listener (`BlueprintReaderLiveServer.cpp`) —
  pure socket I/O, no Slate.
- Console commands (`bp_reader.list`, `bp_reader.read`) registered
  via `FAutoConsoleCommand` — no `FUICommandList`.

So `FUICommandList` / `FSlateApplication` changes in 5.7 don't
break bp-reader directly. If a future tool adds in-editor UI
(menu entries, toolbar buttons), that's where the dependency
appears.

**Not verified — see TODO:** UE5.7 Slate API breaks. Slate
typically has minor `SWidget` virtual-function signature shuffles
each release (e.g. `OnPaint` argument changes, `FArrangedChildren`
mutation rules). None affect bp-reader as long as we stay headless.

### `EditorUtilityWidget`

No bp-reader handling. The class derives from `UEditorUtilityWidget`
(itself a `UUserWidget`), so `read_widget_blueprint` should
handle it the same as any other WBP. Live-mode runs in a normal
editor process, so EUWs are loadable via the standard
`StaticLoadObject` path.

**Not verified — see TODO:** Any new `UEditorUtilityWidget` BP-side
APIs in 5.7 (e.g. new auto-discovered entry-point functions
that the BP author overrides). Likely none — EUW is in
maintenance mode.

### Other editor APIs bp-reader touches

For completeness, since these are the ones we'd notice if 5.7
broke them:

- `FBlueprintEditorUtils::AddMemberVariable` — used by
  `add_variable`.
- `FBlueprintEditorUtils::AddFunctionGraph` — used by
  `add_function` (`BlueprintReaderCommandlet.cpp` adds a
  `K2Node_FunctionResult` for outputs).
- `FKismetEditorUtilities::CompileBlueprint` — used by every
  write op via `CompileAndSaveBlueprint`.
- `UPackage::SavePackage` / `IAssetTools::DuplicateAsset` —
  `save_blueprint`, `duplicate_blueprint`.

All come from the `UnrealEd` module per
`BlueprintReaderEditor.Build.cs:67-103`. The build script
explicitly avoids the unrelated `Kismet` / `KismetCompiler`
modules.

---

## 9. bp-reader impact summary

Cross-referenced with [`docs/design/10-bp-to-cpp-node-coverage.md`](../design/10-bp-to-cpp-node-coverage.md).

### Deprecated node classes still appearing in older assets

bp-reader must continue to handle these — the 5.7 engine still
loads them from older `.uasset` files; the introspector should
emit `unsupported` with a useful hint rather than crashing.

| Class                          | Replacement in 5.7                      | bp-reader action |
|--------------------------------|-----------------------------------------|------------------|
| `K2Node_InputAction`           | `K2Node_EnhancedInputAction`            | Already ❌ with port hint |
| `K2Node_InputKey`              | `K2Node_EnhancedInputAction`            | Already ❌ |
| `K2Node_InputAxisEvent`        | `K2Node_EnhancedInputAction` + value modifiers | Already ❌ |
| `K2Node_InputAxisKeyEvent`     | Same                                    | Already ❌ |
| `K2Node_InputTouch{,Event}`    | EnhancedInput touch chord               | Already ❌ |
| `K2Node_InputVectorAxisEvent`  | EnhancedInput axis with `FInputActionValue::Axis3D` | Already ❌ |
| `K2Node_DeadClass`             | n/a — asset-stale placeholder           | Emit `unsupported` (skip in walk) |
| `K2Node_MatineeController`     | Sequencer (`UMovieSceneSequence`)       | ➖ Out of scope per chart |

### New node classes that lack BPIR coverage

These appear in the 5.7.4 local source and aren't in the coverage
chart. Priority order: container-iteration nodes are the most
likely to bite users mid-transpile.

| Class                              | Suggested C++ lowering                              | Priority |
|------------------------------------|-----------------------------------------------------|----------|
| `K2Node_MapForEach`                | `for (auto& [Key, Val] : Map) { ... }`              | High     |
| `K2Node_SetForEach`                | `for (auto& Elem : Set) { ... }`                    | High     |
| `K2Node_InstancedStruct`           | `InstancedStruct.GetMutable<FT>()` / setter         | Medium   |
| `K2Node_AddComponentByClass`       | `NewObject<UActorComponent>(this, ClassPin)` + Register | Medium |
| `K2Node_PromotableOperator`        | Lower to typed binary op based on resolved pin type | Low — pure exists, generic CallFunction may cover |
| `K2Node_ExternalGraphInterface`    | Internal — skip                                     | Low      |
| `K2Node_GeneratedBoundEvent`       | Internal — skip                                     | Low      |
| `K2Node_GetSubsystem`              | `GetGameInstance()->GetSubsystem<U...>()`           | Low — generic CallFunction may cover |

### Risks if 5.7 changes are unverified

The biggest risk is silent behavior change on the introspection
side:

- A new `FBPVariableDescription` field that we don't read → the
  generated C++ omits a UPROPERTY specifier the BP relied on.
- A new `CPF_*` flag with semantics we should classify (e.g. push-
  model replication) → wire format misses a boolean.
- A new asset-tag key in the registry → `list_blueprints` misses
  classification info that would speed up filters.
- A new `K2Node_*` subclass on a high-traffic shape (e.g. a new
  cast variant) → silent `{unsupported}` in transpile output.

Mitigation: when running against 5.7 for the first time, exercise
`read_blueprint` against a known-complex asset (TPC `BP_ThirdPersonCharacter`
post-import via `BPRoundtripSeedCommandlet`), diff the wire output
against the chart, and add coverage for anything new.

### Action items derived from this audit

1. **Add BPIR + lowering for `K2Node_MapForEach` and
   `K2Node_SetForEach`.** Existing macro `ForEachLoop` handler is
   a near-copy starting point.
2. **Add BPIR + lowering for `K2Node_InstancedStruct`.** Single-
   sentinel emit (`__bpr_instanced_struct_get`).
3. **Verify `FBPVariableDescription` flag set against 5.7.4
   source** (requires Read access to engine source headers —
   denied this session; deferred).
4. **Cross-check `K2Node_*` class list against 5.4 baseline** —
   need a 5.4 checkout to diff. Suggest adding a script that
   inventories the engine's BlueprintGraph module and writes the
   diff into `docs/research/` periodically.
5. **Run TPC live-mode read** to confirm template uses
   `K2Node_EnhancedInputAction` (vs. legacy) and capture wire
   output as a fixture.
6. **Resolve the "Not verified" TODOs above** by fetching the 5.7
   release notes — either via an environment where WebFetch is
   permitted, or by checking out the doc-site repo directly.

---

## See also

- [`docs/design/10-bp-to-cpp-node-coverage.md`](../design/10-bp-to-cpp-node-coverage.md) — current node coverage chart, the canonical "what's lowered today" reference.
- [`docs/design/07-bpir-and-transpile.md`](../design/07-bpir-and-transpile.md) — BPIR contract.
- [`CLAUDE.md`](../../CLAUDE.md) — maintainer-level gotchas (the `UserConstructionScript` note at line 277 is a UE 5.7 specific quirk).
- [`Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintIntrospector.cpp`](../../Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintIntrospector.cpp) — the entry point for everything the introspector reads off a `UBlueprint`.
- [`Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/Decompile.cpp`](../../Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/Decompile.cpp) — the MCP-side BP→BPIR walker and the EnhancedInput / async-task auto-lowering passes.
- [`Plugins/BlueprintReader/Scripts/Patch-Engine.ps1`](../../Plugins/BlueprintReader/Scripts/Patch-Engine.ps1) — the three Build.cs patches the 5.7.4 source needs.
