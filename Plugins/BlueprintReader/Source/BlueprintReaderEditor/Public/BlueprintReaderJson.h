#pragma once

#include "CoreMinimal.h"
#include "BlueprintReaderTypes.h"

class FJsonObject;

class BLUEPRINTREADEREDITOR_API FBlueprintReaderJson
{
public:
	static FString ToString(const FBlueprintInfo& Info, bool bPretty = true);
	static TSharedRef<FJsonObject> ToJson(const FBlueprintInfo& Info);
};
