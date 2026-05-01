#pragma once

#include "CoreMinimal.h"
#include "BlueprintReaderTypes.h"

class UBlueprint;
class UEdGraph;
struct FEdGraphPinType;

class BLUEPRINTREADEREDITOR_API FBlueprintIntrospector
{
public:
	static TOptional<FBlueprintInfo> Read(const FString& AssetPath);
	static TOptional<FBlueprintInfo> Read(UBlueprint* Blueprint);

	static FString FormatPinType(const FEdGraphPinType& Type);

private:
	static FBPGraphInfo ReadGraph(UEdGraph* Graph);
};
