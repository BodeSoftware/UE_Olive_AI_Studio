// Copyright Bode Software. All Rights Reserved.

#include "OliveAIEditorModule.h"
#include "OliveAIEditorCommands.h"
#include "Settings/OliveAISettings.h"
#include "MCP/OliveToolRegistry.h"
#include "MCP/OliveMCPServer.h"
#include "Index/OliveProjectIndex.h"
#include "Chat/OliveConversationManager.h"
#include "Chat/OlivePromptAssembler.h"
#include "Services/OliveValidationEngine.h"
#include "Services/OliveUtilityModel.h"
#include "Catalog/OliveNodeCatalog.h"
#include "MCP/OliveBlueprintToolHandlers.h"
#include "MCP/OliveBTToolHandlers.h"
#include "Catalog/OliveBTNodeCatalog.h"
#include "MCP/OlivePCGToolHandlers.h"
#include "Catalog/OlivePCGNodeCatalog.h"
#include "Utility/OlivePCGAvailability.h"
#if OLIVE_WITH_NIAGARA
#include "MCP/OliveNiagaraToolHandlers.h"
#include "Catalog/OliveNiagaraModuleCatalog.h"
#include "Utility/OliveNiagaraAvailability.h"
#endif
#include "MCP/OliveCppToolHandlers.h"
#include "MCP/OliveCrossSystemToolHandlers.h"
#include "MCP/OlivePythonToolHandlers.h"
#include "MCP/OliveBuildTool.h"
#include "Template/OliveTemplateSystem.h"
#include "OliveMCPPromptTemplates.h"
#include "Chat/OliveEditorChatSession.h"
#include "UI/SOliveAIChatPanel.h"
#include "UI/SOliveClaudeCodePanel.h"

#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "ToolMenus.h"
#include "LevelEditor.h"

DEFINE_LOG_CATEGORY(LogOliveAI);

#define LOCTEXT_NAMESPACE "FOliveAIEditorModule"

const FName FOliveAIEditorModule::ChatTabId = FName(TEXT("OliveAIChatTab"));
const FName FOliveAIEditorModule::ClaudeCodeTabId = FName(TEXT("OliveClaudeCodeTab"));

void FOliveAIEditorModule::StartupModule()
{
	UE_LOG(LogOliveAI, Log, TEXT("OliveAIEditor module starting..."));

	// Register editor commands
	RegisterCommands();

	// Register UI elements
	RegisterUI();

	// Initialize the editor chat session singleton (owns ConversationManager)
	FOliveEditorChatSession::Get().Initialize();

	// NOTE: This module loads at PostEngineInit phase (see .uplugin).
	// By this point, the engine is fully initialized, so we can run
	// subsystem initialization directly. No need to defer via delegate
	// (which would fail anyway since OnPostEngineInit has already fired).
	OnPostEngineInit();

	UE_LOG(LogOliveAI, Log, TEXT("OliveAIEditor module started"));

	// ---- Debug / Test console commands ----

	// Olive.TestPresearch <message>
	// Tests the full pre-search pipeline: keyword extraction -> template search -> format
	IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Olive.TestPresearch"),
		TEXT("Test utility model keyword extraction + template pre-search. Usage: Olive.TestPresearch create a bow and arrow system"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			FString TestMessage;
			for (const FString& Arg : Args)
			{
				if (!TestMessage.IsEmpty()) TestMessage += TEXT(" ");
				TestMessage += Arg;
			}
			if (TestMessage.IsEmpty())
			{
				TestMessage = TEXT("create a bow and arrow system for my character");
			}

			UE_LOG(LogOliveAI, Warning, TEXT("===== PRE-SEARCH TEST ====="));
			UE_LOG(LogOliveAI, Warning, TEXT("Input: %s"), *TestMessage);

			// Step 1: Keyword extraction
			UE_LOG(LogOliveAI, Warning, TEXT("--- Step 1: Keyword Extraction ---"));
			UE_LOG(LogOliveAI, Warning, TEXT("Utility model available: %s"), FOliveUtilityModel::IsAvailable() ? TEXT("YES") : TEXT("NO"));

			TArray<FString> Keywords = FOliveUtilityModel::ExtractSearchKeywords(TestMessage, 12);
			FString KeywordStr = FString::Join(Keywords, TEXT(", "));
			UE_LOG(LogOliveAI, Warning, TEXT("Extracted %d keywords: [%s]"), Keywords.Num(), *KeywordStr);

			// Step 2: Template search
			UE_LOG(LogOliveAI, Warning, TEXT("--- Step 2: Template Search ---"));
			FString SearchQuery = FString::Join(Keywords, TEXT(" "));
			TArray<TSharedPtr<FJsonObject>> Results = FOliveTemplateSystem::Get().SearchTemplates(SearchQuery, 8);
			UE_LOG(LogOliveAI, Warning, TEXT("Found %d matching templates"), Results.Num());

			for (int32 i = 0; i < Results.Num(); ++i)
			{
				FString Id, Desc, Proj;
				Results[i]->TryGetStringField(TEXT("template_id"), Id);
				Results[i]->TryGetStringField(TEXT("catalog_description"), Desc);
				Results[i]->TryGetStringField(TEXT("source_project"), Proj);

				// Truncate description for readability
				if (Desc.Len() > 80)
				{
					Desc = Desc.Left(80) + TEXT("...");
				}
				UE_LOG(LogOliveAI, Warning, TEXT("  [%d] %s (%s): %s"), i, *Id, *Proj, *Desc);
			}

			// Step 3: Settings check
			UE_LOG(LogOliveAI, Warning, TEXT("--- Settings ---"));
			const UOliveAISettings* S = UOliveAISettings::Get();
			if (S)
			{
				UE_LOG(LogOliveAI, Warning, TEXT("  LLM expansion enabled: %s"), S->bEnableLLMKeywordExpansion ? TEXT("YES") : TEXT("NO"));
				UE_LOG(LogOliveAI, Warning, TEXT("  Utility provider: %d"), (int32)S->UtilityModelProvider);
				UE_LOG(LogOliveAI, Warning, TEXT("  Utility model: %s"), *S->UtilityModelId);
				UE_LOG(LogOliveAI, Warning, TEXT("  Has templates: %s"), FOliveTemplateSystem::Get().HasTemplates() ? TEXT("YES") : TEXT("NO"));
				UE_LOG(LogOliveAI, Warning, TEXT("  Catalog block length: %d chars"), FOliveTemplateSystem::Get().GetCatalogBlock().Len());
			}

			UE_LOG(LogOliveAI, Warning, TEXT("===== PRE-SEARCH TEST COMPLETE ====="));
		}),
		ECVF_Default
	);
}

void FOliveAIEditorModule::ShutdownModule()
{
	UE_LOG(LogOliveAI, Log, TEXT("OliveAIEditor module shutting down..."));

	// Unregister UI
	UnregisterUI();

	// Unregister Python tools
	FOlivePythonToolHandlers::Get().UnregisterAllTools();

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

#if OLIVE_WITH_NIAGARA
	// Unregister Niagara tools
	if (FOliveNiagaraAvailability::IsNiagaraAvailable())
	{
		FOliveNiagaraToolHandlers::Get().UnregisterAllTools();
		FOliveNiagaraModuleCatalog::Get().Shutdown();
	}
#endif

	// Unregister BT/BB tools
	FOliveBTToolHandlers::Get().UnregisterAllTools();

	// Shutdown BT node catalog
	FOliveBTNodeCatalog::Get().Shutdown();

	// Shutdown template system
	FOliveTemplateSystem::Get().Shutdown();

	// Unregister Blueprint tools
	FOliveBlueprintToolHandlers::Get().UnregisterAllTools();

	// Shutdown node catalog
	FOliveNodeCatalog::Get().Shutdown();

	// Stop MCP server (singleton)
	FOliveMCPServer::Get().Stop();

	// Shutdown project index (singleton)
	FOliveProjectIndex::Get().Shutdown();

	// Shutdown editor chat session (releases ConversationManager, queue, retry manager)
	FOliveEditorChatSession::Get().Shutdown();

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

	// Register nomad tab spawner for the Claude Code companion panel
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		ClaudeCodeTabId,
		FOnSpawnTab::CreateRaw(this, &FOliveAIEditorModule::SpawnClaudeCodeTab)
	)
	.SetDisplayName(LOCTEXT("ClaudeCodeTabTitle", "Olive AI -- Claude Code"))
	.SetTooltipText(LOCTEXT("ClaudeCodeTabTooltip", "Open the Claude Code integration panel (MCP status, setup, activity feed)"))
	.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
	.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Viewports"));

	// Extend Tools menu
	ExtendToolsMenu();
}

void FOliveAIEditorModule::UnregisterUI()
{
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(ChatTabId);
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(ClaudeCodeTabId);

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

		Section.AddEntry(FToolMenuEntry::InitMenuEntry(
			"OpenClaudeCodePanel",
			LOCTEXT("OpenClaudeCodePanel", "Olive AI -- Claude Code"),
			LOCTEXT("OpenClaudeCodePanelTooltip", "Open the Claude Code integration panel (MCP status, setup, activity feed)"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Viewports"),
			FUIAction(FExecuteAction::CreateLambda([]()
			{
				FGlobalTabmanager::Get()->TryInvokeTab(ClaudeCodeTabId);
			}))
		));
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

TSharedRef<SDockTab> FOliveAIEditorModule::SpawnClaudeCodeTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		.Label(LOCTEXT("ClaudeCodeTabLabel", "Claude Code"))
		[
			SNew(SOliveClaudeCodePanel)
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

	// Register olive.build batch executor (after built-in tools, before domain-specific tools)
	FOliveBuildTool::RegisterTool();

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

#if OLIVE_WITH_NIAGARA
	// Initialize Niagara tools (guarded by plugin availability)
	if (FOliveNiagaraAvailability::IsNiagaraAvailable())
	{
		FOliveNiagaraModuleCatalog::Get().Initialize();
		FOliveNiagaraToolHandlers::Get().RegisterAllTools();
		UE_LOG(LogOliveAI, Log, TEXT("Niagara tools registered"));
	}
	else
	{
		UE_LOG(LogOliveAI, Log, TEXT("Niagara plugin not available, skipping Niagara tools"));
	}
#endif

	// Register C++ tools
	FOliveCppToolHandlers::Get().RegisterAllTools();
	UE_LOG(LogOliveAI, Log, TEXT("C++ tools registered"));

	// Register Cross-System tools
	FOliveCrossSystemToolHandlers::Get().RegisterAllTools();
	UE_LOG(LogOliveAI, Log, TEXT("Cross-System tools registered"));

	// Register Python tools
	FOlivePythonToolHandlers::Get().RegisterAllTools();
	UE_LOG(LogOliveAI, Log, TEXT("Python tools registered"));

	// Register Cross-System validation rules
	FOliveValidationEngine::Get().RegisterCrossSystemRules();

	// Initialize template system (scans Content/Templates/ for JSON templates)
	FOliveTemplateSystem::Get().Initialize();

	// Initialize prompt assembler after tool registration.
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
