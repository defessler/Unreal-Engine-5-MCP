# Chapter 6 — Your first write op

So far every tool reads. Chapter 4's introspector pulled metadata out
of a `UBlueprint*`; Chapter 5 piped that metadata through a child
editor process. Now you write.

The milestone for this chapter is `add_variable`. An agent calls it
with a blueprint path, a name, and a type; the plugin loads the BP,
calls `FBlueprintEditorUtils::AddMemberVariable`, compiles, and saves
the package. Re-reading the BP shows the new variable.

## The write pipeline

Every write op in this project follows the same four-stage pipeline:

1. **Load** the BP, anchored against GC.
2. **Mutate** it via a UE editor utility call.
3. **Compile** with `FKismetEditorUtilities::CompileBlueprint`.
4. **Save** the package with `UPackage::SavePackage`.

Steps 3 and 4 are shared by every write tool, so we'll factor them
into a single `CompileAndSaveBlueprint` helper. Step 1 is also shared
(`LoadMutableBlueprint`). The op-specific work is steps 2 plus
calling the helpers.

See [../design/03-plugin-internals.md](../design/03-plugin-internals.md)
for a deeper dive on the write pipeline, including why we save inline
rather than letting the editor dirty-list pile up.

## LoadMutableBlueprint

```cpp
UBlueprint* LoadMutableBlueprint(const FString& AssetPath)
{
    FString Resolved = AssetPath;
    if (!Resolved.Contains(TEXT(".")))
    {
        FString Leaf;
        if (Resolved.Split(TEXT("/"), nullptr, &Leaf,
                           ESearchCase::IgnoreCase, ESearchDir::FromEnd))
        {
            Resolved = Resolved + TEXT(".") + Leaf;
        }
    }
    // LOAD_NoWarn + LOAD_Quiet suppresses the default LogLinker
    // "Failed to load X" warning so that, on the failure path, the
    // agent-facing diagnostic below is the ONLY error in the daemon
    // tail — no duplicate noise to parse around.
    UBlueprint* BP = LoadObject<UBlueprint>(
        nullptr, *Resolved, nullptr, LOAD_NoWarn | LOAD_Quiet);
    if (!BP)
    {
        FBlueprintIntrospector::DiagnoseFailedBlueprintLoad(Resolved);
    }
    return BP;
}
```

The wire format always uses package paths (`/Game/AI/BP_Foo`) but
`LoadObject` wants object paths (`/Game/AI/BP_Foo.BP_Foo`). Append
the leaf if no `.` is present and call once. Failures route through
`DiagnoseFailedBlueprintLoad`, which logs the package-not-found vs
class-mismatch vs missing-uasset-on-disk distinction — useful when
your agent passes a typo'd path and you'd rather not waste a round
trip on a confusing error.

### GC anchor

The same load works for reads. For writes we add one defensive line
near the top of the op:

```cpp
TStrongObjectPtr<UBlueprint> Anchor(BP);
```

`FKismetEditorUtilities::CompileBlueprint` creates and destroys
intermediate UObjects; nodes can be `MarkAsGarbage`'d during
recompilation. Without an explicit anchor, a GC pass kicked off by
the compiler could collect the BP itself if its outer package's
reference graph happens to have weak links to the world. In daemon
mode (Chapter 9) this is rare but real — `TStrongObjectPtr` adds a
strong ref for the scope of the op.

For one-shot commandlet mode this is overkill (the process exits
right after the op), but we add the anchor anywhere the same helper
will later be reused under the daemon. Belt and braces.

## CompileAndSaveBlueprint

Here's the shared tail-end of every write op:

```cpp
bool CompileAndSaveBlueprint(UBlueprint* BP)
{
    TStrongObjectPtr<UBlueprint> Anchor(BP);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

    FCompilerResultsLog Results;
    FKismetEditorUtilities::CompileBlueprint(
        BP, EBlueprintCompileOptions::None, &Results);

    UPackage* Package = BP->GetOutermost();
    if (!Package) return false;

    const FString FileName = FPackageName::LongPackageNameToFilename(
        Package->GetName(), FPackageName::GetAssetPackageExtension());

    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    Args.SaveFlags     = SAVE_NoError;
    Args.Error         = GError;
    return UPackage::SavePackage(Package, BP, *FileName, Args);
}
```

Each line earns its keep:

- `MarkBlueprintAsStructurallyModified` invalidates the compiled
  skeleton class and any cached node validation state. Without it,
  the compiler can short-circuit and produce a recompile that
  doesn't see your edit.
- `CompileBlueprint` walks every K2 graph, regenerates the BP class,
  and runs validation. `FCompilerResultsLog` captures errors and
  warnings; we collect them so apply_ops (Chapter 8) can attribute
  diagnostics back to specific ops.
- `LongPackageNameToFilename` turns `/Game/AI/BP_Foo` into the
  on-disk `.uasset` path, respecting the project's content root.
- `RF_Public | RF_Standalone` are the standard flags the editor sets
  on top-level Blueprint assets.

### When SavePackage fails

The most common cause is a Windows sharing violation: the editor is
open and holds a write handle. Production code probes for this
explicitly so the error message points at the actual problem rather
than at "SavePackage returned false". A minimal version of that
probe:

```cpp
#if PLATFORM_WINDOWS
HANDLE Probe = ::CreateFileW(*FileName,
    GENERIC_READ | GENERIC_WRITE,
    0,                  // no share — fail if anyone has it open
    nullptr,
    OPEN_EXISTING,      // never create or truncate
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
if (Probe == INVALID_HANDLE_VALUE) {
    DWORD Err = ::GetLastError();
    if (Err == ERROR_SHARING_VIOLATION) {
        UE_LOG(LogBlueprintReader, Error,
            TEXT("SavePackage failed: %s — file is locked. "
                 "Close the editor before running commandlet writes."),
            *FileName);
    }
}
::CloseHandle(Probe);
#endif
```

`OPEN_EXISTING` plus zero share-mode is the right shape — early
versions used `IPlatformFile::OpenWrite`, which truncates on success
and would turn a non-lock failure into asset corruption.

## Parent class via FBlueprintTags

Before we get to the write op, one detail you'll have noticed in
read-side metadata: blueprints carry a `parent_class` field, and the
introspector pulls it from `FAssetData` rather than from a loaded
UBlueprint. That's because the asset registry caches a string tag
keyed by `FBlueprintTags::ParentClassPath`:

```cpp
#include "Engine/Blueprint.h"   // brings in FBlueprintTags

FString ParentClass;
AssetData.GetTagValue(FBlueprintTags::ParentClassPath, ParentClass);
```

This matters for write ops too: when `apply_ops` (Chapter 8) creates
a new BP, it has to set the parent class before the asset has any
graphs to read from, and the tag is what `list_blueprints` will use
to surface that parent in the next read. There's a small set of
similar tags (`NativeParentClassPath`, `BlueprintType`, etc.) that
together let read tools attribute classification without a full load.

## The op handler

Now wire it up on the plugin side. Add an `EOp::AddVariable` value,
a `ParseOp` entry, a `RunOneOp` dispatch line, and the implementation:

```cpp
int32 RunAddVariableOp(const FString& Params,
                       const FString& OutputPath, bool bPretty)
{
    const FString AssetPath = ResolveAssetPath(Params);
    FString VarName, DefaultValue, Category;
    FParse::Value(*Params, TEXT("Name="),     VarName);
    FParse::Value(*Params, TEXT("Default="),  DefaultValue);
    FParse::Value(*Params, TEXT("Category="), Category);
    const bool bReplicated = FParse::Param(*Params, TEXT("Replicated"));
    const bool bEditable   = FParse::Param(*Params, TEXT("Editable"));

    FString TypeCategory, TypeSubCategory, TypeSubObject;
    FParse::Value(*Params, TEXT("TypeCategory="),          TypeCategory);
    FParse::Value(*Params, TEXT("TypeSubCategory="),       TypeSubCategory);
    FParse::Value(*Params, TEXT("TypeSubCategoryObject="), TypeSubObject);

    if (AssetPath.IsEmpty() || VarName.IsEmpty() || TypeCategory.IsEmpty())
    {
        UE_LOG(LogBlueprintReader, Error,
            TEXT("AddVariable requires -Asset= -Name= -TypeCategory="));
        return 1;
    }

    auto TypeJson = MakeShared<FJsonObject>();
    TypeJson->SetStringField(TEXT("category"), TypeCategory);
    if (!TypeSubCategory.IsEmpty())
        TypeJson->SetStringField(TEXT("sub_category"), TypeSubCategory);
    if (!TypeSubObject.IsEmpty())
        TypeJson->SetStringField(TEXT("sub_category_object"), TypeSubObject);
    TypeJson->SetBoolField(TEXT("is_array"), FParse::Param(*Params, TEXT("TypeIsArray")));
    TypeJson->SetBoolField(TEXT("is_set"),   FParse::Param(*Params, TEXT("TypeIsSet")));
    TypeJson->SetBoolField(TEXT("is_map"),   FParse::Param(*Params, TEXT("TypeIsMap")));

    FEdGraphPinType PinType;
    if (!FBlueprintReaderWireJson::ParseWirePinType(TypeJson, PinType))
    {
        UE_LOG(LogBlueprintReader, Error, TEXT("AddVariable: failed to build pin type"));
        return 1;
    }

    UBlueprint* BP = LoadMutableBlueprint(AssetPath);
    if (!BP) return 4;

    const FName NewName(*VarName);
    if (FBlueprintEditorUtils::FindNewVariableIndex(BP, NewName) != INDEX_NONE)
    {
        UE_LOG(LogBlueprintReader, Error,
            TEXT("AddVariable: variable %s already exists on %s"),
            *VarName, *AssetPath);
        return 1;
    }

    FBlueprintEditorUtils::AddMemberVariable(BP, NewName, PinType, DefaultValue);
    const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(BP, NewName);
    if (Index == INDEX_NONE) return 1;

    FBPVariableDescription& Var = BP->NewVariables[Index];
    if (!Category.IsEmpty())
        Var.Category = FText::FromString(Category);
    if (bEditable)
        Var.PropertyFlags |= CPF_Edit | CPF_BlueprintVisible;
    if (bReplicated) {
        Var.PropertyFlags |= CPF_Net;
        Var.ReplicationCondition = COND_None;
    }

    if (!CompileAndSaveBlueprint(BP)) return 5;
    return EmitOk(OutputPath, bPretty);
}
```

A few things worth highlighting:

- The type is decomposed into individual flags because passing the
  full BPPinType JSON as a single `-Type=` arg loses every embedded
  quote to `FParse`. The MCP side already knows to send the flag
  decomposition; on the UE side we just reassemble a `TypeJson`
  object and reuse `ParseWirePinType` from the read path.
- `FBlueprintEditorUtils::AddMemberVariable` creates the variable
  but doesn't apply category, replication, or editable flags — those
  live on the `FBPVariableDescription` we mutate post-hoc.
- We return exit code 4 for "asset not found" (so the MCP side can
  raise `AssetNotFound`), 1 for arg errors, 5 for compile/save
  failures. The MCP backend turns each into a typed exception.

### Why no Kismet module dep

`FBlueprintEditorUtils` lives in `UnrealEd` and `FKismetEditorUtilities`
also lives in `UnrealEd` — despite the `Kismet2/` include path. Don't
add `Kismet` or `KismetCompiler` to `BlueprintReaderEditor.Build.cs`:
those are unrelated modules. `BlueprintGraph` is also needed (for the
schema types), and `Json + JsonUtilities` for wire serialization.
Private deps for the whole editor module:

```csharp
PrivateDependencyModuleNames.AddRange(new[] {
    "UnrealEd", "BlueprintGraph", "Json", "JsonUtilities",
    "AssetRegistry", "Networking", "Sockets",
});
```

## The MCP side: AddVariable

The backend interface adds one method:

```cpp
// IBlueprintReader.h
virtual void AddVariable(std::string_view assetPath,
                         std::string_view name,
                         const BPPinType& type,
                         std::string_view defaultValue,
                         std::string_view category,
                         bool replicated, bool editable) = 0;
```

The commandlet implementation passes the type as individual flags:

```cpp
void CommandletBlueprintReader::AddVariable(std::string_view assetPath,
                                            std::string_view name,
                                            const BPPinType& type,
                                            std::string_view defaultValue,
                                            std::string_view category,
                                            bool replicated, bool editable) {
    std::vector<std::wstring> args;
    args.push_back(L"-Op=AddVariable");
    args.push_back(L"-Asset=" + Widen(assetPath));
    args.push_back(L"-Name=" + Widen(name));
    args.push_back(L"-TypeCategory=" + Widen(type.Category));
    if (type.SubCategory && !type.SubCategory->empty())
        args.push_back(L"-TypeSubCategory=" + Widen(*type.SubCategory));
    if (type.SubCategoryObject && !type.SubCategoryObject->empty())
        args.push_back(L"-TypeSubCategoryObject=" + Widen(*type.SubCategoryObject));
    if (type.IsArray) args.push_back(L"-TypeIsArray");
    if (type.IsSet)   args.push_back(L"-TypeIsSet");
    if (type.IsMap)   args.push_back(L"-TypeIsMap");
    if (!defaultValue.empty())
        args.push_back(L"-Default=" + Widen(defaultValue));
    if (!category.empty())
        args.push_back(L"-Category=" + Widen(category));
    if (replicated) args.push_back(L"-Replicated");
    if (editable)   args.push_back(L"-Editable");
    (void)RunOp(args);
}
```

The mock backend gets a write stub that throws — the mock backend is
read-only by design, so adding writes there means the test pretends
something happened that didn't:

```cpp
void MockBlueprintReader::AddVariable(...) override {
    throw BlueprintReaderError("mock backend is read-only");
}
```

## The tool wrapper

`BlueprintTools.cpp` is where the JSON-RPC tool registration lives.
The pattern: build a `ToolDescriptor` with name + description + JSON
schema, then register a handler that pulls args, calls the backend,
and returns a JSON result.

```cpp
{
    ToolDescriptor d;
    d.name = "add_variable";
    d.description =
        "Add a member variable to a blueprint. `type` accepts either a "
        "shorthand string (\"float\", \"int\", \"bool\", \"string\", "
        "\"object:Actor\", \"struct:FVector\", \"[]float\", "
        "\"{string:int}\") or the canonical BPPinType object. "
        "Idempotent: if a variable with this name already exists, "
        "returns {ok:true, already_existed:true} without modifying it.";
    d.input_schema = {
        {"type", "object"},
        {"properties", {
            {"asset_path",    {{"type","string"}}},
            {"name",          {{"type","string"}}},
            {"type",          {{"description","Type shorthand or BPPinType object."}}},
            {"default_value", {{"type","string"}}},
            {"category",      {{"type","string"}}},
            {"replicated",    {{"type","boolean"}}},
            {"editable",      {{"type","boolean"}}},
        }},
        {"required", nlohmann::json::array({"asset_path","name","type"})},
    };
    registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
        const std::string& asset = RequireString(args, "asset_path");
        const std::string& name  = RequireString(args, "name");
        auto typeIt = args.find("type");
        if (typeIt == args.end()) {
            throw std::invalid_argument(R"(missing argument "type")");
        }
        BPPinType type = ParseTypeArg(*typeIt);

        std::string defaultValue = OptString(args, "default_value", "");
        std::string category     = OptString(args, "category",      "");
        bool replicated = args.value("replicated", false);
        bool editable   = args.value("editable",   false);

        // Idempotency: check first. The pre-flight ListVariables call
        // is cheap relative to a write; if a variable with this name
        // already exists, skip the write and report it.
        try {
            auto existing = reader.ListVariables(asset);
            for (const auto& v : existing) {
                if (v.Name == name) {
                    return nlohmann::json{
                        {"ok", true}, {"already_existed", true}};
                }
            }
        } catch (...) {
            // Pre-flight failure doesn't block the write — fall
            // through; the write itself will surface the real error.
        }
        reader.AddVariable(asset, name, type, defaultValue,
                           category, replicated, editable);
        return nlohmann::json{
            {"ok", true}, {"already_existed", false}};
    });
}
```

### Idempotency

Pay attention to the idempotency block. Agents are imperfect — they
re-issue the same `add_variable` call when they lose track of prior
state. Returning `already_existed: true` instead of throwing keeps
the call non-destructive: the agent gets a clear signal, no error,
no log spam.

The plugin's `RunAddVariableOp` rejects duplicates with exit code 1.
We could lift the idempotency check entirely down into the plugin,
but doing it MCP-side means the existence check is one cheap read op
(`ListVariables` returns cached asset registry data in many backends)
rather than a full editor invocation. In commandlet mode that's still
two child-process launches — fine for now; daemon mode in Chapter 9
makes it free.

## Checkpoint

Build the plugin, then drive the MCP server end-to-end:

```pwsh
$env:BP_READER_BACKEND   = "commandlet"
$env:BP_READER_PROJECT   = "D:\Projects\UE5_MCP\UE5_MCP.uproject"
$env:BP_READER_ENGINE_DIR = "D:\Projects\Unreal Engine 5"

Plugins\BlueprintReader\mcp-server\scripts\roundtrip.ps1 `
    -Tool add_variable `
    -Args '{"asset_path":"/Game/AI/BP_TestEnemy","name":"Health","type":"float"}'
```

Expect a result like `{"ok":true,"already_existed":false}` followed
by, on a re-run, `{"ok":true,"already_existed":true}`. Confirm the
variable is real by reading it back:

```pwsh
Plugins\BlueprintReader\mcp-server\scripts\roundtrip.ps1 `
    -Tool list_variables `
    -Args '{"asset_path":"/Game/AI/BP_TestEnemy"}'
```

`Health` (type `float`) should appear in the response array.

If you re-open the project in the editor and check the Variables
panel on BP_TestEnemy, the variable persists across editor sessions:
that's the SavePackage step doing its job. You've completed the
round trip — write to disk via commandlet, read back via commandlet,
verified in the editor UI.

Next chapter: nodes and pins.
