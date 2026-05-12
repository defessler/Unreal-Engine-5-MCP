// Console commands for invoking the runtime introspector from a
// running game / dedicated server / development build. Useful for
// "is the BP I shipped actually carrying what I think it carries"
// triage and for the TCP listener spike below.
//
// All registered under the `bp_reader.` prefix so they're easy to
// find via `?` in the in-game console.

#include "BlueprintReaderRuntimeJson.h"
#include "BlueprintRuntimeIntrospector.h"

#include "HAL/IConsoleManager.h"
#include "Misc/OutputDevice.h"

namespace
{
	// `bp_reader.list <Path>` — dumps every BP under <Path> as
	// summary JSON. Default path is /Game.
	void Cmd_List(const TArray<FString>& Args, UWorld*, FOutputDevice& Ar)
	{
		const FString Path = Args.Num() > 0 ? Args[0] : TEXT("/Game");
		TArray<FBPRRAssetSummary> Assets =
			FBlueprintRuntimeIntrospector::ListBlueprints(Path);
		Ar.Log(FBlueprintReaderRuntimeJson::WriteListString(Assets, /*bPretty=*/true));
	}

	// `bp_reader.read <AssetPath>` — dumps the full blueprint data
	// as JSON. AssetPath is /Game/Foo/BP_Bar or its _C class form.
	void Cmd_Read(const TArray<FString>& Args, UWorld*, FOutputDevice& Ar)
	{
		if (Args.Num() < 1)
		{
			Ar.Logf(ELogVerbosity::Warning,
				TEXT("usage: bp_reader.read <AssetPath>"));
			return;
		}
		TOptional<FBPRRBlueprint> BP =
			FBlueprintRuntimeIntrospector::Read(Args[0]);
		if (!BP.IsSet())
		{
			Ar.Logf(ELogVerbosity::Warning,
				TEXT("bp_reader.read: asset not found: %s"), *Args[0]);
			return;
		}
		Ar.Log(FBlueprintReaderRuntimeJson::WriteObjectString(
			FBlueprintReaderRuntimeJson::BlueprintToJson(*BP), /*bPretty=*/true));
	}

	// Console-command lifetime is process-long; registering at module
	// startup keeps the commands available without explicit linkage from
	// game code. Auto-registered via static initializers below.
	FAutoConsoleCommandWithWorldArgsAndOutputDevice GListCmd(
		TEXT("bp_reader.list"),
		TEXT("List Blueprint assets under a content path. Usage: "
			 "bp_reader.list <Path = /Game>"),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic(&Cmd_List));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GReadCmd(
		TEXT("bp_reader.read"),
		TEXT("Read a Blueprint's class + variables + functions + components "
			 "via UClass reflection (works in cooked builds). Usage: "
			 "bp_reader.read <AssetPath>"),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic(&Cmd_Read));
}
