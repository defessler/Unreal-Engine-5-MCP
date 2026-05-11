#include "BlueprintReaderEditor.h"
#include "BlueprintReaderLiveServer.h"
#include "BlueprintReaderLogSink.h"
#include "Modules/ModuleManager.h"

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
}

void FBlueprintReaderEditorModule::ShutdownModule()
{
    if (BlueprintReader::FLiveServer* S = BlueprintReader::GetLiveServer())
    {
        S->Stop();
    }
    BlueprintReader::StopLogSink();
}

IMPLEMENT_MODULE(FBlueprintReaderEditorModule, BlueprintReaderEditor);
