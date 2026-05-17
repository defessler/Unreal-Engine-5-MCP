#pragma once

#include "Commandlets/Commandlet.h"
#include "CoreMinimal.h"
#include "BlueprintReaderCommandlet.generated.h"

UCLASS()
class UBlueprintReaderCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UBlueprintReaderCommandlet();

	virtual int32 Main(const FString& Params) override;
};
