// Wire-format JSON for the MCP server / Shared/BlueprintReaderTypes.h.
//
// The plugin's primary FBlueprintReaderJson emits a rich, plugin-internal
// shape (camelCase, K2-extras as a sub-object). The MCP server consumes a
// different, narrower shape (snake_case, BPNode.meta as a free-form JSON
// object). Both serializers live in the plugin so the commandlet can emit
// either format on demand.
//
// Wire shapes mirror Shared/BlueprintReaderTypes.h:
//   * BPMetadata  — top-level summary returned by `read_blueprint`
//   * BPGraph     — { name, type, nodes[], connections[] }
//   * BPFunction  — { name, inputs[], outputs[], locals[], graph }
//   * BPVariable  — { name, type, default_value, category, is_replicated, is_editable }
//   * BPAssetSummary — used by `list_blueprints`
//
// All `*_value`, `*_path`, etc. keys are snake_case. `meta` is emitted as a
// real nested JSON object — never a string-of-JSON.
#pragma once

#include "CoreMinimal.h"
#include "BlueprintReaderTypes.h"

class FJsonObject;
class FJsonValue;

class BLUEPRINTREADEREDITOR_API FBlueprintReaderWireJson
{
public:
	static TSharedRef<FJsonObject> SummaryToJson(const FBlueprintInfo& Info, const FString& ModifiedIso);
	static TSharedRef<FJsonObject> MetadataToJson(const FBlueprintInfo& Info);
	static TSharedPtr<FJsonObject> GraphToJson(const FBlueprintInfo& Info, const FString& GraphName);
	static TSharedPtr<FJsonObject> FunctionToJson(const FBlueprintInfo& Info, const FString& FunctionName);
	static TArray<TSharedPtr<FJsonValue>> VariablesToJson(const FBlueprintInfo& Info);
	static TArray<TSharedPtr<FJsonValue>> FindNodesAsJson(const FBlueprintInfo& Info,
	                                                      const FString& Query,
	                                                      const FString& Kind = FString());

	// Helpers exposed for the commandlet's List op. Builds a summary object
	// from raw asset registry fields without needing a full FBlueprintInfo.
	static TSharedRef<FJsonObject> SummaryToJson(const FString& AssetPath,
	                                             const FString& Name,
	                                             const FString& ParentClassPath,
	                                             const FString& ModifiedIso);

	static FString WriteString(const TSharedRef<FJsonObject>& Object, bool bPretty);
	static FString WriteArrayString(const TArray<TSharedPtr<FJsonValue>>& Array, bool bPretty);
};
