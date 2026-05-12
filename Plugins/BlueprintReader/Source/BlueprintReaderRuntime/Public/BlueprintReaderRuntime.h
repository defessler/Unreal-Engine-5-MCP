// BlueprintReaderRuntime — module entry point.
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FBlueprintReaderRuntimeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
