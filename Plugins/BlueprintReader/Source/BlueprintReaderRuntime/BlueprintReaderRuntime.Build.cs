// BlueprintReaderRuntime — runtime-safe BP introspection.
//
// Lives in non-editor builds (packaged games / servers / clients) and
// reads what UE preserves through cook: UClass reflection, CDO default
// values, UFunction signatures, asset registry entries, interfaces.
// What it CAN'T read in a cooked build (and would just return empty
// for): K2 node graphs (visual graph is stripped during cook — only
// compiled bytecode remains), source-level event-graph topology.
//
// Deliberately depends on only Core / CoreUObject / Engine / Json /
// AssetRegistry + Networking/Sockets (for the opt-in TCP listener).
// No editor modules. Verified by building the project's non-editor
// Game target — UBT skips editor modules entirely there, so any
// editor dep would break the packaged build.

using UnrealBuildTool;

public class BlueprintReaderRuntime : ModuleRules
{
	public BlueprintReaderRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json",
			"JsonUtilities",
			"AssetRegistry",
			// Optional TCP listener — same wire format as the editor-side
			// BlueprintReaderLiveServer so the MCP server can speak the
			// same protocol whether the target is editor or a packaged
			// game. Opt-in: only listens when CVar `bp.reader.listen`
			// is non-zero.
			"Sockets",
			"Networking"
		});
	}
}
