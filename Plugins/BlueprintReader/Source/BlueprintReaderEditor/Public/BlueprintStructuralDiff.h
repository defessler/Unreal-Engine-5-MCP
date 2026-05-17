// BlueprintStructuralDiff — position-independent comparison of two
// UBlueprint*s. Lives in the plugin because it needs full UBlueprint /
// USCS_Node / UEdGraphNode reflection. Output is a flat list of
// differences with a path string for human readability.
#pragma once

#include "CoreMinimal.h"

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Dom/JsonObject.h"

class UBlueprint;

namespace BlueprintStructuralDiff {

struct FCompareOptions
{
	bool bIgnoreNodePositions = true;
	bool bIgnoreCommentNodes  = false;
	TArray<TPair<FString, FString>> AllowedNameSubstitutions;  // (from, to)
};

struct FDifference
{
	FString Path;     // dotted path, e.g. "variables.Health.type"
	FString Kind;     // "missing", "extra", "value_mismatch", "type_mismatch"
	FString ValueA;
	FString ValueB;
};

struct FResult
{
	bool bEqual = false;
	TArray<FDifference> Differences;

	TSharedRef<FJsonObject> ToJson() const;
};

FResult Compare(UBlueprint* A, UBlueprint* B, const FCompareOptions& Options);

}    // namespace BlueprintStructuralDiff
