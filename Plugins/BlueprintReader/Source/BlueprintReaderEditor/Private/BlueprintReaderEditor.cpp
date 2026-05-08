#include "BlueprintReaderEditor.h"
#include "BlueprintReaderLiveServer.h"
#include "Modules/ModuleManager.h"

void FBlueprintReaderEditorModule::StartupModule()
{
    // Start the live-mode TCP listener if BP_READER_LIVE_PORT is set.
    // No-op when the env var is unset → editor module behaves exactly as
    // before. The MCP server's commandlet daemon backend is unaffected
    // either way (live mode is opt-in via BP_READER_BACKEND=live on the
    // server side).
    BlueprintReader::GetLiveServer()->Start();
}

void FBlueprintReaderEditorModule::ShutdownModule()
{
    if (BlueprintReader::FLiveServer* S = BlueprintReader::GetLiveServer())
    {
        S->Stop();
    }
}

IMPLEMENT_MODULE(FBlueprintReaderEditorModule, BlueprintReaderEditor);
