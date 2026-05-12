#include "BlueprintReaderRuntime.h"

#include "Modules/ModuleManager.h"

IMPLEMENT_MODULE(FBlueprintReaderRuntimeModule, BlueprintReaderRuntime)

void FBlueprintReaderRuntimeModule::StartupModule()
{
	// Runtime introspector is stateless — nothing to register at startup.
	// The opt-in TCP listener is started on-demand by the runtime user
	// (game code, dev console command, automation script) since binding
	// a socket from every shipped game's startup is the wrong default.
}

void FBlueprintReaderRuntimeModule::ShutdownModule()
{
}
