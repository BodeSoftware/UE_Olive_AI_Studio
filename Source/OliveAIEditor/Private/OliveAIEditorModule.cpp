// Copyright Bode Software. All Rights Reserved.

#include "OliveAIEditorModule.h"
#include "OliveAIEditorCommands.h"
#include "Settings/OliveAISettings.h"
#include "MCP/OliveToolRegistry.h"
#include "MCP/OliveMCPServer.h"
#include "Index/OliveProjectIndex.h"
#include "Chat/OliveConversationManager.h"
#include "Chat/OlivePromptAssembler.h"
#include "Profiles/OliveFocusProfileManager.h"
#include "Brain/OliveToolPackManager.h"
#include "Services/OliveValidationEngine.h"
#include "Catalog/OliveNodeCatalog.h"
#include "MCP/OliveBlueprintToolHandlers.h"
#include "MCP/OliveBTToolHandlers.h"
#include "Catalog/OliveBTNodeCatalog.h"
#include "MCP/OlivePCGToolHandlers.h"
#include "Catalog/OlivePCGNodeCatalog.h"
#include "Utility/OlivePCGAvailability.h"
#include "MCP/OliveCppToolHandlers.h"
#include "MCP/OliveCrossSystemToolHandlers.h"
#include "OliveMCPPromptTemplates.h"
#include "UI/SOliveAIChatPanel.h"

#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "ToolMenus.h"
#include "LevelEditor.h"

DEFINE_LOG_CATEGORY(LogOliveAI);

#define LOCTEXT_NAMESPACE "FOliveAIEditorModule"

const FName FOliveAIEditorModule::ChatTabId = FName(TEXT("OliveAIChatTab"));

void FOliveAIEditorModule::StartupModule()
{
	UE_LOG(LogOliveAI, Log, TEXT("OliveAIEditor module starting..."));

	// Register editor commands
	RegisterCommands();

	// Register UI elements
	RegisterUI();

	// NOTE: This module loads at PostEngineInit phase (see .uplugin).
	// By this point, the engine is fully initialized, so we can run
	// subsystem initialization directly. No need to defer via delegate
	// (which would fail anyway since OnPostEngineInit has already fired).
	OnPostEngineInit();

	UE_LOG(LogOliveAI, Log, TEXT("OliveAIEditor module started"));
}

void FOliveAIEditorModule::ShutdownModule()
{
	UE_LOG(LogOliveAI, Log, TEXT("OliveAIEditor module shutting down..."));

	// Unregister UI
	UnregisterUI();

	// Unregister Cross-System tools
	FOliveCrossSystemToolHandlers::Get().UnregisterAllTools();

	// Unregister C++ tools
	FOliveCppToolHandlers::Get().UnregisterAllTools();

	// Unregister PCG tools
	if (FOlivePCGAvailability::IsPCGAvailable())
	{
		FOlivePCGToolHandlers::Get().UnregisterAllTools();
		FOlivePCGNodeCatalog::Get().Shutdown();
	}

	// Unregister BT/BB tools
	FOliveBTToolHandlers::Get().UnregisterAllTools();

	// Shutdown BT node catalog
	FOliveBTNodeCatalog::Get().Shutdown();

	// Unregister Blueprint tools
	FOliveBlueprintToolHandlers::Get().UnregisterAllTools();

	// Shutdown node catalog
	FOliveNodeCatalog::Get().Shutdown();

	// Stop MCP server (singleton)
	FOliveMCPServer::Get().Stop();

	// Shutdown project index (singleton)
	FOliveProjectIndex::Get().Shutdown();

	// Unregister commands
	FOliveAIEditorCommands::Unregister();

	UE_LOG(LogOliveAI, Log, TEXT("OliveAIEditor module shutdown complete"));
}

void FOliveAIEditorModule::RegisterCommands()
{
	FOliveAIEditorCommands::Register();
	PluginCommands = MakeShared<FUICommandList>();

	PluginCommands->MapAction(
		FOliveAIEditorCommands::Get().OpenChatPanel,
		FExecuteAction::CreateLambda([]()
		{
			FGlobalTabmanager::Get()->TryInvokeTab(ChatTabId);
		}),
		FCanExecuteAction()
	);
}

void FOliveAIEditorModule::RegisterUI()
{
	// Register nomad tab spawner for the chat panel
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		ChatTabId,
		FOnSpawnTab::CreateRaw(this, &FOliveAIEditorModule::SpawnChatTab)
	)
	.SetDisplayName(LOCTEXT("ChatTabTitle", "Olive AI Chat"))
	.SetTooltipText(LOCTEXT("ChatTabTooltip", "Open the Olive AI Chat panel"))
	.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
	.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"));

	// Extend Tools menu
	ExtendToolsMenu();
}

void FOliveAIEditorModule::UnregisterUI()
{
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(ChatTabId);

	// Remove menu section if it was added
	UToolMenus* ToolMenus = UToolMenus::Get();
	if (ToolMenus)
	{
		UToolMenu* ToolsMenu = ToolMenus->FindMenu("LevelEditor.MainMenu.Tools");
		if (ToolsMenu)
		{
			ToolsMenu->RemoveSection("OliveAI");
		}
	}
}

void FOliveAIEditorModule::ExtendToolsMenu()
{
	UToolMenus* ToolMenus = UToolMenus::Get();
	UToolMenu* ToolsMenu = ToolMenus->ExtendMenu("LevelEditor.MainMenu.Tools");

	if (ToolsMenu)
	{
		FToolMenuSection& Section = ToolsMenu->FindOrAddSection("OliveAI");
		Section.Label = LOCTEXT("OliveAISection", "Olive AI");

		Section.AddMenuEntryWithCommandList(
			FOliveAIEditorCommands::Get().OpenChatPanel,
			PluginCommands,
			LOCTEXT("OpenChatPanel", "Olive AI Chat"),
			LOCTEXT("OpenChatPanelTooltip", "Open the Olive AI Chat panel for AI-assisted development"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details")
		);
	}
}

TSharedRef<SDockTab> FOliveAIEditorModule::SpawnChatTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SOliveAIChatPanel)
		];
}

void FOliveAIEditorModule::OnPostEngineInit()
{
	UE_LOG(LogOliveAI, Log, TEXT("Post-engine initialization..."));

	// Register core validation rules
	FOliveValidationEngine::Get().RegisterCoreRules();

	// Initialize project index
	FOliveProjectIndex::Get().Initialize();

	// Register built-in tools
	FOliveToolRegistry::Get().RegisterBuiltInTools();

	// Initialize node catalog
	FOliveNodeCatalog::Get().Initialize();

	// Register Blueprint tools (replaces stubs with real handlers)
	FOliveBlueprintToolHandlers::Get().RegisterAllTools();

	// Initialize BT node catalog
	FOliveBTNodeCatalog::Get().Initialize();

	// Register BT/BB tools
	FOliveBTToolHandlers::Get().RegisterAllTools();

	// Register BT validation rules
	FOliveValidationEngine::Get().RegisterBehaviorTreeRules();

	// Register C++ validation rules
	FOliveValidationEngine::Get().RegisterCppRules();

	// Initialize PCG tools (guarded by plugin availability)
	if (FOlivePCGAvailability::IsPCGAvailable())
	{
		FOlivePCGNodeCatalog::Get().Initialize();
		FOlivePCGToolHandlers::Get().RegisterAllTools();
		UE_LOG(LogOliveAI, Log, TEXT("PCG tools registered"));
	}
	else
	{
		UE_LOG(LogOliveAI, Log, TEXT("PCG plugin not available, skipping PCG tools"));
	}

	// Register C++ tools
	FOliveCppToolHandlers::Get().RegisterAllTools();
	UE_LOG(LogOliveAI, Log, TEXT("C++ tools registered"));

	// Register Cross-System tools
	FOliveCrossSystemToolHandlers::Get().RegisterAllTools();
	UE_LOG(LogOliveAI, Log, TEXT("Cross-System tools registered"));

	// Register Cross-System validation rules
	FOliveValidationEngine::Get().RegisterCrossSystemRules();

	// Initialize focus profiles after tool registration so validation has complete category/tool coverage.
	FOliveFocusProfileManager::Get().Initialize();

	// Initialize tool packs after all tool handlers are registered.
	FOliveToolPackManager::Get().Initialize();

	// Initialize prompt assembler after focus profile registration.
	FOlivePromptAssembler::Get().Initialize();

	// Initialize prompt templates
	FOliveMCPPromptTemplates::Get().Initialize();

	// Start MCP server if configured
	const UOliveAISettings* Settings = UOliveAISettings::Get();
	if (Settings && Settings->bAutoStartMCPServer)
	{
		bool bStarted = FOliveMCPServer::Get().Start(Settings->MCPServerPort);
		if (bStarted)
		{
			UE_LOG(LogOliveAI, Log, TEXT("MCP server started on port %d"), FOliveMCPServer::Get().GetActualPort());
		}
		else
		{
			UE_LOG(LogOliveAI, Warning, TEXT("Failed to start MCP server on port %d"), Settings->MCPServerPort);
		}
	}

	UE_LOG(LogOliveAI, Log, TEXT("Post-engine initialization complete"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FOliveAIEditorModule, OliveAIEditor)
