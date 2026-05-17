#include "BlueprintRuntimeIntrospector.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"

// ----- Asset list ---------------------------------------------------------

TArray<FBPRRAssetSummary> FBlueprintRuntimeIntrospector::ListBlueprints(
	const FString& PathFilter)
{
	const FString Path = PathFilter.IsEmpty() ? TEXT("/Game") : PathFilter;

	FAssetRegistryModule& ARM =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();
	AR.ScanPathsSynchronous({Path}, /*bForceRescan=*/false);

	FARFilter Filter;
	Filter.bRecursivePaths = true;
	Filter.PackagePaths.Add(*Path);
	// In editor builds the Asset Registry holds UBlueprint entries.
	// In cooked builds those are stripped — only the generated
	// UBlueprintGeneratedClass remains. Filter for both so the same
	// code path works in either configuration.
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.ClassPaths.Add(UBlueprintGeneratedClass::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;

	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	TArray<FBPRRAssetSummary> Out;
	Out.Reserve(Assets.Num());
	for (const FAssetData& A : Assets)
	{
		FBPRRAssetSummary S;
		S.AssetPath = A.PackageName.ToString();
		S.Name      = A.AssetName.ToString();
		// ParentClass tag exists on cooked BP assets; falls back to
		// NativeParentClass on older serialization paths.
		FString Parent;
		A.GetTagValue(TEXT("ParentClass"), Parent);
		if (Parent.IsEmpty())
		{
			A.GetTagValue(TEXT("NativeParentClass"), Parent);
		}
		// Tag form is `Class'/Script/Engine.Actor'` — strip wrappers
		// so the wire shape matches read.
		int32 OpenQuote = INDEX_NONE, CloseQuote = INDEX_NONE;
		if (Parent.FindChar(TEXT('\''), OpenQuote) &&
		    Parent.FindLastChar(TEXT('\''), CloseQuote) &&
		    CloseQuote > OpenQuote)
		{
			Parent = Parent.Mid(OpenQuote + 1, CloseQuote - OpenQuote - 1);
		}
		S.ParentClass = Parent;
		Out.Add(MoveTemp(S));
	}
	return Out;
}

// ----- Class resolution ---------------------------------------------------

UClass* FBlueprintRuntimeIntrospector::ResolveClass(const FString& AssetPath)
{
	if (AssetPath.IsEmpty())
	{
		return nullptr;
	}

	// Two resolution strategies in priority order:
	//   1. /Game/Foo/BP_Bar.BP_Bar_C — direct UClass path. Works in both
	//      editor and cooked builds.
	//   2. /Game/Foo/BP_Bar — package path. In cooked builds the BP asset
	//      itself isn't loadable but the generated class is; we resolve
	//      by appending `.{Name}_C` and trying again.
	//
	// Editor builds also have UBlueprint::GeneratedClass that we'd
	// hit via LoadObject<UBlueprint>, but the _C-suffix path works
	// uniformly so we use it everywhere.

	auto TryLoad = [](const FString& Path) -> UClass*
	{
		// FindObject is non-loading; if the class is already in memory
		// we use that. Otherwise LoadObject pulls it in.
		if (UClass* C = FindObject<UClass>(nullptr, *Path))
		{
			return C;
		}
		return LoadObject<UClass>(nullptr, *Path);
	};

	// Strategy 1: caller already passed the full class path.
	if (AssetPath.EndsWith(TEXT("_C")))
	{
		if (UClass* C = TryLoad(AssetPath))
		{
			return C;
		}
	}

	// Strategy 2: package path. Derive the class path.
	int32 LastSlash = INDEX_NONE;
	AssetPath.FindLastChar(TEXT('/'), LastSlash);
	const FString AssetName = (LastSlash != INDEX_NONE)
		? AssetPath.RightChop(LastSlash + 1)
		: AssetPath;
	const FString ClassPath = AssetPath + TEXT(".") + AssetName + TEXT("_C");
	if (UClass* C = TryLoad(ClassPath))
	{
		return C;
	}

	// Strategy 3: native C++ classes don't have the `_C` suffix.
	// Try the bare object path.
	const FString ObjectPath = AssetPath + TEXT(".") + AssetName;
	if (UClass* C = TryLoad(ObjectPath))
	{
		return C;
	}

	return nullptr;
}

// ----- Type-shorthand renderer --------------------------------------------
// Mirrors what TypeShorthand.cpp in the mcp-server emits — keeps the wire
// format consistent across editor / runtime sources.

static FString StripScriptPrefix(const FString& In)
{
	int32 Dot = INDEX_NONE;
	if (In.FindLastChar(TEXT('.'), Dot))
	{
		return In.RightChop(Dot + 1);
	}
	return In;
}

FString FBlueprintRuntimeIntrospector::PropertyTypeShorthand(const FProperty* P)
{
	if (!P)
	{
		return TEXT("void");
	}

	// Container types — recurse on the inner property.
	if (const FArrayProperty* AP = CastField<FArrayProperty>(P))
	{
			return FString::Printf(TEXT("[]%s"), *PropertyTypeShorthand(AP->Inner));
	}
	if (const FSetProperty* SP = CastField<FSetProperty>(P))
	{
			return FString::Printf(TEXT("{}%s"), *PropertyTypeShorthand(SP->ElementProp));
	}
	if (const FMapProperty* MP = CastField<FMapProperty>(P))
	{
			return FString::Printf(TEXT("{%s:%s}"),
				*PropertyTypeShorthand(MP->KeyProp),
				*PropertyTypeShorthand(MP->ValueProp));
	}

	// Primitives.
	if (P->IsA<FBoolProperty>())
	{
		return TEXT("bool");
	}
	if (P->IsA<FByteProperty>())
	{
		return TEXT("byte");
	}
	if (P->IsA<FIntProperty>())
	{
		return TEXT("int");
	}
	if (P->IsA<FInt64Property>())
	{
		return TEXT("int64");
	}
	if (P->IsA<FFloatProperty>())
	{
		return TEXT("real:float");
	}
	if (P->IsA<FDoubleProperty>())
	{
		return TEXT("real:double");
	}
	if (P->IsA<FStrProperty>())
	{
		return TEXT("string");
	}
	if (P->IsA<FNameProperty>())
	{
		return TEXT("name");
	}
	if (P->IsA<FTextProperty>())
	{
		return TEXT("text");
	}

	// Object / class / soft refs.
	if (const FObjectProperty* OP = CastField<FObjectProperty>(P))
	{
		const FString Cls = OP->PropertyClass
			? StripScriptPrefix(OP->PropertyClass->GetPathName())
			: TEXT("");
		return Cls.IsEmpty() ? TEXT("object") : FString::Printf(TEXT("object:%s"), *Cls);
	}
	if (const FSoftObjectProperty* SOP = CastField<FSoftObjectProperty>(P))
	{
		const FString Cls = SOP->PropertyClass
			? StripScriptPrefix(SOP->PropertyClass->GetPathName())
			: TEXT("");
		return Cls.IsEmpty() ? TEXT("soft_object") : FString::Printf(TEXT("soft_object:%s"), *Cls);
	}
	if (const FClassProperty* CP = CastField<FClassProperty>(P))
	{
		const FString Cls = CP->MetaClass
			? StripScriptPrefix(CP->MetaClass->GetPathName())
			: TEXT("");
		return Cls.IsEmpty() ? TEXT("class") : FString::Printf(TEXT("class:%s"), *Cls);
	}
	if (const FSoftClassProperty* SCP = CastField<FSoftClassProperty>(P))
	{
		const FString Cls = SCP->MetaClass
			? StripScriptPrefix(SCP->MetaClass->GetPathName())
			: TEXT("");
		return Cls.IsEmpty() ? TEXT("soft_class") : FString::Printf(TEXT("soft_class:%s"), *Cls);
	}
	if (const FInterfaceProperty* IP = CastField<FInterfaceProperty>(P))
	{
		const FString Cls = IP->InterfaceClass
			? StripScriptPrefix(IP->InterfaceClass->GetPathName())
			: TEXT("");
		return Cls.IsEmpty() ? TEXT("interface") : FString::Printf(TEXT("interface:%s"), *Cls);
	}
	if (const FStructProperty* SP = CastField<FStructProperty>(P))
	{
		const FString Cls = SP->Struct
			? StripScriptPrefix(SP->Struct->GetPathName())
			: TEXT("");
		return Cls.IsEmpty() ? TEXT("struct") : FString::Printf(TEXT("struct:%s"), *Cls);
	}

	// Unrecognized — pass through the C++ type name.
	return P->GetCPPType();
}

// ----- Variable extraction ------------------------------------------------

static FString CDOPropertyValueAsText(const FProperty* Property, const UObject* CDO)
{
	if (!Property || !CDO)
	{
		return FString();
	}
	// PropertyAddr is the byte offset into the CDO for this property.
	const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CDO);
	FString Out;
	Property->ExportTextItem_Direct(Out, ValuePtr, /*Delta*/nullptr,
		/*Parent*/const_cast<UObject*>(CDO), PPF_None);
	return Out;
}

static FBPRRVariable PropertyToVariable(const FProperty* Property, const UObject* CDO)
{
	FBPRRVariable V;
	V.Name = Property->GetName();
	V.TypeShorthand = FBlueprintRuntimeIntrospector::PropertyTypeShorthand(Property);
	V.DefaultValue = CDOPropertyValueAsText(Property, CDO);
	V.bIsReplicated = Property->HasAnyPropertyFlags(CPF_Net);
	V.bIsEditable = Property->HasAnyPropertyFlags(CPF_Edit) &&
	                 !Property->HasAnyPropertyFlags(CPF_DisableEditOnInstance);

	// FProperty::GetMetaData / GetBoolMetaData are editor-only — UE
	// strips per-property metadata during cook (it's only used by the
	// editor's reflection-driven UI + automation). Category +
	// ExposeOnSpawn round-trip through the cooked .uasset for replication
	// purposes but not via the metadata path, so cooked builds get the
	// runtime-knowable subset only.
#if WITH_EDITORONLY_DATA
	V.Category = Property->GetMetaData(TEXT("Category"));
	V.bIsExposeOnSpawn = Property->GetBoolMetaData(TEXT("ExposeOnSpawn"));
#endif    // WITH_EDITORONLY_DATA

	// RepNotify: UE stores the callback function name in the property's
	// RepNotifyFunc field. Always present (replication needs it at runtime).
	if (Property->HasAnyPropertyFlags(CPF_RepNotify))
	{
		V.RepNotifyFunc = Property->RepNotifyFunc.ToString();
	}
	// Replication condition lives on the property's RepLifetimeCondition
	// field. CDOPropertyValueAsText doesn't surface it; we'd have to
	// inspect FLifetimeProperty data populated by
	// GetLifetimeReplicatedProps, which requires a temporary actor
	// instance. Skip for runtime; editor side carries it via reflection.

	return V;
}

// ----- Function extraction ------------------------------------------------

static FBPRRFunction FunctionToBPRR(UFunction* Function)
{
	FBPRRFunction F;
	F.Name = Function->GetName();
	F.bIsBlueprintCallable = Function->HasAnyFunctionFlags(FUNC_BlueprintCallable);
	F.bIsBlueprintPure = Function->HasAnyFunctionFlags(FUNC_BlueprintPure);
	F.bIsBlueprintImplementableEvent =
		Function->HasAnyFunctionFlags(FUNC_BlueprintEvent) &&
		!Function->HasAnyFunctionFlags(FUNC_Native);
	F.bIsBlueprintNativeEvent =
		Function->HasAnyFunctionFlags(FUNC_BlueprintEvent | FUNC_Native);
	F.bIsNetServer = Function->HasAnyFunctionFlags(FUNC_NetServer);
	F.bIsNetClient = Function->HasAnyFunctionFlags(FUNC_NetClient);
	F.bIsNetMulticast = Function->HasAnyFunctionFlags(FUNC_NetMulticast);
	F.bIsNetReliable = Function->HasAnyFunctionFlags(FUNC_NetReliable);

	// Walk function parameters. CPF_OutParm = output, otherwise input.
	// Return value is also CPF_OutParm + CPF_ReturnParm.
	for (TFieldIterator<FProperty> It(Function); It; ++It)
	{
		FProperty* P = *It;
		if (!P->HasAnyPropertyFlags(CPF_Parm))
		{
			continue;
		}

		FBPRRVariable V;
		V.Name = P->GetName();
		V.TypeShorthand = FBlueprintRuntimeIntrospector::PropertyTypeShorthand(P);

		if (P->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm))
		{
			F.Outputs.Add(MoveTemp(V));
		}
		else
		{
			F.Inputs.Add(MoveTemp(V));
		}
	}
	return F;
}

// ----- Component extraction -----------------------------------------------

static void GatherComponentsFromSCS(UClass* Class, TArray<FBPRRComponent>& Out)
{
	// Walk the SCS chain on the class. Cooked BPs lose SCS but native
	// component declarations on the parent class still hold; we walk
	// the SCS for any class that has one (typically only editor-time
	// BPGCs in editor builds; in cooked builds we fall back to CDO).
	UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(Class);
	if (!BPGC || !BPGC->SimpleConstructionScript)
	{
		return;
	}

	for (USCS_Node* Node : BPGC->SimpleConstructionScript->GetAllNodes())
	{
		if (!Node || !Node->ComponentClass)
		{
			continue;
		}
		FBPRRComponent C;
		C.Name = Node->GetVariableName().ToString();
		C.ClassPath = StripScriptPrefix(Node->ComponentClass->GetPathName());
		if (USCS_Node* Parent = BPGC->SimpleConstructionScript->FindParentNode(Node))
		{
			C.ParentName = Parent->GetVariableName().ToString();
		}
		C.bIsRoot = BPGC->SimpleConstructionScript->GetRootNodes().Contains(Node);
		Out.Add(MoveTemp(C));
	}
}

static void GatherComponentsFromCDO(UClass* Class, TArray<FBPRRComponent>& Out)
{
	// Cooked-build path: walk the CDO actor's owned components. Loses
	// the SCS hierarchy but recovers the runtime component instances.
	UObject* CDO = Class->GetDefaultObject();
	AActor* CDOActor = Cast<AActor>(CDO);
	if (!CDOActor)
	{
		return;
	}

	const USceneComponent* Root = CDOActor->GetRootComponent();
	if (Root)
	{
		FBPRRComponent RootC;
		RootC.Name = Root->GetName();
		RootC.ClassPath = StripScriptPrefix(Root->GetClass()->GetPathName());
		RootC.bIsRoot = true;
		Out.Add(MoveTemp(RootC));
	}

	for (UActorComponent* Component : CDOActor->GetComponents())
	{
		if (!Component || Component == Root)
		{
			continue;
		}
		FBPRRComponent C;
		C.Name = Component->GetName();
		C.ClassPath = StripScriptPrefix(Component->GetClass()->GetPathName());
		if (const USceneComponent* SC = Cast<USceneComponent>(Component))
		{
			if (SC->GetAttachParent())
			{
				C.ParentName = SC->GetAttachParent()->GetName();
			}
		}
		Out.Add(MoveTemp(C));
	}
}

// ----- Top-level read ----------------------------------------------------

TOptional<FBPRRBlueprint> FBlueprintRuntimeIntrospector::Read(const FString& AssetPath)
{
	UClass* Class = ResolveClass(AssetPath);
	if (!Class)
	{
		return TOptional<FBPRRBlueprint>();
	}

	FBPRRBlueprint Out;
	// Normalize asset path: caller may have passed the _C class path or
	// the bare package path. Emit the package path form for consistency
	// with what list_blueprints returns.
	FString PackagePath = AssetPath;
	if (PackagePath.EndsWith(TEXT("_C")))
	{
		// /Game/Foo/BP.BP_C → /Game/Foo/BP
		int32 LastDot = INDEX_NONE;
		PackagePath.FindLastChar(TEXT('.'), LastDot);
		if (LastDot != INDEX_NONE)
		{
			PackagePath.LeftInline(LastDot);
		}
	}
	int32 BareDot = INDEX_NONE;
	if (PackagePath.FindLastChar(TEXT('.'), BareDot))
	{
		// /Game/Foo/BP.BP → /Game/Foo/BP
		PackagePath.LeftInline(BareDot);
	}
	Out.AssetPath = PackagePath;
	Out.Name = Class->GetName();
	if (Out.Name.EndsWith(TEXT("_C")))
	{
		Out.Name.LeftChopInline(2);
	}

	if (UClass* Super = Class->GetSuperClass())
	{
		Out.ParentClassPath = Super->GetPathName();
	}

	for (const FImplementedInterface& Impl : Class->Interfaces)
	{
		if (Impl.Class)
		{
			Out.Interfaces.Add(Impl.Class->GetPathName());
		}
	}

	// Variables: walk class properties, skip ones inherited from the
	// parent (those belong to it, not us). Match the editor introspector
	// behavior: only show fields declared on THIS class.
	UObject* CDO = Class->GetDefaultObject();
	for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		const FProperty* P = *It;
		if (!P)
		{
			continue;
		}
		Out.Variables.Add(PropertyToVariable(P, CDO));
	}

	// Functions: same — only THIS class's UFunctions.
	for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		UFunction* F = *It;
		if (!F)
		{
			continue;
		}
		Out.Functions.Add(FunctionToBPRR(F));
	}

	// Components: SCS in editor + cooked-with-SCS-preserved cases, CDO
	// in pure cooked.
	GatherComponentsFromSCS(Class, Out.Components);
	if (Out.Components.Num() == 0)
	{
		GatherComponentsFromCDO(Class, Out.Components);
	}

	return Out;
}
