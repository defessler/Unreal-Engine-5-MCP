#include "BlueprintReaderCommandlet.h"

#include "BlueprintIntrospector.h"
#include "BlueprintReaderJson.h"
#include "BlueprintReaderWireJson.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
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
		Find,
	};

	bool ParseOp(const FString& Params, EOp& OutOp)
	{
		FString OpStr;
		if (!FParse::Value(*Params, TEXT("Op="), OpStr))
		{
			OutOp = EOp::Legacy;
			return true;
		}
		if (OpStr.Equals(TEXT("List"), ESearchCase::IgnoreCase))      { OutOp = EOp::List; return true; }
		if (OpStr.Equals(TEXT("Read"), ESearchCase::IgnoreCase))      { OutOp = EOp::Read; return true; }
		if (OpStr.Equals(TEXT("Graph"), ESearchCase::IgnoreCase))     { OutOp = EOp::Graph; return true; }
		if (OpStr.Equals(TEXT("Function"), ESearchCase::IgnoreCase))  { OutOp = EOp::Function; return true; }
		if (OpStr.Equals(TEXT("Variables"), ESearchCase::IgnoreCase)) { OutOp = EOp::Variables; return true; }
		if (OpStr.Equals(TEXT("Find"), ESearchCase::IgnoreCase))      { OutOp = EOp::Find; return true; }
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

	// List op without AssetRegistry: walk the project's Content tree on disk,
	// load each .uasset as UBlueprint, emit a summary if it loaded. Slower
	// than an AssetRegistry query, but doesn't require a Build.cs dep change.
	int32 RunListOp(const FString& Params, const FString& OutputPath, bool bPretty)
	{
		FString PathFilter;
		FParse::Value(*Params, TEXT("Path="), PathFilter);
		if (PathFilter.IsEmpty())
		{
			PathFilter = TEXT("/Game");
		}

		FString FilesystemDir;
		if (!FPackageName::TryConvertLongPackageNameToFilename(PathFilter / TEXT(""), FilesystemDir))
		{
			UE_LOG(LogBlueprintReader, Error, TEXT("Cannot resolve package path: %s"), *PathFilter);
			return 4;
		}
		// Strip trailing slash + suffix to make IFileManager happy.
		FPaths::NormalizeDirectoryName(FilesystemDir);

		IFileManager& FM = IFileManager::Get();
		TArray<FString> UAssetPaths;
		FM.FindFilesRecursive(UAssetPaths, *FilesystemDir, TEXT("*.uasset"), /*bFiles=*/true, /*bDirs=*/false);

		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(UAssetPaths.Num());
		for (const FString& File : UAssetPaths)
		{
			FString PackageName;
			if (!FPackageName::TryConvertFilenameToLongPackageName(File, PackageName))
			{
				continue;
			}

			const TOptional<FBlueprintInfo> Info = FBlueprintIntrospector::Read(PackageName);
			if (!Info.IsSet()) continue;

			const FString Modified = IsoDateForFile(File);
			TSharedRef<FJsonObject> Summary = FBlueprintReaderWireJson::SummaryToJson(
				Info->Path, Info->Name, Info->ParentClassPath, Modified);
			Out.Add(MakeShared<FJsonValueObject>(Summary));
		}

		Out.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
		{
			return A->AsObject()->GetStringField(TEXT("asset_path")) < B->AsObject()->GetStringField(TEXT("asset_path"));
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
