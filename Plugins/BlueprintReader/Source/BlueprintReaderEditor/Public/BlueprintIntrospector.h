#pragma once

#include "BlueprintReaderTypes.h"
#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

struct FEdGraphPinType;
class UBlueprint;
class UEdGraph;

// Shared log category for the editor-side module. Declared here so both
// the commandlet and the introspector can UE_LOG into it (the
// introspector's DiagnoseFailedBlueprintLoad needs it for the issue #3
// and #4 messages — Codex review on PR #58 caught the read-path gap).
BLUEPRINTREADEREDITOR_API DECLARE_LOG_CATEGORY_EXTERN(LogBlueprintReader, Log, All);

class BLUEPRINTREADEREDITOR_API FBlueprintIntrospector
{
public:
	static TOptional<FBlueprintInfo> Read(const FString& AssetPath);
	static TOptional<FBlueprintInfo> Read(UBlueprint* Blueprint);

	static FString FormatPinType(const FEdGraphPinType& Type);
	static FBPStructuredPinType MakeStructuredPinType(const FEdGraphPinType& Type);

	// When a LoadObject<UBlueprint>(AssetPath) returns null, probe the
	// asset registry for the asset's class + parent_class tag and emit
	// a specific diagnostic. Covers (a) non-Blueprint asset misrouted
	// to bp-reader (issue #4), (b) parent class declared in an
	// uncompiled C++ module (issue #3), and (c) generic
	// PostLoad/ConstructDefaultObject failures (defer to the editor
	// log). Public + shared so both Read (issue #3 repro path) and
	// LoadMutableBlueprint (writes) call into the same logic.
	static void DiagnoseFailedBlueprintLoad(const FString& AssetPath);

private:
	static FBPGraphInfo ReadGraph(UEdGraph* Graph);
};
