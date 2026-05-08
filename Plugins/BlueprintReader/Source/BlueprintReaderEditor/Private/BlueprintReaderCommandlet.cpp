#include "BlueprintReaderCommandlet.h"

#include "BlueprintIntrospector.h"
#include "BlueprintReaderJson.h"
#include "BlueprintReaderWireJson.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Factories/BlueprintFactory.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FormatText.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "K2Node_MakeArray.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_Self.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/UObjectToken.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintReader, Log, All);

UBlueprintReaderCommandlet::UBlueprintReaderCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
	ShowErrorCount = false;
}

namespace
{
	enum class EOp : uint8
	{
		Legacy,    // No -Op specified — emit the rich plugin shape (backward compat).
		List,
		Read,
		Graph,
		Function,
		Variables,
		Components,
		Find,
		// Write ops (Phase 1.5):
		AddVariable,
		SetNodePosition,
		DeleteNode,
		AddNode,
		WirePins,
		DeleteVariable,
		RenameVariable,
		AddFunction,
		AddFunctionInput,
		AddFunctionOutput,
		DeleteFunction,
		SetVariableDefault,
		// Batch sentinels (A1): wrap a sequence of write ops so the
		// expensive CompileBlueprint + SavePackage runs once per affected
		// BP at EndBatch instead of once per op.
		BeginBatch,
		EndBatch,
		// Asset creation (A3).
		CreateBlueprint,
		// Pin default (B1) — supports compile_function's {lit:value}.
		SetPinDefault,
		// Variable lifecycle gaps reported by users:
		RetypeVariable,         // change a var's type without delete+re-add (preserves nodes)
		SetVariableCategory,    // change a var's My-Blueprint-panel category label
		DuplicateBlueprint,     // file-level duplicate (BP-5)
		// Phase 2C — write transpiled source into the project tree.
		WriteGeneratedSource,
	};

	bool ParseOp(const FString& Params, EOp& OutOp)
	{
		FString OpStr;
		if (!FParse::Value(*Params, TEXT("Op="), OpStr))
		{
			OutOp = EOp::Legacy;
			return true;
		}
		if (OpStr.Equals(TEXT("List"), ESearchCase::IgnoreCase))             { OutOp = EOp::List; return true; }
		if (OpStr.Equals(TEXT("Read"), ESearchCase::IgnoreCase))             { OutOp = EOp::Read; return true; }
		if (OpStr.Equals(TEXT("Graph"), ESearchCase::IgnoreCase))            { OutOp = EOp::Graph; return true; }
		if (OpStr.Equals(TEXT("Function"), ESearchCase::IgnoreCase))         { OutOp = EOp::Function; return true; }
		if (OpStr.Equals(TEXT("Variables"), ESearchCase::IgnoreCase))        { OutOp = EOp::Variables; return true; }
		if (OpStr.Equals(TEXT("Components"), ESearchCase::IgnoreCase))       { OutOp = EOp::Components; return true; }
		if (OpStr.Equals(TEXT("Find"), ESearchCase::IgnoreCase))             { OutOp = EOp::Find; return true; }
		if (OpStr.Equals(TEXT("AddVariable"), ESearchCase::IgnoreCase))      { OutOp = EOp::AddVariable; return true; }
		if (OpStr.Equals(TEXT("SetNodePosition"), ESearchCase::IgnoreCase))  { OutOp = EOp::SetNodePosition; return true; }
		if (OpStr.Equals(TEXT("DeleteNode"), ESearchCase::IgnoreCase))       { OutOp = EOp::DeleteNode; return true; }
		if (OpStr.Equals(TEXT("AddNode"), ESearchCase::IgnoreCase))          { OutOp = EOp::AddNode; return true; }
		if (OpStr.Equals(TEXT("WirePins"), ESearchCase::IgnoreCase))         { OutOp = EOp::WirePins; return true; }
		if (OpStr.Equals(TEXT("DeleteVariable"), ESearchCase::IgnoreCase))     { OutOp = EOp::DeleteVariable; return true; }
		if (OpStr.Equals(TEXT("RenameVariable"), ESearchCase::IgnoreCase))     { OutOp = EOp::RenameVariable; return true; }
		if (OpStr.Equals(TEXT("AddFunction"), ESearchCase::IgnoreCase))        { OutOp = EOp::AddFunction; return true; }
		if (OpStr.Equals(TEXT("AddFunctionInput"), ESearchCase::IgnoreCase))   { OutOp = EOp::AddFunctionInput; return true; }
		if (OpStr.Equals(TEXT("AddFunctionOutput"), ESearchCase::IgnoreCase))  { OutOp = EOp::AddFunctionOutput; return true; }
		if (OpStr.Equals(TEXT("DeleteFunction"), ESearchCase::IgnoreCase))     { OutOp = EOp::DeleteFunction; return true; }
		if (OpStr.Equals(TEXT("SetVariableDefault"), ESearchCase::IgnoreCase)) { OutOp = EOp::SetVariableDefault; return true; }
		if (OpStr.Equals(TEXT("BeginBatch"), ESearchCase::IgnoreCase))         { OutOp = EOp::BeginBatch; return true; }
		if (OpStr.Equals(TEXT("EndBatch"), ESearchCase::IgnoreCase))           { OutOp = EOp::EndBatch; return true; }
		if (OpStr.Equals(TEXT("CreateBlueprint"), ESearchCase::IgnoreCase))    { OutOp = EOp::CreateBlueprint; return true; }
		if (OpStr.Equals(TEXT("SetPinDefault"), ESearchCase::IgnoreCase))      { OutOp = EOp::SetPinDefault; return true; }
		if (OpStr.Equals(TEXT("RetypeVariable"), ESearchCase::IgnoreCase))     { OutOp = EOp::RetypeVariable; return true; }
		if (OpStr.Equals(TEXT("SetVariableCategory"), ESearchCase::IgnoreCase)){ OutOp = EOp::SetVariableCategory; return true; }
		if (OpStr.Equals(TEXT("DuplicateBlueprint"), ESearchCase::IgnoreCase)) { OutOp = EOp::DuplicateBlueprint; return true; }
		if (OpStr.Equals(TEXT("WriteGeneratedSource"), ESearchCase::IgnoreCase)) { OutOp = EOp::WriteGeneratedSource; return true; }
		UE_LOG(LogBlueprintReader, Error, TEXT("Unknown -Op=%s"), *OpStr);
		return false;
	}

	FString ResolveAssetPath(const FString& Params)
	{
		FString Asset;
		if (FParse::Value(*Params, TEXT("Asset="), Asset)) return Asset;
		FParse::Value(*Params, TEXT("Path="), Asset);
		return Asset;
	}

	FString ResolveOutputPath(const FString& Params)
	{
		FString Out;
		if (FParse::Value(*Params, TEXT("Out="), Out)) return Out;
		FParse::Value(*Params, TEXT("Output="), Out);
		return Out;
	}

	int32 EmitJson(const FString& Json, const FString& OutputPath)
	{
		if (!OutputPath.IsEmpty())
		{
			if (!FFileHelper::SaveStringToFile(Json, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				UE_LOG(LogBlueprintReader, Error, TEXT("Failed to write output to: %s"), *OutputPath);
				return 3;
			}
			UE_LOG(LogBlueprintReader, Display, TEXT("Wrote %d bytes to %s"), Json.Len(), *OutputPath);
		}
		else
		{
			FPlatformMisc::LocalPrint(*Json);
			FPlatformMisc::LocalPrint(TEXT("\n"));
		}
		return 0;
	}

	FString IsoDateForFile(const FString& Filename)
	{
		IFileManager& FM = IFileManager::Get();
		const FDateTime DT = FM.GetTimeStamp(*Filename);
		if (DT == FDateTime::MinValue()) return FString();
		return DT.ToIso8601();
	}

	// ----- Shared helpers for write ops -------------------------------------

	UBlueprint* LoadMutableBlueprint(const FString& AssetPath)
	{
		FString Resolved = AssetPath;
		if (!Resolved.Contains(TEXT(".")))
		{
			FString Leaf;
			if (Resolved.Split(TEXT("/"), nullptr, &Leaf, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
			{
				Resolved = Resolved + TEXT(".") + Leaf;
			}
		}
		return LoadObject<UBlueprint>(nullptr, *Resolved);
	}

	UEdGraph* FindGraphByName(UBlueprint* BP, const FString& Name)
	{
		auto Search = [&](const TArray<UEdGraph*>& Graphs) -> UEdGraph*
		{
			for (UEdGraph* G : Graphs)
			{
				if (G && G->GetFName().ToString().Equals(Name, ESearchCase::IgnoreCase))
				{
					return G;
				}
			}
			return nullptr;
		};
		if (UEdGraph* G = Search(BP->UbergraphPages)) return G;
		if (UEdGraph* G = Search(BP->FunctionGraphs)) return G;
		if (UEdGraph* G = Search(BP->MacroGraphs))    return G;
		return nullptr;
	}

	UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FString& Guid)
	{
		FGuid Parsed;
		if (!FGuid::Parse(Guid, Parsed)) return nullptr;
		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (N && N->NodeGuid == Parsed) return N;
		}
		return nullptr;
	}

	// Forward decls — these are defined later in the file but referenced
	// by ops higher up (SetPinDefaultOp / RetypeVariableOp use them).
	// Definitions land in their existing locations alongside the WirePins
	// code / EmitOk / AddVariable helpers.
	UEdGraphPin* FindPinByIdOrName(UEdGraphNode* Node, const FString& Spec);
	int32 EmitOk(const FString& OutputPath, bool bPretty);
	bool BuildPinTypeFromFlags(const FString& Params, FEdGraphPinType& Out);

	// Severity → wire string, matching what the MCP server's tool-result
	// envelope expects. (UE 5.7 removed CriticalError from the enum.)
	const TCHAR* SeverityToString(EMessageSeverity::Type Sev)
	{
		switch (Sev)
		{
		case EMessageSeverity::Error:        return TEXT("error");
		case EMessageSeverity::PerformanceWarning:
		case EMessageSeverity::Warning:      return TEXT("warning");
		case EMessageSeverity::Info:         return TEXT("info");
		default:                              return TEXT("info");
		}
	}

	// Drain `Results.Messages` into a JSON array of {severity, message,
	// node_guid?}. Used by both single-op and batch-end paths to surface
	// compile diagnostics back to the agent (C1).
	TArray<TSharedPtr<FJsonValue>> SerializeDiagnostics(const FCompilerResultsLog& Results)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Results.Messages.Num());
		for (const TSharedRef<FTokenizedMessage>& Msg : Results.Messages)
		{
			auto Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("severity"), SeverityToString(Msg->GetSeverity()));
			Obj->SetStringField(TEXT("message"),  Msg->ToText().ToString());
			// Best-effort: scan the message tokens for a UObject reference
			// pointing at a UEdGraphNode and emit its GUID.
			for (const TSharedRef<IMessageToken>& Tok : Msg->GetMessageTokens())
			{
				if (Tok->GetType() == EMessageToken::Object)
				{
					auto* ObjTok = static_cast<FUObjectToken*>(&Tok.Get());
					if (UObject* O = ObjTok->GetObject().Get())
					{
						if (UEdGraphNode* Node = Cast<UEdGraphNode>(O))
						{
							Obj->SetStringField(TEXT("node_guid"),
								Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
							break;
						}
					}
				}
			}
			Out.Add(MakeShared<FJsonValueObject>(Obj));
		}
		return Out;
	}

	// Compile + save. When `OutDiagnostics` is non-null, populates it with
	// the collected compile diagnostics serialized as JSON values.
	bool CompileAndSaveBlueprint(UBlueprint* BP,
	                             TArray<TSharedPtr<FJsonValue>>* OutDiagnostics = nullptr)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		FCompilerResultsLog Results;
		FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::None, &Results);

		if (OutDiagnostics)
		{
			TArray<TSharedPtr<FJsonValue>> Diags = SerializeDiagnostics(Results);
			OutDiagnostics->Append(MoveTemp(Diags));
		}

		UPackage* Package = BP->GetOutermost();
		if (!Package) return false;

		const FString FileName = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		Args.Error = GError;
		const bool bOk = UPackage::SavePackage(Package, BP, *FileName, Args);
		if (!bOk)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("SavePackage failed: %s"), *FileName);
		}
		return bOk;
	}

	// ----- Batch state (A1) ---------------------------------------------------
	// When a BeginBatch op is seen, every subsequent write op defers its
	// CompileAndSaveBlueprint call. The deferred BPs are tracked here and
	// flushed by EndBatch in one pass — N×compile collapses to 1×compile per
	// affected BP. Single-op (non-batch) callers see no behavior change.
	//
	// Daemon-scoped: the daemon is a single editor process so static state
	// is fine. One-shot mode (subprocess-per-call) never opens a batch.
	bool& BatchDeferFlag()
	{
		static bool bDefer = false;
		return bDefer;
	}
	TArray<TWeakObjectPtr<UBlueprint>>& BatchPending()
	{
		static TArray<TWeakObjectPtr<UBlueprint>> Pending;
		return Pending;
	}

	// Replaces the 12 direct CompileAndSaveBlueprint(BP) call sites. In
	// non-batch mode this is identical to CompileAndSaveBlueprint. In batch
	// mode it just records the BP for EndBatch to process.
	bool MaybeCompileAndSave(UBlueprint* BP)
	{
		if (!BP) return false;
		if (BatchDeferFlag())
		{
			BatchPending().AddUnique(BP);
			// Mark structurally modified now so subsequent ops in the same
			// batch see a consistent state. The compile + save is still
			// deferred to EndBatch.
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
			return true;
		}
		return CompileAndSaveBlueprint(BP);
	}

	int32 RunBeginBatchOp(const FString& /*Params*/, const FString& OutputPath, bool bPretty)
	{
		// Idempotent — re-issuing BeginBatch without an EndBatch is allowed
		// (tests + crash recovery may do this); just clear any stale state.
		BatchDeferFlag() = true;
		BatchPending().Reset();
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetBoolField(TEXT("batch_open"), true);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunEndBatchOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		// Always clear the defer flag first — even if we throw mid-flush we
		// don't want subsequent ops to silently keep deferring.
		BatchDeferFlag() = false;
		TArray<TWeakObjectPtr<UBlueprint>> Pending = MoveTemp(BatchPending());
		BatchPending().Reset();

		// `-Skip` flag (passed by RunOps when on_failure="skip" + a mid-batch
		// failure occurred): discard the pending compile + save. The
		// in-memory UBlueprints stay dirty for this daemon session — agent
		// must avoid acting on them until the daemon restarts or they're
		// explicitly reloaded. Documented limitation; matches the "don't
		// persist partial state" contract of strict atomic mode.
		const bool bSkipCompile = FParse::Param(*Params, TEXT("Skip"));

		TArray<TSharedPtr<FJsonValue>> Recompiled;
		TArray<TSharedPtr<FJsonValue>> AllDiagnostics;
		int32 Failures = 0;
		int32 Errors = 0, Warnings = 0;
		for (TWeakObjectPtr<UBlueprint>& Weak : Pending)
		{
			UBlueprint* BP = Weak.Get();
			if (!BP) continue;  // GC'd between batch ops — nothing to save
			const FString AssetPath = BP->GetPathName();
			if (bSkipCompile)
			{
				// Don't compile, don't save — just record what would have
				// been recompiled so the caller knows which BPs are now
				// in a dirty in-memory state.
				continue;
			}
			TArray<TSharedPtr<FJsonValue>> Diags;
			const bool bOk = CompileAndSaveBlueprint(BP, &Diags);
			if (!bOk) ++Failures;
			// Tag each diagnostic with the asset_path so callers can attribute
			// when multiple BPs compile in one batch.
			for (auto& D : Diags)
			{
				if (D.IsValid() && D->Type == EJson::Object)
				{
					TSharedPtr<FJsonObject> Obj = D->AsObject();
					if (Obj.IsValid())
					{
						Obj->SetStringField(TEXT("asset_path"), AssetPath);
						const FString Sev = Obj->GetStringField(TEXT("severity"));
						if (Sev == TEXT("error"))   ++Errors;
						if (Sev == TEXT("warning")) ++Warnings;
					}
				}
				AllDiagnostics.Add(D);
			}
			Recompiled.Add(MakeShared<FJsonValueString>(AssetPath));
		}

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), Failures == 0);
		Obj->SetArrayField(TEXT("recompiled"), Recompiled);
		Obj->SetNumberField(TEXT("failed"), Failures);
		Obj->SetArrayField(TEXT("diagnostics"), AllDiagnostics);
		Obj->SetNumberField(TEXT("error_count"),   Errors);
		Obj->SetNumberField(TEXT("warning_count"), Warnings);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	// ----- CreateBlueprint (A3) ---------------------------------------------
	// Creates a new BP under `/Game/...` with the given parent class. Idempotent:
	// if the asset already exists, returns {ok:true, already_existed:true}.
	// Adapts the SeedCommandlet pattern (FactoryCreateNew via UBlueprintFactory)
	// and registers with the asset registry so a follow-up op in the same batch
	// can LoadMutableBlueprint it.
	UClass* ResolveParentClass(const FString& Spec)
	{
		if (Spec.IsEmpty()) return nullptr;
		// Direct path form: "/Script/Engine.Actor"
		if (UClass* C = LoadObject<UClass>(nullptr, *Spec)) return C;
		// Short-name form: "Actor", "Pawn", "ACharacter" — try common prefixes.
		const TCHAR* Prefixes[] = {TEXT(""), TEXT("/Script/Engine."), TEXT("/Script/CoreUObject.")};
		for (const TCHAR* Pre : Prefixes)
		{
			FString Trial = FString(Pre) + Spec;
			if (UClass* C = LoadObject<UClass>(nullptr, *Trial)) return C;
			// Drop a leading 'A' or 'U' if present (UE convention).
			if (Spec.Len() > 1 && (Spec[0] == TEXT('A') || Spec[0] == TEXT('U')))
			{
				FString Trim = FString(Pre) + Spec.RightChop(1);
				if (UClass* C = LoadObject<UClass>(nullptr, *Trim)) return C;
			}
		}
		return nullptr;
	}

	int32 RunCreateBlueprintOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString ParentClassSpec;
		FParse::Value(*Params, TEXT("ParentClass="), ParentClassSpec);

		if (AssetPath.IsEmpty() || ParentClassSpec.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("CreateBlueprint requires -Asset=/Game/... and -ParentClass=<UClass path or short name>"));
			return 1;
		}
		if (!AssetPath.StartsWith(TEXT("/Game/")))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("CreateBlueprint: -Asset must be under /Game/ (got: %s)"), *AssetPath);
			return 1;
		}

		// Idempotency probe — if the asset already exists, short-circuit.
		if (UBlueprint* Existing = LoadMutableBlueprint(AssetPath))
		{
			(void)Existing;
			auto Obj = MakeShared<FJsonObject>();
			Obj->SetBoolField(TEXT("ok"), true);
			Obj->SetBoolField(TEXT("already_existed"), true);
			Obj->SetStringField(TEXT("asset_path"), AssetPath);
			return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
		}

		UClass* ParentClass = ResolveParentClass(ParentClassSpec);
		if (!ParentClass)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("CreateBlueprint: parent class not found: %s"), *ParentClassSpec);
			return 4;
		}

		// Derive package name + asset short name from the asset path.
		// `/Game/AI/BP_Foo` -> package `/Game/AI/BP_Foo`, asset `BP_Foo`.
		const FString PackageName = AssetPath;
		const FString AssetName   = FPackageName::GetShortName(AssetPath);

		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("CreatePackage failed: %s"), *PackageName);
			return 5;
		}
		Package->FullyLoad();

		UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
		Factory->ParentClass = ParentClass;

		UBlueprint* BP = Cast<UBlueprint>(Factory->FactoryCreateNew(
			UBlueprint::StaticClass(), Package, *AssetName,
			RF_Public | RF_Standalone, nullptr, GWarn));
		if (!BP)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("FactoryCreateNew failed: %s"), *PackageName);
			return 5;
		}
		BP->MarkPackageDirty();

		// Critical: register with asset registry so a follow-up op in the
		// same batch (e.g. AddVariable) can LoadMutableBlueprint() this asset.
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		ARM.Get().AssetCreated(BP);

		if (!MaybeCompileAndSave(BP)) return 5;

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetBoolField(TEXT("already_existed"), false);
		Obj->SetStringField(TEXT("asset_path"), AssetPath);
		Obj->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	// ----- SetPinDefault (B1) ------------------------------------------------
	// Sets the literal default value on a node's pin. compile_function uses
	// this to materialize {lit:value} expressions — UE has no first-class
	// "literal node", so the value is set as the consumer pin's default.
	int32 RunSetPinDefaultOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString GraphName, NodeId, PinSpec, Value;
		FParse::Value(*Params, TEXT("Graph="), GraphName);
		FParse::Value(*Params, TEXT("Node="),  NodeId);
		FParse::Value(*Params, TEXT("Pin="),   PinSpec);
		FParse::Value(*Params, TEXT("Value="), Value);

		if (AssetPath.IsEmpty() || GraphName.IsEmpty() || NodeId.IsEmpty() || PinSpec.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("SetPinDefault requires -Asset= -Graph= -Node= -Pin= [-Value=]"));
			return 1;
		}
		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 2;
		UEdGraph* Graph = FindGraphByName(BP, GraphName);
		if (!Graph) return 4;
		UEdGraphNode* Node = FindNodeByGuid(Graph, NodeId);
		if (!Node) return 4;
		UEdGraphPin* Pin = FindPinByIdOrName(Node, PinSpec);
		if (!Pin)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("SetPinDefault: pin '%s' not found on node %s"), *PinSpec, *NodeId);
			return 4;
		}
		// Modify the pin's default value. UE 5.7's TrySetDefaultValue
		// returns void — type-mismatch errors surface via the compile log
		// and bubble up through the EndBatch diagnostics path (C1).
		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		Schema->TrySetDefaultValue(*Pin, Value);
		Pin->Modify();
		Node->Modify();
		if (!MaybeCompileAndSave(BP)) return 5;
		return EmitOk(OutputPath, bPretty);
	}

	// ----- RetypeVariable (BP-2) ---------------------------------------
	// Change a member variable's type WITHOUT delete + re-add. Preserves
	// every VariableGet / VariableSet node that references it — UE
	// rewires their pin types in place.
	int32 RunRetypeVariableOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString VarName;
		FParse::Value(*Params, TEXT("Name="), VarName);
		FEdGraphPinType NewType;
		if (AssetPath.IsEmpty() || VarName.IsEmpty() ||
		    !BuildPinTypeFromFlags(Params, NewType))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("RetypeVariable requires -Asset= -Name= -TypeCategory= [-TypeSubCategory=...] [-TypeSubCategoryObject=...]"));
			return 1;
		}
		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 4;
		// Confirm the variable exists before mutating — UE's
		// ChangeMemberVariableType is a void function with no failure
		// signal; the only way to fail loudly is to pre-check.
		if (FBlueprintEditorUtils::FindNewVariableIndex(BP, FName(*VarName)) == INDEX_NONE)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("RetypeVariable: variable '%s' not found on %s"), *VarName, *AssetPath);
			return 4;
		}
		FBlueprintEditorUtils::ChangeMemberVariableType(BP, FName(*VarName), NewType);
		if (!MaybeCompileAndSave(BP)) return 5;
		return EmitOk(OutputPath, bPretty);
	}

	// ----- SetVariableCategory (BP-7) ----------------------------------
	// Change the My-Blueprint-panel category label on a member variable.
	// Empty -Category= clears the category back to the default.
	int32 RunSetVariableCategoryOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString VarName, Category;
		FParse::Value(*Params, TEXT("Name="),     VarName);
		FParse::Value(*Params, TEXT("Category="), Category);
		if (AssetPath.IsEmpty() || VarName.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("SetVariableCategory requires -Asset= -Name= [-Category=]"));
			return 1;
		}
		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 4;
		if (FBlueprintEditorUtils::FindNewVariableIndex(BP, FName(*VarName)) == INDEX_NONE)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("SetVariableCategory: variable '%s' not found on %s"), *VarName, *AssetPath);
			return 4;
		}
		// `nullptr` for InLocalVarScope = member variable (vs function-
		// local). bDontRecompile=true lets MaybeCompileAndSave handle
		// the compile (or defer it under a batch).
		FBlueprintEditorUtils::SetBlueprintVariableCategory(
			BP, FName(*VarName), nullptr, FText::FromString(Category),
			/*bDontRecompile=*/true);
		if (!MaybeCompileAndSave(BP)) return 5;
		return EmitOk(OutputPath, bPretty);
	}

	// ----- WriteGeneratedSource (Phase 2C of BP↔C++) ---------------------
	// Write a generated .h/.cpp file into the project's Source/ tree.
	// Args: -Path=<absolute dest> -ContentFile=<absolute source temp>
	//       [-CreateDirs]
	// The server writes the actual content to a temp file (so we don't
	// have to encode big strings into command-line args; the daemon
	// protocol is line-based). The plugin reads that temp + writes to
	// dest, then deletes the temp. Path validation confines writes to
	// the project's Source/ directory — anything else is rejected.
	int32 RunWriteGeneratedSourceOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString DestPath, ContentPath;
		FParse::Value(*Params, TEXT("Path="),        DestPath);
		FParse::Value(*Params, TEXT("ContentFile="), ContentPath);
		const bool bCreateDirs = FParse::Param(*Params, TEXT("CreateDirs"));

		if (DestPath.IsEmpty() || ContentPath.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("WriteGeneratedSource requires -Path=<dest> -ContentFile=<src>"));
			return 1;
		}

		// Canonicalize paths so prefix-checks work consistently.
		FString DestAbs = FPaths::ConvertRelativePathToFull(DestPath);
		FString ProjectSourceDir = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectDir(), TEXT("Source")));

		if (!DestAbs.StartsWith(ProjectSourceDir))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("WriteGeneratedSource: destination must be under '%s' (got: %s)"),
				*ProjectSourceDir, *DestAbs);
			return 1;
		}

		FString Content;
		if (!FFileHelper::LoadFileToString(Content, *ContentPath))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("WriteGeneratedSource: failed to read content temp file: %s"),
				*ContentPath);
			return 4;
		}

		if (bCreateDirs)
		{
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(DestAbs), /*Tree=*/true);
		}

		if (!FFileHelper::SaveStringToFile(Content, *DestAbs,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("WriteGeneratedSource: failed to write %s"), *DestAbs);
			return 5;
		}

		// Best-effort cleanup of the temp file (the server may have
		// already done this; ignore failures).
		IFileManager::Get().Delete(*ContentPath);

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("path"), DestAbs);
		Obj->SetNumberField(TEXT("bytes_written"), Content.Len());
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	// ----- DuplicateBlueprint (BP-5) -----------------------------------
	// File-level duplicate: source BP at /Game/X → new BP at /Game/Y.
	// Idempotent on the destination path: if /Game/Y already exists,
	// returns already_existed:true without overwriting.
	int32 RunDuplicateBlueprintOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString SourceAsset = ResolveAssetPath(Params);
		FString DestAsset;
		FParse::Value(*Params, TEXT("Dest="), DestAsset);
		if (SourceAsset.IsEmpty() || DestAsset.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("DuplicateBlueprint requires -Asset=/Game/X -Dest=/Game/Y"));
			return 1;
		}
		if (!DestAsset.StartsWith(TEXT("/Game/")))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("DuplicateBlueprint: -Dest must be under /Game/ (got: %s)"), *DestAsset);
			return 1;
		}

		// Idempotency: if the destination already exists, return without
		// touching anything. Same contract as create_blueprint.
		if (UBlueprint* Existing = LoadMutableBlueprint(DestAsset))
		{
			(void)Existing;
			auto Obj = MakeShared<FJsonObject>();
			Obj->SetBoolField(TEXT("ok"), true);
			Obj->SetBoolField(TEXT("already_existed"), true);
			Obj->SetStringField(TEXT("asset_path"), DestAsset);
			return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
		}

		UBlueprint* SourceBP = LoadMutableBlueprint(SourceAsset);
		if (!SourceBP)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("DuplicateBlueprint: source asset not found: %s"), *SourceAsset);
			return 4;
		}

		// IAssetTools::DuplicateAsset takes (NewAssetName, NewPackagePath,
		// SourceObject). Split the destination path: package path is
		// everything up to (but not including) the last segment; asset
		// name is the last segment.
		FString DestPackagePath, DestName;
		{
			int32 LastSlash;
			if (!DestAsset.FindLastChar(TEXT('/'), LastSlash) || LastSlash <= 0)
			{
				UE_LOG(LogBlueprintReader, Error,
					TEXT("DuplicateBlueprint: -Dest='%s' is malformed"), *DestAsset);
				return 1;
			}
			DestPackagePath = DestAsset.Left(LastSlash);
			DestName        = DestAsset.RightChop(LastSlash + 1);
		}

		FAssetToolsModule& AssetToolsModule =
			FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
		IAssetTools& AssetTools = AssetToolsModule.Get();
		UObject* NewObj = AssetTools.DuplicateAsset(DestName, DestPackagePath, SourceBP);
		if (!NewObj)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("DuplicateBlueprint: DuplicateAsset failed for %s -> %s"),
				*SourceAsset, *DestAsset);
			return 5;
		}

		UBlueprint* NewBP = Cast<UBlueprint>(NewObj);
		if (!NewBP)
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("DuplicateBlueprint: duplicate isn't a UBlueprint (was %s)"),
				*NewObj->GetClass()->GetName());
			return 5;
		}
		// MaybeCompileAndSave handles the save + compile. AssetCreated
		// notification fires inside DuplicateAsset already, so a
		// follow-up batch op can LoadMutableBlueprint it.
		if (!MaybeCompileAndSave(NewBP)) return 5;

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetBoolField(TEXT("already_existed"), false);
		Obj->SetStringField(TEXT("asset_path"), DestAsset);
		Obj->SetStringField(TEXT("source_asset_path"), SourceAsset);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	// Emit a small ack JSON blob for a successful write op.
	int32 EmitOk(const FString& OutputPath, bool bPretty)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	int32 RunAddVariableOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString VarName, DefaultValue, Category;
		FParse::Value(*Params, TEXT("Name="),     VarName);
		FParse::Value(*Params, TEXT("Default="),  DefaultValue);
		FParse::Value(*Params, TEXT("Category="), Category);
		const bool bReplicated = FParse::Param(*Params, TEXT("Replicated"));
		const bool bEditable   = FParse::Param(*Params, TEXT("Editable"));

		// Build BPPinType from the individual -TypeFoo= flags.
		FString TypeCategory, TypeSubCategory, TypeSubObject;
		FParse::Value(*Params, TEXT("TypeCategory="),         TypeCategory);
		FParse::Value(*Params, TEXT("TypeSubCategory="),      TypeSubCategory);
		FParse::Value(*Params, TEXT("TypeSubCategoryObject="), TypeSubObject);

		if (AssetPath.IsEmpty() || VarName.IsEmpty() || TypeCategory.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("AddVariable requires -Asset= -Name= -TypeCategory= [-TypeSubCategory=...] [-TypeSubCategoryObject=...]"));
			return 1;
		}

		auto TypeJson = MakeShared<FJsonObject>();
		TypeJson->SetStringField(TEXT("category"), TypeCategory);
		if (!TypeSubCategory.IsEmpty()) TypeJson->SetStringField(TEXT("sub_category"), TypeSubCategory);
		if (!TypeSubObject.IsEmpty())   TypeJson->SetStringField(TEXT("sub_category_object"), TypeSubObject);
		TypeJson->SetBoolField(TEXT("is_array"), FParse::Param(*Params, TEXT("TypeIsArray")));
		TypeJson->SetBoolField(TEXT("is_set"),   FParse::Param(*Params, TEXT("TypeIsSet")));
		TypeJson->SetBoolField(TEXT("is_map"),   FParse::Param(*Params, TEXT("TypeIsMap")));

		FEdGraphPinType PinType;
		if (!FBlueprintReaderWireJson::ParseWirePinType(TypeJson, PinType))
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("AddVariable: failed to build pin type"));
			return 1;
		}

		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("AddVariable: failed to load %s"), *AssetPath);
			return 4;
		}

		const FName NewName(*VarName);
		if (FBlueprintEditorUtils::FindNewVariableIndex(BP, NewName) != INDEX_NONE)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("AddVariable: variable %s already exists on %s"),
				*VarName, *AssetPath);
			return 1;
		}
		FBlueprintEditorUtils::AddMemberVariable(BP, NewName, PinType, DefaultValue);
		const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(BP, NewName);
		if (Index == INDEX_NONE)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("AddVariable: AddMemberVariable failed"));
			return 1;
		}
		FBPVariableDescription& Var = BP->NewVariables[Index];
		if (!Category.IsEmpty()) Var.Category = FText::FromString(Category);
		if (bEditable)   Var.PropertyFlags |= CPF_Edit | CPF_BlueprintVisible;
		if (bReplicated) { Var.PropertyFlags |= CPF_Net; Var.ReplicationCondition = COND_None; }

		if (!MaybeCompileAndSave(BP)) return 5;
		return EmitOk(OutputPath, bPretty);
	}

	int32 RunSetNodePositionOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString GraphName, NodeId;
		int32 X = 0, Y = 0;
		FParse::Value(*Params, TEXT("Graph="), GraphName);
		FParse::Value(*Params, TEXT("Node="),  NodeId);
		FParse::Value(*Params, TEXT("X="),     X);
		FParse::Value(*Params, TEXT("Y="),     Y);

		if (AssetPath.IsEmpty() || GraphName.IsEmpty() || NodeId.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("SetNodePosition requires -Asset= -Graph= -Node= -X= -Y="));
			return 1;
		}

		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 4;

		UEdGraph* Graph = FindGraphByName(BP, GraphName);
		if (!Graph)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("SetNodePosition: graph %s not found"), *GraphName);
			return 4;
		}
		UEdGraphNode* Node = FindNodeByGuid(Graph, NodeId);
		if (!Node)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("SetNodePosition: node %s not found in %s"), *NodeId, *GraphName);
			return 4;
		}
		Node->Modify();
		Node->NodePosX = X;
		Node->NodePosY = Y;

		if (!MaybeCompileAndSave(BP)) return 5;
		return EmitOk(OutputPath, bPretty);
	}

	int32 RunDeleteNodeOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString GraphName, NodeId;
		FParse::Value(*Params, TEXT("Graph="), GraphName);
		FParse::Value(*Params, TEXT("Node="),  NodeId);

		if (AssetPath.IsEmpty() || GraphName.IsEmpty() || NodeId.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("DeleteNode requires -Asset= -Graph= -Node="));
			return 1;
		}

		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 4;
		UEdGraph* Graph = FindGraphByName(BP, GraphName);
		if (!Graph) return 4;
		UEdGraphNode* Node = FindNodeByGuid(Graph, NodeId);
		if (!Node)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("DeleteNode: node %s not found in %s"), *NodeId, *GraphName);
			return 4;
		}

		// Break links + remove from graph. FBlueprintEditorUtils::RemoveNode
		// handles both, plus refreshes any dependent UI state.
		FBlueprintEditorUtils::RemoveNode(BP, Node, /*bDontRecompile=*/true);

		if (!MaybeCompileAndSave(BP)) return 5;
		return EmitOk(OutputPath, bPretty);
	}

	// ----- AddNode ----------------------------------------------------------
	// Resolve a UClass path passed via -ClassPath= or short name (e.g.
	// "Actor" or "/Script/Engine.Actor").
	UClass* ResolveClass(const FString& Spec)
	{
		if (Spec.IsEmpty()) return nullptr;
		// Try as full path first.
		if (UObject* Resolved = StaticLoadObject(UClass::StaticClass(), nullptr, *Spec))
		{
			return Cast<UClass>(Resolved);
		}
		// Try as a short name under /Script/Engine.
		const FString Engine = FString::Printf(TEXT("/Script/Engine.%s"), *Spec);
		return Cast<UClass>(StaticLoadObject(UClass::StaticClass(), nullptr, *Engine));
	}

	// Spawn + register a node in `Graph`. Caller is responsible for any
	// node-specific state (variable/function refs, output pins, etc.).
	template <typename TNode>
	TNode* AddNodeToGraph(UEdGraph* Graph, int32 X, int32 Y)
	{
		TNode* Node = NewObject<TNode>(Graph);
		Node->CreateNewGuid();
		Node->NodePosX = X;
		Node->NodePosY = Y;
		Graph->AddNode(Node, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
		return Node;
	}

	int32 RunAddNodeOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString GraphName, Kind;
		int32 X = 0, Y = 0;
		FParse::Value(*Params, TEXT("Graph="), GraphName);
		FParse::Value(*Params, TEXT("Kind="),  Kind);
		FParse::Value(*Params, TEXT("X="),     X);
		FParse::Value(*Params, TEXT("Y="),     Y);

		if (AssetPath.IsEmpty() || GraphName.IsEmpty() || Kind.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("AddNode requires -Asset= -Graph= -Kind="));
			return 1;
		}

		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 4;
		UEdGraph* Graph = FindGraphByName(BP, GraphName);
		if (!Graph)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("AddNode: graph %s not found"), *GraphName);
			return 4;
		}

		UEdGraphNode* Spawned = nullptr;

		if (Kind.Equals(TEXT("Branch"), ESearchCase::IgnoreCase) ||
		    Kind.Equals(TEXT("IfThenElse"), ESearchCase::IgnoreCase))
		{
			Spawned = AddNodeToGraph<UK2Node_IfThenElse>(Graph, X, Y);
		}
		else if (Kind.Equals(TEXT("Sequence"), ESearchCase::IgnoreCase) ||
		         Kind.Equals(TEXT("ExecutionSequence"), ESearchCase::IgnoreCase))
		{
			Spawned = AddNodeToGraph<UK2Node_ExecutionSequence>(Graph, X, Y);
		}
		else if (Kind.Equals(TEXT("VariableGet"), ESearchCase::IgnoreCase))
		{
			FString VarName;
			FParse::Value(*Params, TEXT("Variable="), VarName);
			if (VarName.IsEmpty())
			{
				UE_LOG(LogBlueprintReader, Error, TEXT("AddNode VariableGet requires -Variable="));
				return 1;
			}
			UK2Node_VariableGet* Get = NewObject<UK2Node_VariableGet>(Graph);
			Get->VariableReference.SetSelfMember(*VarName);
			Get->CreateNewGuid();
			Get->NodePosX = X; Get->NodePosY = Y;
			Graph->AddNode(Get, false, false);
			Get->PostPlacedNewNode();
			Get->AllocateDefaultPins();
			Spawned = Get;
		}
		else if (Kind.Equals(TEXT("VariableSet"), ESearchCase::IgnoreCase))
		{
			FString VarName;
			FParse::Value(*Params, TEXT("Variable="), VarName);
			if (VarName.IsEmpty())
			{
				UE_LOG(LogBlueprintReader, Error, TEXT("AddNode VariableSet requires -Variable="));
				return 1;
			}
			UK2Node_VariableSet* Set = NewObject<UK2Node_VariableSet>(Graph);
			Set->VariableReference.SetSelfMember(*VarName);
			Set->CreateNewGuid();
			Set->NodePosX = X; Set->NodePosY = Y;
			Graph->AddNode(Set, false, false);
			Set->PostPlacedNewNode();
			Set->AllocateDefaultPins();
			Spawned = Set;
		}
		else if (Kind.Equals(TEXT("CallFunction"), ESearchCase::IgnoreCase))
		{
			FString FunctionName, FunctionOwner;
			FParse::Value(*Params, TEXT("Function="),      FunctionName);
			FParse::Value(*Params, TEXT("FunctionOwner="), FunctionOwner);
			if (FunctionName.IsEmpty() || FunctionOwner.IsEmpty())
			{
				UE_LOG(LogBlueprintReader, Error,
					TEXT("AddNode CallFunction requires -Function=<name> -FunctionOwner=<class path>"));
				return 1;
			}
			UClass* OwnerClass = ResolveClass(FunctionOwner);
			if (!OwnerClass)
			{
				UE_LOG(LogBlueprintReader, Error, TEXT("AddNode CallFunction: class %s not found"), *FunctionOwner);
				return 1;
			}
			UFunction* Fn = OwnerClass->FindFunctionByName(*FunctionName);
			if (!Fn)
			{
				UE_LOG(LogBlueprintReader, Error,
					TEXT("AddNode CallFunction: function %s not found on %s"),
					*FunctionName, *OwnerClass->GetPathName());
				return 1;
			}
			UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
			Call->SetFromFunction(Fn);
			Call->CreateNewGuid();
			Call->NodePosX = X; Call->NodePosY = Y;
			Graph->AddNode(Call, false, false);
			Call->PostPlacedNewNode();
			Call->AllocateDefaultPins();
			Spawned = Call;
		}
		else if (Kind.Equals(TEXT("CustomEvent"), ESearchCase::IgnoreCase))
		{
			FString EventName;
			FParse::Value(*Params, TEXT("EventName="), EventName);
			if (EventName.IsEmpty())
			{
				UE_LOG(LogBlueprintReader, Error, TEXT("AddNode CustomEvent requires -EventName="));
				return 1;
			}
			UK2Node_CustomEvent* Evt = NewObject<UK2Node_CustomEvent>(Graph);
			Evt->CustomFunctionName = FName(*EventName);
			Evt->CreateNewGuid();
			Evt->NodePosX = X; Evt->NodePosY = Y;
			Graph->AddNode(Evt, false, false);
			Evt->PostPlacedNewNode();
			Evt->AllocateDefaultPins();
			Spawned = Evt;
		}
		else if (Kind.Equals(TEXT("Cast"), ESearchCase::IgnoreCase) ||
		         Kind.Equals(TEXT("DynamicCast"), ESearchCase::IgnoreCase))
		{
			FString TargetClass;
			FParse::Value(*Params, TEXT("TargetClass="), TargetClass);
			if (TargetClass.IsEmpty())
			{
				UE_LOG(LogBlueprintReader, Error,
					TEXT("AddNode Cast requires -TargetClass=<UClass path or short name>"));
				return 1;
			}
			UClass* Tgt = ResolveClass(TargetClass);
			if (!Tgt)
			{
				UE_LOG(LogBlueprintReader, Error, TEXT("AddNode Cast: class %s not found"), *TargetClass);
				return 1;
			}
			UK2Node_DynamicCast* Cast = NewObject<UK2Node_DynamicCast>(Graph);
			Cast->TargetType = Tgt;
			Cast->CreateNewGuid();
			Cast->NodePosX = X; Cast->NodePosY = Y;
			Graph->AddNode(Cast, false, false);
			Cast->PostPlacedNewNode();
			Cast->AllocateDefaultPins();
			Spawned = Cast;
		}
		else if (Kind.Equals(TEXT("Self"), ESearchCase::IgnoreCase))
		{
			Spawned = AddNodeToGraph<UK2Node_Self>(Graph, X, Y);
		}
		else if (Kind.Equals(TEXT("MakeArray"), ESearchCase::IgnoreCase))
		{
			Spawned = AddNodeToGraph<UK2Node_MakeArray>(Graph, X, Y);
		}
		else if (Kind.Equals(TEXT("MakeStruct"), ESearchCase::IgnoreCase))
		{
			FString StructPath;
			FParse::Value(*Params, TEXT("StructType="), StructPath);
			if (StructPath.IsEmpty())
			{
				UE_LOG(LogBlueprintReader, Error,
					TEXT("AddNode MakeStruct requires -StructType=<UScriptStruct path, e.g. /Script/CoreUObject.Vector>"));
				return 1;
			}
			UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *StructPath);
			if (!Struct)
			{
				UE_LOG(LogBlueprintReader, Error, TEXT("AddNode MakeStruct: struct %s not found"), *StructPath);
				return 1;
			}
			UK2Node_MakeStruct* Make = NewObject<UK2Node_MakeStruct>(Graph);
			Make->StructType = Struct;
			Make->CreateNewGuid();
			Make->NodePosX = X; Make->NodePosY = Y;
			Graph->AddNode(Make, false, false);
			Make->PostPlacedNewNode();
			Make->AllocateDefaultPins();
			Spawned = Make;
		}
		else if (Kind.Equals(TEXT("FormatText"), ESearchCase::IgnoreCase))
		{
			Spawned = AddNodeToGraph<UK2Node_FormatText>(Graph, X, Y);
		}
		else if (Kind.Equals(TEXT("Knot"), ESearchCase::IgnoreCase) ||
		         Kind.Equals(TEXT("Reroute"), ESearchCase::IgnoreCase))
		{
			Spawned = AddNodeToGraph<UK2Node_Knot>(Graph, X, Y);
		}
		else
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("AddNode: unrecognised -Kind=%s; see list_node_kinds for valid values"),
				*Kind);
			return 1;
		}

		if (!Spawned) return 5;
		const FString NewId = Spawned->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);

		if (!MaybeCompileAndSave(BP)) return 5;

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("node_id"), NewId);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	// ----- WirePins ---------------------------------------------------------
	UEdGraphPin* FindPinByIdOrName(UEdGraphNode* Node, const FString& Spec)
	{
		// Try GUID first.
		FGuid AsGuid;
		if (FGuid::Parse(Spec, AsGuid))
		{
			for (UEdGraphPin* P : Node->Pins)
			{
				if (P && P->PinId == AsGuid) return P;
			}
		}
		// Fall back to name match.
		for (UEdGraphPin* P : Node->Pins)
		{
			if (P && P->GetFName().ToString().Equals(Spec, ESearchCase::IgnoreCase))
			{
				return P;
			}
		}
		return nullptr;
	}

	int32 RunWirePinsOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString GraphName, FromNodeId, FromPinSpec, ToNodeId, ToPinSpec;
		FParse::Value(*Params, TEXT("Graph="),    GraphName);
		FParse::Value(*Params, TEXT("FromNode="), FromNodeId);
		FParse::Value(*Params, TEXT("FromPin="),  FromPinSpec);
		FParse::Value(*Params, TEXT("ToNode="),   ToNodeId);
		FParse::Value(*Params, TEXT("ToPin="),    ToPinSpec);

		if (AssetPath.IsEmpty() || GraphName.IsEmpty() ||
		    FromNodeId.IsEmpty() || FromPinSpec.IsEmpty() ||
		    ToNodeId.IsEmpty()   || ToPinSpec.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("WirePins requires -Asset= -Graph= -FromNode= -FromPin= -ToNode= -ToPin="));
			return 1;
		}

		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 4;
		UEdGraph* Graph = FindGraphByName(BP, GraphName);
		if (!Graph) return 4;

		UEdGraphNode* FromNode = FindNodeByGuid(Graph, FromNodeId);
		UEdGraphNode* ToNode   = FindNodeByGuid(Graph, ToNodeId);
		if (!FromNode || !ToNode)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("WirePins: from/to node not found"));
			return 4;
		}
		UEdGraphPin* FromPin = FindPinByIdOrName(FromNode, FromPinSpec);
		UEdGraphPin* ToPin   = FindPinByIdOrName(ToNode,   ToPinSpec);
		if (!FromPin || !ToPin)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("WirePins: from/to pin not found"));
			return 4;
		}

		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (Schema)
		{
			const FPinConnectionResponse Resp = Schema->CanCreateConnection(FromPin, ToPin);
			if (Resp.Response == CONNECT_RESPONSE_DISALLOW)
			{
				UE_LOG(LogBlueprintReader, Error, TEXT("WirePins: schema rejected: %s"),
					*Resp.Message.ToString());
				return 1;
			}
		}
		FromPin->MakeLinkTo(ToPin);

		if (!MaybeCompileAndSave(BP)) return 5;
		return EmitOk(OutputPath, bPretty);
	}

	// ----- DeleteVariable ---------------------------------------------------
	int32 RunDeleteVariableOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString VarName;
		FParse::Value(*Params, TEXT("Name="), VarName);

		if (AssetPath.IsEmpty() || VarName.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("DeleteVariable requires -Asset= -Name="));
			return 1;
		}

		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 4;
		const FName Var(*VarName);
		if (FBlueprintEditorUtils::FindNewVariableIndex(BP, Var) == INDEX_NONE)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("DeleteVariable: %s not found on %s"), *VarName, *AssetPath);
			return 4;
		}
		FBlueprintEditorUtils::RemoveMemberVariable(BP, Var);

		if (!MaybeCompileAndSave(BP)) return 5;
		return EmitOk(OutputPath, bPretty);
	}

	// ----- Function helpers (shared by add_function_input/output) -----------
	bool BuildPinTypeFromFlags(const FString& Params, FEdGraphPinType& Out)
	{
		FString TypeCategory, TypeSubCategory, TypeSubObject;
		FParse::Value(*Params, TEXT("TypeCategory="),         TypeCategory);
		FParse::Value(*Params, TEXT("TypeSubCategory="),      TypeSubCategory);
		FParse::Value(*Params, TEXT("TypeSubCategoryObject="), TypeSubObject);
		if (TypeCategory.IsEmpty()) return false;

		auto TypeJson = MakeShared<FJsonObject>();
		TypeJson->SetStringField(TEXT("category"), TypeCategory);
		if (!TypeSubCategory.IsEmpty()) TypeJson->SetStringField(TEXT("sub_category"), TypeSubCategory);
		if (!TypeSubObject.IsEmpty())   TypeJson->SetStringField(TEXT("sub_category_object"), TypeSubObject);
		TypeJson->SetBoolField(TEXT("is_array"), FParse::Param(*Params, TEXT("TypeIsArray")));
		TypeJson->SetBoolField(TEXT("is_set"),   FParse::Param(*Params, TEXT("TypeIsSet")));
		TypeJson->SetBoolField(TEXT("is_map"),   FParse::Param(*Params, TEXT("TypeIsMap")));
		return FBlueprintReaderWireJson::ParseWirePinType(TypeJson, Out);
	}

	UEdGraph* FindFunctionGraph(UBlueprint* BP, const FString& FunctionName)
	{
		for (UEdGraph* G : BP->FunctionGraphs)
		{
			if (G && G->GetFName().ToString().Equals(FunctionName, ESearchCase::IgnoreCase))
			{
				return G;
			}
		}
		return nullptr;
	}

	UK2Node_FunctionEntry* FindFunctionEntry(UEdGraph* Graph)
	{
		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(N)) return Entry;
		}
		return nullptr;
	}

	UK2Node_FunctionResult* FindOrCreateFunctionResult(UEdGraph* Graph)
	{
		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (UK2Node_FunctionResult* R = Cast<UK2Node_FunctionResult>(N)) return R;
		}
		UK2Node_FunctionResult* Result = NewObject<UK2Node_FunctionResult>(Graph);
		Result->CreateNewGuid();
		Result->NodePosX = 240;
		Result->NodePosY = 0;
		Graph->AddNode(Result, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		Result->PostPlacedNewNode();
		Result->AllocateDefaultPins();
		return Result;
	}

	// ----- AddFunction ------------------------------------------------------
	int32 RunAddFunctionOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString FunctionName;
		FParse::Value(*Params, TEXT("Name="), FunctionName);

		if (AssetPath.IsEmpty() || FunctionName.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("AddFunction requires -Asset= -Name="));
			return 1;
		}

		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 4;
		if (FindFunctionGraph(BP, FunctionName))
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("AddFunction: function %s already exists"), *FunctionName);
			return 1;
		}

		UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
			BP, FName(*FunctionName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, NewGraph, /*bIsUserCreated=*/true, nullptr);

		if (!MaybeCompileAndSave(BP)) return 5;
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("function_name"), FunctionName);
		// Return the FunctionEntry node's GUID so callers (notably
		// compile_function) can wire its `then` exec output into the
		// first statement they spawn — saves a follow-up read+find.
		if (UK2Node_FunctionEntry* Entry = FindFunctionEntry(NewGraph))
		{
			Obj->SetStringField(TEXT("entry_node_id"),
				Entry->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		}
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}

	// ----- AddFunctionInput / AddFunctionOutput -----------------------------
	int32 RunAddFunctionInputOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString FunctionName, ParamName;
		FParse::Value(*Params, TEXT("Function="), FunctionName);
		FParse::Value(*Params, TEXT("Param="),    ParamName);

		FEdGraphPinType PinType;
		if (AssetPath.IsEmpty() || FunctionName.IsEmpty() || ParamName.IsEmpty() ||
		    !BuildPinTypeFromFlags(Params, PinType))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("AddFunctionInput requires -Asset= -Function= -Param= -TypeCategory= [...]"));
			return 1;
		}

		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 4;
		UEdGraph* Graph = FindFunctionGraph(BP, FunctionName);
		if (!Graph)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("AddFunctionInput: function %s not found"), *FunctionName);
			return 4;
		}
		UK2Node_FunctionEntry* Entry = FindFunctionEntry(Graph);
		if (!Entry)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("AddFunctionInput: no FunctionEntry node"));
			return 5;
		}
		Entry->CreateUserDefinedPin(FName(*ParamName), PinType, EGPD_Output, /*bUseUniqueName=*/false);

		if (!MaybeCompileAndSave(BP)) return 5;
		return EmitOk(OutputPath, bPretty);
	}

	int32 RunAddFunctionOutputOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString FunctionName, ParamName;
		FParse::Value(*Params, TEXT("Function="), FunctionName);
		FParse::Value(*Params, TEXT("Param="),    ParamName);

		FEdGraphPinType PinType;
		if (AssetPath.IsEmpty() || FunctionName.IsEmpty() || ParamName.IsEmpty() ||
		    !BuildPinTypeFromFlags(Params, PinType))
		{
			UE_LOG(LogBlueprintReader, Error,
				TEXT("AddFunctionOutput requires -Asset= -Function= -Param= -TypeCategory= [...]"));
			return 1;
		}

		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 4;
		UEdGraph* Graph = FindFunctionGraph(BP, FunctionName);
		if (!Graph)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("AddFunctionOutput: function %s not found"), *FunctionName);
			return 4;
		}
		UK2Node_FunctionResult* Result = FindOrCreateFunctionResult(Graph);
		Result->CreateUserDefinedPin(FName(*ParamName), PinType, EGPD_Input, /*bUseUniqueName=*/false);

		if (!MaybeCompileAndSave(BP)) return 5;
		return EmitOk(OutputPath, bPretty);
	}

	// ----- DeleteFunction ---------------------------------------------------
	int32 RunDeleteFunctionOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString FunctionName;
		FParse::Value(*Params, TEXT("Name="), FunctionName);

		if (AssetPath.IsEmpty() || FunctionName.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("DeleteFunction requires -Asset= -Name="));
			return 1;
		}

		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 4;
		UEdGraph* Graph = FindFunctionGraph(BP, FunctionName);
		if (!Graph)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("DeleteFunction: %s not found"), *FunctionName);
			return 4;
		}
		// EFlags=None purges the graph + any UFUNCTION the compiler emitted.
		FBlueprintEditorUtils::RemoveGraph(BP, Graph, EGraphRemoveFlags::None);

		if (!MaybeCompileAndSave(BP)) return 5;
		return EmitOk(OutputPath, bPretty);
	}

	// ----- SetVariableDefault -----------------------------------------------
	int32 RunSetVariableDefaultOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString VarName, NewDefault;
		FParse::Value(*Params, TEXT("Name="),    VarName);
		FParse::Value(*Params, TEXT("Default="), NewDefault);

		if (AssetPath.IsEmpty() || VarName.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("SetVariableDefault requires -Asset= -Name= [-Default=...]"));
			return 1;
		}

		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 4;
		const FName Var(*VarName);
		const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(BP, Var);
		if (Index == INDEX_NONE)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("SetVariableDefault: %s not found"), *VarName);
			return 4;
		}
		BP->NewVariables[Index].DefaultValue = NewDefault;

		if (!MaybeCompileAndSave(BP)) return 5;
		return EmitOk(OutputPath, bPretty);
	}

	// ----- RenameVariable ---------------------------------------------------
	int32 RunRenameVariableOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		const FString AssetPath = ResolveAssetPath(Params);
		FString OldName, NewName;
		FParse::Value(*Params, TEXT("OldName="), OldName);
		FParse::Value(*Params, TEXT("NewName="), NewName);

		if (AssetPath.IsEmpty() || OldName.IsEmpty() || NewName.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("RenameVariable requires -Asset= -OldName= -NewName="));
			return 1;
		}

		UBlueprint* BP = LoadMutableBlueprint(AssetPath);
		if (!BP) return 4;
		const FName Old(*OldName), New(*NewName);
		if (FBlueprintEditorUtils::FindNewVariableIndex(BP, Old) == INDEX_NONE)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("RenameVariable: %s not found"), *OldName);
			return 4;
		}
		if (FBlueprintEditorUtils::FindNewVariableIndex(BP, New) != INDEX_NONE)
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("RenameVariable: %s already exists"), *NewName);
			return 1;
		}
		FBlueprintEditorUtils::RenameMemberVariable(BP, Old, New);

		if (!MaybeCompileAndSave(BP)) return 5;
		return EmitOk(OutputPath, bPretty);
	}

	// List op via the asset registry. Faster than walking Content/ on disk +
	// LoadObject'ing every .uasset, and gets `parent_class` from asset tags
	// without paying the full BP load cost. Falls back to a disk walk if the
	// asset registry isn't available (e.g. very early commandlet startup
	// before module init is complete).
	int32 RunListOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString PathFilter;
		FParse::Value(*Params, TEXT("Path="), PathFilter);
		if (PathFilter.IsEmpty())
		{
			PathFilter = TEXT("/Game");
		}

		TArray<TSharedPtr<FJsonValue>> Out;

		FAssetRegistryModule& AssetRegistryModule =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AR = AssetRegistryModule.Get();
		// Path-scoped scan, not a full SearchAllAssets. The previous code
		// did `SearchAllAssets(bSync=true)` which forces the entire project's
		// asset registry to finalize and fires the global
		// OnAssetRegistryLoadComplete broadcast. That broadcast invokes
		// every plugin's load handler — and one bad handler (e.g. Niagara
		// loading a NiagaraDataChannel asset whose post-load constructor
		// crashes in some projects) takes the whole commandlet down with
		// exit=3 and a long callstack ending in our RunListOp.
		//
		// ScanPathsSynchronous populates only the requested path's metadata,
		// doesn't invoke the global completion broadcast, and doesn't load
		// asset payloads (only header tags). bForceRescan=false → cheap no-op
		// when the path's already in the registry from earlier startup.
		AR.ScanPathsSynchronous({PathFilter}, /*bForceRescan=*/false);

		FARFilter Filter;
		Filter.bRecursivePaths = true;
		Filter.PackagePaths.Add(*PathFilter);
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());

		TArray<FAssetData> Assets;
		AR.GetAssets(Filter, Assets);

		Out.Reserve(Assets.Num());
		for (const FAssetData& Data : Assets)
		{
			FString ParentClass;
			Data.GetTagValue(TEXT("ParentClass"), ParentClass);
			if (ParentClass.IsEmpty())
			{
				// Older asset tags use NativeParentClass; try that as a fallback.
				Data.GetTagValue(TEXT("NativeParentClass"), ParentClass);
			}
			// UE 5's asset-registry tag value is a FStringClassReference text
			// form: `/Script/CoreUObject.Class'/Script/Engine.Actor'`. Strip
			// the wrapping `Class'...'` so the wire shape matches what
			// read_blueprint emits via Blueprint->ParentClass->GetPathName().
			{
				int32 OpenQuote = INDEX_NONE, CloseQuote = INDEX_NONE;
				if (ParentClass.FindChar(TEXT('\''), OpenQuote) &&
				    ParentClass.FindLastChar(TEXT('\''), CloseQuote) &&
				    CloseQuote > OpenQuote)
				{
					ParentClass = ParentClass.Mid(OpenQuote + 1, CloseQuote - OpenQuote - 1);
				}
			}

			const FString PackagePath = Data.PackageName.ToString();
			const FString FileOnDisk = FPackageName::LongPackageNameToFilename(
				PackagePath, FPackageName::GetAssetPackageExtension());
			const FString Modified = IsoDateForFile(FileOnDisk);

			TSharedRef<FJsonObject> Summary = FBlueprintReaderWireJson::SummaryToJson(
				PackagePath, Data.AssetName.ToString(), ParentClass, Modified);
			Out.Add(MakeShared<FJsonValueObject>(Summary));
		}

		Out.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
		{
			return A->AsObject()->GetStringField(TEXT("asset_path")) <
			       B->AsObject()->GetStringField(TEXT("asset_path"));
		});

		const FString Json = FBlueprintReaderWireJson::WriteArrayString(Out, bPretty);
		return EmitJson(Json, OutputPath);
	}
}

namespace
{
	// Shared dispatch — used by both the one-shot Main path and the daemon
	// loop. Treats `Params` as a commandlet-arg-style string and runs a
	// single op against it.
	int32 RunOneOp(const FString& Params);

	// Daemon mode: read newline-terminated commandlet-arg strings from stdin,
	// run each through RunOneOp, print a sentinel after each so the MCP
	// backend knows the result file is ready.
	int32 RunDaemon();
}

int32 UBlueprintReaderCommandlet::Main(const FString& Params)
{
	if (FParse::Param(*Params, TEXT("Daemon")))
	{
		return RunDaemon();
	}
	return RunOneOp(Params);
}

namespace
{
int32 RunOneOp(const FString& Params)
{
	EOp Op;
	if (!ParseOp(Params, Op)) return 1;

	const bool bPretty = !FParse::Param(*Params, TEXT("Compact"));
	const FString OutputPath = ResolveOutputPath(Params);

	if (Op == EOp::List)
	{
		return RunListOp(Params, OutputPath, bPretty);
	}
	if (Op == EOp::AddVariable)     return RunAddVariableOp(Params, OutputPath, bPretty);
	if (Op == EOp::SetNodePosition) return RunSetNodePositionOp(Params, OutputPath, bPretty);
	if (Op == EOp::DeleteNode)      return RunDeleteNodeOp(Params, OutputPath, bPretty);
	if (Op == EOp::AddNode)         return RunAddNodeOp(Params, OutputPath, bPretty);
	if (Op == EOp::WirePins)        return RunWirePinsOp(Params, OutputPath, bPretty);
	if (Op == EOp::DeleteVariable)     return RunDeleteVariableOp(Params, OutputPath, bPretty);
	if (Op == EOp::RenameVariable)     return RunRenameVariableOp(Params, OutputPath, bPretty);
	if (Op == EOp::AddFunction)        return RunAddFunctionOp(Params, OutputPath, bPretty);
	if (Op == EOp::AddFunctionInput)   return RunAddFunctionInputOp(Params, OutputPath, bPretty);
	if (Op == EOp::AddFunctionOutput)  return RunAddFunctionOutputOp(Params, OutputPath, bPretty);
	if (Op == EOp::DeleteFunction)     return RunDeleteFunctionOp(Params, OutputPath, bPretty);
	if (Op == EOp::SetVariableDefault) return RunSetVariableDefaultOp(Params, OutputPath, bPretty);
	if (Op == EOp::BeginBatch)         return RunBeginBatchOp(Params, OutputPath, bPretty);
	if (Op == EOp::EndBatch)           return RunEndBatchOp(Params, OutputPath, bPretty);
	if (Op == EOp::CreateBlueprint)    return RunCreateBlueprintOp(Params, OutputPath, bPretty);
	if (Op == EOp::SetPinDefault)      return RunSetPinDefaultOp(Params, OutputPath, bPretty);
	if (Op == EOp::RetypeVariable)     return RunRetypeVariableOp(Params, OutputPath, bPretty);
	if (Op == EOp::SetVariableCategory)return RunSetVariableCategoryOp(Params, OutputPath, bPretty);
	if (Op == EOp::DuplicateBlueprint) return RunDuplicateBlueprintOp(Params, OutputPath, bPretty);
	if (Op == EOp::WriteGeneratedSource) return RunWriteGeneratedSourceOp(Params, OutputPath, bPretty);

	const FString AssetPath = ResolveAssetPath(Params);
	if (AssetPath.IsEmpty())
	{
		UE_LOG(LogBlueprintReader, Error, TEXT("Missing required argument: -Asset=<asset path> (or -Path=)"));
		return 1;
	}

	if (Op == EOp::Legacy)
	{
		// Backward-compatible original behavior — emit the rich
		// FBlueprintReaderJson shape (camelCase, K2 extras).
		const TOptional<FBlueprintInfo> Info = FBlueprintIntrospector::Read(AssetPath);
		if (!Info.IsSet())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("Failed to load blueprint at: %s"), *AssetPath);
			return 2;
		}
		const FString Json = FBlueprintReaderJson::ToString(*Info, bPretty);
		return EmitJson(Json, OutputPath);
	}

	const TOptional<FBlueprintInfo> Info = FBlueprintIntrospector::Read(AssetPath);
	if (!Info.IsSet())
	{
		UE_LOG(LogBlueprintReader, Error, TEXT("Failed to load blueprint at: %s"), *AssetPath);
		return 2;
	}

	switch (Op)
	{
	case EOp::Read:
	{
		auto Obj = FBlueprintReaderWireJson::MetadataToJson(*Info);
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj, bPretty), OutputPath);
	}
	case EOp::Graph:
	{
		FString GraphName;
		FParse::Value(*Params, TEXT("Graph="), GraphName);
		if (GraphName.IsEmpty())
		{
			GraphName = TEXT("EventGraph");
		}
		auto Obj = FBlueprintReaderWireJson::GraphToJson(*Info, GraphName);
		if (!Obj.IsValid())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("Graph '%s' not found in %s"), *GraphName, *AssetPath);
			return 4;
		}
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj.ToSharedRef(), bPretty), OutputPath);
	}
	case EOp::Function:
	{
		FString FunctionName;
		FParse::Value(*Params, TEXT("Function="), FunctionName);
		if (FunctionName.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("Missing required argument: -Function=<name>"));
			return 1;
		}
		auto Obj = FBlueprintReaderWireJson::FunctionToJson(*Info, FunctionName);
		if (!Obj.IsValid())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("Function '%s' not found in %s"), *FunctionName, *AssetPath);
			return 4;
		}
		return EmitJson(FBlueprintReaderWireJson::WriteString(Obj.ToSharedRef(), bPretty), OutputPath);
	}
	case EOp::Variables:
	{
		auto Vars = FBlueprintReaderWireJson::VariablesToJson(*Info);
		return EmitJson(FBlueprintReaderWireJson::WriteArrayString(Vars, bPretty), OutputPath);
	}
	case EOp::Components:
	{
		auto Comps = FBlueprintReaderWireJson::ComponentsToJson(*Info);
		return EmitJson(FBlueprintReaderWireJson::WriteArrayString(Comps, bPretty), OutputPath);
	}
	case EOp::Find:
	{
		FString Query;
		FParse::Value(*Params, TEXT("Query="), Query);
		FString Kind;
		FParse::Value(*Params, TEXT("Kind="), Kind);
		// Permit empty Query when Kind is set (kind-only filter).
		if (Query.IsEmpty() && Kind.IsEmpty())
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("find_node requires -Query=<substring> and/or -Kind=<extra>"));
			return 1;
		}
		auto Nodes = FBlueprintReaderWireJson::FindNodesAsJson(*Info, Query, Kind);
		return EmitJson(FBlueprintReaderWireJson::WriteArrayString(Nodes, bPretty), OutputPath);
	}
	default:
		UE_LOG(LogBlueprintReader, Error, TEXT("Unsupported op"));
		return 1;
	}
}
} // anonymous namespace

// Live-server entry point. The TCP listener in BlueprintReaderLiveServer.cpp
// shells op-args through here on the game thread — same dispatch path the
// daemon uses for its stdin lines. Defined at file scope (not in the anon
// namespace) so the symbol has external linkage; the body uses unqualified
// name lookup to find RunOneOp in the anonymous namespace below.
namespace BlueprintReader
{
    // Forward decl of the anon-ns helper, hoisted to the same namespace
    // as our bridge so name lookup picks it up.
    int32 RunOneOpFromLiveServer(const FString& Params);
}
namespace
{
int32 RunOneOp(const FString& Params);  // forward decl from earlier defn
}
int32 BlueprintReader::RunOneOpFromLiveServer(const FString& Params)
{
    return RunOneOp(Params);
}

namespace
{
int32 RunDaemon()
{
	UE_LOG(LogBlueprintReader, Display,
		TEXT("BlueprintReader daemon started; awaiting commandlet-arg lines on stdin"));

	// Use Windows API directly for stdio so UE's runtime redirection (which
	// can route C stdio through its log/output system) doesn't intercept the
	// stream. We need raw bytes hitting the pipe in both directions.
	HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
	HANDLE hIn  = GetStdHandle(STD_INPUT_HANDLE);

	auto WriteAll = [hOut](const char* s, DWORD n)
	{
		while (n > 0)
		{
			DWORD wrote = 0;
			if (!WriteFile(hOut, s, n, &wrote, nullptr) || wrote == 0) return false;
			s += wrote;
			n -= wrote;
		}
		return true;
	};

	const char ready[] = "__BPR_READY__\n";
	WriteAll(ready, (DWORD)(sizeof(ready) - 1));

	// Read a single line (terminated by '\n') from hIn into Out. Returns
	// false on EOF / error.
	auto ReadLine = [hIn](FString& Out) -> bool
	{
		Out.Reset();
		char ch;
		while (true)
		{
			DWORD got = 0;
			if (!ReadFile(hIn, &ch, 1, &got, nullptr) || got == 0)
			{
				return !Out.IsEmpty();  // EOF: return what we have if any
			}
			if (ch == '\r') continue;
			if (ch == '\n') return true;
			Out.AppendChar(static_cast<TCHAR>(ch));
		}
	};

	while (true)
	{
		FString Line;
		if (!ReadLine(Line))
		{
			UE_LOG(LogBlueprintReader, Display, TEXT("Daemon: stdin closed; exiting"));
			return 0;
		}
		Line.TrimStartAndEndInline();
		if (Line.IsEmpty()) continue;
		if (Line.Equals(TEXT("QUIT"), ESearchCase::IgnoreCase))
		{
			UE_LOG(LogBlueprintReader, Display, TEXT("Daemon: QUIT received"));
			return 0;
		}

		UE_LOG(LogBlueprintReader, Display, TEXT("Daemon: received line: %s"), *Line);
		const int32 Code = RunOneOp(Line);
		const FString DoneStr = FString::Printf(TEXT("__BPR_DONE %d__\n"), Code);
		const auto DoneAnsi = StringCast<ANSICHAR>(*DoneStr);
		WriteAll(DoneAnsi.Get(), (DWORD)FCStringAnsi::Strlen(DoneAnsi.Get()));
	}
}
} // namespace
