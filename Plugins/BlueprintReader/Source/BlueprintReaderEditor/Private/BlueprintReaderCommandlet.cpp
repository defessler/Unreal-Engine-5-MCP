#include "BlueprintReaderCommandlet.h"

#include "BlueprintIntrospector.h"
#include "BlueprintReaderJson.h"

#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintReader, Log, All);

UBlueprintReaderCommandlet::UBlueprintReaderCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
	ShowErrorCount = false;
}

int32 UBlueprintReaderCommandlet::Main(const FString& Params)
{
	FString AssetPath;
	if (!FParse::Value(*Params, TEXT("Path="), AssetPath))
	{
		UE_LOG(LogBlueprintReader, Error, TEXT("Missing required argument: -Path=<package path or object path>"));
		return 1;
	}

	FString OutputPath;
	FParse::Value(*Params, TEXT("Output="), OutputPath);

	const bool bPretty = !FParse::Param(*Params, TEXT("Compact"));

	const TOptional<FBlueprintInfo> Info = FBlueprintIntrospector::Read(AssetPath);
	if (!Info.IsSet())
	{
		UE_LOG(LogBlueprintReader, Error, TEXT("Failed to load blueprint at: %s"), *AssetPath);
		return 2;
	}

	const FString Json = FBlueprintReaderJson::ToString(*Info, bPretty);

	if (!OutputPath.IsEmpty())
	{
		if (!FFileHelper::SaveStringToFile(Json, *OutputPath))
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("Failed to write output to: %s"), *OutputPath);
			return 3;
		}
		UE_LOG(LogBlueprintReader, Display, TEXT("Wrote %d bytes to %s"), Json.Len(), *OutputPath);
	}
	else
	{
		FPlatformMisc::LocalPrint(*Json);
		FPlatformMisc::LocalPrint(TEXT("\n"));
	}

	return 0;
}
