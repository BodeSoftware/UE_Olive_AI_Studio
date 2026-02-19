// Copyright Bode Software. All Rights Reserved.

#include "OliveAIRuntimeModule.h"

DEFINE_LOG_CATEGORY(LogOliveAIRuntime);

#define LOCTEXT_NAMESPACE "FOliveAIRuntimeModule"

void FOliveAIRuntimeModule::StartupModule()
{
	UE_LOG(LogOliveAIRuntime, Log, TEXT("OliveAIRuntime module started"));
}

void FOliveAIRuntimeModule::ShutdownModule()
{
	UE_LOG(LogOliveAIRuntime, Log, TEXT("OliveAIRuntime module shutdown"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FOliveAIRuntimeModule, OliveAIRuntime)
