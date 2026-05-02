#pragma once

#include "CoreMinimal.h"

// Structured terminal pin type. Mirrors the wire format's BPPinType
// (Shared/BlueprintReaderTypes.h) so the wire serializer can emit it without
// having to parse FBPPinInfo::Type back into its components. Populated from
// FEdGraphPinType alongside the formatted-string representation.
struct FBPStructuredPinType
{
	FString Category;             // exec | bool | int | real | string | object | class | struct | enum | ...
	FString SubCategory;          // float / double for real, etc.
	FString SubCategoryObject;    // path name when applicable
	bool bIsArray = false;
	bool bIsSet = false;
	bool bIsMap = false;
};

struct FBPPinLinkInfo
{
	FString NodeGuid;
	FString PinName;
	FString PinId;                // GUID of the linked pin — used by wire format
};

struct FBPPinInfo
{
	FString Name;
	FString PinId;                // GUID — added for wire-format `id` field
	FString Direction;
	FString Type;                 // human-readable formatted type
	FBPStructuredPinType StructuredType;
	FString DefaultValue;
	FString DefaultObjectPath;
	FString DefaultText;
	bool bIsHidden = false;
	bool bIsReference = false;
	bool bIsConst = false;
	TArray<FBPPinLinkInfo> LinkedTo;
};

struct FBPVariableInfo
{
	FString Name;
	FString FriendlyName;
	FString Category;
	FString Type;
	FBPStructuredPinType StructuredType;
	FString DefaultValue;
	bool bIsReplicated = false;
	bool bIsTransient = false;
	bool bIsEditable = false;
	bool bIsBlueprintReadOnly = false;
	bool bIsExposeOnSpawn = false;
};

struct FBPNodeInfo
{
	FString Guid;
	FString ClassName;
	FString Title;
	FString Comment;
	int32 PosX = 0;
	int32 PosY = 0;
	bool bEnabled = true;
	TArray<FBPPinInfo> Pins;
	TMap<FString, FString> Extras;
};

struct FBPGraphInfo
{
	FString Name;
	FString SchemaPath;
	FString WireType;             // "EventGraph" | "Function" | "Macro" | "Construction" | "DelegateSignature"
	TArray<FBPNodeInfo> Nodes;
	TArray<FBPVariableInfo> LocalVariables;
};

struct FBPComponentInfo
{
	FString Name;
	FString ClassPath;
	FString ParentName;
	bool bIsRoot = false;
};

struct FBPInterfaceInfo
{
	FString InterfacePath;
};

struct FBlueprintInfo
{
	FString Path;
	FString Name;
	FString BlueprintType;
	FString ParentClassPath;
	FString GeneratedClassPath;
	TArray<FBPInterfaceInfo> Interfaces;
	TArray<FBPVariableInfo> Variables;
	TArray<FBPComponentInfo> Components;
	TArray<FBPGraphInfo> FunctionGraphs;
	TArray<FBPGraphInfo> EventGraphs;
	TArray<FBPGraphInfo> MacroGraphs;
	TArray<FBPGraphInfo> DelegateSignatureGraphs;
};
