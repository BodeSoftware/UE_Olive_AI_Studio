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

	// Add layer decision policy for profiles that include both C++ and BP
	if (FocusProfileName == TEXT("Auto") || FocusProfileName == TEXT("C++ & Blueprint"))
	{
		const FString LayerPolicy = GetLayerDecisionPolicy();
		if (!LayerPolicy.IsEmpty())
		{
			FullPrompt += TEXT("\n\n");
			FullPrompt += LayerPolicy;
		}
	}

	// Add capability knowledge packs (modular, profile-scoped).
	const FString CapabilityKnowledge = GetCapabilityKnowledge(FocusProfileName);
	if (!CapabilityKnowledge.IsEmpty())
	{
		FullPrompt += TEXT("\n\n## Capability Knowledge\n");
		FullPrompt += CapabilityKnowledge;
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
			int32 SkippedCount = AssetPaths.Num() - AssetPaths.IndexOfByKey(Path);
			int32 RemainingTokens = MaxTokens - CurrentTokens;
			UE_LOG(LogOliveAI, Warning, TEXT("Context truncation: skipped %d assets due to token budget (%d tokens remaining)"), SkippedCount, RemainingTokens);
			Context += FString::Printf(TEXT("(+%d more assets not shown due to context limit)\n"), SkippedCount);
			break;
		}

		Context += AssetLine;
		CurrentTokens += LineTokens;
	}

	return Context;
}

FString FOlivePromptAssembler::GetLayerDecisionPolicy() const
{
	return TEXT(R"(## Implementation Layer Decision Policy

When both C++ and Blueprint can satisfy a request, follow this decision process:

1. **Check what already exists first.**
   - Use `cpp.read_class` to check if a C++ class already provides the functionality.
   - Use `blueprint.read` to check if a Blueprint already implements the behavior.
   - If it exists in one layer, extend it there. Do not duplicate across layers.

2. **Default layer preferences:**
   - **Blueprint** for: gameplay logic, prototyping, designer-tweakable behavior, event responses, UI logic, AI behavior trees.
   - **C++** for: performance-critical systems, base classes, engine-level functionality, reusable framework code, math-heavy operations.

3. **Respect explicit `preferred_layer` parameter:**
   - If `preferred_layer` is `"cpp"`, implement in C++ only. Do not create Blueprint assets.
   - If `preferred_layer` is `"blueprint"`, implement in Blueprint only. Do not create C++ files.
   - If `preferred_layer` is `"auto"` or not specified, use the default preferences above.

4. **Never create duplicates.** If functionality exists in C++, do not recreate it in Blueprint, and vice versa.

5. **Report your decision.** When choosing a layer, briefly explain why in your response (e.g., "Implementing in Blueprint because this is gameplay logic that designers may want to tweak").
)");
}

FString FOlivePromptAssembler::GetCapabilityKnowledge(const FString& ProfileName) const
{
	FString NormalizedProfile = FOliveFocusProfileManager::Get().NormalizeProfileName(ProfileName);
	const TArray<FString>* PackIds = ProfileCapabilityPackIds.Find(NormalizedProfile);
	if (!PackIds)
	{
		PackIds = ProfileCapabilityPackIds.Find(TEXT("Auto"));
	}
	if (!PackIds)
	{
		return TEXT("");
	}

	FString Combined;
	for (const FString& PackId : *PackIds)
	{
		const FString* PackText = CapabilityKnowledgePacks.Find(PackId);
		if (!PackText || PackText->IsEmpty())
		{
			continue;
		}

		if (!Combined.IsEmpty())
		{
			Combined += TEXT("\n\n");
		}
		Combined += *PackText;
	}

	return Combined;
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
	CapabilityKnowledgePacks.Empty();
	ProfileCapabilityPackIds.Empty();

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

	// Capability knowledge packs are modular text files under Content/SystemPrompts/Knowledge.
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		const FString KnowledgeDir = FPaths::Combine(PluginDir, TEXT("Content/SystemPrompts/Knowledge"));

		if (PlatformFile.DirectoryExists(*KnowledgeDir))
		{
			PlatformFile.IterateDirectory(*KnowledgeDir, [this](const TCHAR* FilenameOrDirectory, bool bIsDirectory)
			{
				if (bIsDirectory)
				{
					return true;
				}

				FString FilePath(FilenameOrDirectory);
				if (!FilePath.EndsWith(TEXT(".txt")))
				{
					return true;
				}

				const FString PackId = FPaths::GetBaseFilename(FilePath).ToLower();
				FString PackText;
				if (FFileHelper::LoadFileToString(PackText, *FilePath) && !PackText.IsEmpty())
				{
					CapabilityKnowledgePacks.Add(PackId, PackText);
					UE_LOG(LogOliveAI, Log, TEXT("Loaded capability knowledge pack: %s"), *PackId);
				}
				return true;
			});
		}
	}

	// Profile -> capability pack mapping. Add packs here without changing assembly flow.
	ProfileCapabilityPackIds.Add(TEXT("Auto"), { TEXT("blueprint_authoring") });
	ProfileCapabilityPackIds.Add(TEXT("Blueprint"), { TEXT("blueprint_authoring") });
	ProfileCapabilityPackIds.Add(TEXT("C++"), {});

	// Optional profile-specific prompts sourced from the profile manager.
	const TArray<FOliveFocusProfile> Profiles = FOliveFocusProfileManager::Get().GetAllProfiles();
	for (const FOliveFocusProfile& Profile : Profiles)
	{
		if (Profile.PromptTemplateFile.IsEmpty())
		{
			continue;
		}

		const FString ProfilePromptPath = FPaths::Combine(PluginDir, TEXT("Content/SystemPrompts"), Profile.PromptTemplateFile);
		if (!FPaths::FileExists(ProfilePromptPath))
		{
			continue;
		}

		FString FileContent;
		if (FFileHelper::LoadFileToString(FileContent, *ProfilePromptPath) && !FileContent.IsEmpty())
		{
			ProfilePrompts.Add(Profile.Name, FileContent);
			UE_LOG(LogOliveAI, Verbose, TEXT("Loaded profile prompt: %s"), *Profile.Name);
		}
	}

	// Load base rules for workers
	{
		const FString BaseRulesPath = FPaths::Combine(PluginDir, TEXT("Content/SystemPrompts/Base.txt"));
		if (FPaths::FileExists(BaseRulesPath))
		{
			FFileHelper::LoadFileToString(BaseRulesText, *BaseRulesPath);
			UE_LOG(LogOliveAI, Log, TEXT("Loaded base rules from Base.txt"));
		}
	}

	// Load worker domain templates
	{
		const TArray<TPair<FString, FString>> DomainFiles = {
			{TEXT("blueprint"), TEXT("Worker_Blueprint.txt")},
			{TEXT("behaviortree"), TEXT("Worker_BehaviorTree.txt")},
			{TEXT("pcg"), TEXT("Worker_PCG.txt")},
			{TEXT("cpp"), TEXT("Worker_Cpp.txt")},
			{TEXT("integration"), TEXT("Worker_Integration.txt")}
		};

		for (const auto& Pair : DomainFiles)
		{
			const FString FilePath = FPaths::Combine(PluginDir, TEXT("Content/SystemPrompts"), Pair.Value);
			if (FPaths::FileExists(FilePath))
			{
				FString Content;
				if (FFileHelper::LoadFileToString(Content, *FilePath))
				{
					WorkerTemplates.Add(Pair.Key, Content);
					UE_LOG(LogOliveAI, Log, TEXT("Loaded worker template: %s"), *Pair.Key);
				}
			}
		}
	}
}

FString FOlivePromptAssembler::AssembleWorkerPrompt(
	const FString& WorkerDomain,
	const FString& TaskDescription,
	const FString& PreviousStepContext,
	const FString& ProjectRules)
{
	// Find domain template
	const FString* Template = WorkerTemplates.Find(WorkerDomain.ToLower());
	FString Result;

	if (Template && !Template->IsEmpty())
	{
		Result = *Template;
	}
	else
	{
		// Fallback: generic worker prompt
		Result = FString::Printf(
			TEXT("You are a %s specialist for Unreal Engine 5.5.\n\n"
				 "## Your Task\n{TASK_DESCRIPTION}\n\n"
				 "## Context From Previous Steps\n{PREVIOUS_STEP_CONTEXT}\n\n"
				 "## Project Rules\n{PROJECT_RULES}\n\n"
				 "{BASE_RULES}"),
			*WorkerDomain);

		UE_LOG(LogOliveAI, Warning, TEXT("PromptAssembler: No template for domain '%s', using fallback"), *WorkerDomain);
	}

	// Substitute variables
	Result = Result.Replace(TEXT("{TASK_DESCRIPTION}"),
		TaskDescription.IsEmpty() ? TEXT("(no task specified)") : *TaskDescription);

	Result = Result.Replace(TEXT("{PREVIOUS_STEP_CONTEXT}"),
		PreviousStepContext.IsEmpty() ? TEXT("(first step — no previous context)") : *PreviousStepContext);

	Result = Result.Replace(TEXT("{PROJECT_RULES}"),
		ProjectRules.IsEmpty() ? TEXT("(no project-specific rules)") : *ProjectRules);

	Result = Result.Replace(TEXT("{BASE_RULES}"),
		BaseRulesText.IsEmpty() ? TEXT("") : *BaseRulesText);

	// Also substitute engine/project variables
	Result = SubstituteVariables(Result);

	return Result;
}

FString FOlivePromptAssembler::GetProjectRules() const
{
	const UOliveAISettings* Settings = UOliveAISettings::Get();
	if (!Settings)
	{
		return TEXT("");
	}

	FString Rules;

	if (Settings->bEnforceNamingConventions)
	{
		Rules += TEXT("- Enforce UE naming conventions: BP_ prefix for Blueprints, BT_ for Behavior Trees\n");
	}

	if (Settings->MaxVariablesPerBlueprint > 0)
	{
		Rules += FString::Printf(TEXT("- Maximum %d variables per Blueprint\n"), Settings->MaxVariablesPerBlueprint);
	}

	if (Settings->MaxNodesPerFunction > 0)
	{
		Rules += FString::Printf(TEXT("- Maximum %d nodes per function graph\n"), Settings->MaxNodesPerFunction);
	}

	return Rules;
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
