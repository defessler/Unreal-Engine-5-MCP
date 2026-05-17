# ThirdPerson template anatomy

Research note for the **BP roundtrip capability** (spec
[`docs/superpowers/specs/2026-05-16-bp-roundtrip-capability-design.md`](../superpowers/specs/2026-05-16-bp-roundtrip-capability-design.md),
plan [`docs/superpowers/plans/2026-05-16-bp-roundtrip-capability.md`](../superpowers/plans/2026-05-16-bp-roundtrip-capability.md)).
This doc inventories the UE5 ThirdPerson template's Blueprints as
they sit on disk after Task 7 of the plan (`/Game/Imported/ThirdPerson/`)
and serves as the reference fixture for the granular-roundtrip and
BPIR-roundtrip tests in Tasks 20 + 22.

**Methodology.** All structural data was captured by driving the
`bp-reader` MCP server (`Binaries/Win64/BlueprintReaderMcp.exe`)
against the imported BPs with `BP_READER_BACKEND=commandlet` and
daemon mode. Tool calls used: `read_blueprint`, `summarize_blueprint`,
`list_variables`, `get_components`, `get_graph`
(`EventGraph` + `UserConstructionScript`), `get_function`. Captured
JSON payloads are embedded inline as code blocks; nothing here was
hand-transcribed from the editor UI. Asset references (`/Game/...`)
are package paths per the bp-reader wire convention.

**Scope note — what's NOT imported.** Per the
`BPRoundtripSeedCommandlet.cpp` source comment (lines 60–92),
`Templates/TP_ThirdPersonBP/Content/` only ships the three BPs
listed below; `ABP_Manny`, `IA_Move`/`IA_Look`/`IA_Jump`, `IMC_Default`,
and the skeletal-mesh asset chain live in
`Templates/TemplateResources/High/` and are merged at template
instantiation time. The deliberate decision (Task 7, plan §
"Step 1 note") was to import only the BPs and accept the resulting
soft-reference breakage as a roundtrip-test asset — bp-reader works
on the serialized K2 graph, and pin connections survive missing soft
refs (see §6 "Known gotchas").

---

## 1. Asset inventory

Three Blueprints at `/Game/Imported/ThirdPerson/`, totalling 214 KB
on disk:

| Asset | Parent class | Vars | Functions | Macros | Graphs | Components | EventGraph nodes |
|---|---|---:|---:|---:|---:|---:|---:|
| `BP_ThirdPersonCharacter` | `/Script/Engine.Character` | 0 | 2 | 0 | 2 | 2 (SpringArm+Camera) | 19 |
| `BP_ThirdPersonGameMode` | `/Script/Engine.GameModeBase` | 0 | 0 | 0 | 2 | 1 (DefaultSceneRoot) | 0 |
| `BP_ThirdPersonPlayerController` | `/Script/Engine.PlayerController` | 2 | 1 | 0 | 2 | 0 | 20 |

Aggregate (per `summarize_blueprint`):

```json
{
  "BP_ThirdPersonCharacter": {
    "parent_class": "/Script/Engine.Character",
    "variable_count": 0, "function_count": 2,
    "graph_count": 2, "macro_count": 0, "interface_count": 0
  },
  "BP_ThirdPersonGameMode": {
    "parent_class": "/Script/Engine.GameModeBase",
    "variable_count": 0, "function_count": 0,
    "graph_count": 2, "macro_count": 0, "interface_count": 0
  },
  "BP_ThirdPersonPlayerController": {
    "parent_class": "/Script/Engine.PlayerController",
    "variable_count": 2, "function_count": 1,
    "graph_count": 2, "macro_count": 0, "interface_count": 0
  }
}
```

Per-graph node + connection counts (the test-target shape Task 20
will diff against the clone):

| Asset · Graph | Nodes | Connections |
|---|---:|---:|
| `BP_TPC` · `EventGraph` | 19 | 19 |
| `BP_TPC` · `UserConstructionScript` | 1 | 0 |
| `BP_TPC` · function `Move` | 13 | 13 |
| `BP_TPC` · function `Aim` | 7 | 6 |
| `BP_GameMode` · `EventGraph` | 0 | 0 |
| `BP_GameMode` · `UserConstructionScript` | 1 (synthesized) | 0 |
| `BP_PC` · `EventGraph` | 20 | 15 |
| `BP_PC` · `UserConstructionScript` | 1 | 0 |
| `BP_PC` · function `Should Use Touch Controls` | 9 | 7 |

Total across all 9 graphs: **70 nodes / 64 connections.** The single
lonely `K2Node_FunctionEntry` node on each `UserConstructionScript`
is the schema-required entry stub UE auto-creates — it's not
authored, but it IS what the introspector returns, and the roundtrip
needs to preserve it.

The GameMode and PC `UserConstructionScript` graphs report 1 node
each (the `FunctionEntry` stub) even though `summarize_blueprint`
counts `graph_count: 2`; the construction graph exists as a
schema-required graph, not a node-bearing one.

---

## 2. `BP_ThirdPersonCharacter` deep-dive

Primary roundtrip target. The fixture exercises (a) component
hierarchy with SCS overrides on inherited Character components, (b) a
function-graph subroutine (`Move`) that wires multi-pin math through
reroute knots, (c) the `EnhancedInputAction` node class with broken
soft references — all three are first-time-covered shapes in the
roundtrip plan.

### 2.1 Variables

`list_variables` returns `[]`. Every variable surface on this BP
(walk speed, jump z velocity, controller pitch range, etc.) is
inherited from `ACharacter` / `UCharacterMovementComponent` and lives
on the CDO. The BP itself defines **zero member variables.** This is
expected for a tutorial Character — gameplay logic lives in the
event graph, persistent state lives on the inherited components.

For the roundtrip test, "zero variables" is a meaningful test point:
the structural-diff path must distinguish "absent" from "empty
list", and the spec-builder (Task 12) must round-trip an empty
`variables[]` without injecting a placeholder.

### 2.2 Components

Two BP-owned components (rooted at `CameraBoom`, with `FollowCamera`
parented to it) plus everything inherited from `ACharacter`
(capsule, mesh, movement comp — those don't show in
`get_components`, which only returns SCS-authored nodes).

```json
[
  {
    "class": "/Script/Engine.SpringArmComponent",
    "is_root": true,
    "name": "CameraBoom",
    "parent": null,
    "properties": [
      {"name": "TargetArmLength", "type": "FloatProperty", "value": "400.000000"},
      {"name": "bUsePawnControlRotation", "type": "BoolProperty", "value": "True"},
      {"name": "RelativeLocation", "type": "StructProperty",
       "value": "(X=0.000000,Y=0.000000,Z=8.492264)"}
    ]
  },
  {
    "class": "/Script/Engine.CameraComponent",
    "is_root": false,
    "name": "FollowCamera",
    "parent": "CameraBoom",
    "properties": [
      {"name": "OrthoWidth", "type": "FloatProperty", "value": "512.000000"},
      {"name": "bAutoCalculateOrthoPlanes", "type": "BoolProperty", "value": "False"},
      {"name": "OrthoNearClipPlane", "type": "FloatProperty", "value": "0.000000"}
    ]
  }
]
```

Test target for the component-roundtrip path:

- `is_root: true` on `CameraBoom` (NOT the inherited capsule).
- Parent-child link via `parent: "CameraBoom"` on `FollowCamera`.
- Each property carries its UE type-system name (`FloatProperty`,
  `BoolProperty`, `StructProperty`) and the value's stringified form.
  `StructProperty` round-trips as `(X=...,Y=...,Z=...)` text — diff
  must be tolerant of whitespace inside this rep if it ever
  re-serializes through `FVector::ImportFromString`.

### 2.3 Functions

Two: `Move` (13 nodes / 13 connections) and `Aim` (7 / 6).

#### `Move(X Axis: double, Y Axis: double) → void`

Signature per `get_function`:

```json
{
  "name": "Move",
  "inputs": [
    {"name": "X Axis", "type": {"category": "real", "sub_category": "double"}},
    {"name": "Y Axis", "type": {"category": "real", "sub_category": "double"}}
  ],
  "outputs": [],
  "locals": []
}
```

Body — node list (GUIDs abbreviated to first 8 chars for readability):

```
AF9A5A28 K2Node_FunctionEntry  "Move"
ACBC0364 K2Node_CallFunction   AddMovementInput  (Pawn)
5557D9F5 K2Node_CallFunction   AddMovementInput  (Pawn)
FE0D8B66 K2Node_CallFunction   GetControlRotation
F79403D8 K2Node_CallFunction   GetForwardVector  (KismetMathLibrary)
7625C815 K2Node_CallFunction   GetRightVector    (KismetMathLibrary)
109B0B35 K2Node_CallFunction   GetControlRotation
1EFDA261 K2Node_Knot           (reroute)
842EC35A K2Node_Knot           (reroute)
6E192FA3 K2Node_Knot           (reroute)
78E511F5 K2Node_Knot           (reroute)
F281FC32 EdGraphNode_Comment   "Left/Right"
8BF3DB86 EdGraphNode_Comment   "Forward / Backward"
```

Topology highlights — the function fans the FunctionEntry's
`X Axis`/`Y Axis` into two `AddMovementInput` calls, each routed
through a knot. Yaw-only rotation is extracted from the controller
rotation and fed into `GetForwardVector` / `GetRightVector` for the
world-space direction:

```
FunctionEntry/then  →  AddMovementInput/execute
FunctionEntry/X Axis →  Knot/InputPin
FunctionEntry/Y Axis →  Knot/InputPin
AddMovementInput/then →  AddMovementInput/execute
GetControlRotation/ReturnValue_Yaw → GetForwardVector/InRot_Yaw
GetForwardVector/ReturnValue → AddMovementInput/WorldDirection
GetRightVector/ReturnValue   → AddMovementInput/WorldDirection
GetControlRotation/ReturnValue_Roll → GetRightVector/InRot_Roll
GetControlRotation/ReturnValue_Yaw  → GetRightVector/InRot_Yaw
Knot/OutputPin → AddMovementInput/ScaleValue   (×2)
Knot/OutputPin → Knot/InputPin                 (×2)
```

The 4 knots are pure reroute pass-throughs (one per axis-to-scale
path). The roundtrip must preserve them — deleting them would
produce a structurally-different graph even though the wires
collapse to the same net.

The split-struct connections (`ReturnValue_Yaw`, `ReturnValue_Roll`)
mean the introspector is following a `K2Node_BreakStruct` /
auto-promoted struct-pin split for `FRotator` — pin names on the
`from` side are synthesized by the engine; the spec-builder must
emit them verbatim to round-trip cleanly.

#### `Aim(X Axis: float, Y Axis: double) → void`

```json
{
  "name": "Aim",
  "inputs": [
    {"name": "X Axis", "type": {"category": "real", "sub_category": "float"}},
    {"name": "Y Axis", "type": {"category": "real", "sub_category": "double"}}
  ],
  "outputs": [],
  "locals": []
}
```

**Type mismatch noted:** `X Axis` is **`float`** here vs **`double`** in
`Move`. This is an authoring quirk in the upstream template — the
roundtrip MUST preserve the exact `sub_category` value per pin, not
normalize float→double. Test fixture for the float-vs-double pin-type
fidelity case.

Body:

```
8F32BB90 K2Node_FunctionEntry   "Aim"
0AEFF384 K2Node_CallFunction    AddControllerPitchInput
4AF8AF1B K2Node_CallFunction    AddControllerYawInput
D9DA072C K2Node_Knot
E57F4125 K2Node_Knot
010A3AD6 EdGraphNode_Comment    "Left/Right"
CA4B0EAF EdGraphNode_Comment    "Up/Down"
```

Topology — fan-in via a single knot for `Y Axis`:

```
FunctionEntry/then   → AddControllerPitchInput/execute
FunctionEntry/X Axis → AddControllerPitchInput/Val
FunctionEntry/Y Axis → Knot/InputPin
AddControllerPitchInput/then → AddControllerYawInput/execute
Knot/OutputPin → AddControllerYawInput/Val
Knot/OutputPin → Knot/InputPin
```

### 2.4 EventGraph (19 nodes, 19 connections)

Class breakdown: `K2Node_CallFunction` ×7 (Move ×2, Aim ×3, Jump,
StopJumping), `EdGraphNode_Comment` ×4 (section headers),
`K2Node_EnhancedInputAction` ×4 (all broken soft refs — see §2.5),
`K2Node_Event` ×4 (Secondary/Primary Thumbstick, Touch Jump Start/End).

Node-by-node (GUIDs abbreviated):

```
07F096B95 K2Node_CallFunction         StopJumping              (target=Character)
0C32C96A1 EdGraphNode_Comment         "Orbiting Camera Input. ..."
09884EC5F EdGraphNode_Comment         "Movement Input"
0FAF30F87 EdGraphNode_Comment         "Jump Input - ..."
00B68659C K2Node_EnhancedInputAction  "EnhancedInputAction None"   (jump IA, bool ActionValue)
0B418BC36 K2Node_EnhancedInputAction  "EnhancedInputAction None"   (move IA, Vector2D ActionValue)
06D417299 K2Node_EnhancedInputAction  "EnhancedInputAction None"   (look IA, Vector2D ActionValue)
0069783C3 K2Node_CallFunction         Jump                          (target=Character)
0C8BACB74 EdGraphNode_Comment         "A basic Character with ..."
024142E0B K2Node_Event                "Event Secondary Thumbstick"  (eventName=Secondary Thumbstick)
0A0159C81 K2Node_EnhancedInputAction  "EnhancedInputAction None"   (mouse-look IA, Vector2D ActionValue)
00FB356B6 K2Node_Event                "Event Primary Thumbstick"    (eventName=Primary Thumbstick)
03A46A37E K2Node_Event                "Event Touch Jump Start"      (eventName=Touch Jump Start)
09EA24AAF K2Node_Event                "Event Touch Jump End"        (eventName=Touch Jump End)
00451B5CF K2Node_CallFunction         Move                          (self call)
0C5B5D519 K2Node_CallFunction         Move                          (self call)
0F2053F97 K2Node_CallFunction         Aim                           (self call)
0EB957150 K2Node_CallFunction         Aim                           (self call)
029CE1F18 K2Node_CallFunction         Aim                           (self call)
```

Resolved wiring (every connection):

```
EnhancedInputAction(jump)/Started   → Jump/execute
EnhancedInputAction(jump)/Completed → StopJumping/execute
EnhancedInputAction(move)/Triggered  → Move/execute
EnhancedInputAction(move)/ActionValue_X → Move/X Axis
EnhancedInputAction(move)/ActionValue_Y → Move/Y Axis
EnhancedInputAction(look)/Triggered  → Aim/execute
EnhancedInputAction(look)/ActionValue_X → Aim/X Axis
EnhancedInputAction(look)/ActionValue_Y → Aim/Y Axis
Event(Primary Thumbstick)/then   → Move/execute
Event(Primary Thumbstick)/Axis_X → Move/X Axis
Event(Primary Thumbstick)/Axis_Y → Move/Y Axis
EnhancedInputAction(mouse)/Triggered  → Aim/execute
EnhancedInputAction(mouse)/ActionValue_X → Aim/X Axis
EnhancedInputAction(mouse)/ActionValue_Y → Aim/Y Axis
Event(Secondary Thumbstick)/then   → Aim/execute
Event(Secondary Thumbstick)/Axis_X → Aim/X Axis
Event(Secondary Thumbstick)/Axis_Y → Aim/Y Axis
Event(Touch Jump Start)/then → Jump/execute
Event(Touch Jump End)/then   → StopJumping/execute
```

### 2.5 EnhancedInput action bindings (broken refs)

The 4 EnhancedInputAction nodes are all rendered as
**"EnhancedInputAction None"** because their `InputAction` soft
reference resolved to `nullptr` — the upstream IA assets
(`/Game/Input/Actions/IA_*`) were not imported. UE's compile log
emits ~24 errors per load:

```
LogBlueprint: Error: [Compiler] No structure in SubCategoryObject in pin  Action Value
LogBlueprint: Warning: [Compiler] EnhancedInputAction None  does not have a valid Input Action asset!!
LogBlueprint: Error: [Compiler] In use pin  Action Value X  no longer exists on node  EnhancedInputAction None .
LogBlueprint: Error: [Compiler] EnhancedInputActionEvent references invalid 'null' action for  EnhancedInputAction None
LogBlueprint: Error: [Compiler] Could not find a function named "None" in 'BP_ThirdPersonCharacter'. ...
```

But the K2 serialized form **still contains the pins and their
connections** — `ActionValue_X`, `ActionValue_Y` are present on
each of the 3 Vector2D-typed EIA nodes, and their wires into
`Move` / `Aim` remain intact. The pin list per Vector2D EIA:
`Triggered`, `Started`, `Ongoing`, `Canceled`, `Completed` (all
`exec`), `ActionValue` (`bool`, the fallback when the IA's value
type can't be resolved), `ActionValue_X`, `ActionValue_Y`,
`ElapsedSeconds`, `TriggeredSeconds` (all `real:double`).

Of the 4 EIA nodes:

- **1 node** (the Jump IA at `0B68659C`) has only `ActionValue`
  (bool) — its `Started` / `Completed` exec pins drive `Jump` /
  `StopJumping`. Vector2D pins absent because the original IA was
  bool-typed.
- **3 nodes** (move IA at `B418BC36`, look IA at `6D417299`,
  mouse-look IA at `A0159C81`) carry both the bool `ActionValue`
  AND split-struct `ActionValue_X` / `ActionValue_Y` pins — UE's
  PostLoad keeps the pins because connections reference them, even
  though the IA's Value Type can't be re-resolved.

The roundtrip implication is that the **graph-as-data is intact**:
node classes, pin names, pin types, connection topology all
round-trip cleanly through `BlueprintReader`'s read/write surface
even when the IA references are unresolvable. The compile errors
are surface-level — they don't corrupt the serialized form.

**`meta` is empty `{}`** for these nodes (no `targetFunction` /
`eventName` / `variableName` — the introspector doesn't surface
the EIA's `InputAction` soft path through any meta key currently).
Recovering the original IA association from a roundtrip clone would
require either (a) extending `BPNode.meta` with an `inputAction` key,
or (b) accepting that the clone will render the same broken-ref
shape. The plan defaults to (b).

### 2.6 UserConstructionScript

Single auto-spawned `K2Node_FunctionEntry` node, no body. The BP
ships with an empty construction script. The roundtrip preserves
the graph existence + the entry stub.

---

## 3. `BP_ThirdPersonGameMode`

Minimal-override stub. Parent class
`/Script/Engine.GameModeBase`. Zero variables, zero functions, zero
nodes in `EventGraph`, a single `DefaultSceneRoot` SCS component
(BP_GameMode is an `AInfo` descendant — SceneComponent root is the
default added by the BP editor when you create a fresh Actor-derived
BP, even though GameModeBase has no use for it).

```json
{
  "asset_path": "/Game/Imported/ThirdPerson/BP_ThirdPersonGameMode",
  "parent_class": "/Script/Engine.GameModeBase",
  "variables": [], "functions": [], "macros": [], "interfaces": [],
  "graphs": [
    {"name": "EventGraph", "type": "EventGraph"},
    {"name": "UserConstructionScript", "type": "Construction"}
  ]
}
```

Components:

```json
[
  {
    "class": "/Script/Engine.SceneComponent",
    "is_root": true,
    "name": "DefaultSceneRoot",
    "parent": null,
    "properties": []
  }
]
```

The actual gameplay surface (`DefaultPawnClass`, `PlayerControllerClass`,
`HUDClass`) lives on the BP's CDO via class-default overrides — not
in any K2 node, not in `list_variables`. The serialized `.uasset`
carries those as raw property overrides on the BP's `GeneratedClass`.
`BlueprintReader`'s current read surface does NOT expose CDO
property overrides, so "BP_GameMode round-trips cleanly" means "the
K2 surface round-trips; the CDO overrides are a separate fidelity
question" (also covered in §6.6). For Task 20, BP_GameMode is the
minimal-shape fixture — the clone must produce the same skeleton
even with no graph content.

---

## 4. `BP_ThirdPersonPlayerController`

Parent class `/Script/Engine.PlayerController`. Two member variables,
one function, a 20-node `EventGraph`. No SCS components
(PC doesn't typically own visual components).

### 4.1 Variables

```json
[
  {
    "name": "Touch Controls Widget Class",
    "category": "Input|Touch Controls",
    "default_value": null,
    "is_editable": true, "is_replicated": false,
    "type": {
      "category": "class",
      "sub_category_object": "/Script/UMG.UserWidget"
    }
  },
  {
    "name": "Force Touch Controls",
    "category": "Input|Touch Controls",
    "default_value": null,
    "is_editable": true, "is_replicated": false,
    "type": {"category": "bool"}
  }
]
```

Test fixture for typed-variable coverage:

- `category: "class"` with a `sub_category_object` pointing at
  `UUserWidget` — this is `TSubclassOf<UUserWidget>` in C++. Different
  from `object:UserWidget` (an instance ref).
- `category: "bool"`, no `sub_category_object`.
- Both have `is_editable: true` (My-Blueprint-panel-visible) and
  the same `category: "Input|Touch Controls"` (vertical-bar nesting
  is how UE renders nested My-Blueprint folders).
- `default_value: null` on both — defaults are stored on the CDO,
  not in the variable descriptor. The roundtrip must keep
  `default_value` as `null` not `""` to match the wire shape.

### 4.2 Function — `Should Use Touch Controls() → bool`

The function name carries embedded spaces ("Should Use Touch
Controls"). bp-reader's `get_function` accepts that form; the
underlying UFUNCTION's identifier is `ShouldUseTouchControls`
(no spaces) but the My-Blueprint-panel display name retains them.
Test fixture for the spaces-in-display-name case.

Signature:

```json
{
  "inputs": [],
  "outputs": [
    {"name": "Use Mobile Controls", "type": {"category": "bool"}, "default_value": "true"},
    {"name": "Use Mobile Controls", "type": {"category": "bool"}, "default_value": "false"}
  ]
}
```

**The function reports TWO outputs with the same name** —
this is because there are two `K2Node_FunctionResult` nodes in the
graph (one per switch case), each contributing an output pin. The
spec-builder must treat these as ONE logical output (the function
has one returned bool) but TWO physical FunctionResult nodes. See
the topology below for the dispatch shape.

Body — 9 nodes / 7 connections:

```
8E9AD386 K2Node_FunctionEntry   "Should Use Touch Controls"
E87074F9 K2Node_FunctionResult  "Return Node"
B9F80F23 K2Node_CallFunction    GetPlatformName
D40AAE36 K2Node_SwitchString    "Switch on String"
E555B514 K2Node_FunctionResult  "Return Node"           (second result node!)
E9252772 K2Node_VariableGet     "Get Force Touch Controls"
5E9D79CB EdGraphNode_Comment    "Use UMG touch controls if on a mobile platform."
C896239E EdGraphNode_Comment    "You can also force touch controls through ..."
0E9FB450 K2Node_Knot
```

Topology:

```
FunctionEntry/then          → SwitchString/execute
GetPlatformName/ReturnValue → SwitchString/Selection
SwitchString/iOS            → FunctionResult(1)/execute
SwitchString/Android        → FunctionResult(1)/execute
SwitchString/Default        → Knot/InputPin
Knot/OutputPin              → FunctionResult(2)/execute
VariableGet(Force Touch Controls) → FunctionResult(2)/Use Mobile Controls
```

What the function does: switch on platform name, return `true`
hard-coded for iOS/Android (FunctionResult #1, defaultValue
`"true"` on the bool pin), else return the value of `Force Touch
Controls` (FunctionResult #2, default `"false"`).

This is a great test fixture for:

- **`K2Node_SwitchString`** — flagged in
  [`syntactic-sugar-nodes.md`](syntactic-sugar-nodes.md) as a coverage
  gap for the C++ transpile path (renders as `switch (FString)` which
  doesn't compile). For the roundtrip path it's just shape data — the
  fidelity test is whether the case pin names (`iOS`, `Android`,
  `Default`) round-trip verbatim.
- **Two `K2Node_FunctionResult` nodes** — most BP functions have one.
  The roundtrip must spawn both, each with the right `Use Mobile
  Controls` default value (`"true"` on one, `"false"` on the other).

### 4.3 EventGraph (20 nodes, 15 connections)

What it does on `BeginPlay`: check `IsLocalPlayerController()` — if
false, exit. Otherwise `Sequence`: (branch 0) get
`EnhancedInputLocalPlayerSubsystem` → `AddMappingContext` (touch
mapping). (Branch 1) call `Should Use Touch Controls` → if true,
`CreateWidget(Touch Controls Widget Class)` → `AddToPlayerScreen`
(`OwningPlayer` wired from `K2Node_Self`); else route to
`AddMappingContext` (regular mapping).

Class breakdown: `EdGraphNode_Comment` ×7, `K2Node_CallFunction` ×6,
`K2Node_IfThenElse` ×2, `K2Node_GetSubsystem` ×2, plus one each of
`K2Node_VariableGet`, `K2Node_FunctionEntry`, `K2Node_Knot`,
`K2Node_CreateWidget`, `K2Node_ExecutionSequence`, `K2Node_Event`
(BeginPlay), `K2Node_Self`.

Resolved wiring (15 connections):

```
Event(BeginPlay)/then                          → Branch(LocalPC)/execute
GetSubsystem(EnhancedInput)/ReturnValue        → AddMappingContext(regular)/self
ExecutionSequence/then_0                       → AddMappingContext(touch)/execute
ExecutionSequence/then_1                       → Branch(ShouldUseTouch)/execute
IsLocalPlayerController/ReturnValue            → Branch(LocalPC)/Condition
Branch(LocalPC)/then                           → ExecutionSequence/execute
CreateWidget/then                              → AddToPlayerScreen/execute
CreateWidget/ReturnValue                       → AddToPlayerScreen/self
GetSubsystem(EnhancedInput)/ReturnValue        → AddMappingContext(touch)/self
Knot/OutputPin                                 → AddMappingContext(regular)/execute
VariableGet(Touch Controls Widget Class)       → CreateWidget/Class
Self/self                                      → CreateWidget/OwningPlayer
Branch(ShouldUseTouch)/then                    → CreateWidget/execute
Branch(ShouldUseTouch)/else                    → Knot/InputPin
ShouldUseTouchControls/Use Mobile Controls     → Branch(ShouldUseTouch)/Condition
```

Note that the PC EventGraph also references `IA_*` / `IMC_*` via
soft refs (the two `AddMappingContext` calls take `InputMappingContext`
pin parameters whose default values point at the missing IMCs).
Same gotcha as TPC §2.5: the pin connections round-trip; the
unresolved object refs don't.

---

## 5. Histograms (test targets)

### 5.1 Node-kind histogram

**Aggregate across all 9 graphs in all 3 BPs:**

| Class | Total | TPC | PC | GameMode |
|---|---:|---:|---:|---:|
| `K2Node_CallFunction` | 21 | 15 | 6 | 0 |
| `EdGraphNode_Comment` | 15 | 8 | 7 | 0 |
| `K2Node_Knot` | 8 | 6 | 2 | 0 |
| `K2Node_FunctionEntry` | 5 | 3 | 2 | 0 |
| `K2Node_Event` | 5 | 4 | 1 | 0 |
| `K2Node_EnhancedInputAction` | 4 | 4 | 0 | 0 |
| `K2Node_IfThenElse` | 2 | 0 | 2 | 0 |
| `K2Node_FunctionResult` | 2 | 0 | 2 | 0 |
| `K2Node_VariableGet` | 2 | 0 | 2 | 0 |
| `K2Node_GetSubsystem` | 2 | 0 | 2 | 0 |
| `K2Node_CreateWidget` | 1 | 0 | 1 | 0 |
| `K2Node_SwitchString` | 1 | 0 | 1 | 0 |
| `K2Node_ExecutionSequence` | 1 | 0 | 1 | 0 |
| `K2Node_Self` | 1 | 0 | 1 | 0 |
| **Total** | **70** | **40** | **30** | **0** |

The roundtrip success criterion for Task 20: `aggregate(clone) ==
aggregate(original)` for this 14-class table, partitioned per-BP.
Any divergence (extra knot from a normalized wire, missing comment
node because the spec-builder dropped them, etc.) is a fidelity bug.

**Notable absences** — every `K2Node_*` class NOT in the above
table that the BP-reader test corpus has fixtures for: `K2Node_Cast`,
`K2Node_DynamicCast`, `K2Node_MacroInstance` (no ForEachLoop here),
`K2Node_Timeline`, `K2Node_Latent`, `K2Node_MathExpression`,
`K2Node_FormatText`, any input-config nodes other than EIA. The TPC
template is dense in CallFunction + Event + EnhancedInputAction;
exercise of the rest of the node taxonomy lives in
`BP_TestEnemy` / `BP_TestPickup` (the seed-commandlet fixtures).

### 5.2 Pin-type histogram

**Aggregate across every pin on every node of every graph:**

| Pin type | Total | TPC | PC | GameMode |
|---|---:|---:|---:|---:|
| `exec` | 79 | 49 | 30 | 0 |
| `real:double` | 40 | 40 | 0 | 0 |
| `real:float` | 20 | 20 | 0 | 0 |
| `bool` | 14 | 6 | 8 | 0 |
| `object:self` | 7 | 5 | 2 | 0 |
| `object:Pawn` | 6 | 6 | 0 | 0 |
| `delegate` | 5 | 4 | 1 | 0 |
| `struct:Vector` | 4 | 4 | 0 | 0 |
| `struct:Rotator` | 4 | 4 | 0 | 0 |
| `int` | 3 | 0 | 3 | 0 |
| `object:EnhancedInputLocalPlayerSubsystem` | 2 | 0 | 2 | 0 |
| `object:InputMappingContext` | 2 | 0 | 2 | 0 |
| `string` | 2 | 0 | 2 | 0 |
| `struct:Vector2D` | 2 | 2 | 0 | 0 |
| `struct:ModifyContextOptions` | 2 | 0 | 2 | 0 |
| `object:BP_ThirdPersonPlayerController_C` | 2 | 0 | 2 | 0 |
| `object:UserWidget` | 2 | 0 | 2 | 0 |
| `class:UserWidget` | 2 | 0 | 2 | 0 |
| `interface:EnhancedInputSubsystemInterface` | 2 | 0 | 2 | 0 |
| `object:Character` | 2 | 2 | 0 | 0 |
| `object:KismetMathLibrary` | 2 | 2 | 0 | 0 |
| `object:KismetStringLibrary` / `PlayerController` / `GameplayStatics` / `Controller` | 1 each | 0 / 0 / 0 / 0 | 1 / 1 / 1 / 1 | 0 |

24 distinct pin-type signatures. Roundtrip success criterion for
Task 20's pin-type pass: every (category, sub_category,
sub_category_object) tuple has identical occurrence count on the
clone.

The categories observed: `exec`, `real` (with sub `float` / `double`),
`bool`, `int`, `string`, `object`, `class`, `interface`, `struct`,
`delegate`. **NOT observed:** `enum`, `byte`, `name`, `text`, `wildcard`,
`real:double` arrays/sets/maps. The TPC template does not exercise the
container-wrapped pin types (`is_array` / `is_set` / `is_map` are
all `false` for every observed pin). Exercise those in BPIR-roundtrip
fixtures (`BP_TestEnemy` carries `[]int` and `{string:int}` variables).

### 5.3 Pin-type-fidelity edge cases

Watch out for these in the roundtrip diff:

- **`real:float` vs `real:double` on the same function.** `Aim.X Axis`
  is `float` (40-bit), `Aim.Y Axis` is `double`, `Move.X Axis` and
  `Move.Y Axis` are both `double`. The roundtrip cannot normalize
  `float` → `double` even though they share the `real` category.
- **`object:self`** — emitted by every node whose pin is bound to the
  containing BP class. The introspector serializes this as
  `sub_category_object: "/Script/<owner>"` per pin; the spec-builder
  must use whichever resolution matches the read shape.
- **Split-struct pin names** — `ReturnValue_Yaw`, `ReturnValue_Roll`,
  `Axis_X`, `Axis_Y`, `ActionValue_X`, `ActionValue_Y`. These are
  *not* author-named; UE synthesizes them when a struct pin is
  split. The spec-builder must mark them as split-source and emit
  them on the right parent pin, or wiring will fail to round-trip.
- **`delegate`** — observed 5×, all on `EnhancedInputAction` / `Event`
  nodes. The introspector's wire shape for delegate pins isn't
  exercised by the test corpus today; Task 20 should add an explicit
  assert that delegate pins round-trip via their event signature
  (not the typical category + sub_category_object pair).

---

## 6. Known gotchas for the roundtrip

Spotted while gathering the data above. Each is a candidate test
case or a known-deviation to document.

### 6.1 EnhancedInputAction soft-ref breakage is faithful but lossy

Covered in §2.5. Pins, connections, exec wiring all round-trip
cleanly — but the original `InputAction` soft reference is **NOT**
captured in `BPNode.meta`. A clone has the same shape but loses
the EIA node's IA identity. Three remediations in increasing cost:
(1) accept the loss (plan default — structural roundtrip passes);
(2) extend `BPNode.meta` with an `inputAction` key (wire-format
bump); (3) restore the IA assets to the project (see plan Task 7
`TODO(roundtrip-expansion)`).

### 6.2 Split-struct pin name preservation

Per §5.3. The names `ActionValue_X`, `ReturnValue_Yaw`, `Axis_X`
are synthesized by UE's split-struct-pin machinery and are stable
across loads. The spec-builder must preserve the parent-pin split
state (call `SplitPin` on the parent before re-wiring children),
not emit child pins as standalone names — otherwise the clone's
EIA / Event / GetControlRotation nodes will not produce the
expected `_X`/`_Y`/`_Yaw` names.

### 6.3 `K2Node_SwitchString` C++ transpile is broken (BP roundtrip OK)

The PC's `Should Use Touch Controls` function carries a SwitchString.
Per [`syntactic-sugar-nodes.md`](syntactic-sugar-nodes.md), the
BP→C++ transpile path renders this as `switch (FString)` which
doesn't compile. For Tasks 20+22 (BP-only roundtrip) this doesn't
matter — flagged here so the next planner doesn't re-discover it
if BPIR is later reused for the C++ path.

### 6.4 Two FunctionResult nodes on `Should Use Touch Controls`

Per §4.2. The function has two `K2Node_FunctionResult` nodes, each
contributing a `Use Mobile Controls` output pin with its own default
(`"true"` for iOS/Android, `"false"` fallback). The spec-builder
must understand one logical output ↔ N FunctionResult nodes and
round-trip each node's default independently; a naive 1:1 spec
collapses the dispatch.

### 6.5 `K2Node_FunctionEntry` on otherwise-empty graphs

`UserConstructionScript` on every BP has a single FunctionEntry
stub. The roundtrip must spawn this stub even with no authored body
— deleting it changes the BP's compile state.

### 6.6 CDO property overrides are out-of-band

Per §3 / §4.1. `BP_GameMode`'s `DefaultPawnClass` and the PC's
`Touch Controls Widget Class` *defaults* live on the BP's
`GeneratedClass` CDO, not surfaced via `list_variables` / `get_*`.
Roundtrip target is **graph fidelity**, not CDO fidelity — Task 20
should restrict assertions to `get_*` returns. CDO fidelity later
needs new `get_class_defaults` / `set_class_defaults` tools.

### 6.7 BP load triggers compile, compile errors push editor exit=1

Observed during data capture. TPC's broken EIA refs make UE emit 24
compile errors per PostLoad. In one-shot commandlet mode this
propagates to the editor process exit, and
`CommandletBlueprintReader::RunOpOneShot` treats exit≠0 as failure
even though the JSON output was written correctly. **Daemon mode
is unaffected** — per-op handler returns 0, editor stays alive. For
Tasks 20+22 (which hammer the BPs repeatedly), the daemon path is
mandatory. The one-shot CI path either needs an exit=1 tolerance or
the missing IA assets restored.

### 6.8 `meta` field is class-dependent

Patterns observed across the captured graphs:

- `K2Node_CallFunction`: `kind: "CallFunction"`, `targetFunction: "<UFUNCTION>"`.
- `K2Node_Event`: `kind: "Event"`, `eventName: "<display name>"`.
- `K2Node_VariableGet` / `VariableSet`: `kind: "VariableGet|Set"`, `variableName: "<display>"`.
- `K2Node_EnhancedInputAction`: `meta: {}` (no per-IA discriminator).
- All others (`Knot`, `FunctionEntry`, `FunctionResult`, `IfThenElse`, etc.):
  just `kind: "<short-name>"`.

The spec-builder uses `meta` to recreate the right K2 subclass. EIA
is the only case where reconstruction falls back to class name +
pin shape (per §6.1).

---

## 7. Cross-references

- Plan: [`docs/superpowers/plans/2026-05-16-bp-roundtrip-capability.md`](../superpowers/plans/2026-05-16-bp-roundtrip-capability.md) Task 8.
- Spec: [`docs/superpowers/specs/2026-05-16-bp-roundtrip-capability-design.md`](../superpowers/specs/2026-05-16-bp-roundtrip-capability-design.md).
- Import commandlet: [`Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BPRoundtripSeedCommandlet.cpp`](../../Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BPRoundtripSeedCommandlet.cpp).
- BP↔C++ transpile sister note (§6.3 context): [`syntactic-sugar-nodes.md`](syntactic-sugar-nodes.md).
- In-progress spec shape (Task 12): [`Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/roundtrip/BPSpec.h`](../../Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/roundtrip/BPSpec.h).
- `BPNode.meta` wire format: [`Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/BlueprintReaderTypes.h`](../../Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/BlueprintReaderTypes.h).
