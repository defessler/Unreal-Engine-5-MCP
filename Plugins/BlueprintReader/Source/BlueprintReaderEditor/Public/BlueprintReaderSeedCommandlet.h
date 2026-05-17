#pragma once

#include "Commandlets/Commandlet.h"
#include "CoreMinimal.h"
#include "BlueprintReaderSeedCommandlet.generated.h"

// Seeds a small fixed set of test blueprints under /Game/AI for end-to-end
// integration testing of the BlueprintReader commandlet pipeline.
//
// Run via:
//   UnrealEditor-Cmd.exe <uproject> -run=BPRSeed -unattended -nopause -nullrhi -nosplash
UCLASS()
class UBPRSeedCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UBPRSeedCommandlet();

	virtual int32 Main(const FString& Params) override;
};
