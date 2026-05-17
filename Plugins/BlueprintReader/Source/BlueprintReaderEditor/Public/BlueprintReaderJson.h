#pragma once

#include "BlueprintReaderTypes.h"
#include "CoreMinimal.h"

class FJsonObject;

class BLUEPRINTREADEREDITOR_API FBlueprintReaderJson
{
public:
	static FString ToString(const FBlueprintInfo& Info, bool bPretty = true);
	static TSharedRef<FJsonObject> ToJson(const FBlueprintInfo& Info);
};
