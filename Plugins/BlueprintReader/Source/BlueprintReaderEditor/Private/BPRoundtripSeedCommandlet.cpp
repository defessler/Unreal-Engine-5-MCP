#include "BPRoundtripSeedCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogBPRoundtripSeed, Log, All);

UBPRoundtripSeedCommandlet::UBPRoundtripSeedCommandlet()
{
	IsClient       = false;
	IsServer       = false;
	IsEditor       = true;
	LogToConsole   = true;
	ShowErrorCount = true;
}

namespace
{
	FString EngineTemplateContentDir()
	{
		// Engine layout: <EngineRoot>/Templates/TP_ThirdPersonBP/Content/
		// (Templates/ sits beside Engine/, not inside it.)
		return FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::RootDir(), TEXT("Templates"),
			                TEXT("TP_ThirdPersonBP"), TEXT("Content")));
	}
}

int32 UBPRoundtripSeedCommandlet::Main(const FString& Params)
{
	UE_LOG(LogBPRoundtripSeed, Display,
	       TEXT("BPRoundtripSeed: importing TP_ThirdPersonBP into "
	            "/Game/Imported/ThirdPerson/"));

	const FString TemplateContentDir = EngineTemplateContentDir();
	if (!FPaths::DirectoryExists(TemplateContentDir))
	{
		UE_LOG(LogBPRoundtripSeed, Error,
		       TEXT("Template content dir not found: %s"), *TemplateContentDir);
		return 1;
	}

	// Mount the template's Content/ under a temporary /TPTemplate/ root so
	// the asset registry can scan it and LoadAsset/DuplicateAsset can find
	// the source objects by package path.
	FPackageName::RegisterMountPoint(TEXT("/TPTemplate/"), TemplateContentDir);
	ON_SCOPE_EXIT
	{
		FPackageName::UnRegisterMountPoint(TEXT("/TPTemplate/"), TemplateContentDir);
	};

	auto& ARModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
		"AssetRegistry");
	ARModule.Get().ScanPathsSynchronous({ TemplateContentDir }, /*bForce=*/ true);

	// (source mount path → destination package path)
	//
	// NOTE on scope: in UE 5.7, the TP_ThirdPersonBP template directory
	// only ships these 3 Blueprints under ThirdPerson/Blueprints/. The
	// supporting assets (ABP_Manny, IA_Move/Look/Jump, IMC_Default,
	// SKM_Quinn_Simple, etc.) live in `Templates/TemplateResources/High/`
	// and are merged into the project's /Game/ root only at template
	// instantiation time (via the editor's "New Project" wizard) — they
	// are not loadable from the TP_ThirdPersonBP/Content/ tree itself.
	//
	// For roundtrip-test purposes, the 3 BPs below give us:
	//   - BP_ThirdPersonCharacter: rich complexity (Character parent,
	//     component tree, EnhancedInput event-graph, multiple variables)
	//   - BP_ThirdPersonGameMode + BP_ThirdPersonPlayerController:
	//     minimal-override examples (parent class + a couple defaults)
	//
	// EXPECTED COMPILE WARNINGS: BP_ThirdPersonCharacter references
	// `/Game/Input/Actions/IA_*` and `/Game/Characters/Mannequins/
	// Anims/Unarmed/ABP_Unarmed` by soft path. Those packages are absent
	// from the project, so the compiler emits "EnhancedInputAction None"
	// warnings and the AnimBlueprint slot stays null. This does NOT
	// affect the roundtrip test target — bp-reader operates on the
	// serialized K2 graph (variables, components, nodes, pin types),
	// which round-trip independently of asset-resolution status.
	//
	// TODO(roundtrip-expansion): if a future test needs a clean-compile
	// fixture, mount `Templates/TemplateResources/High/Input/Content/`
	// at `/TemplateInput/` and copy `IA_Move`, `IA_Look`, `IA_Jump`,
	// `IA_MouseLook`, `IMC_Default`, plus `Touch/BPI_TouchInterface` to
	// `/Game/Input/...`. The animation tree (ABP_Unarmed + SKM_Quinn
	// + dozens of MM_/MF_ sequences) is intentionally NOT recommended
	// — it would add hundreds of megabytes of skeletal-mesh / anim
	// assets to the repo for no roundtrip-test benefit.
	const TArray<TPair<FString, FString>> Copies = {
		{ TEXT("/TPTemplate/ThirdPerson/Blueprints/BP_ThirdPersonCharacter"),
		  TEXT("/Game/Imported/ThirdPerson/BP_ThirdPersonCharacter") },
		{ TEXT("/TPTemplate/ThirdPerson/Blueprints/BP_ThirdPersonGameMode"),
		  TEXT("/Game/Imported/ThirdPerson/BP_ThirdPersonGameMode") },
		{ TEXT("/TPTemplate/ThirdPerson/Blueprints/BP_ThirdPersonPlayerController"),
		  TEXT("/Game/Imported/ThirdPerson/BP_ThirdPersonPlayerController") },
	};

	int32 Failures = 0;
	int32 Copied   = 0;
	int32 Skipped  = 0;

	for (const auto& Pair : Copies)
	{
		if (UEditorAssetLibrary::DoesAssetExist(Pair.Value))
		{
			UE_LOG(LogBPRoundtripSeed, Display,
			       TEXT("Skipping (exists): %s"), *Pair.Value);
			++Skipped;
			continue;
		}
		UObject* Src = UEditorAssetLibrary::LoadAsset(Pair.Key);
		if (!Src)
		{
			UE_LOG(LogBPRoundtripSeed, Warning,
			       TEXT("Source asset not found: %s (skipping; engine "
			            "template layout may differ from expected)"),
			       *Pair.Key);
			++Failures;
			continue;
		}
		UObject* Dst = UEditorAssetLibrary::DuplicateAsset(Pair.Key, Pair.Value);
		if (!Dst)
		{
			UE_LOG(LogBPRoundtripSeed, Error,
			       TEXT("Failed to duplicate %s -> %s"),
			       *Pair.Key, *Pair.Value);
			++Failures;
			continue;
		}
		UEditorAssetLibrary::SaveAsset(Pair.Value, /*bOnlyIfIsDirty=*/ false);
		UE_LOG(LogBPRoundtripSeed, Display, TEXT("Copied %s -> %s"),
		       *Pair.Key, *Pair.Value);
		++Copied;
	}

	UE_LOG(LogBPRoundtripSeed, Display,
	       TEXT("BPRoundtripSeed done. Copied: %d, Skipped: %d, Failures: %d"),
	       Copied, Skipped, Failures);

	// Allow partial failure (some assets may not exist in every UE5.7
	// template layout) but propagate via exit code so CI can react.
	return Failures > 0 ? 2 : 0;
}
