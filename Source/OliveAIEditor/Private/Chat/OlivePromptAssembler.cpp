// Copyright Bode Software. All Rights Reserved.

#include "Chat/OlivePromptAssembler.h"
#include "Index/OliveProjectIndex.h"
#include "Profiles/OliveFocusProfileManager.h"
#include "Settings/OliveAISettings.h"
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
	return AssembleSystemPromptInternal(BasePromptTemplate, FocusProfileName, ContextAssetPaths, MaxTokens);
}

FString FOlivePromptAssembler::AssembleSystemPromptWithBase(
	const FString& BasePromptOverride,
	const FString& FocusProfileName,
	const TArray<FString>& ContextAssetPaths,
	int32 MaxTokens)
{
	return AssembleSystemPromptInternal(BasePromptOverride, FocusProfileName, ContextAssetPaths, MaxTokens);
}

FString FOlivePromptAssembler::AssembleSystemPromptInternal(
	const FString& BasePrompt,
	const FString& FocusProfileName,
	const TArray<FString>& ContextAssetPaths,
	int32 MaxTokens)
{
	FString FullPrompt = SubstituteVariables(BasePrompt);

	// Add profile-specific guidance
	const FString ProfileAddition = GetProfilePromptAddition(FocusProfileName);
	if (!ProfileAddition.IsEmpty())
	{
		FullPrompt += TEXT("\n\n## Focus Mode\n");
		FullPrompt += ProfileAddition;
	}

	// Add project context
	const FString ProjectContext = GetProjectContext();
	if (!ProjectContext.IsEmpty())
	{
		FullPrompt += TEXT("\n\n## Project Context\n");
		FullPrompt += ProjectContext;
	}

	// Add policy context
	const FString PolicyContext = GetPolicyContext();
	if (!PolicyContext.IsEmpty())
	{
		FullPrompt += TEXT("\n\n## Project Policies\n");
		FullPrompt += PolicyContext;
	}

	// Calculate remaining tokens for asset context
	const int32 UsedTokens = EstimateTokenCount(FullPrompt);
	const int32 RemainingTokens = MaxTokens - UsedTokens - 100; // Reserve 100 tokens for safety

	// Add asset context if we have room
	if (RemainingTokens > 200 && ContextAssetPaths.Num() > 0)
	{
		const FString AssetContext = GetActiveContext(ContextAssetPaths, RemainingTokens);
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
	const FString* FilePrompt = ProfilePrompts.Find(ProfileName);
	if (FilePrompt && !FilePrompt->IsEmpty())
	{
		return *FilePrompt;
	}

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

FString FOlivePromptAssembler::GetPolicyContext() const
{
	const UOliveAISettings* Settings = UOliveAISettings::Get();
	if (!Settings)
	{
		return TEXT("");
	}

	FString Context;
	Context += FString::Printf(TEXT("- MaxVariablesPerBlueprint: %d\n"), Settings->MaxVariablesPerBlueprint);
	Context += FString::Printf(TEXT("- MaxNodesPerFunction: %d\n"), Settings->MaxNodesPerFunction);
	Context += FString::Printf(TEXT("- EnforceNamingConventions: %s\n"), Settings->bEnforceNamingConventions ? TEXT("true") : TEXT("false"));
	Context += FString::Printf(TEXT("- AutoCompileAfterWrite: %s\n"), Settings->bAutoCompileAfterWrite ? TEXT("true") : TEXT("false"));

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
	ProfilePrompts.Empty();

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

	// Optional profile-specific prompts
	struct FProfilePromptFile
	{
		FString ProfileName;
		FString FileName;
	};

	const TArray<FProfilePromptFile> PromptFiles = {
		{ TEXT("Blueprint"), TEXT("ProfileBlueprint.txt") },
		{ TEXT("AI & Behavior"), TEXT("ProfileAIBehavior.txt") },
		{ TEXT("Level & PCG"), TEXT("ProfileLevelPCG.txt") },
		{ TEXT("C++ & Blueprint"), TEXT("ProfileCppBlueprint.txt") }
	};

	for (const FProfilePromptFile& Entry : PromptFiles)
	{
		const FString ProfilePromptPath = FPaths::Combine(PluginDir, TEXT("Content/SystemPrompts"), Entry.FileName);
		if (!FPaths::FileExists(ProfilePromptPath))
		{
			continue;
		}

		FString FileContent;
		if (FFileHelper::LoadFileToString(FileContent, *ProfilePromptPath) && !FileContent.IsEmpty())
		{
			ProfilePrompts.Add(Entry.ProfileName, FileContent);
			UE_LOG(LogOliveAI, Verbose, TEXT("Loaded profile prompt: %s"), *Entry.ProfileName);
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
