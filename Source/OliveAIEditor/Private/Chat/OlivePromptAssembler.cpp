// Copyright Bode Software. All Rights Reserved.

#include "Chat/OlivePromptAssembler.h"
#include "Index/OliveProjectIndex.h"
#include "Profiles/OliveFocusProfileManager.h"
#include "OliveAIEditorModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "Interfaces/IPluginManager.h"

FOlivePromptAssembler& FOlivePromptAssembler::Get()
{
	static FOlivePromptAssembler Instance;
	return Instance;
}

void FOlivePromptAssembler::Initialize()
{
	LoadPromptTemplates();
	UE_LOG(LogOliveAI, Log, TEXT("Prompt Assembler initialized"));
}

// ==========================================
// Prompt Assembly
// ==========================================

FString FOlivePromptAssembler::AssembleSystemPrompt(
	const FString& FocusProfileName,
	const TArray<FString>& ContextAssetPaths,
	int32 MaxTokens)
{
	FString FullPrompt;

	// Start with base prompt
	FullPrompt = SubstituteVariables(BasePromptTemplate);

	// Add profile-specific guidance
	FString ProfileAddition = GetProfilePromptAddition(FocusProfileName);
	if (!ProfileAddition.IsEmpty())
	{
		FullPrompt += TEXT("\n\n## Focus Mode\n");
		FullPrompt += ProfileAddition;
	}

	// Add project context
	FString ProjectContext = GetProjectContext();
	if (!ProjectContext.IsEmpty())
	{
		FullPrompt += TEXT("\n\n## Project Context\n");
		FullPrompt += ProjectContext;
	}

	// Calculate remaining tokens for asset context
	int32 UsedTokens = EstimateTokenCount(FullPrompt);
	int32 RemainingTokens = MaxTokens - UsedTokens - 100; // Reserve 100 tokens for safety

	// Add asset context if we have room
	if (RemainingTokens > 200 && ContextAssetPaths.Num() > 0)
	{
		FString AssetContext = GetActiveContext(ContextAssetPaths, RemainingTokens);
		if (!AssetContext.IsEmpty())
		{
			FullPrompt += TEXT("\n\n## Active Context Assets\n");
			FullPrompt += AssetContext;
		}
	}

	return FullPrompt;
}

// ==========================================
// Components
// ==========================================

FString FOlivePromptAssembler::GetProfilePromptAddition(const FString& ProfileName) const
{
	return FOliveFocusProfileManager::Get().GetSystemPromptAddition(ProfileName);
}

FString FOlivePromptAssembler::GetProjectContext() const
{
	if (!FOliveProjectIndex::Get().IsReady())
	{
		return TEXT("");
	}

	const FOliveProjectConfig& Config = FOliveProjectIndex::Get().GetProjectConfig();

	FString Context;
	Context += FString::Printf(TEXT("- Project: %s\n"), *Config.ProjectName);
	Context += FString::Printf(TEXT("- Engine: %s\n"), *Config.EngineVersion);

	if (Config.EnabledPlugins.Num() > 0)
	{
		Context += TEXT("- Key Plugins: ");
		int32 Count = FMath::Min(5, Config.EnabledPlugins.Num());
		for (int32 i = 0; i < Count; ++i)
		{
			if (i > 0) Context += TEXT(", ");
			Context += Config.EnabledPlugins[i];
		}
		if (Config.EnabledPlugins.Num() > 5)
		{
			Context += FString::Printf(TEXT(" (+%d more)"), Config.EnabledPlugins.Num() - 5);
		}
		Context += TEXT("\n");
	}

	Context += FString::Printf(TEXT("- Assets: %d indexed\n"), FOliveProjectIndex::Get().GetAssetCount());

	return Context;
}

FString FOlivePromptAssembler::GetActiveContext(const TArray<FString>& AssetPaths, int32 MaxTokens) const
{
	if (AssetPaths.Num() == 0)
	{
		return TEXT("");
	}

	FString Context;
	int32 CurrentTokens = 0;

	for (const FString& Path : AssetPaths)
	{
		TOptional<FOliveAssetInfo> AssetInfo = FOliveProjectIndex::Get().GetAssetByPath(Path);

		FString AssetLine;
		if (AssetInfo.IsSet())
		{
			AssetLine = FString::Printf(TEXT("- **%s** (%s)\n  Path: %s\n"),
				*AssetInfo->Name,
				*AssetInfo->AssetClass.ToString(),
				*Path);

			// Add parent class if Blueprint
			if (AssetInfo->bIsBlueprint && !AssetInfo->ParentClass.IsNone())
			{
				AssetLine += FString::Printf(TEXT("  Parent: %s\n"), *AssetInfo->ParentClass.ToString());
			}

			// Add interfaces
			if (AssetInfo->Interfaces.Num() > 0)
			{
				AssetLine += TEXT("  Interfaces: ");
				AssetLine += FString::Join(AssetInfo->Interfaces, TEXT(", "));
				AssetLine += TEXT("\n");
			}
		}
		else
		{
			AssetLine = FString::Printf(TEXT("- %s\n"), *Path);
		}

		int32 LineTokens = EstimateTokenCount(AssetLine);
		if (CurrentTokens + LineTokens > MaxTokens)
		{
			Context += FString::Printf(TEXT("(+%d more assets not shown due to context limit)\n"),
				AssetPaths.Num() - AssetPaths.IndexOfByKey(Path));
			break;
		}

		Context += AssetLine;
		CurrentTokens += LineTokens;
	}

	return Context;
}

// ==========================================
// Token Estimation
// ==========================================

int32 FOlivePromptAssembler::EstimateTokenCount(const FString& Text) const
{
	// Rough approximation: ~4 characters per token for English
	// This is conservative; actual tokenization varies by model
	return FMath::CeilToInt(Text.Len() / CharsPerToken);
}

// ==========================================
// Template Management
// ==========================================

void FOlivePromptAssembler::SetBasePrompt(const FString& Prompt)
{
	BasePromptTemplate = Prompt;
}

void FOlivePromptAssembler::ReloadTemplates()
{
	LoadPromptTemplates();
}

// ==========================================
// Private Methods
// ==========================================

void FOlivePromptAssembler::LoadPromptTemplates()
{
	// Set default base prompt (can be overridden from file)
	BasePromptTemplate = TEXT(R"(You are Olive AI, an expert AI assistant for Unreal Engine development integrated directly into the editor.

## Your Capabilities
- Read and understand any Blueprint, Behavior Tree, PCG graph, or C++ class in the project
- Search the project index to find assets by name, type, or class hierarchy
- Understand class hierarchies and asset dependencies
- Create and modify Blueprints (when implemented in Phase 1+)

## How to Use Tools
1. **Always read before writing.** Before modifying an asset, use the read tool to understand its current state.
2. **Search first.** Use project.search to find assets before attempting to read them.
3. **One operation at a time.** Chain operations logically but don't skip steps.
4. **Self-correct on errors.** If an operation fails, analyze the error and attempt fixes.

## Response Guidelines
- Be concise. Focus on the task at hand.
- For complex tasks, briefly outline your plan before executing.
- If uncertain, ask clarifying questions rather than guessing.
- Report results clearly: what was found/created/modified, any issues encountered.

## Safety Rules
- Never modify assets not mentioned in the user's request without asking.
- Warn before destructive operations (deletion, reparenting).
- If Play in Editor is active, read operations work but write operations will fail.

## Current Context
Engine Version: {ENGINE_VERSION}
Project Name: {PROJECT_NAME}
)");

	// Try to load from file
	FString PluginDir = FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("UE_Olive_AI_Studio"));
	FString PromptFile = FPaths::Combine(PluginDir, TEXT("Content/SystemPrompts/BaseSystemPrompt.txt"));

	if (FPaths::FileExists(PromptFile))
	{
		FString FileContent;
		if (FFileHelper::LoadFileToString(FileContent, *PromptFile))
		{
			BasePromptTemplate = FileContent;
			UE_LOG(LogOliveAI, Log, TEXT("Loaded base system prompt from file"));
		}
	}
}

FString FOlivePromptAssembler::SubstituteVariables(const FString& Template) const
{
	FString Result = Template;

	// Substitute project variables
	if (FOliveProjectIndex::Get().IsReady())
	{
		const FOliveProjectConfig& Config = FOliveProjectIndex::Get().GetProjectConfig();
		Result = Result.Replace(TEXT("{ENGINE_VERSION}"), *Config.EngineVersion);
		Result = Result.Replace(TEXT("{PROJECT_NAME}"), *Config.ProjectName);
	}
	else
	{
		Result = Result.Replace(TEXT("{ENGINE_VERSION}"), TEXT("Unknown"));
		Result = Result.Replace(TEXT("{PROJECT_NAME}"), TEXT("Unknown"));
	}

	return Result;
}
