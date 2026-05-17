// BPRoundtripSeedCommandlet — imports the engine's ThirdPerson template
// into Content/Imported/ThirdPerson/ so the roundtrip tests have a
// real-world BP target. Idempotent: re-running is safe.
//
// Run: UnrealEditor-Cmd.exe <project> -run=BPRoundtripSeed -nullrhi
//      -nosplash -unattended -nopause
#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "BPRoundtripSeedCommandlet.generated.h"

UCLASS()
class UBPRoundtripSeedCommandlet : public UCommandlet
{
	GENERATED_BODY()
public:
	UBPRoundtripSeedCommandlet();
	virtual int32 Main(const FString& Params) override;
};
