// Project Settings UI for the BlueprintReader MCP plugin.
//
// Lives at: Edit -> Project Settings -> Plugins -> BlueprintReader MCP.
// Persists to: <Project>/Saved/Config/<Platform>/EditorPerProjectUserSettings.ini
//
// The MCP server is OUT-OF-PROCESS (a separate exe) and reads its own
// configuration from env vars at startup. To bridge: when settings
// change, the editor writes a snapshot to
// <Project>/Saved/bp-reader-settings.json. The MCP server consults
// that file (in addition to env vars) at startup; env vars still win.
//
// Settings are deliberately a small set — the env-var surface is the
// primary configuration path for headless / CI usage. This UI just
// exposes the everyday knobs to in-editor users who don't want to set
// environment variables.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "BlueprintReaderSettings.generated.h"

UCLASS(config = EditorPerProjectUserSettings, defaultconfig, meta = (DisplayName = "BlueprintReader MCP"))
class BLUEPRINTREADEREDITOR_API UBlueprintReaderSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	// ---- Live server (in-editor TCP) ---------------------------------------

	/** TCP port the in-editor live server publishes for the out-of-process
	 *  MCP server to connect to. 0 = auto-pick. Mirrors BP_READER_LIVE_PORT. */
	UPROPERTY(EditAnywhere, Config, Category = "Live Server", meta = (ClampMin = 0, ClampMax = 65535))
	int32 LiveServerPort = 0;

	/** When true, the live server starts automatically with the editor.
	 *  When false, start manually via the bpr.live_server.start console
	 *  command (or leave the MCP server in commandlet mode). */
	UPROPERTY(EditAnywhere, Config, Category = "Live Server")
	bool bAutoStartLiveServer = true;

	// ---- Tool surface ------------------------------------------------------

	/** Lazy tool discovery mode. When true, the MCP server's tools/list
	 *  returns only list_toolsets / describe_toolset / call_tool /
	 *  shutdown_daemon. Agents discover the full surface on demand via
	 *  the meta-tools. Mirrors BP_READER_TOOL_SEARCH. */
	UPROPERTY(EditAnywhere, Config, Category = "Tool Surface")
	bool bEnableToolSearch = false;

	/** Allow-list patterns. Each pattern is either a tool name
	 *  (read_blueprint), a category name (core, cpp), `all`, or a regex
	 *  wrapped in /.../ (e.g. /^read_.*$/). Empty = all tools active.
	 *  Mirrors BP_READER_TOOLS. */
	UPROPERTY(EditAnywhere, Config, Category = "Tool Surface")
	TArray<FString> ToolAllowList;

	/** Deny-list patterns. Same vocabulary as ToolAllowList. Applied
	 *  after the allow-list. Useful for opting OUT of categories
	 *  (e.g. cook for builds you never want to trigger from chat).
	 *  Mirrors BP_READER_TOOLS_EXCLUDE. */
	UPROPERTY(EditAnywhere, Config, Category = "Tool Surface")
	TArray<FString> ToolDenyList;

	/** When true, expose the 6 BP <-> C++ transpile tools
	 *  (decompile_function, transpile_function, parse_cpp_function, etc.).
	 *  Off by default; the pipeline is rich but produces a large schema
	 *  surface. Mirrors BP_READER_ALLOW_TRANSPILE. */
	UPROPERTY(EditAnywhere, Config, Category = "Tool Surface")
	bool bAllowTranspile = false;

	// ---- Telemetry ---------------------------------------------------------

	/** When true, include per-call _meta in tool results (tool name +
	 *  elapsed_ms). Useful for cost / latency observability. Privacy-
	 *  conscious users can opt out. */
	UPROPERTY(EditAnywhere, Config, Category = "Telemetry")
	bool bEmitToolMeta = true;

	// ---- UDeveloperSettings overrides -------------------------------------

	/** Settings category in Project Settings UI. */
	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }

#if WITH_EDITOR
	virtual FText GetSectionText() const override;
	virtual FText GetSectionDescription() const override;
#endif
};
