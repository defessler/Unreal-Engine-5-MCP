// Wire-format JSON output for the runtime introspector. Emits the same
// snake_case shape `read_blueprint` returns from the editor side, so the
// MCP server can speak to a packaged build with the same parser.
#pragma once

#include "BlueprintRuntimeIntrospector.h"
#include "CoreMinimal.h"

// FJsonObject + JsonValue are templates with an ESPMode default — UE's
// CoreMinimal forwards TSharedRef / TSharedPtr already, but we need
// the FJsonObject declaration for the function signatures below.
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

class BLUEPRINTREADERRUNTIME_API FBlueprintReaderRuntimeJson
{
public:
	// Single-asset summary used by list_blueprints. Match the existing
	// editor wire shape: {asset_path, name, parent_class, modified_iso}.
	// We don't have an mtime in a cooked build (no file on disk to
	// stat — the asset lives in a .pak), so modified_iso is empty.
	static TSharedRef<FJsonObject> SummaryToJson(const FBPRRAssetSummary& Summary);

	// Whole blueprint. Renders read_blueprint's wire shape with empty
	// graphs[] (cooked builds have no source-level graphs to walk).
	static TSharedRef<FJsonObject> BlueprintToJson(const FBPRRBlueprint& BP);

	// Variables alone — list_variables endpoint shape.
	static TArray<TSharedPtr<FJsonValue>> VariablesToJson(const FBPRRBlueprint& BP);

	// Components alone — get_components endpoint shape.
	static TArray<TSharedPtr<FJsonValue>> ComponentsToJson(const FBPRRBlueprint& BP);

	// Render a list of assets as a JSON array of summaries.
	static FString WriteListString(const TArray<FBPRRAssetSummary>& Assets, bool bPretty);

	// Single-object render helper. Returns serialized JSON.
	static FString WriteObjectString(const TSharedRef<FJsonObject>& Object, bool bPretty);
};
