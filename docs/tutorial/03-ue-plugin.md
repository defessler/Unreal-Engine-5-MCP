# Chapter 3 — The UE editor plugin

You have an MCP server. Now you need something that can actually read
blueprints — which means crossing into Unreal Engine. The bridge is an
editor-only UE plugin that exposes a *commandlet*: a UClass-derived
entry point you can invoke headlessly with
`UnrealEditor-Cmd.exe -run=BPR`. The commandlet runs inside
a real editor process (asset registry, GC, the works) but exits when
done — no window, no PIE, no human in the loop.

In this chapter you'll:

1. Lay out a UE plugin (`.uplugin`, `Source/Module/Module.Build.cs`,
   `Module.cpp`).
2. Add a `UCommandlet` subclass with a `Main(const FString&)` that
   prints a log line and returns 0.
3. Build it against your engine.
4. Invoke it with `UnrealEditor-Cmd.exe` and see the log.

By the end you'll have a UE-side entry point you can extend in
chapter 4 with real blueprint introspection.

## Why a commandlet (and not a module callback)

The MCP server is a separate process. It needs to launch the editor,
ask it a question, get an answer, and exit. That's exactly the
commandlet contract: spawn `UnrealEditor-Cmd.exe -run=YourCmdlet
-Foo=bar`, the editor boots into your commandlet, you do work,
`return 0;`, the editor tears down, the spawning process reads stdout.

Long-running modes (a TCP listener, a stdin daemon) come later. They
all still start as commandlets — the commandlet contract is just the
"how do we hand control to your code in a headless editor" handshake.

For comparison, the production server has three modes:

| Backend       | What it spawns                                  |
|---------------|-------------------------------------------------|
| `mock`        | Nothing (chapter 2 — fixture files)             |
| `commandlet`  | `UnrealEditor-Cmd.exe -run=BPR`     |
| `live`        | Talks to a running editor over TCP (chapter 9+) |

Chapter 3 wires up commandlet mode.

## Plugin layout

A UE plugin is a directory under `<Project>/Plugins/` with this shape:

```
Plugins/BlueprintReader/
├── BlueprintReader.uplugin
└── Source/
    └── BlueprintReaderEditor/
        ├── BlueprintReaderEditor.Build.cs
        ├── Public/
        │   └── BlueprintReaderCommandlet.h
        └── Private/
            ├── BlueprintReaderEditor.cpp     (module impl)
            └── BlueprintReaderCommandlet.cpp
```

A few rules UE enforces that bite first-timers:

- The module's folder name (`BlueprintReaderEditor`) must match the
  `Name` field in `.uplugin` and the class name in `.Build.cs`. Be
  consistent.
- Editor-only code lives in a module typed `"Editor"` in the
  `.uplugin`. Don't mark it `"Runtime"` — it won't be packaged into
  shipping builds and you'll forget why your `#include`s break later.
- The `Public/` headers are visible to other modules; `Private/` is
  not. For an editor plugin nothing else depends on, you can keep
  everything in `Private/`. The commandlet header has to be `Public/`
  *only* if other modules use it — they don't.

## BlueprintReader.uplugin

```json
{
    "FileVersion": 3,
    "Version": 1,
    "VersionName": "0.1.0",
    "FriendlyName": "Blueprint Reader",
    "Description": "Headless blueprint introspection for the UE5_MCP server.",
    "Category": "Editor",
    "CanContainContent": false,
    "EnabledByDefault": true,
    "Modules": [
        {
            "Name": "BlueprintReaderEditor",
            "Type": "Editor",
            "LoadingPhase": "Default"
        }
    ]
}
```

Two fields earn their keep:

- **`"Type": "Editor"`** — module only loads in editor / commandlet
  builds. Shipping cooks skip it. The cooked game stays small and your
  `UnrealEd`-dependent code can't accidentally end up in a runtime
  build.
- **`"LoadingPhase": "Default"`** — most editor plugins want this.
  `PostEngineInit` is a knob you only need if your `StartupModule()`
  touches asset-registry data that other modules populate during
  startup; the commandlet itself runs *after* the entire editor is up,
  so `Default` is fine. If you graduate to a `StartupModule()` that
  needs the asset registry pre-warmed, bump to `PostEngineInit`.

For comparison the production `.uplugin` lives at
`Plugins/BlueprintReader/BlueprintReader.uplugin` and adds a second
Runtime module (an empty stub for now) plus a `PreBuildSteps` hook that
also builds the MCP server. You don't need either yet.

## BlueprintReaderEditor.Build.cs

```csharp
using UnrealBuildTool;

public class BlueprintReaderEditor : ModuleRules
{
    public BlueprintReaderEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "UnrealEd",          // UCommandlet, FBlueprintEditorUtils,
                                 // FKismetEditorUtilities all live here
            "BlueprintGraph",    // UEdGraph, K2Node_* types
            "Json",              // FJsonObject for output
            "JsonUtilities",     // FJsonObjectConverter
            "AssetRegistry",     // listing assets without loading them
        });
    }
}
```

A few warnings worth pre-flighting:

- **Don't add `Kismet` or `KismetCompiler`.** Despite the `Kismet2/`
  include path, the API you want (`FBlueprintEditorUtils`,
  `FKismetEditorUtilities`) lives in `UnrealEd`. `Kismet`/
  `KismetCompiler` are different modules — adding them links nothing
  useful and may cause cyclical deps depending on your engine version.
- **`UnrealEd` is huge.** Build times shoot up the first time. If you
  haven't already done a full editor build, expect 20+ minutes the
  first time. After that, incremental rebuilds of plugin-only code
  take 5–10s.

The production module also depends on `AssetTools`, `Sockets`,
`Networking`, `MaterialEditor`, `UMG`, `UMGEditor`, `AIModule` for the
write-side tooling. Add those when you need them, not before.

## The module class

`Private/BlueprintReaderEditor.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FBlueprintReaderEditorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        UE_LOG(LogTemp, Log, TEXT("BlueprintReaderEditor module loaded"));
    }
    virtual void ShutdownModule() override {}
};

IMPLEMENT_MODULE(FBlueprintReaderEditorModule, BlueprintReaderEditor)
```

`IMPLEMENT_MODULE` is the only piece UBT requires. The argument is the
module name from `.uplugin` and `.Build.cs` — getting that wrong gives
you "module not found" at editor startup, not a compile error.

## The commandlet

`Public/BlueprintReaderCommandlet.h`:

```cpp
#pragma once

#include "Commandlets/Commandlet.h"
#include "BlueprintReaderCommandlet.generated.h"

UCLASS()
class UBPRCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    UBPRCommandlet();
    virtual int32 Main(const FString& Params) override;
};
```

`Private/BlueprintReaderCommandlet.cpp`:

```cpp
#include "BlueprintReaderCommandlet.h"

#include "Misc/Parse.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintReader, Log, All);

UBPRCommandlet::UBPRCommandlet()
{
    IsClient = false;
    IsServer = false;
    IsEditor = true;
    LogToConsole = true;
    ShowErrorCount = false;
}

int32 UBPRCommandlet::Main(const FString& Params)
{
    UE_LOG(LogBlueprintReader, Display,
        TEXT("BlueprintReader commandlet running. Params=[%s]"), *Params);

    // Parse a single optional flag so you can sanity-check the arg parser.
    FString Asset;
    if (FParse::Value(*Params, TEXT("-Asset="), Asset))
    {
        UE_LOG(LogBlueprintReader, Display, TEXT("Asset arg = %s"), *Asset);
    }

    return 0;
}
```

The constructor flags matter:

- `IsEditor = true;` — tells UE this commandlet expects the editor
  module suite (UnrealEd, AssetTools, etc.) to be loaded. Required for
  blueprint inspection. Skip this and `LoadObject<UBlueprint>` returns
  nullptr.
- `LogToConsole = true;` — sends UE_LOG output to stdout/stderr in the
  commandlet host. Without this you'd only see logs in
  `<Project>/Saved/Logs/`.
- `ShowErrorCount = false;` — suppresses UE's "(X errors, Y warnings)"
  summary on exit. Helps when your MCP server is parsing stdout for
  JSON; one less line of noise.

`FParse::Value` extracts `-Asset=/Game/AI/BP_Foo` from `Params`. There's
a gotcha worth knowing now even though it doesn't bite until chapter 4:
**`FParse::Value` chokes on values that contain unescaped quotes**, so
you can't pass a whole JSON object as one flag. The production
commandlet decomposes structured inputs into individual flags
(`-TypeCategory=`, `-TypeSubCategory=`, ...) for exactly this reason —
see the "FParse::Value and JSON values" note in `CLAUDE.md`.

## Enable the plugin

If you're starting fresh, add the plugin reference to your
`.uproject`:

```json
{
    "FileVersion": 3,
    "EngineAssociation": "5.7",
    ...
    "Plugins": [
        {"Name": "BlueprintReader", "Enabled": true}
    ]
}
```

If your `.uproject` doesn't already list `Plugins`, add the key. With
`EnabledByDefault: true` in `.uplugin` you can also skip this — UE picks
it up automatically — but listing it explicitly is the convention for
project-local plugins.

## Building

For a project-targeted build (the recommended path for tutorial work)
you'll use UE's `Build.bat`:

```bat
"D:\Projects\Unreal Engine 5\Engine\Build\BatchFiles\Build.bat" ^
  UE5_MCPEditor Win64 Development ^
  -project="D:\Projects\UE5_MCP\UE5_MCP.uproject" ^
  -NoUba -MaxParallelActions=4 -waitmutex
```

A couple of project-specific knobs that are easy to skip and painful to
debug:

- **`-NoUba -MaxParallelActions=4`** — this machine has a small page
  file and Unreal Build Accelerator (UBA) allocates ~2 GB of VAS per
  worker. Without these flags the link step OOMs around module 800.
  See `CLAUDE.md` for the rationale and other machine-specific
  invariants.
- **`Target.cs`** must declare `BuildSettingsVersion.V6` and
  `TargetBuildEnvironment.Shared`. Without `Shared`, project-target
  builds can't reuse the engine's intermediates and you'll trigger a
  full engine rebuild. Again, see `CLAUDE.md` for the full set of
  required settings and three engine `.Build.cs` patches that
  `Shared`-mode project targets need.
- **First build is slow.** ~20 minutes on a modern desktop with the
  flags above. Incremental rebuilds touching only plugin code take 5–
  10 seconds because UBT walks the include graph and only rebuilds
  affected TUs.

If you're using a launcher-installed engine (not a source build), use
`<EngineRoot>/Engine/Build/BatchFiles/Build.bat` with whatever target
your `.uproject` declares.

## Smoke-running the commandlet

Once the editor target builds, invoke the commandlet headlessly:

```bat
"D:\Projects\Unreal Engine 5\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" ^
  "D:\Projects\UE5_MCP\UE5_MCP.uproject" ^
  -run=BPR ^
  -Asset=/Game/AI/BP_Foo ^
  -nullrhi -nosplash -unattended -nopause
```

The flags after `-run=BPR` are yours; everything before is
UE plumbing. Worth a sentence each:

- **`-run=BPR`** — UE strips the `UBPRCommandlet`
  class prefix/suffix and matches on the remaining name. Several spellings
  work (`-run=BlueprintReaderCommandlet`, `-run=BPR`); pick
  one and stick with it. The production server uses the short form.
- **`-nullrhi`** — no rendering. The headless editor has no window, no
  GPU work. Cuts startup time noticeably.
- **`-nosplash`** — no splash screen.
- **`-unattended`** — UE assumes any modal "OK?" dialog should auto-pick
  the safe answer. Without this a stale ddc lock or a dirty-asset prompt
  hangs the process forever.
- **`-nopause`** — don't wait for keypress on exit. (Some UE builds
  honor `PauseOnExit` from your engine config; this overrides it.)

Expected output (truncated; UE log is verbose):

```
LogInit: Display: Running UE_5_7 from C:/.../Engine/
LogPlugins: Mounting Engine plugin BlueprintReader
LogBlueprintReader: Display: BlueprintReader commandlet running. Params=[-Asset=/Game/AI/BP_Foo -nullrhi -nosplash -unattended -nopause]
LogBlueprintReader: Display: Asset arg = /Game/AI/BP_Foo
Log file closed, 11/12/26 15:00:00
```

`%ERRORLEVEL%` (Windows) or `$?` (PowerShell — `$LASTEXITCODE`) should
be 0.

## Checkpoint

Pass all three of these:

1. **Module loads cleanly.** The line `LogPlugins: Mounting ...
   BlueprintReader` (or similar) appears near the top of the log. If
   you instead see `Failed to load 'BlueprintReader': module not found
   in any plugin directory`, your `.uplugin` isn't under
   `Plugins/BlueprintReader/` or its `Modules[].Name` doesn't match the
   directory under `Source/`.
2. **Commandlet matches.** The line
   `LogCommandletPluginSupport: Found commandlet 'BlueprintReader'`
   (wording varies by version) confirms UE found your `UCommandlet`
   subclass. If you see `Commandlet not found: BlueprintReader`, your
   class is named wrong (must be `U<Name>Commandlet` with the
   `Commandlet` suffix UE strips) or not flagged with `GENERATED_BODY`.
3. **Exit code is 0.** Run the command, then `echo %ERRORLEVEL%` on
   cmd.exe or `$LASTEXITCODE` in PowerShell. Anything non-zero means
   `Main` returned an error or `-unattended` didn't suppress a hang
   (look for the message above the exit).

Diagnosing common failures:

- **`Failed to find target 'UE5_MCPEditor'`** — the editor target
  binary didn't build. Re-run `Build.bat` and watch for compile
  errors. Most often: forgot to add `UnrealEd` to `Build.cs`, missing
  `#include`, or a typo in `.uplugin`'s `Modules[].Name`.
- **Hangs at startup, no log past `LogInit`** — a modal dialog is
  blocking. Add `-unattended` if you forgot; otherwise check
  `<Project>/Saved/Logs/UE5_MCP.log` for the prompt (look for
  `LogWindows: Error: Modal dialog`).
- **`Cannot load asset registry`** — your engine binary doesn't have
  `AssetRegistry` module enabled, or `IsEditor = true;` is missing
  from the commandlet constructor. The latter is the usual cause.
- **`LogClass: Failed to load /Script/BlueprintReaderEditor`** — your
  `IMPLEMENT_MODULE` name doesn't match the `.uplugin`'s
  `Modules[].Name`. They must agree exactly, including case.

When you can run the command, see your custom log line, and exit
cleanly, you have a UE entry point. Chapter 4 puts real blueprint
introspection inside `Main()` — loading a `UBlueprint`, walking its
graphs, and emitting the wire-format JSON your mock backend produced
in chapter 2.
