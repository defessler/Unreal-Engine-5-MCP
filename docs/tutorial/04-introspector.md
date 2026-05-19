# Chapter 4 — The introspector: read a blueprint from disk

You have a commandlet that runs inside a real editor and exits 0. Now
you'll teach it to actually read blueprints. By the end you'll have a
`-Op=Read -Asset=/Game/AI/BP_Foo` command that loads a `UBlueprint`,
walks its graphs and member variables, and emits the same wire-format
JSON your mock backend in chapter 2 returned from fixtures. That JSON
is the round-trip pivot: anything the mock backend can serve from a
fixture, the real backend can now serve from a `.uasset`.

This chapter is dense. UE's blueprint object model has a lot of edges,
and your first introspector pass will probably miss one or two — the
production version is ~700 lines for a reason. We'll cover enough
ground to land a usable `Read` op and call out where production code
goes further.

## The blueprint object model in five minutes

A `UBlueprint` is an editor-only authoring asset. The cooked game ships
a `UBlueprintGeneratedClass` (its compiled output, used at runtime),
not a `UBlueprint`. For introspection you want the authoring object.

The key fields, all reachable from `UBlueprint*`:

| Field                      | Type                              | What it holds                                 |
|----------------------------|-----------------------------------|-----------------------------------------------|
| `ParentClass`              | `TSubclassOf<UObject>`            | The native parent (`AActor`, `ACharacter`, …) |
| `ImplementedInterfaces`    | `TArray<FBPInterfaceDescription>` | Interfaces the BP claims to implement         |
| `NewVariables`             | `TArray<FBPVariableDescription>`  | Member variables added in this BP             |
| `UbergraphPages`           | `TArray<UEdGraph*>`               | The big "EventGraph" tab(s)                   |
| `FunctionGraphs`           | `TArray<UEdGraph*>`               | One per user-authored function                |
| `MacroGraphs`              | `TArray<UEdGraph*>`               | One per macro                                 |
| `DelegateSignatureGraphs`  | `TArray<UEdGraph*>`               | Delegate signatures (rare; skip for now)      |

Each `UEdGraph` has `Nodes` (a `TArray<UEdGraphNode*>`). Each node has
`Pins` (a `TArray<UEdGraphPin*>`). Each pin has:

- `PinId` — an `FGuid` you'll use in the wire format.
- `PinName` — the human-readable name.
- `Direction` — `EGPD_Input` or `EGPD_Output`.
- `PinType` — an `FEdGraphPinType` value with the category /
  subcategory / object / container flags you need for `BPPinType`.
- `DefaultValue` — the string-form default shown in the Details panel
  when nothing is connected.
- `LinkedTo` — `TArray<UEdGraphPin*>` of pins on other nodes.

K2 nodes (`UK2Node_*`) are blueprint-specific subclasses of
`UEdGraphNode`. You can pattern-match with `Cast<UK2Node_CallFunction>(N)`
to pull out call-target info, `Cast<UK2Node_VariableGet>(N)` for
variable refs, and so on. The full taxonomy lives in the
`BlueprintGraph` module; production code matches the dozen or so most
common kinds in
`Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintIntrospector.cpp`.

## The wire shape (production version)

The mock backend in chapter 2 emitted summary + metadata. The full
shape — what you'll grow toward — adds graphs and per-function
breakdowns. For chapter 4 we'll target this slice:

```json
{
  "asset_path": "/Game/AI/BP_Enemy",
  "name": "BP_Enemy",
  "parent_class": "ACharacter",
  "interfaces": [],
  "variables": [{
    "name": "Health",
    "type": {"category":"real","sub_category":"float",
             "sub_category_object":null,
             "is_array":false,"is_set":false,"is_map":false},
    "default_value": "100.0",
    "category": "Combat",
    "is_replicated": true,
    "is_editable": true
  }],
  "functions": [{"name":"TakeDamage"}, {"name":"OnDeath"}],
  "graphs":    [{"name":"EventGraph", "type":"EventGraph"},
                {"name":"ConstructionScript", "type":"Construction"}]
}
```

The shape conventions, repeated from chapter 2 because they bite a lot
of first-timers:

- **snake_case** keys on the wire, **PascalCase** in your C++ struct.
- **`null`** for empty optional strings, not `""`.
- **Package paths** (`/Game/AI/BP_Enemy`), not object paths
  (`/Game/AI/BP_Enemy.BP_Enemy`). The `Blueprint->GetPathName()` API
  returns object paths — strip the suffix.
- **`UserConstructionScript`** is UE 5.7's actual graph name for the
  construction script; classify it as `WireType="Construction"` not
  `Function`. Same gotcha called out in `CLAUDE.md`.

Full wire-protocol rationale in `../design/06-wire-protocol.md`.

## The struct mirror

Add `FBlueprintInfo` to your plugin so you have one C++ value to fill
and one place to JSON-serialize. Production code uses
`USTRUCT(BlueprintType)` with `UPROPERTY` reflection so
`FJsonObjectConverter` does most of the work — but the
container-mapping rules want the field names PascalCase, which doesn't
match the snake_case wire shape. The production code therefore hand-
writes the JSON conversion (see
`Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintReaderWireJson.cpp`).
Follow that pattern.

`Public/BlueprintReaderTypes.h`:

```cpp
#pragma once
#include "CoreMinimal.h"

struct FBPStructuredPinType
{
    FString Category;          // "real", "object", "bool"...
    FString SubCategory;       // "float" when Category="real"
    FString SubCategoryObject; // e.g. "Actor" — empty when none
    bool bIsArray = false;
    bool bIsSet   = false;
    bool bIsMap   = false;
};

struct FBPVariableInfo
{
    FString Name;
    FBPStructuredPinType Type;
    FString DefaultValue;
    FString Category;
    bool bIsReplicated = false;
    bool bIsEditable   = false;
};

struct FBPGraphInfo
{
    FString Name;
    FString WireType;  // "EventGraph" / "Function" / "Macro" / "Construction"
};

struct FBPFunctionInfo
{
    FString Name;
};

struct FBlueprintInfo
{
    FString AssetPath;
    FString Name;
    FString ParentClass;
    TArray<FString>          Interfaces;
    TArray<FBPVariableInfo>  Variables;
    TArray<FBPFunctionInfo>  Functions;
    TArray<FBPGraphInfo>     Graphs;
};
```

## Reading a blueprint off disk

Add this helper to your commandlet. `LoadObject<UBlueprint>` is the
single most important call in the chapter.

```cpp
#include "Engine/Blueprint.h"
#include "UObject/UObjectGlobals.h"

UBlueprint* LoadBlueprintReadOnly(const FString& AssetPath)
{
    // LOAD_NoWarn  - don't spam the log if the asset is missing
    // LOAD_Quiet   - same for cook-time warnings about editor-only assets
    // The default LoadObject behavior is fine for everything else; we
    // explicitly do NOT pass LOAD_DisableDependencyPreloading because
    // we want the asset registry's normal pre-warm.
    return LoadObject<UBlueprint>(
        /*Outer=*/ nullptr,
        /*Name =*/ *AssetPath,
        /*Filename=*/ nullptr,
        /*LoadFlags=*/ LOAD_NoWarn | LOAD_Quiet,
        /*Sandbox=*/ nullptr);
}
```

A few notes UE undersells:

- **The path is a package path** (`/Game/AI/BP_Enemy`), not a filesystem
  path. UE resolves it via the asset registry / package name resolver.
  Convert from object path if your input came from a UE API:
  `Path.Left(Path.Find(TEXT("."))).TrimQuotes()`.
- **`nullptr` return is the normal "not found" signal.** Don't throw.
  Report it as a clean error to your caller (your commandlet writes
  `{"error":"asset not found"}` JSON to stdout and `return 1`).
- **Once you have the BP loaded, it's GC-rooted by `Outer` during
  commandlet execution.** You don't need `TStrongObjectPtr` for the
  scope of one `Main()`.

## Pin types

Map UE's `FEdGraphPinType` to your wire struct. The production version
of this helper lives at
`Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintIntrospector.cpp`
(look for `MakeStructuredPinType`). Cut down:

```cpp
#include "EdGraph/EdGraphPin.h"

FBPStructuredPinType ToStructured(const FEdGraphPinType& T)
{
    FBPStructuredPinType S;
    S.Category    = T.PinCategory.ToString();
    S.SubCategory = T.PinSubCategory.IsNone() ? FString() : T.PinSubCategory.ToString();
    if (UObject* O = T.PinSubCategoryObject.Get())
    {
        // Wire format uses the short name for native classes ("Actor")
        // and the package path for BP classes ("/Game/AI/BP_Enemy").
        if (UClass* C = Cast<UClass>(O))
        {
            S.SubCategoryObject = C->IsNative()
                ? C->GetName()
                : O->GetPathName();
        }
        else
        {
            S.SubCategoryObject = O->GetPathName();
        }
    }
    S.bIsArray = T.IsArray();
    S.bIsSet   = T.IsSet();
    S.bIsMap   = T.IsMap();
    return S;
}
```

## Walking variables

```cpp
FBPVariableInfo VariableToInfo(const FBPVariableDescription& V)
{
    FBPVariableInfo Out;
    Out.Name         = V.VarName.ToString();
    Out.Category     = V.Category.ToString();
    Out.Type         = ToStructured(V.VarType);
    Out.DefaultValue = V.DefaultValue;
    Out.bIsReplicated = (V.PropertyFlags & CPF_Net) != 0;
    Out.bIsEditable   = (V.PropertyFlags & CPF_Edit) != 0;
    return Out;
}
```

`FBPVariableDescription` is what `Blueprint->NewVariables` holds. The
flag set is bitwise — `CPF_Net` for replicated, `CPF_Edit` for
"exposed to Details panel". The rest (`CPF_Transient`,
`CPF_BlueprintReadOnly`, `CPF_ExposeOnSpawn`) are useful follow-ups
once you wire up the write tools in later chapters.

## Walking graphs

Every BP has a fixed bucket layout. You want to walk all four (well,
three — skip delegate signatures unless your wire shape claims it).
Watch the `UserConstructionScript` rename — the editor still surfaces
it as "Construction Script" in the My Blueprint panel, but the graph
object's name is `UserConstructionScript`.

```cpp
#include "EdGraph/EdGraph.h"

FString ClassifyGraph(UBlueprint* BP, UEdGraph* Graph)
{
    if (!Graph) return TEXT("Unknown");

    const FString Name = Graph->GetName();
    if (Name == TEXT("UserConstructionScript") ||
        Name == TEXT("ConstructionScript"))
        return TEXT("Construction");

    if (BP->UbergraphPages.Contains(Graph))    return TEXT("EventGraph");
    if (BP->FunctionGraphs.Contains(Graph))    return TEXT("Function");
    if (BP->MacroGraphs.Contains(Graph))       return TEXT("Macro");
    return TEXT("Unknown");
}

void CollectGraphs(UBlueprint* BP, FBlueprintInfo& Info)
{
    auto AddRange = [&](const TArray<UEdGraph*>& Range) {
        for (UEdGraph* G : Range)
        {
            if (!G) continue;
            FBPGraphInfo Entry;
            Entry.Name     = G->GetName();
            Entry.WireType = ClassifyGraph(BP, G);
            Info.Graphs.Add(Entry);

            // For chapter 4 we don't walk the nodes themselves — that's
            // the get_graph tool, which arrives in chapter 5. Stopping
            // here keeps the Read op fast and the wire shape compact.
        }
    };
    AddRange(BP->UbergraphPages);
    AddRange(BP->FunctionGraphs);
    AddRange(BP->MacroGraphs);
}
```

`Functions` is just the function-graph names projected to
`{name: "..."}`:

```cpp
void CollectFunctions(UBlueprint* BP, FBlueprintInfo& Info)
{
    for (UEdGraph* G : BP->FunctionGraphs)
    {
        if (!G) continue;
        // Skip the construction script — it's a graph, not a function.
        const FString Name = G->GetName();
        if (Name == TEXT("UserConstructionScript")) continue;

        FBPFunctionInfo F;
        F.Name = Name;
        Info.Functions.Add(F);
    }
}
```

## Putting it together: BuildInfo

```cpp
FBlueprintInfo BuildInfo(UBlueprint* BP)
{
    FBlueprintInfo Info;
    if (!BP) return Info;

    // Wire format wants the package path, not the object path.
    // UBlueprint::GetPathName returns "/Game/AI/BP_Enemy.BP_Enemy" —
    // strip the suffix.
    FString FullPath = BP->GetPathName();
    int32 Dot = INDEX_NONE;
    Info.AssetPath = FullPath.FindChar(TEXT('.'), Dot)
                        ? FullPath.Left(Dot)
                        : FullPath;
    Info.Name = BP->GetName();
    Info.ParentClass = BP->ParentClass
                          ? BP->ParentClass->GetName()
                          : TEXT("");

    for (const FBPInterfaceDescription& IF : BP->ImplementedInterfaces)
    {
        if (IF.Interface) Info.Interfaces.Add(IF.Interface->GetName());
    }
    for (const FBPVariableDescription& V : BP->NewVariables)
    {
        Info.Variables.Add(VariableToInfo(V));
    }
    CollectGraphs(BP, Info);
    CollectFunctions(BP, Info);
    return Info;
}
```

## Serializing to wire JSON

UE's `FJsonObject` is awkward for null fields (it doesn't have a
first-class null value as a setter — you have to use
`SetField(Key, MakeShared<FJsonValueNull>())`). Wrap the pattern:

```cpp
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

static void SetStringOrNull(TSharedRef<FJsonObject> Obj,
                            const FString& Key, const FString& Value)
{
    if (Value.IsEmpty())
        Obj->SetField(Key, MakeShared<FJsonValueNull>());
    else
        Obj->SetStringField(Key, Value);
}

static TSharedRef<FJsonObject> TypeToJson(const FBPStructuredPinType& T)
{
    auto O = MakeShared<FJsonObject>();
    O->SetStringField(TEXT("category"), T.Category);
    SetStringOrNull(O, TEXT("sub_category"), T.SubCategory);
    SetStringOrNull(O, TEXT("sub_category_object"), T.SubCategoryObject);
    O->SetBoolField(TEXT("is_array"), T.bIsArray);
    O->SetBoolField(TEXT("is_set"),   T.bIsSet);
    O->SetBoolField(TEXT("is_map"),   T.bIsMap);
    return O;
}

static TSharedRef<FJsonObject> VariableToJson(const FBPVariableInfo& V)
{
    auto O = MakeShared<FJsonObject>();
    O->SetStringField(TEXT("name"), V.Name);
    O->SetObjectField(TEXT("type"), TypeToJson(V.Type));
    SetStringOrNull(O, TEXT("default_value"), V.DefaultValue);
    SetStringOrNull(O, TEXT("category"), V.Category);
    O->SetBoolField(TEXT("is_replicated"), V.bIsReplicated);
    O->SetBoolField(TEXT("is_editable"),   V.bIsEditable);
    return O;
}

FString InfoToWireJson(const FBlueprintInfo& Info, bool bPretty)
{
    auto Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("asset_path"),   Info.AssetPath);
    Root->SetStringField(TEXT("name"),         Info.Name);
    Root->SetStringField(TEXT("parent_class"), Info.ParentClass);

    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (const FString& I : Info.Interfaces)
            Arr.Add(MakeShared<FJsonValueString>(I));
        Root->SetArrayField(TEXT("interfaces"), Arr);
    }
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (const FBPVariableInfo& V : Info.Variables)
            Arr.Add(MakeShared<FJsonValueObject>(VariableToJson(V)));
        Root->SetArrayField(TEXT("variables"), Arr);
    }
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (const FBPFunctionInfo& F : Info.Functions)
        {
            auto O = MakeShared<FJsonObject>();
            O->SetStringField(TEXT("name"), F.Name);
            Arr.Add(MakeShared<FJsonValueObject>(O));
        }
        Root->SetArrayField(TEXT("functions"), Arr);
    }
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (const FBPGraphInfo& G : Info.Graphs)
        {
            auto O = MakeShared<FJsonObject>();
            O->SetStringField(TEXT("name"), G.Name);
            O->SetStringField(TEXT("type"), G.WireType);
            Arr.Add(MakeShared<FJsonValueObject>(O));
        }
        Root->SetArrayField(TEXT("graphs"), Arr);
    }

    FString Out;
    if (bPretty)
    {
        auto W = TJsonWriterFactory<>::Create(&Out);
        FJsonSerializer::Serialize(Root, W);
    }
    else
    {
        auto W = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
        FJsonSerializer::Serialize(Root, W);
    }
    return Out;
}
```

## Updating Main() to dispatch -Op

Extend the commandlet to parse `-Op=Read`/`-Asset=...`/`-Output=...`:

```cpp
int32 UBPRCommandlet::Main(const FString& Params)
{
    FString OpName;
    FParse::Value(*Params, TEXT("-Op="), OpName);

    // For chapter 4 we only implement Read. Chapter 5 grows this dispatch.
    if (OpName.Equals(TEXT("Read"), ESearchCase::IgnoreCase))
    {
        FString Asset;
        if (!FParse::Value(*Params, TEXT("-Asset="), Asset) || Asset.IsEmpty())
        {
            UE_LOG(LogBlueprintReader, Error,
                TEXT("-Op=Read requires -Asset=/Game/Path/To/BP"));
            return 1;
        }

        UBlueprint* BP = LoadBlueprintReadOnly(Asset);
        if (!BP)
        {
            UE_LOG(LogBlueprintReader, Error,
                TEXT("asset not found: %s"), *Asset);
            return 2;
        }

        const bool bPretty = !FParse::Param(*Params, TEXT("Compact"));
        const FBlueprintInfo Info = BuildInfo(BP);
        const FString Json = InfoToWireJson(Info, bPretty);

        // Default emit target: stdout. Optional -Output=<path> writes
        // to a file instead. This matters in chapter 5+ when the MCP
        // server needs to read the result without scraping the UE log.
        FString OutPath;
        if (FParse::Value(*Params, TEXT("-Output="), OutPath) && !OutPath.IsEmpty())
        {
            if (!FFileHelper::SaveStringToFile(Json, *OutPath))
            {
                UE_LOG(LogBlueprintReader, Error,
                    TEXT("failed to write output: %s"), *OutPath);
                return 3;
            }
        }
        else
        {
            // Bypass UE's stdout-redirection quirks by writing the JSON
            // directly. UE_LOG would prefix every line with timestamps
            // and category tags — fine for humans, bad for machine
            // parsing. Production code uses GetStdHandle/WriteFile
            // directly; for chapter 4 a simple FPlatformMisc print works.
            FPlatformMisc::LocalPrint(*(Json + LINE_TERMINATOR));
        }

        return 0;
    }

    UE_LOG(LogBlueprintReader, Warning,
        TEXT("no -Op= specified or unknown op: '%s'"), *OpName);
    return 1;
}
```

`FPlatformMisc::LocalPrint` is the polite first version. The
production daemon mode writes to `GetStdHandle(STD_OUTPUT_HANDLE)`
directly because UE in some configs redirects `stdout` to its log
device — see the "UE stdio in commandlet mode" note in `CLAUDE.md`. For
a one-shot commandlet, `LocalPrint` is fine.

## End to end

Build the plugin (same `Build.bat` invocation as chapter 3), then:

```bat
"D:\Projects\Unreal Engine 5\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" ^
  "D:\Projects\UE5_MCP\LyraStarterGame.uproject" ^
  -run=BPR -Op=Read -Asset=/Game/AI/BP_Foo ^
  -nullrhi -nosplash -unattended -nopause
```

You'll get a noisy UE log to stderr and a clean JSON blob mixed into
stdout. Filter with PowerShell (PowerShell preserves the order; the
trick is finding your JSON in the UE log slurry):

```pwsh
$proc = Start-Process `
  "D:\Projects\Unreal Engine 5\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  -ArgumentList @(
    '"D:\Projects\UE5_MCP\LyraStarterGame.uproject"',
    '-run=BPR','-Op=Read',
    '-Asset=/Game/AI/BP_Foo',
    '-Output=D:\Temp\bp.json',
    '-nullrhi','-nosplash','-unattended','-nopause'
  ) -Wait -NoNewWindow -PassThru

if ($proc.ExitCode -eq 0) {
    Get-Content D:\Temp\bp.json | ConvertFrom-Json | Format-List
}
```

`-Output=` is exactly why the production commandlet supports it: when
you're driving this from another process, redirecting JSON to a file
sidesteps every stdio quirk UE has.

For comparison, the production introspector covers nodes, pins, and
connections too — but only when the corresponding op
(`-Op=Graph -Graph=EventGraph`) asks for it. Read is intentionally
lean: agents that want the whole event-graph topology call `get_graph`
separately. Keeping Read fast is what lets `list_blueprints + read` be
the default cheap discovery path.

## Checkpoint

Three things should be true:

1. **`-Op=Read -Asset=/Game/AI/BP_Foo -Output=...`** writes a file with
   valid JSON. `ConvertFrom-Json` (PowerShell) or `python -m json.tool`
   parses it without complaint.
2. The JSON's **`asset_path` is a package path** (no `.BP_Foo` suffix),
   `parent_class` is the short class name (`Actor`, `Character`, ...),
   and `variables[]` has one entry per `NewVariables[]` on the BP.
3. **Exit code is 0 for a found asset, non-zero with a logged error for
   a missing one.** `echo $LASTEXITCODE` after each run.

Failure modes worth recognizing:

- **`asset not found`** for a BP you can see in the editor — your path
  is wrong. Confirm with the Content Browser's right-click > "Copy
  Reference"; strip the `Blueprint'...'` wrapper and the trailing
  `.BP_Foo` to get a package path.
- **`parent_class` is empty** — `ParentClass` is `nullptr`. Either the
  BP failed to fully load (check log for `LogLinker: Warning`) or its
  parent class was renamed/removed and the BP is broken. Open in the
  editor and check the "Blueprint > Class Settings > Parent Class"
  field; if it's red, that's your problem.
- **`graphs[]` lists `UserConstructionScript` with `type: "Function"`** —
  you didn't apply the special-case in `ClassifyGraph`. Fix to
  `Construction`.
- **Empty `variables[]` on a BP that visibly has them** — the variables
  may live on an *inherited* parent BP rather than `NewVariables`. The
  introspector here only reports BP-local additions; inheriting is a
  separate concern, surfaced by the production `IntrospectClass` tool
  in later chapters. (Compare:
  `Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintIntrospector.cpp`.)
- **Garbage characters in JSON, or the JSON appears mid-log** — your
  stdout writer is interleaving with `UE_LOG`. Use `-Output=` until you
  graduate to the direct `WriteFile` path in chapter 6.

When you have a clean wire-format JSON describing a real `.uasset`,
you've closed the loop: the same shape your mock backend served from
fixtures now comes from an actual blueprint. Chapter 5 wires that into
your MCP server's `commandlet` backend so `tools/call read_blueprint`
launches `UnrealEditor-Cmd.exe`, drives this op, parses the JSON, and
returns it through the same `IBlueprintReader` interface the mock
backend implements.
