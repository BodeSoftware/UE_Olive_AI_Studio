// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogOliveAI, Log, All);

/**
 * OliveAIEditor Module
 *
 * Editor-only module providing AI-powered development assistance for Unreal Engine.
 * Features:
 * - Built-in chat UI with streaming responses
 * - MCP server for external agent connections
 * - Project index for asset search and navigation
 * - Tool registry for Blueprint/BT/PCG operations
 *
 * Core subsystems are accessible via singletons:
 * - FOliveToolRegistry::Get()
 * - FOliveMCPServer::Get()
 * - FOliveProjectIndex::Get()
 * - FOlivePromptAssembler::Get()
 * - FOliveValidationEngine::Get()
 */
class OLIVEAIEDITOR_API FOliveAIEditorModule : public IModuleInterface
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
	static FOliveAIEditorModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FOliveAIEditorModule>("OliveAIEditor");
	}

	/**
	 * Checks to see if this module is loaded and ready.
	 *
	 * @return True if the module is loaded and ready to use.
	 */
	static bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("OliveAIEditor");
	}

private:
	/** Register UI elements (menus, tabs, toolbars) */
	void RegisterUI();

	/** Unregister UI elements */
	void UnregisterUI();

	/** Register editor commands */
	void RegisterCommands();

	/** Initialize subsystems after editor is ready */
	void OnPostEngineInit();

	/** Handle menu extension */
	void ExtendToolsMenu();

	/** Spawn the chat panel tab */
	TSharedRef<class SDockTab> SpawnChatTab(const class FSpawnTabArgs& Args);

	/** UI handles */
	TSharedPtr<class FUICommandList> PluginCommands;
	FDelegateHandle ToolMenuExtensionHandle;

	/** Tab identifiers */
	static const FName ChatTabId;
};
