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
		// Engine layout: <EngineDir>/Templates/TP_ThirdPersonBP/Content/
		return FPaths::Combine(FPaths::EngineDir(), TEXT("Templates"),
		                        TEXT("TP_ThirdPersonBP"), TEXT("Content"));
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
	const TArray<TPair<FString, FString>> Copies = {
		{ TEXT("/TPTemplate/ThirdPerson/Blueprints/BP_ThirdPersonCharacter"),
		  TEXT("/Game/Imported/ThirdPerson/BP_ThirdPersonCharacter") },
		{ TEXT("/TPTemplate/ThirdPerson/Blueprints/BP_ThirdPersonGameMode"),
		  TEXT("/Game/Imported/ThirdPerson/BP_ThirdPersonGameMode") },
		{ TEXT("/TPTemplate/ThirdPerson/Blueprints/BP_ThirdPersonPlayerController"),
		  TEXT("/Game/Imported/ThirdPerson/BP_ThirdPersonPlayerController") },
		{ TEXT("/TPTemplate/Characters/Mannequins/Animations/ABP_Manny"),
		  TEXT("/Game/Imported/ThirdPerson/ABP_Manny") },
		{ TEXT("/TPTemplate/ThirdPerson/Input/Actions/IA_Move"),
		  TEXT("/Game/Imported/ThirdPerson/Input/IA_Move") },
		{ TEXT("/TPTemplate/ThirdPerson/Input/Actions/IA_Look"),
		  TEXT("/Game/Imported/ThirdPerson/Input/IA_Look") },
		{ TEXT("/TPTemplate/ThirdPerson/Input/Actions/IA_Jump"),
		  TEXT("/Game/Imported/ThirdPerson/Input/IA_Jump") },
		{ TEXT("/TPTemplate/ThirdPerson/Input/IMC_Default"),
		  TEXT("/Game/Imported/ThirdPerson/Input/IMC_Default") },
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
