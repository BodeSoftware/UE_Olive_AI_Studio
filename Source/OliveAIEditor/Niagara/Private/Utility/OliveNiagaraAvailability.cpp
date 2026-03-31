// Copyright Bode Software. All Rights Reserved.

#include "OliveNiagaraAvailability.h"
#include "Modules/ModuleManager.h"

bool FOliveNiagaraAvailability::IsNiagaraAvailable()
{
	return FModuleManager::Get().IsModuleLoaded(TEXT("NiagaraEditor"));
}
