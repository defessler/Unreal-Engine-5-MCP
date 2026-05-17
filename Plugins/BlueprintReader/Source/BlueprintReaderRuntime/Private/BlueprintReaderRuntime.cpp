#include "BlueprintReaderRuntime.h"

#include "CoreGlobals.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"

namespace BlueprintReaderRuntime
{
	// Declared in BlueprintReaderRuntimeServer.cpp — module-scoped hooks
	// that read the opt-in CVar / env var and conditionally bind the
	// listener. Default behavior is no-op (listener stays off).
	extern void OnPostEngineInit_StartRuntimeListener();
	extern void OnModuleShutdown_StopRuntimeListener();
}    // namespace BlueprintReaderRuntime

IMPLEMENT_MODULE(FBlueprintReaderRuntimeModule, BlueprintReaderRuntime)

void FBlueprintReaderRuntimeModule::StartupModule()
{
	// Defer the listener-start check until PostEngineInit — by that
	// point the AssetRegistry module is loaded and CVars from
	// DefaultGame.ini have been applied. Trying to bind earlier than
	// that risks failing the env-var check and starting the listener
	// without registry-backed asset enumeration available.
	FCoreDelegates::OnPostEngineInit.AddStatic(
		&BlueprintReaderRuntime::OnPostEngineInit_StartRuntimeListener);
}

void FBlueprintReaderRuntimeModule::ShutdownModule()
{
	// Tear down any active listener + connection threads + handshake
	// file. Idempotent — safe to call if the listener never started.
	BlueprintReaderRuntime::OnModuleShutdown_StopRuntimeListener();
}
