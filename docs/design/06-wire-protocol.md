# 06 — Wire protocol

The MCP tool layer, all four backends, and the editor plugin agree on
one set of JSON shapes. This document defines them. The canonical
declaration lives in
`Plugins/BlueprintReader/mcp-server/src/BlueprintReaderTypes.h`; the
plugin's UE-side mirror is at
`Plugins/BlueprintReader/Source/BlueprintReaderEditor/Public/BlueprintReaderTypes.h`.
The two compile from the same header — `#define WITH_UE` swaps in
USTRUCT-decorated mirrors that `FJsonObjectConverter` can drive
(`BlueprintReaderTypes.h:18-39`).

If you're writing a new backend, this is the contract you implement.
If you're writing a new MCP tool, this is the JSON your handler must
return.

See also: [05 — Backends](05-backends.md) for who produces the JSON,
[08 — Errors & diagnostics](08-error-diagnostics.md) for the error
envelope every tool wraps responses in, and `wiki/Tool-Reference.md`
for the full per-tool argument list.


## Conventions

- **snake_case keys throughout.** No camelCase, no PascalCase. JSON
  adapters in `BlueprintReaderTypes.h` enforce this on the C++ side
  (e.g. `BlueprintReaderTypes.h:622-630`: `{"asset_path", ...}`,
  `{"modified_iso", ...}`).
- **Package paths everywhere.** Wire shapes use `/Game/AI/BP_Foo`, not
  the UE object path `/Game/AI/BP_Foo.BP_Foo`. The plugin-side helper
  `BlueprintReaderWireJson::ToPackagePath` strips the trailing
  `.Name`; the CLAUDE.md gotcha section flags this.
- **`null` for empty optional strings.** Adapter at
  `BlueprintReaderTypes.h:378-394` round-trips `std::optional<std::string>`
  to JSON null when unset, never to `""`. This lets consumers
  distinguish "field is empty" from "field is meaningless here" without
  inventing sentinel values.
- **Containers default to empty, not absent.** Arrays come back as
  `[]`, objects as `{}` — readers don't need to check `contains()`
  before iterating.
- **Additive evolution.** New fields decode with sane defaults on old
  payloads; old fields stay around even when new clients don't need
  them. The pin `linked_to` field and node `graph_name` / `graph_type`
  fields are both later additions and both fall back to empty when the
  payload doesn't carry them.


## Type system

`BPPinType` (`BlueprintReaderTypes.h:56-65`) is the wire form of a UE
pin type. It mirrors UE's `FEdGraphPinType` minus the bits the BP
graph editor doesn't use:

```cpp
struct BPPinType {
    BPRString          Category;             // "bool", "int", "real", "object", ...
    BPROptionalString  SubCategory;          // "float" for real, etc.
    BPROptionalString  SubCategoryObject;    // class path for object refs
    bool IsArray = false;
    bool IsSet   = false;
    bool IsMap   = false;
};
```

Wire JSON (`BlueprintReaderTypes.h:395-414`):

```json
{
  "category": "real",
  "sub_category": "float",
  "sub_category_object": null,
  "is_array": false,
  "is_set": false,
  "is_map": false
}
```

The combination `category="real"`, `sub_category="float"` is UE5.7's
canonical float type (5.0 promoted "float" to a sub-category of "real"
so that double-precision pins could share the same machinery). Object
references look like `category="object"`,
`sub_category_object="Actor"`; the sub-category object can be a short
name or a fully-qualified path (`/Script/Engine.StaticMeshActor`).

### Type shorthand

Tools that take a type accept the canonical object form OR a shorthand
string parsed by `tools::ParseTypeArg`
(`tools/TypeShorthand.h:54`). The shorthand grammar
(`TypeShorthand.h:14-40`):

```
"bool"           → bool
"int"            → int
"float"          → real/float
"string"         → string (FString)
"name"           → name (FName)
"object:Actor"   → object reference to AActor
"object:/Game/AI/MyClass.MyClass_C"  // full path also accepted
"struct:FVector" → struct value
"[]float"        → array of float
"{}int"          → set of int
"{string:int}"   → map<string,int>
```

The shorthand exists for token economy: AI agents pay tokens per byte
and reasoning effort per shape, so `{"type": "float"}` beats
`{"type": {"category": "real", "sub_category": "float", ...}}`. The
shorthand parser exists alongside the canonical parser, so both work
for every type-taking tool.


## Core wire types

### BPAssetSummary

`BlueprintReaderTypes.h:308-315` (C++), `:622-637` (JSON adapter).

```json
{
  "asset_path":   "/Game/AI/BP_Enemy",
  "name":         "BP_Enemy",
  "parent_class": "ACharacter",
  "modified_iso": "2026-04-22T18:14:03Z"
}
```

Returned by `list_blueprints`, `list_data_tables`, `list_materials`,
etc. — every "list assets of type X" tool returns an array of these.
`parent_class` is pulled from the asset registry's `ParentClass` tag
without loading the asset; that's what makes the `list_*` tools fast on
large projects.

### BPMetadata

`:357-368`. The top-level shape for a Blueprint:

```json
{
  "asset_path":   "/Game/AI/BP_Enemy",
  "name":         "BP_Enemy",
  "parent_class": "ACharacter",
  "interfaces":   ["IDamageable"],
  "variables":    [BPVariable, ...],
  "functions":    [{"name": "TakeDamage"}, ...],
  "macros":       ["Macro1", ...],
  "graphs":       [BPGraphSummary, ...]
}
```

`functions` here is the lightweight summary form — names only.
`get_function` returns the full `BPFunction` with locals, signature,
and graph body. Same with `graphs`: names + types, deepen via
`get_graph`.

### BPVariable

`:241-252` (C++), `:551-592` (JSON). The basic shape — what
`list_variables` returns:

```json
{
  "name":          "Health",
  "type":          BPPinType,
  "default_value": "100.0",
  "category":      "Combat",
  "is_replicated": true,
  "is_editable":   true
}
```

Three optional fields are emit-when-non-default
(`BlueprintReaderTypes.h:561-572`): `rep_condition` (BP's
Replication Condition dropdown — OwnerOnly, SimulatedOnly, …),
`expose_on_spawn` (the BP variable detail panel checkbox), and
`rep_notify_func` (name of the OnRep_ callback). Older payloads that
predate these omit them entirely; the JSON adapter at lines 583-591
populates them only when present.

### BPFunction

`:285-292`. The full function shape:

```json
{
  "name":    "TakeDamage",
  "inputs":  [BPVariable, ...],
  "outputs": [BPVariable, ...],
  "locals":  [BPVariable, ...],
  "graph":   BPGraph
}
```

Inputs/outputs/locals are reused `BPVariable`s — same shape, different
scope. The function entry node's pin definitions are the source of
truth for the signature; the introspector reads them and emits
`BPVariable`s.

### BPGraph

`:207-213`. A function body or event graph:

```json
{
  "name":        "EventGraph",
  "type":        "EventGraph",
  "nodes":       [BPNode, ...],
  "connections": [BPConnection, ...]
}
```

`type` is one of `"EventGraph" | "Function" | "Macro" | "Construction"`
(`:184` enumerates these). UE 5.7's actual graph name for the
construction script is `UserConstructionScript`; the introspector
classifies both names as `WireType="Construction"` so consumers don't
need to know.

### BPNode

`:136-152` (C++), `:473-505` (JSON). One node in a graph:

```json
{
  "id":       "0F8C3B7A-4E81-4F02-9D11-1A2B3C4D5E6F",
  "class":    "K2Node_IfThenElse",
  "title":    "Branch",
  "position": { "x": 0, "y": 0 },
  "comment":  "Bail out if dead",
  "pins":     [BPPin, ...],
  "meta":     { ... },
  "graph_name": "EventGraph",   // find_node only
  "graph_type": "EventGraph"    // find_node only
}
```

`id` is the K2 node's stable GUID — survives saves, suitable for round-
tripping into `delete_node` / `set_node_position` / `wire_pins`.

### BPConnection

`:167-174`. A pin-to-pin edge:

```json
{
  "from_node": "<guid>",
  "from_pin":  "then",
  "to_node":   "<guid>",
  "to_pin":    "execute"
}
```

Pin spec inside connections is the pin **name**, not its GUID. This is
historical and is what the K2 graph editor itself stores. `wire_pins`
accepts both name and GUID — the plugin's `FindPinByNameOrGuid` resolves
either.

### BPComponent

`:331-337`. One SCS node — `get_components` returns these as a flat
array, parent relationships expressed by `parent` rather than nesting:

```json
{
  "name":    "PickupMesh",
  "class":   "/Script/Engine.StaticMeshComponent",
  "parent":  "DefaultSceneRoot",  // null = root
  "is_root": false
}
```

Components hung off the SCS hierarchy expose this naming because UE's
internal `USCS_Node` does (it's how `FindParentNode` walks the tree).
Native components (defined in the parent C++ class) don't appear here —
`get_components` exposes only the SCS additions.


## Per-pin inline links

Each `BPPin` carries a `linked_to` array — the connections originating
from or terminating at that pin. Adapter at
`BlueprintReaderTypes.h:436-461`:

```json
{
  "id":        "<pin guid>",
  "name":      "then",
  "direction": "Output",
  "type":      BPPinType,
  "default_value": null,
  "linked_to": [
    { "node_id": "<guid>", "pin_id": "<guid>", "pin_name": "execute" }
  ]
}
```

Each `BPPinLink` (`BlueprintReaderTypes.h:81-86`) repeats the
data that's also in `BPGraph.connections`. The duplication is
deliberate (issue #5): a tool that calls `get_node` shouldn't have to
follow up with a `get_graph` and scan `connections[]` just to figure
out what's wired where. Inline links keep `get_node` self-contained.

The `pin_name` field is additive — added later, decodes to empty on
older payloads (`BlueprintReaderTypes.h:428-434`):

```cpp
// pin_name was added later — older wire shapes / fixtures may omit it.
if (j.contains("pin_name") && j["pin_name"].is_string()) {
    j["pin_name"].get_to(v.PinName);
} else {
    v.PinName.clear();
}
```

The graph-level `connections[]` is still authoritative; if you're
walking the graph as a graph, use that. Use `linked_to` when you have
a pin and want its neighbors.


## `BPNode.meta`

`meta` is a free-form JSON object — a nested object, not a stringified
object. (The UE side stores it as `FString` containing serialized JSON
because USTRUCT can't carry a typeless object; the JSON wrapper there
emits/parses it as an inline object so the wire shape stays uniform.)

The introspector populates a handful of well-known keys:

| Key                | When emitted                      | Example value |
|--------------------|-----------------------------------|---------------|
| `kind`             | always                            | `"CallFunction"`, `"VariableGet"`, `"Branch"`, `"Event"`, `"CustomEvent"`, `"Cast"`, `"Sequence"` |
| `targetFunction`   | CallFunction nodes                | `"UKismetMathLibrary::Add_IntInt"` |
| `variableName`     | VariableGet / VariableSet         | `"Health"` |
| `eventName`        | Event / CustomEvent               | `"ReceiveBeginPlay"` |
| `castBroken`       | DynamicCast where the link is dead| `true` |

The mock fixture in `mcp-server/fixtures/BP_Enemy.json:97-99` is a real
example: an Event node with `{ "event_name": "ReceiveBeginPlay" }`. Note
the snake_case variant — the mock fixtures predate the camelCase
convention. `MockBlueprintReader::FindNode`
(`MockBlueprintReader.cpp:300-310`) tolerates both:

```cpp
for (const char* key : {"targetFunction", "function_name",
                        "variableName",   "variable_name",
                        "eventName",      "event_name"}) {
    auto it = n.Meta.find(key);
    if (it != n.Meta.end() && it->is_string()) {
        if (ContainsCI(it->get<std::string>(), query)) return true;
    }
}
```

New plugin code emits camelCase; old fixtures still use snake_case.
Either is a valid wire shape.

## `find_node` hits

`find_node` is the only read that returns nodes spanning multiple
graphs. To make each hit actionable on its own, the plugin tags the
returned `BPNode` with the graph it came from
(`BlueprintReaderTypes.h:484-487`):

```cpp
// GraphName/GraphType only appear on find_node hits — emit only when
// populated so get_node / graph payloads stay unchanged.
if (v.GraphName.has_value()) j["graph_name"] = *v.GraphName;
if (v.GraphType.has_value()) j["graph_type"] = *v.GraphType;
```

The mock backend does the same — `MockBlueprintReader::FindNode`
(`MockBlueprintReader.cpp:315-321`):

```cpp
auto tagAndPush = [&out](const BPNode& src, std::string_view graphName,
                         std::string_view graphType) {
    BPNode copy = src;
    copy.GraphName = std::string(graphName);
    copy.GraphType = std::string(graphType);
    out.push_back(std::move(copy));
};
```

Without these fields, a find result couldn't be passed to `get_node`,
`delete_node`, or `wire_pins` — all three need `graph_name` to locate
the node. Adding them was issue #6; the schema treats them as opt-in
on every `BPNode`, populated only for `find_node`.


## Op frame (live + commandlet)

Backends that round-trip through UE — the commandlet daemon and the
live socket — share a single op-arg format. The MCP server frames each
call as an array of `-Key=Value` flags. From
`LiveBlueprintReader::ListBlueprints`
(`LiveBlueprintReader.cpp:359-364`):

```cpp
std::vector<std::string> args = {"-Op=List"};
if (!path.empty()) args.push_back("-Path=" + std::string(path));
return RunOp(args).get<std::vector<BPAssetSummary>>();
```

The commandlet plugin's `UBlueprintReaderCommandlet::ParseOp` reads
exactly the same `-Op=` flag and dispatches per op kind. The op set
is documented in CLAUDE.md (the
`-Op=List|Read|Graph|Function|Variables|Components|Find|AddVariable|...`
enumeration).

In daemon mode the args are passed as a newline-delimited string of
arg tokens; in live mode they're a JSON array inside an `op` frame
(`LiveBlueprintReader.cpp:310-315`):

```cpp
nlohmann::json frame = {
    {"type", "op"},
    {"id", id},
    {"args", args},
};
```

Quoting differs between transports. Commandlet args go through
`CommandletArgEncoding::EncodeArg` (`backends/CommandletArgEncoding.h:114`)
to handle FParse's quoted-string parsing — see
[05 — Backends](05-backends.md#arg-encoding-for-fparse). Live args ride
over JSON so no quoting is needed; the editor's TCP server just hands
the array elements straight into the same `ParseOp` the commandlet
uses.


## Empty value of optional fields

`std::optional<std::string>` → `null`, not `""`. The adapter
(`BlueprintReaderTypes.h:378-394`):

```cpp
template <> struct adl_serializer<std::optional<std::string>> {
    static void to_json(json& j, const std::optional<std::string>& opt) {
        if (opt.has_value()) j = *opt;
        else                 j = nullptr;
    }
    static void from_json(const json& j, std::optional<std::string>& opt) {
        if (j.is_null()) opt.reset();
        else             opt = j.get<std::string>();
    }
};
```

So `BPNode.comment`, `BPPin.default_value`, `BPVariable.default_value`,
`BPComponent.parent`, and others all come back as JSON `null` when
unset. New tools that expose optional strings should round-trip via the
same `BPROptionalString` typedef rather than declaring a bare
`std::string` and treating `""` as "unset".


## Evolution policy

The wire format is additive. Concretely:

- **Adding a field.** Emit-when-populated on the writer side
  (`BlueprintReaderTypes.h:561-572` shows the pattern for
  `rep_condition` / `expose_on_spawn` / `rep_notify_func`). Decode
  defensively on the reader side: check `contains()` before
  dereferencing, default to a sane zero value otherwise.
- **Renaming a field.** Don't, on the wire. Internal C++ field names
  can change; the JSON key has to stay stable. If you need a different
  semantic, add a new field, keep the old one populated for at least
  one release.
- **Removing a field.** Don't. Even if no consumer reads it; downstream
  AI agents may pattern-match on field presence.

For BPIR — which is the transpile-pipeline AST, not the introspection
wire format — there's a version field instead. See
[07 — BPIR & transpile](07-bpir-and-transpile.md) for how schema
changes propagate through that pipeline.

For tool-level error envelope shape (`_meta.elapsed_ms`, `_meta.tool`,
`_meta.args`), see
[08 — Errors & diagnostics](08-error-diagnostics.md#mcp-error-envelope).


## Source map

| Wire concept           | C++ definition                                | JSON adapter            |
|------------------------|-----------------------------------------------|-------------------------|
| `BPPinType`            | `BlueprintReaderTypes.h:56-65`                | `:395-414`              |
| `BPPin`, `BPPinLink`   | `BlueprintReaderTypes.h:81-99`                | `:416-461`              |
| `BPNode`               | `BlueprintReaderTypes.h:135-152`              | `:473-505`              |
| `BPConnection`         | `BlueprintReaderTypes.h:167-174`              | `:507-522`              |
| `BPGraph`              | `BlueprintReaderTypes.h:207-213`              | `:534-549`              |
| `BPVariable`           | `BlueprintReaderTypes.h:241-252`              | `:551-592`              |
| `BPFunction`           | `BlueprintReaderTypes.h:285-292`              | `:603-620`              |
| `BPMetadata`           | `BlueprintReaderTypes.h:357-368`              | `:639-662`              |
| `BPAssetSummary`       | `BlueprintReaderTypes.h:308-315`              | `:622-637`              |
| `BPComponent`          | `BlueprintReaderTypes.h:331-337`              | `:664-679`              |
| Type shorthand parser  | `tools/TypeShorthand.h`, `TypeShorthand.cpp`  | —                       |
