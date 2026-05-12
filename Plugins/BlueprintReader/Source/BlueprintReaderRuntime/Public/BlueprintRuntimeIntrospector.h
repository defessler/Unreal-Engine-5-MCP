// BlueprintRuntimeIntrospector — reads BP class data at runtime via UClass
// reflection. Works in both editor and packaged-game builds.
//
// What this reads (works in cooked builds):
//   - Asset Registry entries → list of BP assets, parent class, modified time
//   - UClass parent chain → parent_class
//   - UClass::Interfaces → interfaces[]
//   - TFieldIterator<FProperty>(Class) → variables[] with default values
//     read from the CDO (UClass::GetDefaultObject<UObject>())
//   - TFieldIterator<UFunction>(Class) → functions[] with input / output /
//     local parameter signatures (no body — bytecode-compiled, source
//     graph stripped during cook)
//   - USCS / CDO scan → components[]
//
// What this CAN'T read (stripped during cook):
//   - K2 node graphs (visual event graph + function body graphs)
//   - Comments, node positions, BP-specific metadata that's editor-only
//
// The wire-format output is the same JSON shape `read_blueprint` emits
// from the editor-side introspector — just with empty `graphs[]` arrays
// on each function in cooked builds. Editor builds get the same shape
// the existing introspector emits.
#pragma once

#include "CoreMinimal.h"

class UClass;

// One UClass-shaped variable. Maps to BPVariable in the wire format.
struct FBPRRVariable
{
	FString Name;
	FString TypeShorthand;   // "int", "float", "object:Actor", "[]int", "{string:bool}"
	FString DefaultValue;    // CDO value as text (empty if non-serializable)
	FString Category;        // UPROPERTY meta=(Category="...")
	bool bIsReplicated = false;
	bool bIsEditable = false;
	bool bIsExposeOnSpawn = false;
	FString RepCondition;    // e.g. "OwnerOnly" — empty when unconditional
	FString RepNotifyFunc;   // e.g. "OnRep_Health" — empty when not RepNotify
};

// One UFunction signature. Body is empty in cooked builds.
struct FBPRRFunction
{
	FString Name;
	TArray<FBPRRVariable> Inputs;
	TArray<FBPRRVariable> Outputs;
	bool bIsBlueprintCallable = false;
	bool bIsBlueprintPure = false;
	bool bIsBlueprintImplementableEvent = false;
	bool bIsBlueprintNativeEvent = false;
	bool bIsNetServer = false;
	bool bIsNetClient = false;
	bool bIsNetMulticast = false;
	bool bIsNetReliable = false;
};

// One USCS / CDO component instance.
struct FBPRRComponent
{
	FString Name;
	FString ClassPath;
	FString ParentName;
	bool bIsRoot = false;
};

struct FBPRRBlueprint
{
	FString AssetPath;           // /Game/AI/BP_TestEnemy
	FString Name;
	FString ParentClassPath;
	TArray<FString> Interfaces;  // /Script/Game.IDamageable
	TArray<FBPRRVariable> Variables;
	TArray<FBPRRComponent> Components;
	TArray<FBPRRFunction> Functions;
};

// Short asset summary used by the list endpoint.
struct FBPRRAssetSummary
{
	FString AssetPath;
	FString Name;
	FString ParentClass;
};

class BLUEPRINTREADERRUNTIME_API FBlueprintRuntimeIntrospector
{
public:
	// Find all BP-shaped assets under a content path via the Asset
	// Registry. Returns whatever's cooked into the .pak (or whatever
	// the editor knows about in editor builds).
	static TArray<FBPRRAssetSummary> ListBlueprints(const FString& PathFilter);

	// Read a single Blueprint's data via UClass reflection. Asset path
	// is /Game/Foo/BP_Bar — we resolve to the generated UClass and walk.
	// Returns nullopt if the asset doesn't exist or doesn't resolve to
	// a valid UClass.
	static TOptional<FBPRRBlueprint> Read(const FString& AssetPath);

	// Resolve `/Game/Foo/BP_Bar` to the generated UClass. Tries
	// LoadObject<UClass> with the `_C` suffix added if necessary.
	// Returns nullptr on failure.
	static UClass* ResolveClass(const FString& AssetPath);

	// Render a BPIR-style type shorthand from an FProperty: matches what
	// the editor introspector emits ("int" for FIntProperty, "object:X"
	// for FObjectProperty, "[]float" for TArray<float>, etc.).
	static FString PropertyTypeShorthand(const FProperty* Property);
};
