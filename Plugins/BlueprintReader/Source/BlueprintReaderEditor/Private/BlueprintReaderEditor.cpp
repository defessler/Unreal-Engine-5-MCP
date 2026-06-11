#include "BlueprintReaderEditor.h"
#include "BlueprintReaderLiveServer.h"
#include "BlueprintReaderLogSink.h"
#include "BlueprintReaderModalChannel.h"  // TEST-2 P1a: modal side-channel
#include "Containers/Ticker.h"            // UX-P4a: FTSTicker game-thread heartbeat
#include "CoreGlobals.h"                  // TEST-2 P1a: GIsRunningUnattendedScript gate
#include "HAL/PlatformMisc.h"             // TEST-2 P1a: GetEnvironmentVariable
#include "Modules/ModuleManager.h"

// UX-P4a: handle for the game-thread heartbeat ticker, removed on shutdown.
static FTSTicker::FDelegateHandle GHeartbeatTickerHandle;

void FBlueprintReaderEditorModule::StartupModule()
{
	// Start the log-sink ring buffer first so we don't miss any messages
	// emitted during the live-server bring-up. Cost is one FOutputDevice
	// registered on GLog + a small ring buffer (default 1024 entries).
	BlueprintReader::StartLogSink();

	// Start the live-mode TCP listener. No-op when BP_READER_LIVE_DISABLED
	// is set → editor module behaves exactly as before. The MCP server's
	// commandlet daemon backend is unaffected either way.
	BlueprintReader::GetLiveServer()->Start();

	// UX-P4a: advance the game-thread heartbeat ~10x/sec so the live health
	// channel can tell an idle-but-alive editor (fresh heartbeat) from a wedged
	// one (stale). The core ticker is pumped on the GAME THREAD by the editor's
	// main loop (and by the daemon's explicit Tick() pump), so this fires even
	// with zero MCP connections — and CANNOT fire while the game thread is halted,
	// which is exactly the "paused" signal the worker-thread probe reads. Return
	// true to stay registered.
	GHeartbeatTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([](float /*DeltaTime*/) -> bool
		{
			BlueprintReader::BumpGameThreadHeartbeat();
			// TEST-2 P1a: lazily hook the modal-loop drainer once Slate is up
			// (a single bool check after the first success), and service the
			// modal side-channel queue here for the idle (no-modal) case — the
			// modal-loop delegate covers the wedged case.
			BlueprintReader::RegisterModalTickIfNeeded();
			BlueprintReader::DrainModalCommands();
			return true;
		}),
		0.1f);
	// Seed an initial value so a probe in the first 100ms reads "fresh", not -1.
	BlueprintReader::BumpGameThreadHeartbeat();

	// TEST-2 P1a prevention gate (opt-in): BP_READER_GUI_AUTOMATION=1 sets
	// GIsRunningUnattendedScript persistently so AddModalWindow auto-cancels
	// non-slow-task modals (SlateApplication.cpp:2128) — they never block an
	// automated editor. This is the complement to the recovery side-channel;
	// it is opt-in because it also suppresses dialogs a co-working human might
	// want to answer.
	const FString GuiAutomation =
		FPlatformMisc::GetEnvironmentVariable(TEXT("BP_READER_GUI_AUTOMATION"));
	if (GuiAutomation == TEXT("1") || GuiAutomation.Equals(TEXT("true"), ESearchCase::IgnoreCase))
	{
		GIsRunningUnattendedScript = true;
		UE_LOG(LogTemp, Display,
			TEXT("[BlueprintReader] BP_READER_GUI_AUTOMATION=1 — GIsRunningUnattendedScript set; modal dialogs will auto-cancel."));
	}
}

void FBlueprintReaderEditorModule::ShutdownModule()
{
	if (GHeartbeatTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GHeartbeatTickerHandle);
		GHeartbeatTickerHandle.Reset();
	}
	// TEST-2 P1a: drop the OnModalLoopTickEvent registration.
	BlueprintReader::UnregisterModalTick();
	if (BlueprintReader::FLiveServer* S = BlueprintReader::GetLiveServer())
	{
		S->Stop();
	}
	BlueprintReader::StopLogSink();
}

IMPLEMENT_MODULE(FBlueprintReaderEditorModule, BlueprintReaderEditor);
