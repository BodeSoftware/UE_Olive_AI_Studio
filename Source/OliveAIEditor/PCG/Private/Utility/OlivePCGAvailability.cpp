// Copyright Bode Software. All Rights Reserved.

#include "OlivePCGAvailability.h"
#include "Modules/ModuleManager.h"

bool FOlivePCGAvailability::IsPCGAvailable()
{
	return FModuleManager::Get().IsModuleLoaded(TEXT("PCG"));
}
