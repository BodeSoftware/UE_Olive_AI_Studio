// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogOliveAIRuntime, Log, All);

/**
 * OliveAIRuntime Module
 *
 * Minimal runtime module containing only IR (Intermediate Representation) struct definitions.
 * These structs define the JSON format used for communication between the AI and the plugin.
 */
class FOliveAIRuntimeModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/**
	 * Singleton-like access to this module's interface.
	 *
	 * @return Returns singleton instance, loading the module on demand if needed.
	 */
	static FOliveAIRuntimeModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FOliveAIRuntimeModule>("OliveAIRuntime");
	}

	/**
	 * Checks to see if this module is loaded and ready.
	 *
	 * @return True if the module is loaded and ready to use.
	 */
	static bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("OliveAIRuntime");
	}
};
