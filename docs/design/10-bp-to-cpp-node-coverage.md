# BP -> C++ Node Coverage Chart

This document inventories every K2Node_* class commonly encountered in
Blueprint graphs and shows how the `transpile_function` /
`transpile_blueprint` pipeline lowers each one to C++. Cross-referenced
with Hazelight Angelscript syntax where the same construct exists.

**Status legend:**

| Symbol | Meaning |
|---|---|
| ✅ | Fully supported -- emits compileable C++ matching the BP semantics. |
| 🔄 | Passthrough -- the node has no C++ analog but the pipeline transparently traces through it (knots, tunnels). |
| ⚠️ | Approximation -- emits C++ that captures the intent but may need manual fixup (e.g. typed arg pack, async refactor). |
| 🔵 | Sentinel -- decompile emits a `__bpr_*` call sentinel that CppEmit lowers; no manual port needed for common cases. |
| ❌ | Unsupported -- emits `{unsupported}` BPIR + a TODO comment + a sidecar entry. Manual port required. |
| ➖ | Out of scope -- internal / editor-only node that doesn't appear in shipping BP graphs. |

PR numbers refer to the merge that added the support.

---

## Variable nodes

| K2 class | Status | C++ idiom | Notes |
|---|---|---|---|
| `K2Node_VariableGet` | ✅ | `MemberName` or `this->MemberName` (shadow-aware) | Scope tagged input/member/local; `this->` injected when LHS shadows a param (#104). |
| `K2Node_VariableSet` | ✅ | `<Name> = <expr>;` | Same scope + shadow handling. |
| `K2Node_AssignmentStatement` | ✅ | `<Lhs> = <Rhs>;` | Lowered to existing `{set, to}` form (#112). |
| `K2Node_VariableSetRef` | ❌ | -- | Rare; falls through to unsupported. |
| `K2Node_LocalVariable` | ✅ | declared via function `locals[]` | Surfaced through BPIR's `locals` field. |
| `K2Node_TemporaryVariable` | ❌ | -- | Internal node, rare in user-authored graphs. |
| `K2Node_PureAssignmentStatement` | ❌ | -- | Specialized internal use. |

## Function call nodes

| K2 class | Status | C++ idiom | Notes |
|---|---|---|---|
| `K2Node_CallFunction` | ✅ | `Owner::Func(args)` after path-strip + sanitize | Stripping of `/Script/Mod.` + `/Game/Path.Asset_C` (#104). |
| `K2Node_CallFunction` (cross-BP target) | ⚠️ | `FindFunction(TEXT(...)) -> ProcessEvent` reflection stub | When owner is a BP class only (#104). Arg types come out as `auto` since BPIR can't recover them. |
| `K2Node_CallParentFunction` | ✅ | `Super::Func(args)` | Existing handler. |
| `K2Node_CallArrayFunction` | ✅ | Method-call alias (e.g. `Arr.Add(x)`) | Routed via `MethodCallAliases()` map. |
| `K2Node_CallMaterialParameterCollectionFunction` | ⚠️ | `UKismetMaterialLibrary::Set*` calls | Generic CallFunction handles in most cases. |
| `K2Node_CallFunctionOnMember` | ⚠️ | `<Member>->Func(args)` | Generic CallFunction; receiver picked from `self` pin. |
| `K2Node_CallDataTableFunction` | ✅ | `__bpr_get_data_table_row` sentinel | Statement-form `if (auto* Row = DataTable->FindRow<FRowType>(...))` block (#103 era). |
| `K2Node_Message` | ❌ | -- | Interface call. Fall through. |

## Flow control

| K2 class | Status | C++ idiom | Notes |
|---|---|---|---|
| `K2Node_IfThenElse` | ✅ | `if (<cond>) { ... } else { ... }` | |
| `K2Node_ExecutionSequence` | ✅ | Inlined ordered statements (no markers) | Empty branches dropped (#103). |
| `K2Node_MultiGate` | ❌ | -- | Stateful (requires int32 member); structured TODO. |
| `K2Node_DoOnceMultiInput` | ❌ | -- | Stateful; structured TODO. |
| `K2Node_Switch{Enum,Integer,String,Name}` | ✅ | `switch (<sel>) { case X: ... default: ... }` | |
| `K2Node_Select` | ✅ | Ternary (N=2) or chained ternaries (N>2) | `__bpr_select_*` sentinels (#112). |
| `K2Node_DynamicCast` (statement) | ✅ | `if (auto* AsX = Cast<X>(...)) { ... } else { ... }` | Path-strip + identifier sanitize on `to` + `as` (#103/#104). |
| `K2Node_DynamicCast` (expression) | ✅ | `Cast<X>(...)` |  |
| `K2Node_ClassDynamicCast` | ⚠️ | `Cast<X>(<UClass*>->GetSomething())` | Treated as DynamicCast variant; tighter handling pending. |

## Loops (via macros)

| Macro (under `K2Node_MacroInstance`) | Status | C++ idiom | Notes |
|---|---|---|---|
| `ForEachLoop` | ✅ | `for (auto& Element : <Array>) { ... }` | (#103) |
| `ForEachLoopWithBreak` | ✅ | Same; `break;` inside body honored | |
| `ReverseForEachLoop` | ✅ | `for (int32 i = Array.Num()-1; i >= 0; --i)` shape | Stage 1 keeps the same for-each shape; reverse flag stored on the BPIR for future tightening. |
| `WhileLoop` | ✅ | `while (<cond>) { ... }` | |
| `IsValid` | ✅ | `if (IsValid(X)) { ... } else { ... }` | (#103) |
| `IsValidClass` | ❌ | -- | Falls through to unsupported. Add as a near-zero-cost follow-up. |
| `Gate` | ❌ | -- | Stateful (bool member + branch). |
| `DoOnce` | ❌ | -- | Stateful (bool guard). |
| `DoN` | ❌ | -- | Stateful (counter). |
| `FlipFlop` | ❌ | -- | Stateful (toggle member). |
| `ForEachElementInEnum` | ❌ | -- | Could lower to a for-loop over `EnumType::Type::MAX`; not done. |

## Casting / type checks

| K2 class | Status | C++ idiom | Notes |
|---|---|---|---|
| `K2Node_DynamicCast` | ✅ | See above. | |
| `K2Node_ClassDynamicCast` | ⚠️ | See above. | |
| `K2Node_CastByteToEnum` | ❌ | -- | Lower to `static_cast<EFoo>(<byte>)`. Pending. |

## Structs

| K2 class | Status | C++ idiom | Notes |
|---|---|---|---|
| `K2Node_MakeStruct` | ✅ | `FFoo{ /*Field=*/<expr>, ... }` | |
| `K2Node_BreakStruct` | ✅ | `<expr>.<field>` (uses BPIR `member`) | (#111) |
| `K2Node_BreakStructHelper` | ✅ | Same as BreakStruct (substring match) | |
| `K2Node_SetFieldsInStruct` | ❌ | -- | Could lower to sequence of `Struct.Field = <expr>;` calls. Pending. |
| `K2Node_StructMemberGet` | ❌ | -- | Similar to BreakStruct + indexing. |
| `K2Node_StructMemberSet` | ❌ | -- | Similar to a chain of `set` statements. |

## Containers

| K2 class | Status | C++ idiom | Notes |
|---|---|---|---|
| `K2Node_MakeArray` | ✅ | `TArray<T>{ ... }` brace init | |
| `K2Node_MakeSet` | ✅ | `TSet<T>{ ... }` brace init | (#111) |
| `K2Node_MakeMap` | ✅ | `TMap<K,V>{ {k,v}, ... }` brace init | (#111) |
| `K2Node_MakeContainer` | ✅ | Dispatched per container kind | |
| `K2Node_GetArrayItem` | ✅ | `<Array>[<idx>]` via BPIR `index` | |

## Constants / literals / self

| K2 class | Status | C++ idiom | Notes |
|---|---|---|---|
| `K2Node_Literal` | ✅ | Scalar literal pass-through; object-ref strings preserved | |
| `K2Node_EnumLiteral` | ✅ | `EEnumType::ValueName` | (#112) |
| `K2Node_LiteralEnum` | ✅ | Same as EnumLiteral (substring match) | |
| `K2Node_BitmaskLiteral` | ❌ | -- | Could lower to `1 << N` |  patterns. |
| `K2Node_Self` | ✅ | `this` | |

## Events

| K2 class | Status | C++ idiom | Notes |
|---|---|---|---|
| `K2Node_Event` (override of parent UFUNCTION) | ✅ | `virtual void <Name>() override` (void-return whitelist) | (#109) |
| `K2Node_Event` (other) | ⚠️ | `UFUNCTION() void <Name>(args)` | Bare UFUNCTION fallback. |
| `K2Node_CustomEvent` | ✅ | `UFUNCTION(BlueprintCallable) void <Name>()` | Output pins -> input params via FunctionEntry handler (#104). |
| `K2Node_ActorBoundEvent` | ⚠️ | Bound handler + `AddDynamic` | Resolved on Add/RemoveDelegate side. |
| `K2Node_ComponentBoundEvent` | ⚠️ | Same as ActorBoundEvent | |
| `K2Node_InputAction` / `_InputKey` / `_InputAxis` / `_InputTouch` / `_InputVectorAxisEvent` | ❌ | -- | EnhancedInput migration recommended; manual port for now. |
| `K2Node_InputActionEvent` and event-shaped variants | ❌ | -- | Same as above. |

## Delegates

| K2 class | Status | C++ idiom | Notes |
|---|---|---|---|
| `K2Node_CallDelegate` (Broadcast) | ✅ | `<Delegate>.Broadcast(args)` | (#103) |
| `K2Node_AddDelegate` | ✅ | `<Delegate>.AddDynamic(this, &ThisClass::<Handler>)` | (#103) |
| `K2Node_RemoveDelegate` | ✅ | `<Delegate>.RemoveDynamic(...)` | (#103) |
| `K2Node_ClearDelegate` | ✅ | `<Delegate>.Clear()` | (#103) |
| `K2Node_CreateDelegate` | ✅ | Resolved as handler ref by Add/Remove sites | (#103) |
| `K2Node_BaseMCDelegate` | ✅ | Base class for the four above; intermediate | (#103) |
| `K2Node_AssignDelegate` | ⚠️ | Single-delegate variant; falls through; rare | |
| `K2Node_DelegateSet` | ⚠️ | Same | |
| Multicast delegate **variable** declaration | ✅ | `DECLARE_DYNAMIC_MULTICAST_DELEGATE[_NParams](F<Name>[, T1, P1, ...]); FOnX MyDelegate;` | (#105, #150) Plugin introspector reads SignatureFunction params; codegen picks `_OneParam`/`_TwoParams`/... up to `_NineParams` automatically. |

## Function definition

| K2 class | Status | C++ idiom | Notes |
|---|---|---|---|
| `K2Node_FunctionEntry` | ✅ | Generates function signature; output pins resolve to param names | (#104) |
| `K2Node_FunctionResult` | ✅ | Generates `return` statement / out-param assignments | (#103: multi-output -> out-params; no `std::make_tuple`) |
| `K2Node_FunctionTerminator` | ✅ | Same as Entry/Result base | |

## Object creation / lifecycle

| K2 class | Status | C++ idiom | Notes |
|---|---|---|---|
| `K2Node_SpawnActor` | ✅ | `GetWorld()->SpawnActor<AActor>(Class, Xform)` or w/ `FActorSpawnParameters` lambda | Sentinel-lowered. |
| `K2Node_SpawnActorFromClass` | ✅ | Same as above | |
| `K2Node_ConstructObjectFromClass` | ✅ | `NewObject<T>(Owner)` | Sentinel-lowered. |
| `K2Node_GenericCreateObject` | ✅ | Same as ConstructObject | |
| `K2Node_AddComponent` | ✅ | `NewObject<T>(...)` + `RegisterComponent` + `AttachToComponent` block | Statement-form sentinel. |
| `K2Node_DestroyActor` | ✅ | `<Target>->Destroy()` | |
| `K2Node_DestroyComponent` | ✅ | `<Target>->DestroyComponent()` | Generic CallFunction. |

## Asset loading

| K2 class | Status | C++ idiom | Notes |
|---|---|---|---|
| `K2Node_LoadAsset` / `K2Node_LoadAssetClass` | ⚠️ | TODO + canonical hint (FStreamableManager / RequestAsyncLoad) | Async control flow; manual refactor. |
| `K2Node_ConvertAsset` | ⚠️ | -- | Falls through. |

## Macros / composites / passthrough

| K2 class | Status | C++ idiom | Notes |
|---|---|---|---|
| `K2Node_MacroInstance` | ✅ / ❌ | See "Loops" + "IsValid" table; unknown macros emit unsupported | |
| `K2Node_Composite` | ➖ | -- | Collapsed nodes; BP-side authoring artifact, not seen post-compile. |
| `K2Node_Tunnel` | 🔄 | Passthrough (like Knot) | (#112) |
| `K2Node_TunnelBoundary` | ➖ | -- | Compiler artifact only. |
| `K2Node_Knot` | 🔄 | Passthrough; expr+stmt forms | |

## Special / utility

| K2 class | Status | C++ idiom | Notes |
|---|---|---|---|
| `K2Node_FormatText` | ✅ | `FText::Format(LOCTEXT(...), Args)` with FFormatNamedArguments | `__bpr_format_text` sentinel. |
| `K2Node_GetClassDefaults` | ✅ | `GetDefault<T>()->Field` | `__bpr_get_class_defaults` sentinel. |
| `K2Node_GetDataTableRow` | ✅ | `if (auto* Row = DataTable->FindRow<FRowType>(...)) { ... }` | Sentinel. |
| `K2Node_Timeline` | ❌ | -- | Latent-action restructure required; sidecar TODO. |
| `K2Node_EaseFunction` | ❌ | -- | Could lower to `UKismetMathLibrary::Ease(...)`. Pending. |
| `K2Node_MathExpression` | ⚠️ | -- | Currently emits as a `K2Node_CallFunction` chain after BP compile. |
| `K2Node_Copy` | ❌ | -- | Specialized; falls through. |
| `K2Node_MatineeController` | ➖ | -- | Legacy; replaced by Sequencer. |

## RPC / replication

| Specifier on `K2Node_FunctionEntry`'s ufunction_specifiers | Status | C++ idiom | Notes |
|---|---|---|---|
| `Server` / `Client` / `NetMulticast` | ✅ | `_Implementation` suffix on impl, bare name on header decl | (#107) |
| `BlueprintNativeEvent` | ✅ | `_Implementation` suffix on impl | (#110) |
| `BlueprintImplementableEvent` | ✅ | Header decl only; impl skipped (UE generates the dispatcher) | (#110) |
| `BlueprintAuthorityOnly` | ✅ | No suffix; emits as normal UFUNCTION | (#107 — explicitly not treated as RPC) |
| `Replicated` (variable-level) | ✅ | `UPROPERTY(Replicated)` + `GetLifetimeReplicatedProps` body | Pre-existing. |
| `ReplicatedUsing` | ✅ | `UPROPERTY(ReplicatedUsing=OnRep_X)` + OnRep handler decl | Pre-existing. |

## Async / latent

| K2 class | Status | C++ idiom | Notes |
|---|---|---|---|
| `K2Node_BaseAsyncTask` | ❌ | -- | Latent UBlueprintAsyncActionBase pattern. Falls through. |
| `K2Node_AsyncAction` | ❌ | -- | Same. UE generates the dispatcher; C++ port requires explicit Activate/OnCompleted wiring. |
| `K2Node_LatentGameplayTaskCall` | ❌ | -- | UGameplayTask subclass; manual port. |
| `K2Node_CallFunction` (latent: Delay / RetriggerableDelay / DelayUntilNextTick) | ❌ | -- | Structured TODO with canonical FTimerHandle hint. |

## Class / interface emission (whole-class transpile)

| Pattern | Status | C++ idiom | Notes |
|---|---|---|---|
| UCLASS header w/ Blueprintable | ✅ | `UCLASS(MinimalAPI, Blueprintable[, meta=(...)])` | `uclass_meta` arg (#106). |
| Module API macro | ✅ | `MYGAME_API` | |
| Class-name prefix | ✅ | `A<Prefix><CamelName>` | `class_name_prefix` arg (#106). |
| Class-name suffix | ✅ | `<Name>_Generated` | Configurable, default `_Generated`. |
| Variable Categories | ✅ | `Category="..."` with default + remap | `category_default` / `category_remap` args (#106). |
| ExposeOnSpawn | ✅ | `meta=(ExposeOnSpawn="true")` | |
| Replication | ✅ | DOREPLIFETIME / DOREPLIFETIME_CONDITION | Pre-existing. |
| SCS components | ✅ | `UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")` + `CreateDefaultSubobject` ctor body + `SetupAttachment` | (#108) |
| Component default property overrides | ✅ | `Comp->Property = FVector(...) / FRotator(...) / 0.5f` etc. in ctor | (#114) Translates Float/Int/Bool/Name/Str/Text/Byte/Enum/StructProperty/etc.; FObjectFinder scaffold for asset refs. |
| Asset-ref `FObjectFinder<T>` template arg | ✅ | `ConstructorHelpers::FObjectFinder<USkeletalMesh> Finder(TEXT("..."));` | (#150) Plugin captures PropertyClass at introspection time; codegen fills in `T`. Falls back to `T` placeholder + TODO when older plugin didn't surface it. |
| Interfaces (BP-implemented) | ✅ | `, public IFoo` in inheritance list | Pre-existing. |
| Generated header position of .generated.h | ✅ | Last include before UCLASS | |
| Delegate type declarations | ✅ | `DECLARE_DYNAMIC_MULTICAST_DELEGATE[_NParams](F<Name>[, T1, P1, ...])` | (#105, #150) Plugin reads delegate SignatureFunction params; codegen picks `_OneParam` through `_NineParams` automatically. |
| Forward decls for component classes | ✅ | `class UStaticMeshComponent;` (etc.) | (#108) |
| UFUNCTION decls | ✅ | `UFUNCTION(BlueprintCallable, Category="...")` or `BlueprintPure` | |
| Virtual override for void parent virtuals | ✅ | `virtual void BeginPlay() override` for BeginPlay/Tick/EndPlay/... | (#109) |
| ConstructionScript -> OnConstruction override | ✅ | `virtual void OnConstruction(const FTransform& Transform) override` for BP's ConstructionScript / UserConstructionScript | (#150) |

## Remaining gaps (inherent static-transpile limits)

These are NOT fixable by plumbing more data through -- they're
structural differences between bp-reader (static C++ generation) and
Angelscript (runtime reflection). They emit clear TODO scaffolds with
sidecar entries; the agent does the manual port.

| Item | Reason | Workaround |
|---|---|---|
| Latent actions (Delay, Timeline, Async tasks) | Post-latent exec flow has to become a separate function (callback). Can't be expressed as a single in-line statement. | Sidecar TODO with canonical `FTimerHandle` + `GetTimerManager().SetTimer(...)` hint. |
| Stateful macros (Gate, DoOnce, DoN, FlipFlop) | Need injected member variables; current pipeline only adds vars that exist in the BP. | Sidecar TODO. Agent adds the state member + branch by hand. |
| EnhancedInput | InputAction asset registration is a runtime concern; static transpile can't preserve the asset bindings. | Sidecar TODO. Agent registers `UInputAction*` member + binding in `SetupPlayerInputComponent`. |
| BP function `targetClass` -> include path | Resolver knows the bare class name (via `ResolveAssetPath`) but not which header file to include. | Agent adds the `#include` manually. Could be automated by indexing `Source/<Module>/**/*.h` at session start. |
| EnhancedInput migration | Modern UE uses InputAction assets, not legacy K2Node_InputAction | Map InputAction asset -> `UInputAction*` member + binding in `SetupPlayerInputComponent`. |

## Sentinel reference

Sentinel `__bpr_*` call names are part of the BPIR contract -- decompile
emits them, CppEmit's `EmitCallExpr` dispatches them. Adding a new
sentinel is a two-side change (decompile emits, codegen renders).

| Sentinel | Renders as |
|---|---|
| `__bpr_spawn_actor_from_class` | `GetWorld()->SpawnActor<AActor>(Class, Xform[, FActorSpawnParameters])` |
| `__bpr_construct_object_from_class` | `NewObject<T>(Outer)` |
| `__bpr_add_component` | `NewObject<T>(Outer)` + `RegisterComponent` + `AttachToComponent` |
| `__bpr_destroy_actor` | `<Target>->Destroy()` |
| `__bpr_format_text` | `FText::Format(LOCTEXT(...), Args)` |
| `__bpr_get_class_defaults` | `GetDefault<T>()->Field` |
| `__bpr_get_data_table_row` | `if (auto* Row = DataTable->FindRow<FRowType>(...)) { ... }` |
| `__bpr_select_ternary` | `(<Index> ? <True> : <False>)` |
| `__bpr_select_n` | `(<Index> == 0 ? <O_0> : (<Index> == 1 ? <O_1> : <O_N>))` |

## See also

- [01-overview.md](01-overview.md) -- pipeline architecture.
- [02-architecture.md](02-architecture.md) -- component layout + wire flows.
- [04-mcp-server.md](04-mcp-server.md) -- BPIR + CppEmit internals.
- [Hazelight Angelscript](https://angelscript.hazelight.se/) -- syntax parity reference for BP-equivalent C++ idioms.
- [OlssonDev: Intro to K2Nodes](https://olssondev.github.io/2023-02-13-K2Nodes/) -- background.
- [S1T2: Brief intro to K2Nodes](https://s1t2.com/blog/brief-intro-k2nodes) -- node-authoring overview.
- [UE4.BlueprintGraph namespace listing](https://ue4dotnet.github.io/api/UE4.BlueprintGraph.html) -- exhaustive class enumeration.
