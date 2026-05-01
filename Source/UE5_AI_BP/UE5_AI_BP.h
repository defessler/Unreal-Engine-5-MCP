#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FUE5_AI_BPModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
