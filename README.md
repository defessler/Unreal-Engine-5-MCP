# UE5_AI_BP

C++ Unreal Engine 5.7.4 project. Scaffolded against a source-built engine at
`./UnrealEngine/` (sibling to the `.uproject`, gitignored).

## Engine build (deferred)

The engine source has been cloned but **not built**. To bring it online:

```bat
cd UnrealEngine
Setup.bat
GenerateProjectFiles.bat
```

Then open `UnrealEngine\UE5.sln` in Visual Studio 2022 (Game development with C++
workload + Windows 10/11 SDK) and build the **`UnrealEditor`** target in
*Development Editor / Win64*. First build is typically 1–3 hours.

Heads up on disk: `Setup.bat` pulls ~70–80 GB of binary dependencies. Make sure
the destination drive has the room before running it.

## Engine association

`UE5_AI_BP.uproject` currently has `"EngineAssociation": ""` because the
source build hasn't been registered yet. After the editor builds:

```bat
cd UnrealEngine\Engine\Binaries\Win64
UnrealVersionSelector.exe -register
```

That writes a GUID under `HKCU\SOFTWARE\Epic Games\Unreal Engine\Builds`. Then
either:

- Right-click `UE5_AI_BP.uproject` → **Switch Unreal Engine version…** and pick
  the source build, which will write the GUID into the `.uproject`, or
- Edit `EngineAssociation` in `UE5_AI_BP.uproject` directly to that GUID.

After the GUID is bound, generate project files for *this* project:

```bat
"UnrealEngine\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" ^
  -projectfiles -project="D:\Projects\UE5_AI_BP\UE5_AI_BP.uproject" ^
  -game -rocket -progress
```

That produces `UE5_AI_BP.sln` next to the `.uproject`. Build the
`UE5_AI_BPEditor` target to verify the scaffold compiles.

## Layout

See [PLAN.md](PLAN.md).
