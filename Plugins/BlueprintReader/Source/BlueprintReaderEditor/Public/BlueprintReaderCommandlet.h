#pragma once

#include "Commandlets/Commandlet.h"
#include "CoreMinimal.h"
#include "BlueprintReaderCommandlet.generated.h"

UCLASS()
class UBPRCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UBPRCommandlet();

	virtual int32 Main(const FString& Params) override;
};
