#pragma once

#include "Commandlets/Commandlet.h"
#include "CoreMinimal.h"
#include "BlueprintReaderSeedCommandlet.generated.h"

// Seeds a small fixed set of test blueprints under /Game/AI for end-to-end
// integration testing of the BlueprintReader commandlet pipeline.
//
// Run via:
//   UnrealEditor-Cmd.exe <uproject> -run=BlueprintReaderSeed -unattended -nopause -nullrhi -nosplash
UCLASS()
class UBlueprintReaderSeedCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UBlueprintReaderSeedCommandlet();

	virtual int32 Main(const FString& Params) override;
};
