# Chapter 7 — Graph mutations: nodes and pins

`add_variable` was the easy write. Graph editing is where you meet
the K2 schema — UE's runtime type system for Blueprint nodes and the
pins that connect them. This chapter builds `add_node` and
`wire_pins` end to end, and ends with an agent assembling a
working three-node graph (`BeginPlay → Branch → PrintString`) in one
sequence of MCP tool calls.

See [../design/03-plugin-internals.md](../design/03-plugin-internals.md)
for the full K2 schema discussion. We hit the highlights here, not
the corners.

## A short K2 vocabulary

A `UEdGraph` is one canvas. A `UEdGraphNode` is one box on that
canvas. Each node owns an array of `UEdGraphPin` — the colored
sockets on its left and right edges, plus its exec triangles. A pin
has:

- A direction (`EGPD_Input` or `EGPD_Output`).
- An `FEdGraphPinType`: category (`exec`, `bool`, `int`, `object`,
  `struct`, `wildcard`, ...), optional sub-category, optional
  sub-category object (the `UClass*` for an object pin, the
  `UScriptStruct*` for a struct pin), and array/set/map flags.
- An `FGuid` `PinId`, stable across saves.
- Zero or more `LinkedTo` entries pointing at other pins.

A schema (`UEdGraphSchema_K2`) sits beneath every Blueprint graph
and decides what's a valid connection, what nodes can be placed,
and how to convert types on the fly. Every interesting edit goes
through the schema, not by mutating link arrays directly. The reason
will become important when we get to `wire_pins`.

## Spawning a node: the ritual

Every K2 node spawn follows the same five-step ritual:

```cpp
template <typename TNode>
TNode* AddNodeToGraph(UEdGraph* Graph, int32 X, int32 Y)
{
    TNode* Node = NewObject<TNode>(Graph);
    Node->CreateNewGuid();
    Node->NodePosX = X;
    Node->NodePosY = Y;
    Graph->AddNode(Node, /*bFromUI=*/false, /*bSelectNewNode=*/false);
    Node->PostPlacedNewNode();
    Node->AllocateDefaultPins();
    return Node;
}
```

Each step does something specific:

- `NewObject<TNode>(Graph)` creates the UObject with the graph as
  its outer. The outer-as-graph relationship is what makes the node
  travel with the BP through save/load.
- `CreateNewGuid` mints `NodeGuid`. We return this to the agent so
  later ops can reference the node by GUID.
- `NodePosX/Y` set the canvas position. The graph editor reads these
  directly; if you skip them, every new node stacks at (0, 0) and
  is invisible without manual rearrangement.
- `AddNode(Graph, bFromUI, bSelectNewNode)` registers the node with
  the graph. `bFromUI=false` keeps the editor's undo/selection state
  out of the picture (we're not coming from an editor interaction).
- `PostPlacedNewNode` is the node's chance to do post-spawn work.
  For most K2 nodes this is a no-op, but `UK2Node_DynamicCast` and
  others initialize internal state here.
- `AllocateDefaultPins` is the load-bearing step: it asks the node
  to populate its `Pins` array. Without this, the node has no
  sockets and you can't wire anything.

That ritual works for nodes whose default state is "self-contained" —
`UK2Node_IfThenElse`, `UK2Node_ExecutionSequence`, `UK2Node_Self`,
`UK2Node_MakeArray`, `UK2Node_Knot`. The interesting cases need
extra state set *between* `CreateNewGuid` and `AllocateDefaultPins`,
because the default pins depend on that state.

## Per-kind setup

`add_node` accepts a `kind` string and a small dictionary of
per-kind extras. The plugin's dispatcher branches on `kind`. Here
are the four shapes you'll see most:

### VariableGet / VariableSet

```cpp
UK2Node_VariableGet* Get = NewObject<UK2Node_VariableGet>(Graph);
Get->VariableReference.SetSelfMember(*VarName);
Get->CreateNewGuid();
Get->NodePosX = X; Get->NodePosY = Y;
Graph->AddNode(Get, false, false);
Get->PostPlacedNewNode();
Get->AllocateDefaultPins();
```

`SetSelfMember` binds the variable reference to a member of *this*
BP. The `AllocateDefaultPins` call reads that reference and creates
the output pin with the variable's pin type. If you swap the order
(allocate first, set reference second), the output pin comes out
typed as `wildcard` and you have to ResolveReference + RecreatePins
to fix it.

### CallFunction

```cpp
UClass* OwnerClass = ResolveClass(FunctionOwner);
UFunction* Fn = OwnerClass->FindFunctionByName(*FunctionName);

UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
Call->SetFromFunction(Fn);
Call->CreateNewGuid();
Call->NodePosX = X; Call->NodePosY = Y;
Graph->AddNode(Call, false, false);
Call->PostPlacedNewNode();
Call->AllocateDefaultPins();
```

`ResolveClass` accepts either a full path (`/Script/Engine.KismetSystemLibrary`)
or a short name under `/Script/Engine`. `SetFromFunction` populates the
internal `FunctionReference` so `AllocateDefaultPins` can read the
parameter list off the UFunction signature.

### CustomEvent

```cpp
UK2Node_CustomEvent* Evt = NewObject<UK2Node_CustomEvent>(Graph);
Evt->CustomFunctionName = FName(*EventName);
Evt->CreateNewGuid();
// ...
```

### Cast

```cpp
UK2Node_DynamicCast* Cast = NewObject<UK2Node_DynamicCast>(Graph);
Cast->TargetType = ResolveClass(TargetClass);
Cast->CreateNewGuid();
// ...
```

### Dispatching all of them

```cpp
int32 RunAddNodeOp(const FString& Params, const FString& OutputPath, bool bPretty)
{
    const FString AssetPath = ResolveAssetPath(Params);
    FString GraphName, Kind;
    int32 X = 0, Y = 0;
    FParse::Value(*Params, TEXT("Graph="), GraphName);
    FParse::Value(*Params, TEXT("Kind="),  Kind);
    FParse::Value(*Params, TEXT("X="),     X);
    FParse::Value(*Params, TEXT("Y="),     Y);

    if (AssetPath.IsEmpty() || GraphName.IsEmpty() || Kind.IsEmpty()) {
        UE_LOG(LogBlueprintReader, Error, TEXT("AddNode requires -Asset= -Graph= -Kind="));
        return 1;
    }

    UBlueprint* BP = LoadMutableBlueprint(AssetPath);
    if (!BP) return 4;
    UEdGraph* Graph = FindGraphByName(BP, GraphName);
    if (!Graph) return 4;

    UEdGraphNode* Spawned = nullptr;

    if (Kind.Equals(TEXT("Branch"), ESearchCase::IgnoreCase) ||
        Kind.Equals(TEXT("IfThenElse"), ESearchCase::IgnoreCase)) {
        Spawned = AddNodeToGraph<UK2Node_IfThenElse>(Graph, X, Y);
    }
    else if (Kind.Equals(TEXT("VariableGet"), ESearchCase::IgnoreCase)) {
        FString VarName;
        FParse::Value(*Params, TEXT("Variable="), VarName);
        if (VarName.IsEmpty()) return 1;
        // ... (the per-kind block shown above)
    }
    else if (Kind.Equals(TEXT("CallFunction"), ESearchCase::IgnoreCase)) {
        FString FunctionName, FunctionOwner;
        FParse::Value(*Params, TEXT("Function="),      FunctionName);
        FParse::Value(*Params, TEXT("FunctionOwner="), FunctionOwner);
        if (FunctionName.IsEmpty() || FunctionOwner.IsEmpty()) return 1;
        // ...
    }
    // ... more kinds ...
    else {
        UE_LOG(LogBlueprintReader, Error,
            TEXT("AddNode: unrecognised -Kind=%s; see list_node_kinds for valid values"),
            *Kind);
        return 1;
    }

    if (!Spawned) return 5;
    const FString NewId = Spawned->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);

    if (!CompileAndSaveBlueprint(BP)) return 5;

    auto Obj = MakeShared<FJsonObject>();
    Obj->SetBoolField(TEXT("ok"), true);
    Obj->SetStringField(TEXT("node_id"), NewId);
    return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
}
```

The full plugin handles a couple of dozen kinds. The pattern repeats:
parse the per-kind extras, set the node's pre-pin state, spawn through
the same ritual.

## The extras tunneling problem

There's a quiet asymmetry between the MCP-side names and the
commandlet-side flag names that's easy to miss:

| MCP arg (JSON)    | Commandlet flag    |
|-------------------|--------------------|
| `variable`        | `-Variable=`       |
| `function`        | `-Function=`       |
| `function_owner`  | `-FunctionOwner=`  |
| `event_name`      | `-EventName=`      |
| `target_class`    | `-TargetClass=`    |
| `struct_type`     | `-StructType=`     |

The MCP wire uses snake_case (project convention); the commandlet
side uses flag-case (UE convention). Mapping happens in
`BlueprintTools.cpp`'s `add_node` handler:

```cpp
std::map<std::string, std::string, std::less<>> extras;
auto put = [&](const char* mcpKey, const char* flagKey) {
    std::string v = OptString(args, mcpKey, "");
    if (!v.empty()) extras.emplace(flagKey, std::move(v));
};
put("variable",       "Variable");
put("function",       "Function");
put("function_owner", "FunctionOwner");
put("event_name",     "EventName");
put("target_class",   "TargetClass");
put("struct_type",    "StructType");
reader.AddNode(asset, graph, kind, x, y, extras);
```

The backend turns the `extras` map into `-Key=Value` flag args. When
you add a new kind that needs new extras, both ends need updating —
that's noted in `CLAUDE.md` as a recurring footgun.

## Wiring pins: schema, not MakeLinkTo

You can connect two pins with `Pin->MakeLinkTo(OtherPin)`. Don't.
That call mutates the link arrays on both pins, but skips the schema
review that the editor's drag-drop handler runs. Skipping the
schema costs you several things:

- **Type promotion.** If you connect a `float` source to an `int`
  target pin, the schema inserts a `K2Node_CallFunction` conversion
  node (`Conv_FloatToInt`). `MakeLinkTo` connects them with mismatched
  types and the next compile fails.
- **Wildcard propagation.** Array-library nodes like
  `UK2Node_CallArrayFunction` and `UK2Node_Select` have a `wildcard`
  input that takes its concrete type from whatever is connected to
  it. The schema fires `PinConnectionListChanged` on both owning
  nodes after a successful link, which is the hook those nodes use
  to propagate the type into every linked wildcard slot. Without it
  the `TargetArray` pin stays `wildcard` and compile fails with
  "The type of Target Array is undetermined".
- **Break-others response codes.** The schema can respond with
  `BREAK_OTHERS_A`, `BREAK_OTHERS_B`, `BREAK_OTHERS_AB`,
  `MAKE_WITH_CONVERSION_NODE`, or `MAKE_WITH_PROMOTION`. Each
  models a real editor interaction (replacing an existing link,
  inserting a conversion node). `MakeLinkTo` flattens all of those
  into a single behaviour that doesn't match the editor.

The right entry point is `UEdGraphSchema::TryCreateConnection`:

```cpp
int32 RunWirePinsOp(const FString& Params, const FString& OutputPath, bool bPretty)
{
    const FString AssetPath = ResolveAssetPath(Params);
    FString GraphName, FromNodeId, FromPinSpec, ToNodeId, ToPinSpec;
    FParse::Value(*Params, TEXT("Graph="),    GraphName);
    FParse::Value(*Params, TEXT("FromNode="), FromNodeId);
    FParse::Value(*Params, TEXT("FromPin="),  FromPinSpec);
    FParse::Value(*Params, TEXT("ToNode="),   ToNodeId);
    FParse::Value(*Params, TEXT("ToPin="),    ToPinSpec);

    UBlueprint* BP = LoadMutableBlueprint(AssetPath);
    if (!BP) return 4;
    UEdGraph* Graph = FindGraphByName(BP, GraphName);
    if (!Graph) return 4;

    UEdGraphNode* FromNode = FindNodeByGuid(Graph, FromNodeId);
    UEdGraphNode* ToNode   = FindNodeByGuid(Graph, ToNodeId);
    if (!FromNode || !ToNode) return 4;

    UEdGraphPin* FromPin = FindPinByIdOrName(FromNode, FromPinSpec);
    UEdGraphPin* ToPin   = FindPinByIdOrName(ToNode,   ToPinSpec);
    if (!FromPin || !ToPin) return 4;

    const UEdGraphSchema* Schema = Graph->GetSchema();
    if (!Schema) return 1;

    const FPinConnectionResponse Resp = Schema->CanCreateConnection(FromPin, ToPin);
    if (Resp.Response == CONNECT_RESPONSE_DISALLOW) {
        UE_LOG(LogBlueprintReader, Error, TEXT("WirePins: schema rejected: %s"),
            *Resp.Message.ToString());
        return 1;
    }
    const bool bMade = Schema->TryCreateConnection(FromPin, ToPin);
    if (!bMade) return 1;

    if (!CompileAndSaveBlueprint(BP)) return 5;
    return EmitOk(OutputPath, bPretty);
}
```

We call `CanCreateConnection` first so we can surface the schema's
human-readable rejection message ("Pin types are incompatible",
"Cannot connect exec to data", etc.) verbatim to the agent.
`TryCreateConnection` is the call that actually links and fires
`PinConnectionListChanged`.

### Finding pins by id, name, or friendly name

Agents will pass either pin GUIDs (returned from a prior read) or pin
names. Names come in two flavours: the underlying `FName` (which UE
uses internally) and the friendly display name (which users see in
the graph). We support all three:

```cpp
UEdGraphPin* FindPinByIdOrName(UEdGraphNode* Node, const FString& Spec)
{
    FGuid AsGuid;
    if (FGuid::Parse(Spec, AsGuid)) {
        for (UEdGraphPin* P : Node->Pins) {
            if (P && P->PinId == AsGuid) return P;
        }
    }
    for (UEdGraphPin* P : Node->Pins) {
        if (P && P->GetFName().ToString().Equals(Spec, ESearchCase::IgnoreCase))
            return P;
    }
    for (UEdGraphPin* P : Node->Pins) {
        if (!P) continue;
        const FString Friendly = P->PinFriendlyName.IsEmpty()
            ? FString() : P->PinFriendlyName.ToString();
        if (!Friendly.IsEmpty() && Friendly.Equals(Spec, ESearchCase::IgnoreCase))
            return P;
    }
    return nullptr;
}
```

Friendly-name matching matters for `UFUNCTION` parameters that use
`meta=(DisplayName="Dummy Targets")`: the underlying `FName` is
`TargetArray` but the agent reading the graph from the BP editor will
type the friendly version. Without the fallback, `wire_pins`
silently misses the pin and fails the connection.

## Building FEdGraphPinType (when you need to)

Most of the time you don't. `add_node` lets `AllocateDefaultPins`
populate pins off node state. Workflows that construct pin types
directly (function inputs, member variables) use the constants on
`UEdGraphSchema_K2` — `PC_Float`, `PC_Bool`, `PC_Object` (with a
`UClass*` in `PinSubCategoryObject`), `PC_Struct` (with a
`UScriptStruct*`), plus `ContainerType` for arrays/sets/maps. The
wire format uses snake_case keys and the plugin's
`FBlueprintReaderWireJson::ParseWirePinType` does the round trip.

## The "add a new tool" workflow

`add_node` and `wire_pins` are the model for every other write tool.
Whenever you add one, the steps are:

1. **Plugin** (`BlueprintReaderCommandlet.cpp`):
   - Add an `EOp` enum value.
   - Add a `ParseOp` mapping (`"AddNode"` to `EOp::AddNode`).
   - Add a `RunOneOp` dispatch line.
   - Implement `RunFooOp(Params, OutputPath, bPretty)` using
     `LoadMutableBlueprint`, `FindGraphByName`, `FindNodeByGuid`,
     `CompileAndSaveBlueprint`, `EmitOk`.

2. **MCP interface** (`IBlueprintReader.h`): one pure-virtual method.

3. **MockBlueprintReader**: throw "mock backend is read-only" for
   writes (or hand-coded fixture data for reads).

4. **CommandletBlueprintReader**: serialize args, call `RunOp`. Skip
   empty optional flags (the FParse trap from Chapter 5).

5. **`BlueprintTools.cpp`**: register the tool with input schema +
   handler. Handler calls the `IBlueprintReader` method.

6. **Tests**: a mock case (asserts shape or throws-as-expected) and
   a live case if the op needs a real BP.

7. **Tool count assertions** in `test_tools.cpp` and `test_mcp.cpp`
   need to be bumped (`spec.size() == N`).

If the new tool is a node-spawning op, also add an entry to
`list_node_kinds` in `BlueprintTools.cpp` so the discoverability
table stays in lockstep with the dispatcher.

`CLAUDE.md` ([../../CLAUDE.md](../../CLAUDE.md)) reproduces this
workflow as the project's house style.

## Three reads worth bundling into the response

`add_node` returns more than just a `node_id`. The MCP wrapper
fetches the spawned node's pins and packs them into the result so
the agent can wire to them without a follow-up read:

```cpp
nlohmann::json pinsJson = nlohmann::json::array();
auto g = reader.GetGraph(asset, graph);
for (const auto& n : g.Nodes) {
    if (n.Id == newId) {
        for (const auto& p : n.Pins) {
            nlohmann::json t = {{"category", p.Type.Category}};
            if (p.Type.SubCategory)
                t["sub_category"] = *p.Type.SubCategory;
            if (p.Type.SubCategoryObject)
                t["sub_category_object"] = *p.Type.SubCategoryObject;
            if (p.Type.IsArray) t["is_array"] = true;
            if (p.Type.IsSet)   t["is_set"]   = true;
            if (p.Type.IsMap)   t["is_map"]   = true;
            pinsJson.push_back({
                {"name",      p.Name},
                {"guid",      p.Id},
                {"direction", p.Direction},
                {"type",      std::move(t)},
            });
        }
        break;
    }
}
return {{"ok", true}, {"node_id", newId}, {"pins", std::move(pinsJson)}};
```

This is a small but important pattern: agents reason better when
write tools return enough context for the next call. The cost is a
single extra `GetGraph` per `add_node` — and we're about to make that
free with batching in Chapter 8.

## Checkpoint

Assemble a three-node graph end to end from the MCP server. The
goal: `Event BeginPlay` fires `PrintString` through a `Branch` whose
condition is hard-coded to `true`.

```pwsh
# 1. Spawn the branch node.
$branch = .\scripts\roundtrip.ps1 -Tool add_node -Args @'
{"asset_path":"/Game/AI/BP_TestEnemy","graph_name":"EventGraph",
 "kind":"Branch","x":300,"y":0}
'@

# 2. Spawn a PrintString call.
$print = .\scripts\roundtrip.ps1 -Tool add_node -Args @'
{"asset_path":"/Game/AI/BP_TestEnemy","graph_name":"EventGraph",
 "kind":"CallFunction","function":"PrintString",
 "function_owner":"/Script/Engine.KismetSystemLibrary",
 "x":600,"y":0}
'@

# 3. The Event BeginPlay node is auto-spawned in any new BP. Read
#    the graph to find its node_id (or use find_node with kind=Event).
$find = .\scripts\roundtrip.ps1 -Tool find_node -Args @'
{"asset_path":"/Game/AI/BP_TestEnemy","query":"BeginPlay","kind":"Event"}
'@

# 4. Wire BeginPlay.then -> Branch.execute.
.\scripts\roundtrip.ps1 -Tool wire_pins -Args @"
{"asset_path":"/Game/AI/BP_TestEnemy","graph_name":"EventGraph",
 "from_node":"$($beginPlayId)","from_pin":"then",
 "to_node":"$($branch.node_id)","to_pin":"execute"}
"@

# 5. Wire Branch.True -> PrintString.execute.
.\scripts\roundtrip.ps1 -Tool wire_pins -Args @"
{"asset_path":"/Game/AI/BP_TestEnemy","graph_name":"EventGraph",
 "from_node":"$($branch.node_id)","from_pin":"True",
 "to_node":"$($print.node_id)","to_pin":"execute"}
"@
```

Open the BP in the editor. Three connected nodes, no compile errors.
That's a full editor-grade graph mutation pipeline, driven by an
agent through JSON-RPC calls.

Five round trips for three nodes plus two wires is more than we'd
like. The next chapter collapses that into a single `apply_ops`
call — one compile, one save, plus rich diagnostics if anything in
the middle fails.
