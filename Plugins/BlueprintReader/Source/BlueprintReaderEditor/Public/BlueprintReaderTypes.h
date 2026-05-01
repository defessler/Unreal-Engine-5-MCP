#pragma once

#include "CoreMinimal.h"

struct FBPPinLinkInfo
{
	FString NodeGuid;
	FString PinName;
};

struct FBPPinInfo
{
	FString Name;
	FString Direction;
	FString Type;
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
