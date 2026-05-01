#include "BlueprintReaderCommandlet.h"

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
	UE_LOG(LogTemp, Display, TEXT("BlueprintReaderCommandlet stub — params: %s"), *Params);
	return 0;
}
