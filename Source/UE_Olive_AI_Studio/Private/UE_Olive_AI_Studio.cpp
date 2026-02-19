// Copyright Epic Games, Inc. All Rights Reserved.

#include "UE_Olive_AI_Studio.h"

#define LOCTEXT_NAMESPACE "FUE_Olive_AI_StudioModule"

void FUE_Olive_AI_StudioModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
}

void FUE_Olive_AI_StudioModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FUE_Olive_AI_StudioModule, UE_Olive_AI_Studio)