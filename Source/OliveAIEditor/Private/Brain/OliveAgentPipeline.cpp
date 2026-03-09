// Copyright Bode Software. All Rights Reserved.

/**
 * OliveAgentPipeline.cpp -- Agent pipeline core implementation.
 *
 * Runs a chain of lightweight LLM sub-agents (Router -> Scout -> Researcher
 * -> Architect -> Validator) to produce structured context for the Builder.
 * Each agent uses SendAgentCompletion() with role-specific model resolution
 * and a tick-pump blocking pattern identical to FOliveUtilityModel.
 */

#include "Brain/OliveAgentPipeline.h"
#include "OliveAIEditorModule.h"
#include "Settings/OliveAISettings.h"
#include "Providers/IOliveAIProvider.h"
#include "Providers/OliveClaudeCodeProvider.h"
#include "Services/OliveUtilityModel.h"
#include "Index/OliveProjectIndex.h"
#include "Template/OliveTemplateSystem.h"
#include "Reader/OliveBlueprintReader.h"
#include "IR/BlueprintIR.h"
#include "MCP/OliveMCPServer.h"
#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Components/ActorComponent.h"
#include "UObject/UObjectGlobals.h"
#include "Writer/OliveClassAPIHelper.h"
#include "Interfaces/IPluginManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogOliveAgentPipeline, Log, All);

// ---------------------------------------------------------------------------
// Anonymous namespace helpers
// ---------------------------------------------------------------------------

namespace
{

// Forward declaration -- defined later in file after all class methods
FString BuildAssetStateSummary(const TArray<FString>& AssetPaths);

/**
 * Load a knowledge pack file from the plugin's Content/SystemPrompts/Knowledge/ directory.
 * Returns empty string on failure (caller logs the warning).
 */
FString LoadKnowledgePack(const FString& Filename)
{
	// Resolve plugin content directory
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UE_Olive_AI_Studio"));
	if (!Plugin.IsValid())
	{
		UE_LOG(LogOliveAgentPipeline, Warning,
			TEXT("LoadKnowledgePack: Plugin 'UE_Olive_AI_Studio' not found"));
		return FString();
	}

	const FString PluginContentDir = FPaths::Combine(
		Plugin->GetBaseDir(),
		TEXT("Content/SystemPrompts/Knowledge"));

	const FString FilePath = FPaths::Combine(PluginContentDir, Filename);

	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *FilePath))
	{
		UE_LOG(LogOliveAgentPipeline, Warning,
			TEXT("Failed to load knowledge pack: %s"), *FilePath);
		return FString();
	}
	return Content;
}

/**
 * Load blueprint_design_patterns.txt but only sections 0-3.
 * Truncates at the "## 4." header (section 4+ is execution-level detail, not planning).
 */
FString LoadDesignPatternsSections0To3()
{
	FString Full = LoadKnowledgePack(TEXT("blueprint_design_patterns.txt"));
	if (Full.IsEmpty()) return Full;

	// Find the "## 4." section header and truncate
	int32 Section4Pos = Full.Find(TEXT("\n## 4."), ESearchCase::CaseSensitive);
	if (Section4Pos != INDEX_NONE)
	{
		Full.LeftInline(Section4Pos);
	}
	return Full;
}

/**
 * Build the knowledge injection block for the Planner (used by both RunPlanner and RunPlannerWithTools).
 * Loads events_vs_functions.txt and blueprint_design_patterns.txt (sections 0-3) from disk.
 * Falls back to a minimal inline knowledge block if files cannot be loaded.
 * Always appends the compact plan_json ops reference.
 */
FString BuildPlannerKnowledgeBlock()
{
	FString Block;
	Block.Reserve(12288);

	FString EventsVsFunctions = LoadKnowledgePack(TEXT("events_vs_functions.txt"));
	FString DesignPatterns = LoadDesignPatternsSections0To3();

	if (!EventsVsFunctions.IsEmpty() || !DesignPatterns.IsEmpty())
	{
		Block += TEXT("\n## Blueprint Architecture Knowledge\n\n");

		if (!EventsVsFunctions.IsEmpty())
		{
			// Strip the TAGS: header line if present (not useful for the Planner)
			int32 TagsEnd = EventsVsFunctions.Find(TEXT("\n---\n"));
			if (TagsEnd != INDEX_NONE)
			{
				EventsVsFunctions.RightChopInline(TagsEnd + 5);
			}
			Block += EventsVsFunctions;
			Block += TEXT("\n");
		}

		if (!DesignPatterns.IsEmpty())
		{
			Block += DesignPatterns;
			Block += TEXT("\n");
		}
	}
	else
	{
		// Fallback: if files couldn't be loaded, keep minimal inline knowledge
		Block += TEXT("\n## Blueprint Architecture Knowledge\n\n");
		Block += TEXT("- USE A FUNCTION when: returns a value AND logic is synchronous\n");
		Block += TEXT("- USE AN EVENT when: logic spans multiple frames OR no return value needed\n");
		Block += TEXT("- Functions CANNOT contain Timeline, Delay, or latent actions\n");
		Block += TEXT("- Interface: no outputs = implementable event, has outputs = synchronous function\n");
	}

	// Keep compact ops reference inline (not in the knowledge pack files)
	Block += TEXT("\n### Plan JSON Ops Reference\n");
	Block += TEXT("- Dispatchers: use `call_delegate` to fire, `bind_dispatcher` to bind, `call_dispatcher` to call\n");
	Block += TEXT("- Overlap events: `{\"op\":\"event\",\"target\":\"OnComponentBeginOverlap\",\"properties\":{\"component_name\":\"CompName\"}}`\n");
	Block += TEXT("- Interface calls: use `target_class` with the interface name\n");
	Block += TEXT("- Enhanced Input: `{\"op\":\"event\",\"target\":\"IA_ActionName\"}`\n");
	Block += TEXT("- Timeline nodes require EventGraph -- use Custom Events, not functions\n");
	Block += TEXT("- plan_json creates NEW nodes only -- cannot reference existing nodes by ID\n");
	Block += TEXT("- For binding to dispatchers, use granular add_node (K2Node_AssignDelegate), not plan_json\n\n");

	return Block;
}

/** Convert provider enum to string name for FOliveProviderFactory::CreateProvider(). */
FString ProviderEnumToName(EOliveAIProvider Provider)
{
	switch (Provider)
	{
	case EOliveAIProvider::ClaudeCode:        return TEXT("ClaudeCode");
	case EOliveAIProvider::Codex:             return TEXT("Codex");
	case EOliveAIProvider::OpenRouter:        return TEXT("OpenRouter");
	case EOliveAIProvider::ZAI:              return TEXT("ZAI");
	case EOliveAIProvider::Anthropic:         return TEXT("Anthropic");
	case EOliveAIProvider::OpenAI:            return TEXT("OpenAI");
	case EOliveAIProvider::Google:            return TEXT("Google");
	case EOliveAIProvider::Ollama:            return TEXT("Ollama");
	case EOliveAIProvider::OpenAICompatible:  return TEXT("OpenAICompatible");
	default:                                  return TEXT("Unknown");
	}
}

/** Convert agent role enum to a human-readable string for logging. */
FString RoleToString(EOliveAgentRole Role)
{
	switch (Role)
	{
	case EOliveAgentRole::Router:     return TEXT("Router");
	case EOliveAgentRole::Scout:      return TEXT("Scout");
	case EOliveAgentRole::Researcher: return TEXT("Researcher");
	case EOliveAgentRole::Architect:  return TEXT("Architect");
	case EOliveAgentRole::Reviewer:   return TEXT("Reviewer");
	default:                          return TEXT("Unknown");
	}
}

/** Total pipeline timeout in seconds. If exceeded, remaining stages are skipped. */
static constexpr double PIPELINE_TOTAL_TIMEOUT = 300.0;

/** Maximum number of existing assets to return from project index search. */
static constexpr int32 MAX_PROJECT_SEARCH_RESULTS = 20;

/** Maximum number of relevant assets the Scout LLM should rank. */
static constexpr int32 MAX_SCOUT_RANKED_ASSETS = 5;

/** Maximum number of assets the Researcher loads for analysis. */
static constexpr int32 MAX_RESEARCHER_ASSETS = 3;

/** Maximum modified assets reported by BuildAssetStateSummary. */
static constexpr int32 MAX_REVIEWER_ASSETS = 8;

/** Class alias table for the Validator. Maps short names to full UE class names. */
const TMap<FString, FString>& GetClassAliasMap()
{
	static const TMap<FString, FString> Aliases = {
		// Actors
		{ TEXT("Actor"),                TEXT("AActor") },
		{ TEXT("Pawn"),                 TEXT("APawn") },
		{ TEXT("Character"),            TEXT("ACharacter") },
		{ TEXT("PlayerController"),     TEXT("APlayerController") },
		{ TEXT("GameModeBase"),         TEXT("AGameModeBase") },
		{ TEXT("GameMode"),             TEXT("AGameMode") },
		{ TEXT("GameStateBase"),        TEXT("AGameStateBase") },
		{ TEXT("GameState"),            TEXT("AGameState") },
		{ TEXT("PlayerState"),          TEXT("APlayerState") },
		{ TEXT("HUD"),                  TEXT("AHUD") },
		{ TEXT("Info"),                 TEXT("AInfo") },
		// Components
		{ TEXT("ActorComponent"),                   TEXT("UActorComponent") },
		{ TEXT("SceneComponent"),                   TEXT("USceneComponent") },
		{ TEXT("SphereComponent"),                  TEXT("USphereComponent") },
		{ TEXT("BoxComponent"),                     TEXT("UBoxComponent") },
		{ TEXT("CapsuleComponent"),                 TEXT("UCapsuleComponent") },
		{ TEXT("StaticMeshComponent"),              TEXT("UStaticMeshComponent") },
		{ TEXT("SkeletalMeshComponent"),            TEXT("USkeletalMeshComponent") },
		{ TEXT("ProjectileMovementComponent"),      TEXT("UProjectileMovementComponent") },
		{ TEXT("FloatingPawnMovement"),             TEXT("UFloatingPawnMovement") },
		{ TEXT("FloatingPawnMovementComponent"),    TEXT("UFloatingPawnMovementComponent") },
		{ TEXT("CharacterMovementComponent"),       TEXT("UCharacterMovementComponent") },
		{ TEXT("ArrowComponent"),                   TEXT("UArrowComponent") },
		{ TEXT("WidgetComponent"),                  TEXT("UWidgetComponent") },
		{ TEXT("AudioComponent"),                   TEXT("UAudioComponent") },
		{ TEXT("ParticleSystemComponent"),          TEXT("UParticleSystemComponent") },
		{ TEXT("NiagaraComponent"),                 TEXT("UNiagaraComponent") },
		{ TEXT("PointLightComponent"),              TEXT("UPointLightComponent") },
		{ TEXT("SpotLightComponent"),               TEXT("USpotLightComponent") },
		{ TEXT("DirectionalLightComponent"),        TEXT("UDirectionalLightComponent") },
		{ TEXT("BillboardComponent"),               TEXT("UBillboardComponent") },
		{ TEXT("SpringArmComponent"),               TEXT("USpringArmComponent") },
		{ TEXT("CameraComponent"),                  TEXT("UCameraComponent") },
		{ TEXT("ChildActorComponent"),              TEXT("UChildActorComponent") },
		{ TEXT("SplineComponent"),                  TEXT("USplineComponent") },
		{ TEXT("TextRenderComponent"),              TEXT("UTextRenderComponent") },
		{ TEXT("DecalComponent"),                   TEXT("UDecalComponent") },
		// Other common short names
		{ TEXT("UserWidget"),          TEXT("UUserWidget") },
		{ TEXT("Object"),              TEXT("UObject") },
		{ TEXT("BlueprintFunctionLibrary"), TEXT("UBlueprintFunctionLibrary") },
		{ TEXT("Interface"),           TEXT("UInterface") },
		{ TEXT("AnimInstance"),         TEXT("UAnimInstance") },
		{ TEXT("DataAsset"),           TEXT("UDataAsset") },
	};
	return Aliases;
}

/**
 * Send a single-shot blocking LLM completion via an HTTP provider.
 * Mirrors the pattern from FOliveUtilityModel::TrySendCompletion().
 *
 * @param ProviderName    Provider factory name (e.g., "OpenRouter", "Anthropic")
 * @param ModelId         Model identifier to use
 * @param ApiKey          API key for the provider
 * @param BaseUrl         Base URL for the provider endpoint
 * @param Temperature     Temperature for generation
 * @param MaxTokens       Maximum tokens in response
 * @param Timeout         Timeout in seconds
 * @param SystemPrompt    System-level instructions
 * @param UserPrompt      User prompt content
 * @param OutResponse     Populated with the response text on success
 * @param OutError        Populated with error details on failure
 * @return true if a response was obtained
 */
bool TrySendViaProvider(
	const FString& ProviderName,
	const FString& ModelId,
	const FString& ApiKey,
	const FString& BaseUrl,
	float Temperature,
	int32 MaxTokens,
	float Timeout,
	const FString& SystemPrompt,
	const FString& UserPrompt,
	FString& OutResponse,
	FString& OutError)
{
	TSharedPtr<IOliveAIProvider> Provider = FOliveProviderFactory::CreateProvider(ProviderName);
	if (!Provider.IsValid())
	{
		OutError = FString::Printf(TEXT("Failed to create provider: %s"), *ProviderName);
		return false;
	}

	// Configure the provider
	FOliveProviderConfig Config;
	Config.ProviderName = ProviderName;
	Config.ApiKey = ApiKey;
	Config.ModelId = ModelId;
	Config.BaseUrl = BaseUrl;
	Config.Temperature = Temperature;
	Config.MaxTokens = MaxTokens;
	Config.TimeoutSeconds = FMath::CeilToInt32(Timeout);
	Provider->Configure(Config);

	// Build messages array: system + user
	TArray<FOliveChatMessage> Messages;

	if (!SystemPrompt.IsEmpty())
	{
		FOliveChatMessage SystemMsg;
		SystemMsg.Role = EOliveChatRole::System;
		SystemMsg.Content = SystemPrompt;
		SystemMsg.Timestamp = FDateTime::Now();
		Messages.Add(MoveTemp(SystemMsg));
	}

	{
		FOliveChatMessage UserMsg;
		UserMsg.Role = EOliveChatRole::User;
		UserMsg.Content = UserPrompt;
		UserMsg.Timestamp = FDateTime::Now();
		Messages.Add(MoveTemp(UserMsg));
	}

	FOliveRequestOptions Options;
	Options.MaxTokens = MaxTokens;
	Options.Temperature = Temperature;
	Options.TimeoutSeconds = FMath::CeilToInt32(Timeout);

	// Blocking completion with tick-pumping
	bool bCompleted = false;
	bool bSuccess = false;
	FString ResponseText;
	FString ErrorText;

	Provider->SendMessage(
		Messages,
		TArray<FOliveToolDefinition>(), // No tools
		FOnOliveStreamChunk::CreateLambda([&ResponseText](const FOliveStreamChunk& Chunk)
		{
			if (!Chunk.bIsToolCall && !Chunk.Text.IsEmpty())
			{
				ResponseText += Chunk.Text;
			}
		}),
		FOnOliveToolCall::CreateLambda([](const FOliveStreamChunk& /*ToolCall*/) {}),
		FOnOliveComplete::CreateLambda([&bCompleted, &bSuccess](const FString& /*FullResponse*/, const FOliveProviderUsage& /*Usage*/)
		{
			bSuccess = true;
			bCompleted = true;
		}),
		FOnOliveError::CreateLambda([&bCompleted, &ErrorText](const FString& Error)
		{
			ErrorText = Error;
			bCompleted = true;
		}),
		Options
	);

	// Pump the ticker until completion or timeout
	const double StartTime = FPlatformTime::Seconds();
	const double TimeoutLimit = static_cast<double>(Timeout);

	while (!bCompleted)
	{
		const double Elapsed = FPlatformTime::Seconds() - StartTime;
		if (Elapsed >= TimeoutLimit)
		{
			Provider->CancelRequest();
			OutError = FString::Printf(TEXT("Timed out after %.1fs"), Elapsed);
			return false;
		}

		FTSTicker::GetCoreTicker().Tick(0.01f);
		FPlatformProcess::Sleep(0.01f);
	}

	if (bSuccess && !ResponseText.IsEmpty())
	{
		OutResponse = ResponseText.TrimStartAndEnd();
		return true;
	}

	OutError = ErrorText.IsEmpty() ? TEXT("Empty response from provider") : ErrorText;
	return false;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// FOliveAgentPipelineResult
// ---------------------------------------------------------------------------

FString FOliveAgentPipelineResult::FormatForPromptInjection() const
{
	if (!bValid)
	{
		return FString();
	}

	FString Output;
	Output.Reserve(4096);

	// Section 1: Task Analysis
	Output += TEXT("## Task Analysis\n\n");
	const TCHAR* ComplexityStr =
		Router.Complexity == EOliveTaskComplexity::Simple ? TEXT("Simple") :
		Router.Complexity == EOliveTaskComplexity::Moderate ? TEXT("Moderate") :
		TEXT("Complex");
	Output += TEXT("**Complexity**: ");
	Output += ComplexityStr;
	if (!Router.Reasoning.IsEmpty())
	{
		Output += TEXT(" -- ") + Router.Reasoning;
	}
	Output += TEXT("\n\n");

	// Detect whether the user explicitly wants simple/stub logic
	bool bWantsSimpleLogic = false;
	{
		// Check Router reasoning (it captures user intent keywords)
		FString Combined = Router.Reasoning.ToLower();
		// Also check the Architect's plan for stub indicators
		Combined += TEXT(" ") + Architect.BuildPlan.ToLower();

		static const TArray<FString> SimpleIndicators = {
			TEXT("placeholder"), TEXT("stub"), TEXT("just printstring"),
			TEXT("print string only"), TEXT("skeleton"), TEXT("empty logic"),
			TEXT("no logic"), TEXT("structure only"), TEXT("stub it out"),
			TEXT("just the structure"), TEXT("mockup"), TEXT("prototype only")
		};

		for (const FString& Indicator : SimpleIndicators)
		{
			if (Combined.Contains(Indicator))
			{
				bWantsSimpleLogic = true;
				break;
			}
		}
	}

	// Section 2: Reference Templates (from Scout's discovery)
	if (!Scout.DiscoveryBlock.IsEmpty())
	{
		Output += Scout.DiscoveryBlock;
		Output += TEXT("\n\n");
	}

	// Section 2.5: Template references for Builder (fetch full data on demand)
	if (Scout.TemplateReferences.Num() > 0 && !bWantsSimpleLogic)
	{
		Output += TEXT("## Reference Templates (fallback)\n\n");
		Output += TEXT("The Build Plan above contains detailed function descriptions based on these templates. ");
		Output += TEXT("Only call `blueprint.get_template(template_id, pattern=\"FunctionName\")` if a specific function fails ");
		Output += TEXT("and you need to see the original node graph for troubleshooting.\n\n");

		for (const FOliveTemplateReference& Ref : Scout.TemplateReferences)
		{
			Output += TEXT("- **") + Ref.TemplateId + TEXT("**");
			if (!Ref.ParentClass.IsEmpty())
			{
				Output += TEXT(" (") + Ref.ParentClass + TEXT(")");
			}
			if (Ref.MatchedFunctions.Num() > 0)
			{
				Output += TEXT(": functions ");
				for (int32 i = 0; i < Ref.MatchedFunctions.Num(); i++)
				{
					if (i > 0) Output += TEXT(", ");
					Output += TEXT("`") + Ref.MatchedFunctions[i] + TEXT("`");
				}
			}
			Output += TEXT("\n");
		}
		Output += TEXT("\n");
	}

	// Section 3: Build Plan (the core deliverable)
	if (Architect.bSuccess && !Architect.BuildPlan.IsEmpty())
	{
		Output += Architect.BuildPlan;
		Output += TEXT("\n");

		// Inline validator warnings beneath the plan
		if (Validator.Issues.Num() > 0)
		{
			Output += TEXT("\n### Validator Warnings\n\n");
			for (const auto& Issue : Validator.Issues)
			{
				Output += TEXT("- **");
				Output += Issue.AssetName;
				Output += TEXT("** (");
				Output += Issue.Category;
				Output += TEXT("): ");
				Output += Issue.Message;
				if (!Issue.Suggestion.IsEmpty())
				{
					Output += TEXT(" Suggestion: ") + Issue.Suggestion;
				}
				Output += TEXT("\n");
			}
			Output += TEXT("\nFix these issues during implementation. ");
			Output += TEXT("Use the corrected class/component names from suggestions.\n");
		}
		Output += TEXT("\n");
	}

	// Section 3.5: Component API Reference (Aider-style repo map)
	if (!Architect.ComponentAPIMap.IsEmpty())
	{
		Output += TEXT("\n");
		Output += Architect.ComponentAPIMap;
		Output += TEXT("\n");
	}

	// Section 4: Existing Asset Context (from Scout + Researcher)
	if (Scout.RelevantAssets.Num() > 0)
	{
		Output += TEXT("## Existing Assets\n\n");
		for (const auto& Asset : Scout.RelevantAssets)
		{
			Output += TEXT("- ");
			Output += Asset.Path;
			Output += TEXT(" (");
			Output += Asset.AssetClass;
			Output += TEXT(") -- ");
			Output += Asset.Relevance;
			Output += TEXT("\n");
		}

		// Researcher analysis follows if available
		if (Researcher.bSuccess && !Researcher.ArchitecturalAnalysis.IsEmpty())
		{
			Output += TEXT("\n");
			Output += Researcher.ArchitecturalAnalysis;
		}
		Output += TEXT("\n");
	}

	// Section 5: Execution directive
	Output += TEXT("## Execution\n\n");
	Output += TEXT("Follow the Build Plan above. For each asset in Order:\n");
	Output += TEXT("1. Create structure first (components, variables, interfaces, dispatchers) -- batch these freely\n");
	Output += TEXT("2. Add all function signatures -- batch these freely\n");
	Output += TEXT("3. Compile the structure (catches type errors early)\n");

	if (bWantsSimpleLogic)
	{
		Output += TEXT("4. Write graph logic as described in the plan. The user wants ");
		Output += TEXT("placeholder/simple logic -- PrintString stubs are acceptable.\n");
	}
	else
	{
		Output += TEXT("4. For each function/event:\n");
		Output += TEXT("   a. Write graph logic with apply_plan_json based on the function description in the Build Plan. ");
		Output += TEXT("Use `@step.auto` for pin wiring. Do not simplify your design.\n");
		Output += TEXT("   b. If plan_json fails, check the Reference Templates section -- call ");
		Output += TEXT("`blueprint.get_template(template_id, pattern=\"FunctionName\")` for the specific failed function only.\n");
		Output += TEXT("   c. Compile after each function. Fix the first error before moving on.\n");
	}

	Output += TEXT("5. Do not move to the next asset until current asset compiles to 0 errors.\n");
	Output += TEXT("Do not stop until every asset in the plan is fully built and compiled.\n");

	return Output;
}

// ---------------------------------------------------------------------------
// FOliveAgentPipeline -- LLM Communication
// ---------------------------------------------------------------------------

bool FOliveAgentPipeline::SendAgentCompletion(
	EOliveAgentRole Role,
	const FString& SystemPrompt,
	const FString& UserPrompt,
	FString& OutResponse,
	FString& OutError)
{
	const UOliveAISettings* Settings = UOliveAISettings::Get();
	if (!Settings)
	{
		OutError = TEXT("OliveAISettings not available");
		return false;
	}

	const float Temperature = GetTemperature(Role);
	const int32 MaxTokens = GetMaxTokens(Role);
	const float Timeout = GetTimeout(Role);

	// --- Tier 1: Per-agent model configuration ---
	{
		const FOliveAgentModelConfig AgentConfig = Settings->GetAgentModelConfig(Role);

		if (AgentConfig.bIsValid && !AgentConfig.bIsCLIFallback)
		{
			const FString ProviderName = ProviderEnumToName(AgentConfig.Provider);
			FString Tier1Error;

			if (TrySendViaProvider(ProviderName, AgentConfig.ModelId, AgentConfig.ApiKey,
				AgentConfig.BaseUrl, Temperature, MaxTokens, Timeout,
				SystemPrompt, UserPrompt, OutResponse, Tier1Error))
			{
				UE_LOG(LogOliveAgentPipeline, Verbose,
					TEXT("[%s] Tier 1 success (%s/%s), response length: %d"),
					*RoleToString(Role), *ProviderName, *AgentConfig.ModelId,
					OutResponse.Len());
				return true;
			}

			UE_LOG(LogOliveAgentPipeline, Verbose,
				TEXT("[%s] Tier 1 (%s) failed: %s"),
				*RoleToString(Role), *ProviderName, *Tier1Error);
		}
	}

	// --- Tier 2: Main chat provider (if HTTP-based) ---
	{
		const EOliveAIProvider MainProvider = Settings->Provider;

		if (MainProvider != EOliveAIProvider::ClaudeCode && MainProvider != EOliveAIProvider::Codex)
		{
			const FString MainApiKey = Settings->GetCurrentApiKey();
			const FString MainBaseUrl = Settings->GetCurrentBaseUrl();
			const FString MainModel = Settings->GetSelectedModelForProvider(MainProvider);

			const bool bMainHasCredentials = !MainApiKey.IsEmpty()
				|| MainProvider == EOliveAIProvider::Ollama
				|| MainProvider == EOliveAIProvider::OpenAICompatible;

			if (bMainHasCredentials && !MainModel.IsEmpty())
			{
				const FString ProviderName = ProviderEnumToName(MainProvider);
				FString Tier2Error;

				if (TrySendViaProvider(ProviderName, MainModel, MainApiKey,
					MainBaseUrl, Temperature, MaxTokens, Timeout,
					SystemPrompt, UserPrompt, OutResponse, Tier2Error))
				{
					UE_LOG(LogOliveAgentPipeline, Verbose,
						TEXT("[%s] Tier 2 success (%s), response length: %d"),
						*RoleToString(Role), *ProviderName, OutResponse.Len());
					return true;
				}

				UE_LOG(LogOliveAgentPipeline, Verbose,
					TEXT("[%s] Tier 2 (%s) failed: %s"),
					*RoleToString(Role), *ProviderName, *Tier2Error);
			}
		}
	}

	// --- Tier 3: Claude Code CLI --print via stdin pipe ---
	// Uses CreateProc + stdin pipe instead of ExecProcess to avoid Windows'
	// ~32K character limit on command-line arguments. The prompt is piped via
	// stdin, and stdout is read after the process exits.
	{
		if (FOliveClaudeCodeProvider::IsClaudeCodeInstalled())
		{
			const FString ClaudePath = FOliveClaudeCodeProvider::GetClaudeExecutablePath();
			if (!ClaudePath.IsEmpty())
			{
				// Build combined prompt (system + user)
				FString CombinedPrompt;
				if (!SystemPrompt.IsEmpty())
				{
					CombinedPrompt = SystemPrompt + TEXT("\n\n") + UserPrompt;
				}
				else
				{
					CombinedPrompt = UserPrompt;
				}

				const bool bIsJs = ClaudePath.EndsWith(TEXT(".js")) || ClaudePath.EndsWith(TEXT(".mjs"));

				// Args: --print reads from stdin when no prompt argument is given
				const FString BaseArgs = TEXT("--print --output-format text --max-turns 1");

				FString Executable;
				FString Args;
				if (bIsJs)
				{
					Executable = TEXT("node");
					Args = FString::Printf(TEXT("\"%s\" %s"), *ClaudePath, *BaseArgs);
				}
				else
				{
					Executable = ClaudePath;
					Args = BaseArgs;
				}

				// Create pipes for stdin and stdout
				void* StdoutRead = nullptr;
				void* StdoutWrite = nullptr;
				void* StdinRead = nullptr;
				void* StdinWrite = nullptr;

				if (!FPlatformProcess::CreatePipe(StdoutRead, StdoutWrite))
				{
					UE_LOG(LogOliveAgentPipeline, Verbose,
						TEXT("[%s] Tier 3: failed to create stdout pipe"), *RoleToString(Role));
				}
				else if (!FPlatformProcess::CreatePipe(StdinRead, StdinWrite, /*bWritePipeLocal=*/true))
				{
					FPlatformProcess::ClosePipe(StdoutRead, StdoutWrite);
					UE_LOG(LogOliveAgentPipeline, Verbose,
						TEXT("[%s] Tier 3: failed to create stdin pipe"), *RoleToString(Role));
				}
				else
				{
					// Spawn process with pipes
					uint32 ProcessId = 0;
					FProcHandle ProcHandle = FPlatformProcess::CreateProc(
						*Executable, *Args,
						false, true, true,  // bLaunchDetached, bLaunchHidden, bLaunchReallyHidden
						&ProcessId, 0, nullptr,
						StdoutWrite,  // child writes stdout here
						StdinRead     // child reads stdin from here
					);

					if (!ProcHandle.IsValid())
					{
						FPlatformProcess::ClosePipe(StdoutRead, StdoutWrite);
						FPlatformProcess::ClosePipe(StdinRead, StdinWrite);
						UE_LOG(LogOliveAgentPipeline, Verbose,
							TEXT("[%s] Tier 3: failed to spawn CLI process"), *RoleToString(Role));
					}
					else
					{
						// Close pipe ends we don't use
						FPlatformProcess::ClosePipe(nullptr, StdoutWrite);
						StdoutWrite = nullptr;
						FPlatformProcess::ClosePipe(StdinRead, nullptr);
						StdinRead = nullptr;

						// Write prompt via stdin, then close to signal EOF
						FPlatformProcess::WritePipe(StdinWrite, CombinedPrompt);
						FPlatformProcess::ClosePipe(nullptr, StdinWrite);
						StdinWrite = nullptr;

						// Read stdout while waiting for process to exit
						FString StdOut;
						const double Tier3Start = FPlatformTime::Seconds();
						bool bTimedOut = false;

						while (FPlatformProcess::IsProcRunning(ProcHandle))
						{
							StdOut += FPlatformProcess::ReadPipe(StdoutRead);

							if (FPlatformTime::Seconds() - Tier3Start >= static_cast<double>(Timeout))
							{
								FPlatformProcess::TerminateProc(ProcHandle, true);
								FPlatformProcess::ClosePipe(StdoutRead, nullptr);
								bTimedOut = true;
								UE_LOG(LogOliveAgentPipeline, Warning,
									TEXT("[%s] Tier 3 (Claude CLI) timed out after %.1fs"),
									*RoleToString(Role), Timeout);
								break;
							}

							FPlatformProcess::Sleep(0.05f);
						}

						// Read any remaining output after exit (skip if already closed by timeout)
						if (!bTimedOut)
						{
							StdOut += FPlatformProcess::ReadPipe(StdoutRead);
							FPlatformProcess::ClosePipe(StdoutRead, nullptr);
						}

						int32 ReturnCode = -1;
						FPlatformProcess::GetProcReturnCode(ProcHandle, &ReturnCode);
						FPlatformProcess::CloseProc(ProcHandle);

						if (ReturnCode == 0)
						{
							FString Trimmed = StdOut.TrimStartAndEnd();
							if (!Trimmed.IsEmpty())
							{
								OutResponse = Trimmed;
								UE_LOG(LogOliveAgentPipeline, Verbose,
									TEXT("[%s] Tier 3 (Claude CLI) success, response length: %d"),
									*RoleToString(Role), OutResponse.Len());
								return true;
							}
						}

						UE_LOG(LogOliveAgentPipeline, Verbose,
							TEXT("[%s] Tier 3 (Claude CLI) failed: code=%d, output=%d chars"),
							*RoleToString(Role), ReturnCode, StdOut.Len());
					}
				}
			}
		}
	}

	OutError = FString::Printf(
		TEXT("Agent %s: all provider tiers exhausted. Configure an API key or install Claude Code CLI."),
		*RoleToString(Role));
	return false;
}

// ---------------------------------------------------------------------------
// Pipeline Entry Points
// ---------------------------------------------------------------------------

FOliveAgentPipelineResult FOliveAgentPipeline::Execute(
	const FString& UserMessage,
	const TArray<FString>& ContextAssetPaths)
{
	// Detect CLI-only mode: if all agents would fall through to CLI --print,
	// use the optimized 2-call path (Scout + Planner) to minimize cold-start overhead.
	// CLI cold start is ~4-5s per process, so 5 sequential agents = 20-25s overhead.
	if (IsCLIOnlyMode())
	{
		UE_LOG(LogOliveAgentPipeline, Log,
			TEXT("CLI-only mode detected. Using optimized 2-call pipeline (Scout + Planner)."));
		return ExecuteCLIPath(UserMessage, ContextAssetPaths);
	}

	FOliveAgentPipelineResult Result;
	const double PipelineStartTime = FPlatformTime::Seconds();

	UE_LOG(LogOliveAgentPipeline, Log,
		TEXT("Agent pipeline starting for message: \"%s\" (%d context assets)"),
		*UserMessage.Left(100), ContextAssetPaths.Num());

	// --- Stage 1: Router ---
	{
		Result.Router = RunRouter(UserMessage);

		UE_LOG(LogOliveAgentPipeline, Log,
			TEXT("  Router: %s (%.1fs) -- %s"),
			Result.Router.Complexity == EOliveTaskComplexity::Simple ? TEXT("SIMPLE") :
			Result.Router.Complexity == EOliveTaskComplexity::Moderate ? TEXT("MODERATE") :
			TEXT("COMPLEX"),
			Result.Router.ElapsedSeconds,
			*Result.Router.Reasoning.Left(80));
	}

	// Check total pipeline timeout
	const double ElapsedSoFar = FPlatformTime::Seconds() - PipelineStartTime;
	if (ElapsedSoFar >= PIPELINE_TOTAL_TIMEOUT)
	{
		UE_LOG(LogOliveAgentPipeline, Warning,
			TEXT("Agent pipeline timed out after Router (%.1fs). Returning partial result."),
			ElapsedSoFar);
		Result.TotalElapsedSeconds = ElapsedSoFar;
		Result.bValid = false;
		return Result;
	}

	// --- Stage 2: Scout ---
	{
		Result.Scout = RunScout(UserMessage, ContextAssetPaths);

		UE_LOG(LogOliveAgentPipeline, Log,
			TEXT("  Scout: %d relevant assets, discovery block %d chars (%.1fs)"),
			Result.Scout.RelevantAssets.Num(),
			Result.Scout.DiscoveryBlock.Len(),
			Result.Scout.ElapsedSeconds);
	}

	if (FPlatformTime::Seconds() - PipelineStartTime >= PIPELINE_TOTAL_TIMEOUT)
	{
		UE_LOG(LogOliveAgentPipeline, Warning,
			TEXT("Agent pipeline timed out after Scout. Returning partial result."));
		Result.TotalElapsedSeconds = FPlatformTime::Seconds() - PipelineStartTime;
		Result.bValid = false;
		return Result;
	}

	// --- Stage 3: Researcher (skip for Simple tasks) ---
	if (Result.Router.Complexity != EOliveTaskComplexity::Simple)
	{
		Result.Researcher = RunResearcher(UserMessage, Result.Scout);

		UE_LOG(LogOliveAgentPipeline, Log,
			TEXT("  Researcher: %d assets analyzed, %d chars output (%.1fs)"),
			Result.Researcher.AnalyzedAssets.Num(),
			Result.Researcher.ArchitecturalAnalysis.Len(),
			Result.Researcher.ElapsedSeconds);
	}
	else
	{
		UE_LOG(LogOliveAgentPipeline, Log,
			TEXT("  Researcher: skipped (Simple task)"));
	}

	if (FPlatformTime::Seconds() - PipelineStartTime >= PIPELINE_TOTAL_TIMEOUT)
	{
		UE_LOG(LogOliveAgentPipeline, Warning,
			TEXT("Agent pipeline timed out after Researcher. Returning partial result."));
		Result.TotalElapsedSeconds = FPlatformTime::Seconds() - PipelineStartTime;
		Result.bValid = false;
		return Result;
	}

	// --- Stage 4: Architect ---
	{
		Result.Architect = RunArchitect(UserMessage, Result.Router, Result.Scout, Result.Researcher);

		UE_LOG(LogOliveAgentPipeline, Log,
			TEXT("  Architect: %s, %d assets in plan, %d chars (%.1fs)"),
			Result.Architect.bSuccess ? TEXT("success") : TEXT("FAILED"),
			Result.Architect.AssetOrder.Num(),
			Result.Architect.BuildPlan.Len(),
			Result.Architect.ElapsedSeconds);
	}

	// If the Architect failed, the pipeline is still valid but with degraded output.
	// The Builder will work without a Build Plan (old behavior).

	// --- Stage 5: Validator (C++ only, no timeout concerns) ---
	if (Result.Architect.bSuccess)
	{
		Result.Validator = RunValidator(Result.Architect);

		UE_LOG(LogOliveAgentPipeline, Log,
			TEXT("  Validator: %d issues, blocking=%s (%.3fs)"),
			Result.Validator.Issues.Num(),
			Result.Validator.bHasBlockingIssues ? TEXT("YES") : TEXT("no"),
			Result.Validator.ElapsedSeconds);
	}
	else
	{
		UE_LOG(LogOliveAgentPipeline, Log,
			TEXT("  Validator: skipped (Architect failed)"));
	}

	// The pipeline is valid if either the Architect succeeded or at least the Scout
	// produced some discovery results.
	Result.bValid = Result.Architect.bSuccess
		|| !Result.Scout.DiscoveryBlock.IsEmpty()
		|| Result.Scout.RelevantAssets.Num() > 0;

	Result.TotalElapsedSeconds = FPlatformTime::Seconds() - PipelineStartTime;

	UE_LOG(LogOliveAgentPipeline, Log,
		TEXT("Agent pipeline complete: valid=%s, %.1fs total"),
		Result.bValid ? TEXT("true") : TEXT("false"),
		Result.TotalElapsedSeconds);

	return Result;
}

FOliveReviewerResult FOliveAgentPipeline::RunReviewer(
	const FOliveAgentPipelineResult& PipelineResult,
	const TArray<FString>& ModifiedAssets)
{
	FOliveReviewerResult Result;
	const double StartTime = FPlatformTime::Seconds();

	if (!PipelineResult.Architect.bSuccess || PipelineResult.Architect.BuildPlan.IsEmpty())
	{
		UE_LOG(LogOliveAgentPipeline, Warning,
			TEXT("Reviewer skipped: no valid Build Plan to compare against."));
		return Result;
	}

	if (ModifiedAssets.Num() == 0)
	{
		UE_LOG(LogOliveAgentPipeline, Warning,
			TEXT("Reviewer skipped: no modified assets to review."));
		return Result;
	}

	// Build user prompt: Build Plan + current asset state
	FString UserPrompt;
	UserPrompt.Reserve(4096);
	UserPrompt += TEXT("## Original Build Plan\n\n");
	UserPrompt += PipelineResult.Architect.BuildPlan;
	UserPrompt += TEXT("\n\n## Current Asset State\n\n");
	UserPrompt += BuildAssetStateSummary(ModifiedAssets);

	// Send to LLM
	FString SystemPrompt = BuildReviewerSystemPrompt();
	FString Response;
	FString Error;

	if (!SendAgentCompletion(EOliveAgentRole::Reviewer, SystemPrompt, UserPrompt, Response, Error))
	{
		UE_LOG(LogOliveAgentPipeline, Warning,
			TEXT("Reviewer LLM call failed: %s"), *Error);
		Result.ElapsedSeconds = FPlatformTime::Seconds() - StartTime;
		return Result;
	}

	Result.bSuccess = true;

	// Parse response
	FString TrimmedResponse = Response.TrimStartAndEnd();

	if (TrimmedResponse.StartsWith(TEXT("SATISFIED")))
	{
		Result.bPlanSatisfied = true;
	}
	else
	{
		// Parse MISSING, DEVIATIONS, CORRECTION sections
		enum class ESection { None, Missing, Deviations, Correction };
		ESection CurrentSection = ESection::None;

		TArray<FString> Lines;
		TrimmedResponse.ParseIntoArrayLines(Lines);

		for (const FString& Line : Lines)
		{
			FString TrimLine = Line.TrimStartAndEnd();

			if (TrimLine.StartsWith(TEXT("MISSING:")) || TrimLine.StartsWith(TEXT("MISSING")))
			{
				CurrentSection = ESection::Missing;
				// If there's content after "MISSING:", capture it
				FString After = TrimLine.Mid(TrimLine.Contains(TEXT(":")) ? TrimLine.Find(TEXT(":")) + 1 : 7).TrimStartAndEnd();
				if (!After.IsEmpty())
				{
					Result.MissingItems.Add(After);
				}
				continue;
			}
			if (TrimLine.StartsWith(TEXT("DEVIATIONS:")) || TrimLine.StartsWith(TEXT("DEVIATIONS")))
			{
				CurrentSection = ESection::Deviations;
				continue;
			}
			if (TrimLine.StartsWith(TEXT("CORRECTION:")) || TrimLine.StartsWith(TEXT("CORRECTION")))
			{
				CurrentSection = ESection::Correction;
				// If there's content after "CORRECTION:", capture it
				FString After = TrimLine.Mid(TrimLine.Contains(TEXT(":")) ? TrimLine.Find(TEXT(":")) + 1 : 10).TrimStartAndEnd();
				if (!After.IsEmpty())
				{
					Result.CorrectionDirective += After;
				}
				continue;
			}

			// Skip empty lines
			if (TrimLine.IsEmpty())
			{
				if (CurrentSection == ESection::Correction && !Result.CorrectionDirective.IsEmpty())
				{
					Result.CorrectionDirective += TEXT("\n");
				}
				continue;
			}

			// Strip leading "- " for list items
			FString Content = TrimLine;
			if (Content.StartsWith(TEXT("- ")))
			{
				Content = Content.Mid(2);
			}

			switch (CurrentSection)
			{
			case ESection::Missing:
				Result.MissingItems.Add(Content);
				break;
			case ESection::Deviations:
				Result.Deviations.Add(Content);
				break;
			case ESection::Correction:
				if (!Result.CorrectionDirective.IsEmpty())
				{
					Result.CorrectionDirective += TEXT("\n");
				}
				Result.CorrectionDirective += TrimLine;
				break;
			default:
				break;
			}
		}

		Result.CorrectionDirective = Result.CorrectionDirective.TrimStartAndEnd();
		Result.bPlanSatisfied = (Result.MissingItems.Num() == 0 && Result.Deviations.Num() == 0);
	}

	Result.ElapsedSeconds = FPlatformTime::Seconds() - StartTime;

	UE_LOG(LogOliveAgentPipeline, Log,
		TEXT("Reviewer: %s, %d missing, %d deviations (%.1fs)"),
		Result.bPlanSatisfied ? TEXT("SATISFIED") : TEXT("issues found"),
		Result.MissingItems.Num(), Result.Deviations.Num(),
		Result.ElapsedSeconds);

	return Result;
}

// ---------------------------------------------------------------------------
// Pipeline Stages
// ---------------------------------------------------------------------------

FOliveRouterResult FOliveAgentPipeline::RunRouter(const FString& UserMessage)
{
	FOliveRouterResult Result;
	const double StartTime = FPlatformTime::Seconds();

	FString SystemPrompt = BuildRouterSystemPrompt();
	FString Response;
	FString Error;

	if (SendAgentCompletion(EOliveAgentRole::Router, SystemPrompt, UserMessage, Response, Error))
	{
		Result = ParseRouterResponse(Response);
		Result.bSuccess = true;
	}
	else
	{
		// On failure: default to Moderate (safe middle ground)
		UE_LOG(LogOliveAgentPipeline, Warning,
			TEXT("Router LLM call failed: %s. Defaulting to Moderate."), *Error);
		Result.Complexity = EOliveTaskComplexity::Moderate;
		Result.Reasoning = TEXT("Router unavailable; defaulted to Moderate");
		Result.bSuccess = false;
	}

	Result.ElapsedSeconds = FPlatformTime::Seconds() - StartTime;
	return Result;
}

FOliveScoutResult FOliveAgentPipeline::RunScout(
	const FString& UserMessage,
	const TArray<FString>& ContextAssetPaths)
{
	FOliveScoutResult Result;
	const double StartTime = FPlatformTime::Seconds();

	// Part 1: Template discovery pass (reuses existing FOliveUtilityModel infrastructure)
	FOliveDiscoveryResult DiscoveryResult = FOliveUtilityModel::RunDiscoveryPass(UserMessage, 8);
	Result.DiscoveryBlock = FOliveUtilityModel::FormatDiscoveryForPrompt(DiscoveryResult);

	// Part 1.5: Build structural overviews + reference list from top library matches.
	// GetTemplateOverview() reads from in-memory metadata -- instant, no disk I/O.
	{
		const FOliveLibraryIndex& LibIndex = FOliveTemplateSystem::Get().GetLibraryIndex();

		if (LibIndex.IsInitialized())
		{
			static constexpr int32 MAX_OVERVIEW_TEMPLATES = 3;
			int32 TemplatesLoaded = 0;
			FString OverviewBlock;

			for (const FOliveDiscoveryEntry& Entry : DiscoveryResult.Entries)
			{
				if (TemplatesLoaded >= MAX_OVERVIEW_TEMPLATES)
				{
					break;
				}

				// Only load library templates (factory/reference don't have structural metadata)
				if (Entry.SourceType != TEXT("library"))
				{
					continue;
				}

				FString Overview = LibIndex.GetTemplateOverview(Entry.TemplateId);
				if (Overview.IsEmpty())
				{
					continue;
				}

				if (OverviewBlock.IsEmpty())
				{
					OverviewBlock += TEXT("## Template Architecture Reference\n\n");
					OverviewBlock += TEXT("Structural overviews of relevant library templates.\n\n");
				}

				OverviewBlock += Overview;
				OverviewBlock += TEXT("\n");
				TemplatesLoaded++;

				// Build reference entry for Builder
				if (Entry.MatchedFunctions.Num() > 0)
				{
					FOliveTemplateReference Ref;
					Ref.TemplateId = Entry.TemplateId;
					Ref.DisplayName = Entry.DisplayName;
					Ref.ParentClass = Entry.ParentClass;
					Ref.MatchedFunctions = Entry.MatchedFunctions;
					Result.TemplateReferences.Add(MoveTemp(Ref));
				}
			}

			Result.TemplateOverviews = OverviewBlock;

			if (!OverviewBlock.IsEmpty())
			{
				UE_LOG(LogOliveAgentPipeline, Log,
					TEXT("  Scout: loaded %d template overviews, %d chars, %d references"),
					TemplatesLoaded, OverviewBlock.Len(), Result.TemplateReferences.Num());
			}
		}
	}

	// Part 2: Search project index for existing assets
	TArray<FOliveScoutResult::FAssetEntry> RawAssets;

	// Include @-mentioned context assets first
	for (const FString& ContextPath : ContextAssetPaths)
	{
		TOptional<FOliveAssetInfo> AssetInfo = FOliveProjectIndex::Get().GetAssetByPath(ContextPath);
		if (AssetInfo.IsSet())
		{
			FOliveScoutResult::FAssetEntry Entry;
			Entry.Path = AssetInfo->Path;
			Entry.AssetClass = AssetInfo->AssetClass.ToString();
			Entry.Relevance = TEXT("explicitly referenced by user");
			RawAssets.Add(MoveTemp(Entry));
		}
	}

	// Search for related assets by keywords from the user message
	TArray<FString> Keywords = FOliveUtilityModel::ExtractSearchKeywords(UserMessage, 8);
	for (const FString& Keyword : Keywords)
	{
		TArray<FOliveAssetInfo> SearchResults = FOliveProjectIndex::Get().SearchAssets(Keyword, 10);
		for (const FOliveAssetInfo& AssetInfo : SearchResults)
		{
			// Avoid duplicates
			bool bAlreadyAdded = false;
			for (const auto& Existing : RawAssets)
			{
				if (Existing.Path == AssetInfo.Path)
				{
					bAlreadyAdded = true;
					break;
				}
			}
			if (bAlreadyAdded)
			{
				continue;
			}

			FOliveScoutResult::FAssetEntry Entry;
			Entry.Path = AssetInfo.Path;
			Entry.AssetClass = AssetInfo.AssetClass.ToString();
			Entry.Relevance = TEXT(""); // Will be filled by LLM ranking
			RawAssets.Add(MoveTemp(Entry));

			if (RawAssets.Num() >= MAX_PROJECT_SEARCH_RESULTS)
			{
				break;
			}
		}

		if (RawAssets.Num() >= MAX_PROJECT_SEARCH_RESULTS)
		{
			break;
		}
	}

	// Part 3: Use LLM to rank relevance of found assets
	if (RawAssets.Num() > 0)
	{
		// Build asset list string for the Scout LLM
		FString AssetListStr;
		for (int32 i = 0; i < RawAssets.Num(); i++)
		{
			AssetListStr += FString::Printf(TEXT("%d. %s (%s)\n"),
				i + 1, *RawAssets[i].Path, *RawAssets[i].AssetClass);
		}

		FString ScoutUserPrompt;
		ScoutUserPrompt += TEXT("Task: ") + UserMessage + TEXT("\n\n");
		ScoutUserPrompt += TEXT("Existing project assets:\n") + AssetListStr;

		FString ScoutSystemPrompt = BuildScoutSystemPrompt();
		FString Response;
		FString Error;

		if (SendAgentCompletion(EOliveAgentRole::Scout, ScoutSystemPrompt, ScoutUserPrompt, Response, Error))
		{
			Result.RelevantAssets = ParseScoutResponse(Response, RawAssets);
			Result.bSuccess = true;
		}
		else
		{
			// On failure: pass through context assets only (they were explicitly mentioned)
			UE_LOG(LogOliveAgentPipeline, Warning,
				TEXT("Scout LLM ranking failed: %s. Using unranked context assets."), *Error);
			for (auto& Entry : RawAssets)
			{
				if (Entry.Relevance == TEXT("explicitly referenced by user"))
				{
					Result.RelevantAssets.Add(MoveTemp(Entry));
				}
			}
		}
	}

	Result.ElapsedSeconds = FPlatformTime::Seconds() - StartTime;
	return Result;
}

FOliveResearcherResult FOliveAgentPipeline::RunResearcher(
	const FString& UserMessage,
	const FOliveScoutResult& ScoutResult)
{
	FOliveResearcherResult Result;
	const double StartTime = FPlatformTime::Seconds();

	if (ScoutResult.RelevantAssets.Num() == 0)
	{
		UE_LOG(LogOliveAgentPipeline, Log,
			TEXT("Researcher: no relevant assets to analyze, skipping."));
		Result.ElapsedSeconds = FPlatformTime::Seconds() - StartTime;
		return Result;
	}

	// Load Blueprint IR for each relevant asset (up to MAX_RESEARCHER_ASSETS)
	FString AssetDataStr;
	const int32 AssetsToAnalyze = FMath::Min(ScoutResult.RelevantAssets.Num(), MAX_RESEARCHER_ASSETS);

	for (int32 i = 0; i < AssetsToAnalyze; i++)
	{
		const FOliveScoutResult::FAssetEntry& Asset = ScoutResult.RelevantAssets[i];

		// Only analyze Blueprints
		if (!Asset.AssetClass.Contains(TEXT("Blueprint")))
		{
			continue;
		}

		TOptional<FOliveIRBlueprint> IR = FOliveBlueprintReader::Get().ReadBlueprintSummary(Asset.Path);
		if (!IR.IsSet())
		{
			continue;
		}

		Result.AnalyzedAssets.Add(Asset.Path);

		// Format IR data as structured text for the Researcher LLM
		const FOliveIRBlueprint& BP = IR.GetValue();
		AssetDataStr += TEXT("### ") + BP.Name + TEXT("\n");
		AssetDataStr += TEXT("- Path: ") + BP.Path + TEXT("\n");
		AssetDataStr += TEXT("- Parent: ") + BP.ParentClass.Name + TEXT("\n");

		// Variables
		if (BP.Variables.Num() > 0)
		{
			AssetDataStr += TEXT("- Variables: ");
			int32 VarCount = 0;
			for (const FOliveIRVariable& Var : BP.Variables)
			{
				if (Var.Name.StartsWith(TEXT("DefaultComponent_")))
				{
					continue; // Skip SCS-internal vars
				}
				if (VarCount > 0)
				{
					AssetDataStr += TEXT(", ");
				}
				AssetDataStr += Var.Name + TEXT(" (") + Var.Type.GetDisplayName() + TEXT(")");
				VarCount++;
			}
			AssetDataStr += TEXT("\n");
		}

		// Components
		if (BP.Components.Num() > 0)
		{
			AssetDataStr += TEXT("- Components: ");
			for (int32 j = 0; j < BP.Components.Num(); j++)
			{
				if (j > 0)
				{
					AssetDataStr += TEXT(", ");
				}
				AssetDataStr += BP.Components[j].Name + TEXT(" (") + BP.Components[j].ComponentClass + TEXT(")");
			}
			AssetDataStr += TEXT("\n");
		}

		// Functions
		if (BP.FunctionNames.Num() > 0)
		{
			AssetDataStr += TEXT("- Functions: ");
			for (int32 j = 0; j < BP.FunctionNames.Num(); j++)
			{
				if (j > 0)
				{
					AssetDataStr += TEXT(", ");
				}
				AssetDataStr += BP.FunctionNames[j];
			}
			AssetDataStr += TEXT("\n");
		}

		// Interfaces
		if (BP.Interfaces.Num() > 0)
		{
			AssetDataStr += TEXT("- Interfaces: ");
			for (int32 j = 0; j < BP.Interfaces.Num(); j++)
			{
				if (j > 0)
				{
					AssetDataStr += TEXT(", ");
				}
				AssetDataStr += BP.Interfaces[j].Name;
			}
			AssetDataStr += TEXT("\n");
		}

		// Event dispatchers
		if (BP.EventDispatchers.Num() > 0)
		{
			AssetDataStr += TEXT("- Dispatchers: ");
			for (int32 j = 0; j < BP.EventDispatchers.Num(); j++)
			{
				if (j > 0)
				{
					AssetDataStr += TEXT(", ");
				}
				AssetDataStr += BP.EventDispatchers[j].Name;
				if (BP.EventDispatchers[j].Parameters.Num() > 0)
				{
					AssetDataStr += TEXT("(");
					for (int32 k = 0; k < BP.EventDispatchers[j].Parameters.Num(); k++)
					{
						if (k > 0)
						{
							AssetDataStr += TEXT(", ");
						}
						AssetDataStr += BP.EventDispatchers[j].Parameters[k].Type.GetDisplayName();
					}
					AssetDataStr += TEXT(")");
				}
			}
			AssetDataStr += TEXT("\n");
		}

		AssetDataStr += TEXT("\n");
	}

	if (AssetDataStr.IsEmpty())
	{
		UE_LOG(LogOliveAgentPipeline, Log,
			TEXT("Researcher: no Blueprint assets to analyze."));
		Result.ElapsedSeconds = FPlatformTime::Seconds() - StartTime;
		return Result;
	}

	// Send to LLM
	FString ResearcherUserPrompt;
	ResearcherUserPrompt += TEXT("Task: ") + UserMessage + TEXT("\n\n");
	ResearcherUserPrompt += TEXT("Blueprint IR data:\n\n") + AssetDataStr;

	FString SystemPrompt = BuildResearcherSystemPrompt();
	FString Response;
	FString Error;

	if (SendAgentCompletion(EOliveAgentRole::Researcher, SystemPrompt, ResearcherUserPrompt, Response, Error))
	{
		Result.ArchitecturalAnalysis = Response;
		Result.bSuccess = true;
	}
	else
	{
		// On failure: skip analysis, Architect works without it
		UE_LOG(LogOliveAgentPipeline, Warning,
			TEXT("Researcher LLM call failed: %s"), *Error);
	}

	Result.ElapsedSeconds = FPlatformTime::Seconds() - StartTime;
	return Result;
}

FOliveArchitectResult FOliveAgentPipeline::RunArchitect(
	const FString& UserMessage,
	const FOliveRouterResult& RouterResult,
	const FOliveScoutResult& ScoutResult,
	const FOliveResearcherResult& ResearcherResult)
{
	FOliveArchitectResult Result;
	const double StartTime = FPlatformTime::Seconds();

	// Build the user prompt with all gathered context
	FString ArchitectUserPrompt;
	ArchitectUserPrompt.Reserve(4096);

	// Complexity context
	const TCHAR* ComplexityStr =
		RouterResult.Complexity == EOliveTaskComplexity::Simple ? TEXT("Simple") :
		RouterResult.Complexity == EOliveTaskComplexity::Moderate ? TEXT("Moderate") :
		TEXT("Complex");
	ArchitectUserPrompt += TEXT("## Task\n\n");
	ArchitectUserPrompt += UserMessage;
	ArchitectUserPrompt += TEXT("\n\n**Complexity**: ");
	ArchitectUserPrompt += ComplexityStr;
	if (!RouterResult.Reasoning.IsEmpty())
	{
		ArchitectUserPrompt += TEXT(" (") + RouterResult.Reasoning + TEXT(")");
	}
	ArchitectUserPrompt += TEXT("\n");

	// Existing assets
	if (ScoutResult.RelevantAssets.Num() > 0)
	{
		ArchitectUserPrompt += TEXT("\n## Existing Assets\n\n");
		for (const auto& Asset : ScoutResult.RelevantAssets)
		{
			ArchitectUserPrompt += TEXT("- ");
			ArchitectUserPrompt += Asset.Path;
			ArchitectUserPrompt += TEXT(" (") + Asset.AssetClass + TEXT(")");
			if (!Asset.Relevance.IsEmpty())
			{
				ArchitectUserPrompt += TEXT(" -- ") + Asset.Relevance;
			}
			ArchitectUserPrompt += TEXT("\n");
		}
	}

	// Researcher analysis
	if (ResearcherResult.bSuccess && !ResearcherResult.ArchitecturalAnalysis.IsEmpty())
	{
		ArchitectUserPrompt += TEXT("\n## Architecture Analysis\n\n");
		ArchitectUserPrompt += ResearcherResult.ArchitecturalAnalysis;
		ArchitectUserPrompt += TEXT("\n");
	}

	// Template references (from discovery)
	if (!ScoutResult.DiscoveryBlock.IsEmpty())
	{
		ArchitectUserPrompt += TEXT("\n");
		ArchitectUserPrompt += ScoutResult.DiscoveryBlock;
		ArchitectUserPrompt += TEXT("\n");
	}

	// Template structural overviews (architecture-level, no node graph data)
	if (!ScoutResult.TemplateOverviews.IsEmpty())
	{
		ArchitectUserPrompt += TEXT("\n");
		ArchitectUserPrompt += ScoutResult.TemplateOverviews;
		ArchitectUserPrompt += TEXT("\n");
	}

	// Send to LLM
	FString SystemPrompt = BuildArchitectSystemPrompt();
	FString Response;
	FString Error;

	if (SendAgentCompletion(EOliveAgentRole::Architect, SystemPrompt, ArchitectUserPrompt, Response, Error))
	{
		Result.BuildPlan = Response;
		Result.bSuccess = true;

		// Parse the plan to extract structured data for the Validator
		ParseBuildPlan(Result.BuildPlan, Result);

		// Build compact API reference for components mentioned in the plan
		Result.ComponentAPIMap = BuildComponentAPIMap(Result);
	}
	else
	{
		// On failure: fall back to a minimal decomposition directive
		UE_LOG(LogOliveAgentPipeline, Warning,
			TEXT("Architect LLM call failed: %s. No Build Plan produced."), *Error);
		Result.bSuccess = false;
	}

	Result.ElapsedSeconds = FPlatformTime::Seconds() - StartTime;
	return Result;
}

FOliveValidatorResult FOliveAgentPipeline::RunValidator(const FOliveArchitectResult& ArchitectResult)
{
	FOliveValidatorResult Result;
	const double StartTime = FPlatformTime::Seconds();

	// Validate parent classes
	for (const auto& Pair : ArchitectResult.ParentClasses)
	{
		const FString& AssetName = Pair.Key;
		const FString& ClassName = Pair.Value;

		if (ClassName.IsEmpty())
		{
			continue;
		}

		UClass* Resolved = TryResolveClass(ClassName);
		if (!Resolved)
		{
			FOliveValidatorResult::FValidationIssue Issue;
			Issue.AssetName = AssetName;
			Issue.Category = TEXT("Parent Class");
			Issue.Value = ClassName;
			Issue.Message = FString::Printf(TEXT("Class '%s' not found in engine."), *ClassName);

			// Try to suggest a close match from the alias table
			const TMap<FString, FString>& Aliases = GetClassAliasMap();
			for (const auto& AliasPair : Aliases)
			{
				if (AliasPair.Key.Contains(ClassName) || ClassName.Contains(AliasPair.Key))
				{
					Issue.Suggestion = FString::Printf(TEXT("Did you mean '%s'?"), *AliasPair.Value);
					break;
				}
			}
			Result.Issues.Add(MoveTemp(Issue));
		}
	}

	// Validate components
	for (const auto& Pair : ArchitectResult.Components)
	{
		const FString& AssetName = Pair.Key;
		for (const auto& CompPair : Pair.Value)
		{
			const FString& CompClassName = CompPair.Value;
			if (CompClassName.IsEmpty())
			{
				continue;
			}

			UClass* Resolved = TryResolveComponentClass(CompClassName);
			if (!Resolved)
			{
				FOliveValidatorResult::FValidationIssue Issue;
				Issue.AssetName = AssetName;
				Issue.Category = TEXT("Component");
				Issue.Value = CompClassName;

				// Try resolving as a regular class to give specific feedback
				UClass* AsRegular = TryResolveClass(CompClassName);
				if (AsRegular)
				{
					Issue.Message = FString::Printf(
						TEXT("'%s' is not a UActorComponent subclass."),
						*CompClassName);
				}
				else
				{
					Issue.Message = FString::Printf(
						TEXT("Component class '%s' not found."),
						*CompClassName);
				}

				// Suggest aliased component
				const TMap<FString, FString>& Aliases = GetClassAliasMap();
				for (const auto& AliasPair : Aliases)
				{
					if (AliasPair.Key.Contains(CompClassName) || CompClassName.Contains(AliasPair.Key))
					{
						// Verify the alias is actually a component
						UClass* AliasResolved = TryResolveComponentClass(AliasPair.Value);
						if (AliasResolved)
						{
							Issue.Suggestion = FString::Printf(TEXT("Did you mean '%s'?"), *AliasPair.Value);
							break;
						}
					}
				}
				Result.Issues.Add(MoveTemp(Issue));
			}
		}
	}

	// Validate interfaces
	for (const auto& Pair : ArchitectResult.Interfaces)
	{
		const FString& AssetName = Pair.Key;
		for (const FString& InterfaceName : Pair.Value)
		{
			if (InterfaceName.IsEmpty())
			{
				continue;
			}

			if (!IsValidInterface(InterfaceName))
			{
				FOliveValidatorResult::FValidationIssue Issue;
				Issue.AssetName = AssetName;
				Issue.Category = TEXT("Interface");
				Issue.Value = InterfaceName;
				Issue.Message = FString::Printf(
					TEXT("Interface '%s' not found. It may need to be created during the build."),
					*InterfaceName);
				// Interfaces are non-blocking: they might be created as part of the build
				Result.Issues.Add(MoveTemp(Issue));
			}
		}
	}

	// Validate @-referenced modify targets: asset must exist
	for (const FString& AssetName : ArchitectResult.AssetOrder)
	{
		if (AssetName.StartsWith(TEXT("@")))
		{
			// The asset name after @ should be searchable
			FString SearchName = AssetName.Mid(1);

			// Search project index
			TArray<FOliveAssetInfo> Results = FOliveProjectIndex::Get().SearchAssets(SearchName, 1);
			if (Results.Num() == 0)
			{
				FOliveValidatorResult::FValidationIssue Issue;
				Issue.AssetName = AssetName;
				Issue.Category = TEXT("Modify Target");
				Issue.Value = SearchName;
				Issue.Message = FString::Printf(
					TEXT("Existing asset '%s' not found in project. Cannot modify a non-existent asset."),
					*SearchName);
				// This is a blocking issue -- cannot modify what doesn't exist
				Result.bHasBlockingIssues = true;
				Result.Issues.Add(MoveTemp(Issue));
			}
		}
	}

	Result.ElapsedSeconds = FPlatformTime::Seconds() - StartTime;
	return Result;
}

// ---------------------------------------------------------------------------
// CLI-Optimized Pipeline
// ---------------------------------------------------------------------------

bool FOliveAgentPipeline::IsCLIOnlyMode() const
{
	const UOliveAISettings* Settings = UOliveAISettings::Get();
	if (!Settings)
	{
		return false;
	}

	// If per-agent models are configured, check whether any agent has API access.
	// Even one HTTP-capable agent means we should use the full pipeline for that agent.
	if (Settings->bCustomizeAgentModels)
	{
		// Probe representative roles to see if any tier resolves to HTTP
		static const EOliveAgentRole RolesToProbe[] = {
			EOliveAgentRole::Router,
			EOliveAgentRole::Architect
		};

		for (EOliveAgentRole Role : RolesToProbe)
		{
			const FOliveAgentModelConfig AgentConfig = Settings->GetAgentModelConfig(Role);
			if (AgentConfig.bIsValid && !AgentConfig.bIsCLIFallback)
			{
				return false;  // At least one agent has HTTP access
			}
		}
	}

	// If the main provider is HTTP-based, Tier 2 will work for all agents
	if (Settings->Provider != EOliveAIProvider::ClaudeCode
		&& Settings->Provider != EOliveAIProvider::Codex)
	{
		return false;
	}

	// All tiers fall through to CLI --print
	return true;
}

FOliveAgentPipelineResult FOliveAgentPipeline::ExecuteCLIPath(
	const FString& UserMessage,
	const TArray<FString>& ContextAssetPaths)
{
	FOliveAgentPipelineResult Result;
	const double PipelineStartTime = FPlatformTime::Seconds();

	UE_LOG(LogOliveAgentPipeline, Log,
		TEXT("CLI pipeline starting for message: \"%s\" (%d context assets)"),
		*UserMessage.Left(100), ContextAssetPaths.Num());

	// --- Skip Router: default to Moderate ---
	// Saves one CLI cold start (~4-5s). Moderate is the safe middle ground --
	// it enables Researcher-level analysis (embedded in Planner) without being excessive.
	Result.Router.Complexity = EOliveTaskComplexity::Moderate;
	Result.Router.Reasoning = TEXT("CLI mode: Router skipped, defaulted to Moderate");
	Result.Router.bSuccess = true;
	Result.Router.ElapsedSeconds = 0.0;

	UE_LOG(LogOliveAgentPipeline, Log,
		TEXT("  Router: SKIPPED (CLI mode, defaulted to Moderate)"));

	// --- Stage 1: Scout (pure C++ -- zero LLM calls) ---
	// Template discovery + content auto-load + project index keyword search.
	// The LLM ranking pass (Part 3 of RunScout) is skipped entirely.
	{
		const double ScoutStart = FPlatformTime::Seconds();

		// Part 1: Template discovery pass (pure C++)
		FOliveDiscoveryResult DiscoveryResult = FOliveUtilityModel::RunDiscoveryPass(UserMessage, 8);
		Result.Scout.DiscoveryBlock = FOliveUtilityModel::FormatDiscoveryForPrompt(DiscoveryResult);

		// Part 1.5: Build structural overviews + reference list from top library matches.
		// GetTemplateOverview() reads from in-memory metadata -- instant, no disk I/O.
		{
			const FOliveLibraryIndex& LibIndex = FOliveTemplateSystem::Get().GetLibraryIndex();

			if (LibIndex.IsInitialized())
			{
				static constexpr int32 MAX_OVERVIEW_TEMPLATES = 3;
				int32 TemplatesLoaded = 0;
				FString OverviewBlock;

				for (const FOliveDiscoveryEntry& Entry : DiscoveryResult.Entries)
				{
					if (TemplatesLoaded >= MAX_OVERVIEW_TEMPLATES)
					{
						break;
					}

					// Only load library templates (factory/reference don't have structural metadata)
					if (Entry.SourceType != TEXT("library"))
					{
						continue;
					}

					FString Overview = LibIndex.GetTemplateOverview(Entry.TemplateId);
					if (Overview.IsEmpty())
					{
						continue;
					}

					if (OverviewBlock.IsEmpty())
					{
						OverviewBlock += TEXT("## Template Architecture Reference\n\n");
						OverviewBlock += TEXT("Structural overviews of relevant library templates.\n\n");
					}

					OverviewBlock += Overview;
					OverviewBlock += TEXT("\n");
					TemplatesLoaded++;

					// Build reference entry for Builder
					if (Entry.MatchedFunctions.Num() > 0)
					{
						FOliveTemplateReference Ref;
						Ref.TemplateId = Entry.TemplateId;
						Ref.DisplayName = Entry.DisplayName;
						Ref.ParentClass = Entry.ParentClass;
						Ref.MatchedFunctions = Entry.MatchedFunctions;
						Result.Scout.TemplateReferences.Add(MoveTemp(Ref));
					}
				}

				Result.Scout.TemplateOverviews = OverviewBlock;

				if (!OverviewBlock.IsEmpty())
				{
					UE_LOG(LogOliveAgentPipeline, Log,
						TEXT("  Scout (CLI): loaded %d template overviews, %d chars, %d references"),
						TemplatesLoaded, OverviewBlock.Len(), Result.Scout.TemplateReferences.Num());
				}
			}
		}

		// Part 2: Project index search (pure C++, no LLM ranking)
		// Include @-mentioned context assets with explicit relevance tag
		for (const FString& ContextPath : ContextAssetPaths)
		{
			TOptional<FOliveAssetInfo> AssetInfo = FOliveProjectIndex::Get().GetAssetByPath(ContextPath);
			if (AssetInfo.IsSet())
			{
				FOliveScoutResult::FAssetEntry Entry;
				Entry.Path = AssetInfo->Path;
				Entry.AssetClass = AssetInfo->AssetClass.ToString();
				Entry.Relevance = TEXT("explicitly referenced by user");
				Result.Scout.RelevantAssets.Add(MoveTemp(Entry));
			}
		}

		// Keyword search for additional related assets (no LLM ranking, just top hits)
		TArray<FString> Keywords = FOliveUtilityModel::ExtractSearchKeywords(UserMessage, 8);
		for (const FString& Keyword : Keywords)
		{
			TArray<FOliveAssetInfo> SearchResults = FOliveProjectIndex::Get().SearchAssets(Keyword, 5);
			for (const FOliveAssetInfo& AssetInfo : SearchResults)
			{
				// Avoid duplicates
				bool bAlreadyAdded = false;
				for (const auto& Existing : Result.Scout.RelevantAssets)
				{
					if (Existing.Path == AssetInfo.Path)
					{
						bAlreadyAdded = true;
						break;
					}
				}
				if (bAlreadyAdded)
				{
					continue;
				}

				FOliveScoutResult::FAssetEntry Entry;
				Entry.Path = AssetInfo.Path;
				Entry.AssetClass = AssetInfo.AssetClass.ToString();
				Entry.Relevance = TEXT("keyword match");
				Result.Scout.RelevantAssets.Add(MoveTemp(Entry));

				if (Result.Scout.RelevantAssets.Num() >= 10)
				{
					break;
				}
			}

			if (Result.Scout.RelevantAssets.Num() >= 10)
			{
				break;
			}
		}

		Result.Scout.bSuccess = true;  // Pure C++, always succeeds
		Result.Scout.ElapsedSeconds = FPlatformTime::Seconds() - ScoutStart;

		UE_LOG(LogOliveAgentPipeline, Log,
			TEXT("  Scout (CLI, no LLM): %d assets, discovery %d chars, template overviews %d chars, %d refs (%.1fs)"),
			Result.Scout.RelevantAssets.Num(),
			Result.Scout.DiscoveryBlock.Len(),
			Result.Scout.TemplateOverviews.Len(),
			Result.Scout.TemplateReferences.Num(),
			Result.Scout.ElapsedSeconds);
	}

	// --- Stage 2: Planner (MCP-enabled if possible, otherwise single-shot) ---
	{
		// Try MCP-enabled Planner first (reads templates on demand via tool calls).
		// Falls back to single-shot if MCP server isn't running or CLI not available.
		if (FOliveMCPServer::Get().IsRunning() && FOliveClaudeCodeProvider::IsClaudeCodeInstalled())
		{
			Result.Architect = RunPlannerWithTools(UserMessage, Result.Scout, ContextAssetPaths);
		}

		// Fallback: single-shot Planner with all template data inlined
		if (!Result.Architect.bSuccess)
		{
			UE_LOG(LogOliveAgentPipeline, Log,
				TEXT("  Planner (MCP) %s, falling back to single-shot"),
				Result.Architect.ElapsedSeconds > 0.0 ? TEXT("failed") : TEXT("skipped"));
			Result.Architect = RunPlanner(UserMessage, Result.Scout, ContextAssetPaths);
		}

		UE_LOG(LogOliveAgentPipeline, Log,
			TEXT("  Planner (CLI): %s, %d assets in plan, %d chars (%.1fs)"),
			Result.Architect.bSuccess ? TEXT("success") : TEXT("FAILED"),
			Result.Architect.AssetOrder.Num(),
			Result.Architect.BuildPlan.Len(),
			Result.Architect.ElapsedSeconds);
	}

	// --- Stage 3: Validator (pure C++, no LLM) ---
	if (Result.Architect.bSuccess)
	{
		Result.Validator = RunValidator(Result.Architect);

		UE_LOG(LogOliveAgentPipeline, Log,
			TEXT("  Validator: %d issues, blocking=%s (%.3fs)"),
			Result.Validator.Issues.Num(),
			Result.Validator.bHasBlockingIssues ? TEXT("YES") : TEXT("no"),
			Result.Validator.ElapsedSeconds);
	}

	// The pipeline is valid if the Planner succeeded or Scout produced discovery results
	Result.bValid = Result.Architect.bSuccess
		|| !Result.Scout.DiscoveryBlock.IsEmpty()
		|| Result.Scout.RelevantAssets.Num() > 0;

	Result.TotalElapsedSeconds = FPlatformTime::Seconds() - PipelineStartTime;

	UE_LOG(LogOliveAgentPipeline, Log,
		TEXT("CLI pipeline complete: valid=%s, %.1fs total"),
		Result.bValid ? TEXT("true") : TEXT("false"),
		Result.TotalElapsedSeconds);

	return Result;
}

FOliveArchitectResult FOliveAgentPipeline::RunPlanner(
	const FString& UserMessage,
	const FOliveScoutResult& ScoutResult,
	const TArray<FString>& ContextAssetPaths)
{
	FOliveArchitectResult Result;
	const double StartTime = FPlatformTime::Seconds();

	// Build user prompt with ALL context (what Router+Scout+Researcher+Architect
	// normally gather across 4 separate agents)
	FString PlannerUserPrompt;
	PlannerUserPrompt.Reserve(8192);

	// Task description
	PlannerUserPrompt += TEXT("## Task\n\n");
	PlannerUserPrompt += UserMessage;
	PlannerUserPrompt += TEXT("\n\n**Complexity**: Moderate\n");

	// Existing assets with inline IR data (replaces Researcher's analysis)
	if (ScoutResult.RelevantAssets.Num() > 0)
	{
		PlannerUserPrompt += TEXT("\n## Existing Assets\n\n");

		const int32 AssetsToAnalyze = FMath::Min(ScoutResult.RelevantAssets.Num(), MAX_RESEARCHER_ASSETS);
		for (int32 i = 0; i < AssetsToAnalyze; i++)
		{
			const FOliveScoutResult::FAssetEntry& Asset = ScoutResult.RelevantAssets[i];
			PlannerUserPrompt += TEXT("- ");
			PlannerUserPrompt += Asset.Path;
			PlannerUserPrompt += TEXT(" (") + Asset.AssetClass + TEXT(")");
			if (!Asset.Relevance.IsEmpty())
			{
				PlannerUserPrompt += TEXT(" -- ") + Asset.Relevance;
			}
			PlannerUserPrompt += TEXT("\n");

			// Load Blueprint IR inline (what Researcher normally does in a separate LLM call)
			if (Asset.AssetClass.Contains(TEXT("Blueprint")))
			{
				TOptional<FOliveIRBlueprint> IR = FOliveBlueprintReader::Get().ReadBlueprintSummary(Asset.Path);
				if (IR.IsSet())
				{
					const FOliveIRBlueprint& BP = IR.GetValue();
					PlannerUserPrompt += TEXT("  Parent: ") + BP.ParentClass.Name + TEXT("\n");

					// Variables (skip SCS-internal)
					{
						TArray<FString> VarEntries;
						for (const FOliveIRVariable& Var : BP.Variables)
						{
							if (!Var.Name.StartsWith(TEXT("DefaultComponent_")))
							{
								VarEntries.Add(Var.Name + TEXT(" (") + Var.Type.GetDisplayName() + TEXT(")"));
							}
						}
						if (VarEntries.Num() > 0)
						{
							PlannerUserPrompt += TEXT("  Variables: ") + FString::Join(VarEntries, TEXT(", ")) + TEXT("\n");
						}
					}

					// Components
					if (BP.Components.Num() > 0)
					{
						TArray<FString> CompEntries;
						for (const FOliveIRComponent& Comp : BP.Components)
						{
							CompEntries.Add(Comp.Name + TEXT(" (") + Comp.ComponentClass + TEXT(")"));
						}
						PlannerUserPrompt += TEXT("  Components: ") + FString::Join(CompEntries, TEXT(", ")) + TEXT("\n");
					}

					// Functions
					if (BP.FunctionNames.Num() > 0)
					{
						PlannerUserPrompt += TEXT("  Functions: ") + FString::Join(BP.FunctionNames, TEXT(", ")) + TEXT("\n");
					}

					// Interfaces
					if (BP.Interfaces.Num() > 0)
					{
						TArray<FString> IntEntries;
						for (const auto& Iface : BP.Interfaces)
						{
							IntEntries.Add(Iface.Name);
						}
						PlannerUserPrompt += TEXT("  Interfaces: ") + FString::Join(IntEntries, TEXT(", ")) + TEXT("\n");
					}

					// Event dispatchers
					if (BP.EventDispatchers.Num() > 0)
					{
						TArray<FString> DispEntries;
						for (const auto& Disp : BP.EventDispatchers)
						{
							DispEntries.Add(Disp.Name);
						}
						PlannerUserPrompt += TEXT("  Dispatchers: ") + FString::Join(DispEntries, TEXT(", ")) + TEXT("\n");
					}
				}
			}
		}
	}

	// Template discovery block
	if (!ScoutResult.DiscoveryBlock.IsEmpty())
	{
		PlannerUserPrompt += TEXT("\n");
		PlannerUserPrompt += ScoutResult.DiscoveryBlock;
		PlannerUserPrompt += TEXT("\n");
	}

	// Template structural overviews (architecture-level, no node graph data)
	if (!ScoutResult.TemplateOverviews.IsEmpty())
	{
		PlannerUserPrompt += TEXT("\n");
		PlannerUserPrompt += ScoutResult.TemplateOverviews;
		PlannerUserPrompt += TEXT("\n");
	}

	// Inject comprehensive knowledge packs from disk (same knowledge the Builder gets)
	PlannerUserPrompt += BuildPlannerKnowledgeBlock();

	// Send to LLM (uses Architect role for model config -- same tier resolution)
	FString SystemPrompt = BuildPlannerSystemPrompt();
	FString Response;
	FString Error;

	if (SendAgentCompletion(EOliveAgentRole::Architect, SystemPrompt, PlannerUserPrompt, Response, Error))
	{
		Result.BuildPlan = Response;
		Result.bSuccess = true;

		// Parse the plan to extract structured data for the Validator
		ParseBuildPlan(Result.BuildPlan, Result);

		// Build compact API reference for components mentioned in the plan
		Result.ComponentAPIMap = BuildComponentAPIMap(Result);
	}
	else
	{
		UE_LOG(LogOliveAgentPipeline, Warning,
			TEXT("Planner LLM call failed: %s. No Build Plan produced."), *Error);
		Result.bSuccess = false;
	}

	Result.ElapsedSeconds = FPlatformTime::Seconds() - StartTime;
	return Result;
}

// ---------------------------------------------------------------------------
// RunPlannerWithTools -- MCP-enabled Planner
// ---------------------------------------------------------------------------

FOliveArchitectResult FOliveAgentPipeline::RunPlannerWithTools(
	const FString& UserMessage,
	const FOliveScoutResult& ScoutResult,
	const TArray<FString>& ContextAssetPaths)
{
	FOliveArchitectResult Result;
	const double StartTime = FPlatformTime::Seconds();

	// --- Build user prompt (compact: no template overviews, just headers) ---
	FString PlannerUserPrompt;
	PlannerUserPrompt.Reserve(4096);

	// Task description
	PlannerUserPrompt += TEXT("## Task\n\n");
	PlannerUserPrompt += UserMessage;
	PlannerUserPrompt += TEXT("\n\n**Complexity**: Moderate\n");

	// Existing assets with inline IR data (same as RunPlanner)
	if (ScoutResult.RelevantAssets.Num() > 0)
	{
		PlannerUserPrompt += TEXT("\n## Existing Assets\n\n");

		const int32 AssetsToAnalyze = FMath::Min(ScoutResult.RelevantAssets.Num(), MAX_RESEARCHER_ASSETS);
		for (int32 i = 0; i < AssetsToAnalyze; i++)
		{
			const FOliveScoutResult::FAssetEntry& Asset = ScoutResult.RelevantAssets[i];
			PlannerUserPrompt += TEXT("- ");
			PlannerUserPrompt += Asset.Path;
			PlannerUserPrompt += TEXT(" (") + Asset.AssetClass + TEXT(")");
			if (!Asset.Relevance.IsEmpty())
			{
				PlannerUserPrompt += TEXT(" -- ") + Asset.Relevance;
			}
			PlannerUserPrompt += TEXT("\n");

			// Load Blueprint IR inline
			if (Asset.AssetClass.Contains(TEXT("Blueprint")))
			{
				TOptional<FOliveIRBlueprint> IR = FOliveBlueprintReader::Get().ReadBlueprintSummary(Asset.Path);
				if (IR.IsSet())
				{
					const FOliveIRBlueprint& BP = IR.GetValue();
					PlannerUserPrompt += TEXT("  Parent: ") + BP.ParentClass.Name + TEXT("\n");

					// Variables (skip SCS-internal)
					{
						TArray<FString> VarEntries;
						for (const FOliveIRVariable& Var : BP.Variables)
						{
							if (!Var.Name.StartsWith(TEXT("DefaultComponent_")))
							{
								VarEntries.Add(Var.Name + TEXT(" (") + Var.Type.GetDisplayName() + TEXT(")"));
							}
						}
						if (VarEntries.Num() > 0)
						{
							PlannerUserPrompt += TEXT("  Variables: ") + FString::Join(VarEntries, TEXT(", ")) + TEXT("\n");
						}
					}

					// Components
					if (BP.Components.Num() > 0)
					{
						TArray<FString> CompEntries;
						for (const FOliveIRComponent& Comp : BP.Components)
						{
							CompEntries.Add(Comp.Name + TEXT(" (") + Comp.ComponentClass + TEXT(")"));
						}
						PlannerUserPrompt += TEXT("  Components: ") + FString::Join(CompEntries, TEXT(", ")) + TEXT("\n");
					}

					// Functions
					if (BP.FunctionNames.Num() > 0)
					{
						PlannerUserPrompt += TEXT("  Functions: ") + FString::Join(BP.FunctionNames, TEXT(", ")) + TEXT("\n");
					}

					// Interfaces
					if (BP.Interfaces.Num() > 0)
					{
						TArray<FString> IntEntries;
						for (const auto& Iface : BP.Interfaces)
						{
							IntEntries.Add(Iface.Name);
						}
						PlannerUserPrompt += TEXT("  Interfaces: ") + FString::Join(IntEntries, TEXT(", ")) + TEXT("\n");
					}

					// Event dispatchers
					if (BP.EventDispatchers.Num() > 0)
					{
						TArray<FString> DispEntries;
						for (const auto& Disp : BP.EventDispatchers)
						{
							DispEntries.Add(Disp.Name);
						}
						PlannerUserPrompt += TEXT("  Dispatchers: ") + FString::Join(DispEntries, TEXT(", ")) + TEXT("\n");
					}
				}
			}
		}
	}

	// Template discovery block (compact catalog info)
	if (!ScoutResult.DiscoveryBlock.IsEmpty())
	{
		PlannerUserPrompt += TEXT("\n");
		PlannerUserPrompt += ScoutResult.DiscoveryBlock;
		PlannerUserPrompt += TEXT("\n");
	}

	// Template references as compact headers (~300-400 chars each).
	// NOTE: TemplateOverviews is intentionally NOT included. The Planner
	// fetches full details on-demand via get_template tool calls.
	if (ScoutResult.TemplateReferences.Num() > 0)
	{
		PlannerUserPrompt += TEXT("\n## Available Templates\n\n");
		PlannerUserPrompt += TEXT("Read these with `blueprint.get_template` before writing the Build Plan.\n\n");
		for (const FOliveTemplateReference& Ref : ScoutResult.TemplateReferences)
		{
			PlannerUserPrompt += TEXT("- **") + Ref.DisplayName + TEXT("** (`") + Ref.TemplateId + TEXT("`)");
			if (!Ref.ParentClass.IsEmpty())
			{
				PlannerUserPrompt += TEXT(" -- Parent: ") + Ref.ParentClass;
			}
			if (Ref.MatchedFunctions.Num() > 0)
			{
				PlannerUserPrompt += TEXT("\n  Matched to your task: ") + FString::Join(Ref.MatchedFunctions, TEXT(", "));
			}
			PlannerUserPrompt += TEXT("\n");
		}
	}

	// Inject comprehensive knowledge packs from disk (same knowledge the Builder gets)
	PlannerUserPrompt += BuildPlannerKnowledgeBlock();

	// --- Build system prompt ---
	FString SystemPrompt = BuildPlannerSystemPrompt();

	// Append tool usage instructions
	SystemPrompt += TEXT("\n\n## Available Tools\n\n");
	SystemPrompt += TEXT("You have access to these read-only MCP tools:\n");
	SystemPrompt += TEXT("- `blueprint.get_template(template_id, pattern?)` -- Read a template's structure or a specific function's node graph\n");
	SystemPrompt += TEXT("- `blueprint.list_templates(query?)` -- Search for templates by keyword\n");
	SystemPrompt += TEXT("- `blueprint.describe(path)` -- Read an existing Blueprint's structure\n");
	SystemPrompt += TEXT("- `olive.get_recipe(query)` -- Get tested wiring patterns for interfaces, dispatchers, overlap events, timelines, and other common Blueprint patterns\n\n");

	if (ScoutResult.TemplateReferences.Num() > 0)
	{
		SystemPrompt += TEXT("If templates are listed in the prompt, call `blueprint.get_template` on each matched template ");
		SystemPrompt += TEXT("to study their function implementations before writing the Build Plan. ");
		SystemPrompt += TEXT("Base your function descriptions on real patterns you observe.\n");
	}
	else
	{
		SystemPrompt += TEXT("If no templates are listed, design the plan from your Unreal Engine knowledge.\n");
	}
	SystemPrompt += TEXT("When done researching, output ONLY the Build Plan (starting with ## Build Plan header).\n");

	// --- Set MCP tool filter (only read tools) ---
	// Use exact tool names as prefixes. These are unique enough that no other
	// tool name starts with any of them (e.g., nothing starts with "blueprint.describe"
	// other than "blueprint.describe" itself).
	const TSet<FString> PlannerToolPrefixes = {
		TEXT("blueprint.get_template"),
		TEXT("blueprint.list_templates"),
		TEXT("blueprint.describe"),
		TEXT("olive.get_recipe")
	};
	FOliveMCPServer::Get().SetToolFilter(PlannerToolPrefixes);

	// --- Set up sandbox directory ---
	const FString SandboxDir = SetupPlannerSandbox();
	if (SandboxDir.IsEmpty())
	{
		UE_LOG(LogOliveAgentPipeline, Warning, TEXT("  Planner (MCP): failed to create sandbox"));
		FOliveMCPServer::Get().ClearToolFilter();
		return Result;  // bSuccess = false triggers fallback
	}

	// --- Spawn CLI process ---
	const FString ClaudePath = FOliveClaudeCodeProvider::GetClaudeExecutablePath();
	const bool bIsJs = ClaudePath.EndsWith(TEXT(".js")) || ClaudePath.EndsWith(TEXT(".mjs"));

	// Key differences from SendAgentCompletion Tier 3:
	// --max-turns 15: enough to read 3-5 templates, not enough for adventures
	// --output-format stream-json: parseable output for text extraction
	static constexpr int32 PLANNER_MAX_TURNS = 15;
	const FString BaseArgs = FString::Printf(
		TEXT("--print --output-format stream-json --verbose --dangerously-skip-permissions --max-turns %d"),
		PLANNER_MAX_TURNS);

	FString Executable;
	FString Args;
	if (bIsJs)
	{
		Executable = TEXT("node");
		Args = FString::Printf(TEXT("\"%s\" %s"), *ClaudePath, *BaseArgs);
	}
	else
	{
		Executable = ClaudePath;
		Args = BaseArgs;
	}

	// Combine system + user prompt for stdin delivery
	FString StdinContent = SystemPrompt + TEXT("\n\n---\n\n") + PlannerUserPrompt;

	// Create pipes
	void* StdoutRead = nullptr;
	void* StdoutWrite = nullptr;
	void* StdinRead = nullptr;
	void* StdinWrite = nullptr;

	bool bPipesOk = FPlatformProcess::CreatePipe(StdoutRead, StdoutWrite)
		&& FPlatformProcess::CreatePipe(StdinRead, StdinWrite, /*bWritePipeLocal=*/true);

	if (!bPipesOk)
	{
		if (StdoutRead)
		{
			FPlatformProcess::ClosePipe(StdoutRead, StdoutWrite);
		}
		UE_LOG(LogOliveAgentPipeline, Warning, TEXT("  Planner (MCP): failed to create pipes"));
		FOliveMCPServer::Get().ClearToolFilter();
		return Result;
	}

	uint32 ProcessId = 0;
	FProcHandle ProcHandle = FPlatformProcess::CreateProc(
		*Executable, *Args,
		false, true, true,  // bLaunchDetached, bLaunchHidden, bLaunchReallyHidden
		&ProcessId, 0,
		*SandboxDir,        // Working directory with .mcp.json
		StdoutWrite,        // child writes stdout here
		StdinRead           // child reads stdin from here
	);

	if (!ProcHandle.IsValid())
	{
		FPlatformProcess::ClosePipe(StdoutRead, StdoutWrite);
		FPlatformProcess::ClosePipe(StdinRead, StdinWrite);
		UE_LOG(LogOliveAgentPipeline, Warning, TEXT("  Planner (MCP): failed to spawn CLI process"));
		FOliveMCPServer::Get().ClearToolFilter();
		return Result;
	}

	// Close pipe ends we don't use
	FPlatformProcess::ClosePipe(nullptr, StdoutWrite);
	StdoutWrite = nullptr;
	FPlatformProcess::ClosePipe(StdinRead, nullptr);
	StdinRead = nullptr;

	// Write prompt via stdin, then close to signal EOF
	FPlatformProcess::WritePipe(StdinWrite, StdinContent);
	FPlatformProcess::ClosePipe(nullptr, StdinWrite);
	StdinWrite = nullptr;

	UE_LOG(LogOliveAgentPipeline, Log,
		TEXT("  Planner (MCP): launched with %d char prompt, max %d turns, PID=%u"),
		StdinContent.Len(), PLANNER_MAX_TURNS, ProcessId);

	// --- Read loop with tick-pumping ---
	// Unlike SendAgentCompletion Tier 3 which just sleeps, this pumps the game
	// thread ticker so the MCP server can process tool calls during our wait.
	// Tool call flow: Claude -> mcp-bridge.js -> HTTP -> MCP server -> Tick() handler
	static constexpr double PLANNER_TIMEOUT_SECONDS = 180.0;

	FString RawOutput;
	bool bTimedOut = false;

	while (FPlatformProcess::IsProcRunning(ProcHandle))
	{
		// Read any available stdout
		RawOutput += FPlatformProcess::ReadPipe(StdoutRead);

		// Check timeout
		if (FPlatformTime::Seconds() - StartTime >= PLANNER_TIMEOUT_SECONDS)
		{
			FPlatformProcess::TerminateProc(ProcHandle, true);
			bTimedOut = true;
			UE_LOG(LogOliveAgentPipeline, Warning,
				TEXT("  Planner (MCP): timed out after %.0fs"), PLANNER_TIMEOUT_SECONDS);
			break;
		}

		// Pump game thread ticker (processes MCP HTTP listener)
		FTSTicker::GetCoreTicker().Tick(0.01f);
		// Drain game thread task queue — this is where AsyncTask(GameThread) lambdas
		// from the MCP server's tool dispatch actually execute.
		// Guard: ProcessThreadUntilIdle asserts if called recursively (e.g., when
		// we're inside an AsyncTask(GameThread) lambda from auto-continue).
		if (IsInGameThread() && !FTaskGraphInterface::Get().IsThreadProcessingTasks(ENamedThreads::GameThread))
		{
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		}
		FPlatformProcess::Sleep(0.01f);
	}

	// Read remaining output after process exits
	if (!bTimedOut)
	{
		RawOutput += FPlatformProcess::ReadPipe(StdoutRead);
	}
	FPlatformProcess::ClosePipe(StdoutRead, nullptr);

	int32 ReturnCode = -1;
	FPlatformProcess::GetProcReturnCode(ProcHandle, &ReturnCode);
	FPlatformProcess::CloseProc(ProcHandle);

	// --- Clear tool filter ---
	FOliveMCPServer::Get().ClearToolFilter();

	// --- Extract final text from stream-json ---
	if (ReturnCode == 0 || !RawOutput.IsEmpty())
	{
		FString PlanText = ParseStreamJsonFinalText(RawOutput);
		if (!PlanText.IsEmpty())
		{
			Result.BuildPlan = PlanText;
			Result.bSuccess = true;
			ParseBuildPlan(Result.BuildPlan, Result);

			// Build compact API reference for components mentioned in the plan
			Result.ComponentAPIMap = BuildComponentAPIMap(Result);

			UE_LOG(LogOliveAgentPipeline, Log,
				TEXT("  Planner (MCP): success, %d char plan, %d assets, %.1fs"),
				Result.BuildPlan.Len(), Result.AssetOrder.Num(),
				FPlatformTime::Seconds() - StartTime);
		}
	}

	if (!Result.bSuccess)
	{
		UE_LOG(LogOliveAgentPipeline, Warning,
			TEXT("  Planner (MCP): failed (code=%d, output=%d chars, timedOut=%s)"),
			ReturnCode, RawOutput.Len(), bTimedOut ? TEXT("true") : TEXT("false"));
	}

	Result.ElapsedSeconds = FPlatformTime::Seconds() - StartTime;
	return Result;
}

// ---------------------------------------------------------------------------
// SetupPlannerSandbox
// ---------------------------------------------------------------------------

FString FOliveAgentPipeline::SetupPlannerSandbox()
{
	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString SandboxDir = FPaths::Combine(ProjectDir, TEXT("Saved/OliveAI/PlannerSandbox"));

	if (!IFileManager::Get().MakeDirectory(*SandboxDir, /*Tree=*/true))
	{
		UE_LOG(LogOliveAgentPipeline, Warning,
			TEXT("SetupPlannerSandbox: failed to create directory: %s"), *SandboxDir);
		return FString();
	}

	// .mcp.json -- same format as FOliveClaudeCodeProvider::WriteProviderSpecificSandboxFiles
	const FString PluginDir = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("UE_Olive_AI_Studio")));
	const FString BridgePath = FPaths::Combine(PluginDir, TEXT("mcp-bridge.js"));
	FString BridgePathJson = BridgePath.Replace(TEXT("\\"), TEXT("/"));

	const FString McpConfig = FString::Printf(
		TEXT("{\n")
		TEXT("  \"mcpServers\": {\n")
		TEXT("    \"olive-ai-studio\": {\n")
		TEXT("      \"command\": \"node\",\n")
		TEXT("      \"args\": [\"%s\"]\n")
		TEXT("    }\n")
		TEXT("  }\n")
		TEXT("}\n"),
		*BridgePathJson
	);

	const FString McpConfigPath = FPaths::Combine(SandboxDir, TEXT(".mcp.json"));
	if (!FFileHelper::SaveStringToFile(McpConfig, *McpConfigPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogOliveAgentPipeline, Warning,
			TEXT("SetupPlannerSandbox: failed to write .mcp.json to %s"), *McpConfigPath);
		return FString();
	}

	// CLAUDE.md -- minimal, Planner-specific
	const FString ClaudeMd =
		TEXT("# Planner Agent\n\n")
		TEXT("You are an Unreal Engine Blueprint architect. Your job is to research templates ")
		TEXT("and produce a Build Plan.\n\n")
		TEXT("## Rules\n")
		TEXT("- Use ONLY the MCP tools provided (blueprint.get_template, blueprint.list_templates, blueprint.describe)\n")
		TEXT("- Do NOT create or modify any files\n")
		TEXT("- Do NOT use bash, read, write, or any other tools\n")
		TEXT("- After researching, output the Build Plan as your final text response\n");

	const FString ClaudeMdPath = FPaths::Combine(SandboxDir, TEXT("CLAUDE.md"));
	if (!FFileHelper::SaveStringToFile(ClaudeMd, *ClaudeMdPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogOliveAgentPipeline, Warning,
			TEXT("SetupPlannerSandbox: failed to write CLAUDE.md to %s"), *ClaudeMdPath);
		return FString();
	}

	return SandboxDir;
}

// ---------------------------------------------------------------------------
// ParseStreamJsonFinalText
// ---------------------------------------------------------------------------

FString FOliveAgentPipeline::ParseStreamJsonFinalText(const FString& StreamOutput)
{
	TArray<FString> TextBlocks;
	FString CurrentBlock;

	TArray<FString> Lines;
	StreamOutput.ParseIntoArrayLines(Lines);

	for (const FString& Line : Lines)
	{
		FString Trimmed = Line.TrimStartAndEnd();
		if (Trimmed.IsEmpty() || !Trimmed.StartsWith(TEXT("{")))
		{
			continue;
		}

		TSharedPtr<FJsonObject> JsonObject;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
		if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
		{
			continue;
		}

		FString Type;
		if (!JsonObject->TryGetStringField(TEXT("type"), Type))
		{
			continue;
		}

		if (Type == TEXT("assistant"))
		{
			// Start of a new assistant message -- save any accumulated block
			if (!CurrentBlock.IsEmpty())
			{
				TextBlocks.Add(CurrentBlock);
				CurrentBlock.Empty();
			}

			// Extract text content from message.content[]
			const TSharedPtr<FJsonObject>* MessageObj = nullptr;
			if (JsonObject->TryGetObjectField(TEXT("message"), MessageObj))
			{
				const TArray<TSharedPtr<FJsonValue>>* ContentArray = nullptr;
				if ((*MessageObj)->TryGetArrayField(TEXT("content"), ContentArray))
				{
					for (const TSharedPtr<FJsonValue>& ContentValue : *ContentArray)
					{
						const TSharedPtr<FJsonObject>* ContentObj = nullptr;
						if (ContentValue->TryGetObject(ContentObj))
						{
							FString ContentType;
							if ((*ContentObj)->TryGetStringField(TEXT("type"), ContentType)
								&& ContentType == TEXT("text"))
							{
								FString TextValue;
								if ((*ContentObj)->TryGetStringField(TEXT("text"), TextValue))
								{
									CurrentBlock += TextValue;
								}
							}
						}
					}
				}
			}
		}
		else if (Type == TEXT("content_block_delta"))
		{
			// Streaming delta -- accumulate text from delta.text
			const TSharedPtr<FJsonObject>* DeltaObj = nullptr;
			if (JsonObject->TryGetObjectField(TEXT("delta"), DeltaObj))
			{
				FString DeltaType;
				if ((*DeltaObj)->TryGetStringField(TEXT("type"), DeltaType)
					&& DeltaType == TEXT("text_delta"))
				{
					FString TextValue;
					if ((*DeltaObj)->TryGetStringField(TEXT("text"), TextValue))
					{
						CurrentBlock += TextValue;
					}
				}
			}
		}
		else if (Type == TEXT("tool_use") || Type == TEXT("tool_call")
			|| Type == TEXT("content_block_start"))
		{
			// Tool call boundary -- check if this is a tool_use content block
			bool bIsToolBlock = (Type == TEXT("tool_use") || Type == TEXT("tool_call"));
			if (Type == TEXT("content_block_start"))
			{
				const TSharedPtr<FJsonObject>* ContentBlockObj = nullptr;
				if (JsonObject->TryGetObjectField(TEXT("content_block"), ContentBlockObj))
				{
					FString BlockType;
					if ((*ContentBlockObj)->TryGetStringField(TEXT("type"), BlockType)
						&& BlockType == TEXT("tool_use"))
					{
						bIsToolBlock = true;
					}
				}
			}

			if (bIsToolBlock && !CurrentBlock.IsEmpty())
			{
				TextBlocks.Add(CurrentBlock);
				CurrentBlock.Empty();
			}
		}
		// tool_result, result, message_stop, message_start, error -- skip
	}

	// Save final block
	if (!CurrentBlock.IsEmpty())
	{
		TextBlocks.Add(CurrentBlock);
	}

	if (TextBlocks.Num() == 0)
	{
		// No JSON parsed -- treat raw output as plain text (fallback for
		// non-stream-json output or if Claude outputs plain text)
		FString Fallback = StreamOutput.TrimStartAndEnd();
		return Fallback.IsEmpty() ? FString() : Fallback;
	}

	// Look for the block containing "## Build Plan" (search from the end)
	for (int32 i = TextBlocks.Num() - 1; i >= 0; --i)
	{
		int32 HeaderIdx = TextBlocks[i].Find(TEXT("## Build Plan"));
		if (HeaderIdx != INDEX_NONE)
		{
			// Extract from "## Build Plan" to end of block
			return TextBlocks[i].Mid(HeaderIdx).TrimStartAndEnd();
		}
	}

	// No "## Build Plan" header found -- return the last text block
	return TextBlocks.Last().TrimStartAndEnd();
}

// ---------------------------------------------------------------------------
// Prompt Builders
// ---------------------------------------------------------------------------

FString FOliveAgentPipeline::BuildRouterSystemPrompt()
{
	return TEXT(
		"You classify Unreal Engine Blueprint tasks by complexity.\n"
		"\n"
		"SIMPLE: Single asset, 1-3 functions, no inter-asset communication.\n"
		"  Examples: a health pickup, a spinning actor, a door with timeline.\n"
		"\n"
		"MODERATE: 2-3 assets OR single complex asset (5+ functions, components with state).\n"
		"  Examples: a weapon with projectile, a door with key, AI character with behavior tree.\n"
		"\n"
		"COMPLEX: 4+ assets, cross-system interactions, or requires event dispatchers between assets.\n"
		"  Examples: inventory system, full combat suite with melee/ranged/projectile/damage.\n"
		"\n"
		"Respond with EXACTLY one line:\n"
		"COMPLEXITY: [SIMPLE|MODERATE|COMPLEX]\n"
		"\n"
		"Then a second line with a brief reason (max 20 words).\n"
		"\n"
		"No other text."
	);
}

FString FOliveAgentPipeline::BuildScoutSystemPrompt()
{
	return TEXT(
		"You rank Unreal Engine project assets by relevance to a task.\n"
		"\n"
		"Given a list of existing assets (path + class) and a task description, "
		"output the TOP 5 most relevant assets. For each, explain in <10 words "
		"WHY it is relevant (e.g., \"parent class for weapon actors\", "
		"\"existing projectile to reference\").\n"
		"\n"
		"Format:\n"
		"1. /Game/Path/AssetName (ClassName) -- reason\n"
		"2. ...\n"
		"\n"
		"If none are relevant, output: NONE\n"
		"\n"
		"No other text."
	);
}

FString FOliveAgentPipeline::BuildResearcherSystemPrompt()
{
	return TEXT(
		"You analyze Unreal Engine Blueprint architecture for an AI Builder agent.\n"
		"\n"
		"Given Blueprint IR data (variables, components, functions, interfaces, dispatchers), "
		"produce a concise architectural summary that helps the Builder understand:\n"
		"\n"
		"1. The asset's role (what it does, its parent class)\n"
		"2. Public interface: functions other assets call, event dispatchers they bind to\n"
		"3. Component architecture: which components carry state, which handle collision\n"
		"4. Integration points: interfaces implemented, dispatch events exposed\n"
		"\n"
		"Format per asset:\n"
		"\n"
		"### AssetName\n"
		"- **Role**: One sentence\n"
		"- **Parent**: ClassName\n"
		"- **Public API**: Function1(params), Function2(params)\n"
		"- **Dispatchers**: OnDamage(float), OnDeath()\n"
		"- **Components**: CompName (UClass) -- purpose\n"
		"- **Interfaces**: IInterface1, IInterface2\n"
		"\n"
		"Keep each asset summary under 150 words. Focus on information the Builder "
		"needs to CREATE NEW assets that interact with these existing ones."
	);
}

FString FOliveAgentPipeline::BuildArchitectSystemPrompt()
{
	return TEXT(
		"You are an Unreal Engine Blueprint architect. Given a task description, "
		"existing asset context, and template references, produce a Build Plan.\n"
		"\n"
		"## Build Plan Format\n"
		"\n"
		"### Order\n"
		"1. BP_Name (create)\n"
		"2. @ExistingBP (modify)\n"
		"\n"
		"### BP_Name\n"
		"- **Action**: create\n"
		"- **Parent Class**: <real UE class, e.g., AActor, APawn, ACharacter, UActorComponent>\n"
		"- **Components**: VarName (UComponentClass) -- purpose\n"
		"- **Variables**: VarName (Type, default: value) -- purpose\n"
		"- **Event Dispatchers**: Name(ParamType Param, ...) -- purpose\n"
		"- **Interfaces**: UInterfaceName -- purpose\n"
		"- **Functions**: Name(Params) -> ReturnType -- natural language description of logic\n"
		"- **Events**: EventName [ComponentName if delegate] -- what happens\n"
		"\n"
		"### @ExistingBP\n"
		"- **Action**: modify\n"
		"- **Add Variables**: ...\n"
		"- **Add Functions**: ...\n"
		"- **Add Events**: ...\n"
		"\n"
		"### Interactions\n"
		"- How assets communicate (dispatchers, interfaces, direct calls)\n"
		"\n"
		"## Rules\n"
		"- Order is mandatory: assets must be listed in dependency order (referenced before referencing)\n"
		"- Use UE class names. Short names are fine (Actor, Character, SphereComponent) -- the Validator normalizes them\n"
		"- Function logic is natural language (the Builder translates to plan_json)\n"
		"- For delegate events, specify which component they belong to (e.g., OnComponentBeginOverlap [BoxComp])\n"
		"- Modify blocks (@prefix) only list CHANGES, not the full asset\n"
		"- If a function needs latent operations (Delay, Timeline), note it must be a Custom Event, not a Function\n"
		"- Keep function descriptions to 1-2 sentences\n"
		"- If Implementation Reference content is provided, study the function graph patterns "
		"(node types, wiring flow, variable usage) and base your function descriptions on "
		"those real patterns. Do not simplify to PrintString unless the user explicitly asks for stubs.\n"
		"- When describing function logic, reference specific patterns you observed "
		"(e.g., \"line trace -> branch on hit -> apply damage\" not \"deal damage to target\")"
	);
}

FString FOliveAgentPipeline::BuildReviewerSystemPrompt()
{
	return TEXT(
		"You review the output of an Unreal Engine Blueprint Builder against a Build Plan.\n"
		"\n"
		"Given:\n"
		"1. The original Build Plan (from the Architect)\n"
		"2. The current state of each asset (variables, components, functions, dispatchers, compile status)\n"
		"\n"
		"Compare the actual state to the plan. Report:\n"
		"\n"
		"SATISFIED: (if everything in the plan exists in the assets)\n"
		"\n"
		"OR:\n"
		"\n"
		"MISSING:\n"
		"- BP_Name: MissingFunction(params), MissingVariable, etc.\n"
		"- @ExistingBP: MissingEvent, etc.\n"
		"\n"
		"DEVIATIONS:\n"
		"- BP_Name: FunctionX has wrong signature (planned: X, actual: Y)\n"
		"\n"
		"CORRECTION:\n"
		"One paragraph telling the Builder exactly what to do next to complete the plan.\n"
		"\n"
		"Be precise. Only report genuinely missing items -- do not flag cosmetic differences "
		"(naming case, extra helper functions)."
	);
}

FString FOliveAgentPipeline::BuildPlannerSystemPrompt()
{
	return TEXT(
		"You are an Unreal Engine Blueprint architect and analyst. Given a task description, "
		"existing asset data (with Blueprint structure), and template references:\n"
		"\n"
		"1. First, analyze the architecture of any existing assets provided "
		"(parent classes, components, public interfaces, dispatchers). "
		"Understand how the new assets must interact with them.\n"
		"\n"
		"2. Then produce a Build Plan following this format:\n"
		"\n"
		"## Build Plan\n"
		"\n"
		"### Order\n"
		"1. BP_Name (create)\n"
		"2. @ExistingBP (modify)\n"
		"\n"
		"### BP_Name\n"
		"- **Action**: create\n"
		"- **Parent Class**: <real UE class, e.g., AActor, APawn, ACharacter, UActorComponent>\n"
		"- **Components**: VarName (UComponentClass) -- purpose\n"
		"- **Variables**: VarName (Type, default: value) -- purpose\n"
		"- **Event Dispatchers**: Name(ParamType Param, ...) -- purpose\n"
		"- **Interfaces**: UInterfaceName -- purpose\n"
		"- **Functions**: Name(Params) -> ReturnType\n"
		"  Node-level description: what components/variables are accessed, what functions are called\n"
		"  (with target classes for component functions), wiring flow, and any gotchas.\n"
		"  Reference template: template_id:FunctionName (if based on a template)\n"
		"- **Events**: EventName [ComponentName if delegate] -- what happens\n"
		"\n"
		"### @ExistingBP\n"
		"- **Action**: modify\n"
		"- **Add Variables**: ...\n"
		"- **Add Functions**: ...\n"
		"- **Add Events**: ...\n"
		"\n"
		"### Interactions\n"
		"- How assets communicate (dispatchers, interfaces, direct calls)\n"
		"\n"
		"## Rules\n"
		"- Order is mandatory: assets must be listed in dependency order\n"
		"- Use UE class names (Actor, Character, SphereComponent are fine -- Validator normalizes)\n"
		"- For delegate events, specify which component (e.g., OnComponentBeginOverlap [BoxComp])\n"
		"- Modify blocks (@prefix) only list CHANGES\n"
		"- If a function needs latent ops (Delay, Timeline), note it must be a Custom Event\n"
		"- Function descriptions must include node-level detail.\n"
		"  Example: \"Get ProjectileMovementComp via GetComponentByClass -> set InitialSpeed, "
		"MaxSpeed via property access -> store Instigator ref as variable\"\n"
		"  Include: component access patterns, function call targets, branch conditions, "
		"variable get/set operations, and any by-ref pins that need wires.\n"
		"- If Implementation Reference content is provided, study the function graph patterns "
		"(node types, wiring flow, variable usage) and include the specific node patterns "
		"in your function descriptions. The Builder executes directly from your descriptions.\n"
		"- When a function is based on a template, add: Reference: template_id:FunctionName\n"
		"  The Builder will only fetch the template if it has trouble executing the function.\n"
		"- When describing function logic, reference specific patterns you observed "
		"(e.g., \"line trace -> branch on hit -> apply damage\")\n"
		"\n"
		"Output ONLY the Build Plan. No preamble, no explanation."
	);
}

// ---------------------------------------------------------------------------
// Parse Helpers
// ---------------------------------------------------------------------------

FOliveRouterResult FOliveAgentPipeline::ParseRouterResponse(const FString& Response)
{
	FOliveRouterResult Result;
	Result.bSuccess = true;

	FString TrimmedResponse = Response.TrimStartAndEnd();

	// Parse "COMPLEXITY: SIMPLE|MODERATE|COMPLEX" from the response.
	// Strategy: first look for the precise "COMPLEXITY:" line. Only fall back to
	// bare keyword matching if no such line exists.
	TArray<FString> Lines;
	TrimmedResponse.ParseIntoArrayLines(Lines);

	bool bFoundComplexityLine = false;

	for (int32 i = 0; i < Lines.Num(); i++)
	{
		FString Line = Lines[i].TrimStartAndEnd().ToUpper();

		if (!Line.Contains(TEXT("COMPLEXITY")))
		{
			continue;
		}

		bFoundComplexityLine = true;

		// Extract the token after "COMPLEXITY:" (if colon present)
		int32 ColonPos = Line.Find(TEXT(":"));
		FString ValuePart = (ColonPos != INDEX_NONE) ? Line.Mid(ColonPos + 1).TrimStartAndEnd() : Line;

		if (ValuePart.Contains(TEXT("SIMPLE")))
		{
			Result.Complexity = EOliveTaskComplexity::Simple;
		}
		else if (ValuePart.Contains(TEXT("MODERATE")))
		{
			Result.Complexity = EOliveTaskComplexity::Moderate;
		}
		else if (ValuePart.Contains(TEXT("COMPLEX")))
		{
			Result.Complexity = EOliveTaskComplexity::Complex;
		}

		// The next line (if exists) is the reasoning
		if (i + 1 < Lines.Num())
		{
			Result.Reasoning = Lines[i + 1].TrimStartAndEnd();
		}
		break;
	}

	// Fallback: bare keyword matching when no "COMPLEXITY:" line found
	if (!bFoundComplexityLine)
	{
		FString Upper = TrimmedResponse.ToUpper();

		if (Upper.Contains(TEXT("SIMPLE")))
		{
			Result.Complexity = EOliveTaskComplexity::Simple;
		}
		else if (Upper.Contains(TEXT("MODERATE")))
		{
			Result.Complexity = EOliveTaskComplexity::Moderate;
		}
		else if (Upper.Contains(TEXT("COMPLEX")))
		{
			Result.Complexity = EOliveTaskComplexity::Complex;
		}
		else
		{
			// Cannot parse: default to Moderate
			Result.Complexity = EOliveTaskComplexity::Moderate;
			Result.Reasoning = TEXT("Unparseable Router response; defaulted to Moderate");
			return Result;
		}

		// If we have no reasoning yet, use lines beyond the first
		if (Lines.Num() > 1)
		{
			Result.Reasoning = Lines[1].TrimStartAndEnd();
		}
	}

	return Result;
}

TArray<FOliveScoutResult::FAssetEntry> FOliveAgentPipeline::ParseScoutResponse(
	const FString& Response,
	const TArray<FOliveScoutResult::FAssetEntry>& RawAssets)
{
	TArray<FOliveScoutResult::FAssetEntry> Result;

	FString TrimmedResponse = Response.TrimStartAndEnd();

	// Handle "NONE" case
	if (TrimmedResponse.ToUpper().Contains(TEXT("NONE")))
	{
		return Result;
	}

	// Parse numbered list: "1. /Game/Path/Asset (Class) -- reason"
	TArray<FString> Lines;
	TrimmedResponse.ParseIntoArrayLines(Lines);

	for (const FString& Line : Lines)
	{
		FString TrimLine = Line.TrimStartAndEnd();
		if (TrimLine.IsEmpty())
		{
			continue;
		}

		// Strip leading number and period (e.g., "1. ", "2. ")
		FString Content = TrimLine;
		if (Content.Len() > 2 && FChar::IsDigit(Content[0]))
		{
			int32 DotIdx = INDEX_NONE;
			Content.FindChar('.', DotIdx);
			if (DotIdx != INDEX_NONE && DotIdx < 4)
			{
				Content = Content.Mid(DotIdx + 1).TrimStartAndEnd();
			}
		}

		// Extract path: find first /Game/ occurrence
		int32 PathStart = Content.Find(TEXT("/Game/"));
		if (PathStart == INDEX_NONE)
		{
			PathStart = Content.Find(TEXT("/"));
		}
		if (PathStart == INDEX_NONE)
		{
			continue;
		}

		// Extract the path (up to space or parenthesis)
		FString PathCandidate;
		for (int32 i = PathStart; i < Content.Len(); i++)
		{
			if (Content[i] == ' ' || Content[i] == '(' || Content[i] == '\t')
			{
				break;
			}
			PathCandidate += Content[i];
		}

		if (PathCandidate.IsEmpty())
		{
			continue;
		}

		// Find matching raw asset
		const FOliveScoutResult::FAssetEntry* MatchedRaw = nullptr;
		for (const auto& Raw : RawAssets)
		{
			if (Raw.Path == PathCandidate || Raw.Path.Contains(PathCandidate) || PathCandidate.Contains(Raw.Path))
			{
				MatchedRaw = &Raw;
				break;
			}
		}

		// Extract reason after "--"
		FString Reason;
		int32 DashDashIdx = Content.Find(TEXT("--"));
		if (DashDashIdx != INDEX_NONE)
		{
			Reason = Content.Mid(DashDashIdx + 2).TrimStartAndEnd();
		}

		FOliveScoutResult::FAssetEntry Entry;
		if (MatchedRaw)
		{
			Entry.Path = MatchedRaw->Path;
			Entry.AssetClass = MatchedRaw->AssetClass;
		}
		else
		{
			Entry.Path = PathCandidate;
			Entry.AssetClass = TEXT("Unknown");
		}
		Entry.Relevance = Reason.IsEmpty() ? TEXT("relevant to task") : Reason;

		Result.Add(MoveTemp(Entry));

		if (Result.Num() >= MAX_SCOUT_RANKED_ASSETS)
		{
			break;
		}
	}

	return Result;
}

void FOliveAgentPipeline::ParseBuildPlan(
	const FString& BuildPlan,
	FOliveArchitectResult& OutResult)
{
	TArray<FString> Lines;
	BuildPlan.ParseIntoArrayLines(Lines);

	bool bInOrderSection = false;
	FString CurrentAsset;

	// Section tracking for multi-line lists. When a "**Components**:" or "**Interfaces**:"
	// header is found with content only on the header line, or followed by indented "- "
	// continuation lines, we accumulate items until a new top-level "- **" marker is hit.
	enum class EListSection : uint8 { None, Components, Interfaces, Variables };
	EListSection CurrentListSection = EListSection::None;

	// Lambda: parse a single component entry "VarName (ClassName) -- purpose" and add it
	auto ParseComponentEntry = [&OutResult, &CurrentAsset](const FString& Entry)
	{
		FString TrimPart = Entry.TrimStartAndEnd();
		// Strip leading "- " for list items
		if (TrimPart.StartsWith(TEXT("- ")))
		{
			TrimPart = TrimPart.Mid(2).TrimStartAndEnd();
		}

		// Match: "VarName (ClassName)" using parentheses
		int32 OpenParen = INDEX_NONE;
		int32 CloseParen = INDEX_NONE;
		TrimPart.FindChar('(', OpenParen);
		TrimPart.FindLastChar(')', CloseParen);

		if (OpenParen != INDEX_NONE && CloseParen != INDEX_NONE && CloseParen > OpenParen)
		{
			FString VarName = TrimPart.Left(OpenParen).TrimStartAndEnd();
			FString ClassName = TrimPart.Mid(OpenParen + 1, CloseParen - OpenParen - 1).TrimStartAndEnd();

			if (!VarName.IsEmpty() && !ClassName.IsEmpty())
			{
				if (!OutResult.Components.Contains(CurrentAsset))
				{
					OutResult.Components.Add(CurrentAsset, TArray<TPair<FString, FString>>());
				}
				OutResult.Components[CurrentAsset].Add(TPair<FString, FString>(VarName, ClassName));
			}
		}
	};

	// Lambda: parse a single interface entry "InterfaceName -- purpose" and add it
	auto ParseInterfaceEntry = [&OutResult, &CurrentAsset](const FString& Entry)
	{
		FString TrimPart = Entry.TrimStartAndEnd();
		// Strip leading "- " for list items
		if (TrimPart.StartsWith(TEXT("- ")))
		{
			TrimPart = TrimPart.Mid(2).TrimStartAndEnd();
		}

		// Strip "-- purpose" suffix
		int32 DashDashIdx = TrimPart.Find(TEXT("--"));
		if (DashDashIdx != INDEX_NONE)
		{
			TrimPart = TrimPart.Left(DashDashIdx).TrimStartAndEnd();
		}

		if (!TrimPart.IsEmpty())
		{
			if (!OutResult.Interfaces.Contains(CurrentAsset))
			{
				OutResult.Interfaces.Add(CurrentAsset, TArray<FString>());
			}
			OutResult.Interfaces[CurrentAsset].Add(TrimPart);
		}
	};

	for (const FString& Line : Lines)
	{
		FString TrimLine = Line.TrimStartAndEnd();
		if (TrimLine.IsEmpty())
		{
			continue;
		}

		// Detect "### Order" section
		if (TrimLine.StartsWith(TEXT("### Order")) || TrimLine.StartsWith(TEXT("## Order")))
		{
			bInOrderSection = true;
			CurrentListSection = EListSection::None;
			continue;
		}

		// Detect new asset blocks: "### BP_Name" or "### @ExistingBP"
		if (TrimLine.StartsWith(TEXT("### ")) && !TrimLine.StartsWith(TEXT("### Order"))
			&& !TrimLine.StartsWith(TEXT("### Interactions"))
			&& !TrimLine.StartsWith(TEXT("### Validator")))
		{
			bInOrderSection = false;
			CurrentListSection = EListSection::None;
			CurrentAsset = TrimLine.Mid(4).TrimStartAndEnd();
			// Remove any trailing text in parentheses
			int32 ParenIdx = INDEX_NONE;
			CurrentAsset.FindChar('(', ParenIdx);
			if (ParenIdx != INDEX_NONE)
			{
				CurrentAsset = CurrentAsset.Left(ParenIdx).TrimStartAndEnd();
			}
			continue;
		}

		// Parse order list items: "1. BP_Name (create)" or "2. @ExistingBP (modify)"
		if (bInOrderSection)
		{
			// Match: digits + period + space + name + optional parenthetical
			FString OrderLine = TrimLine;

			// Strip leading number + period
			if (OrderLine.Len() > 2 && FChar::IsDigit(OrderLine[0]))
			{
				int32 DotIdx = INDEX_NONE;
				OrderLine.FindChar('.', DotIdx);
				if (DotIdx != INDEX_NONE && DotIdx < 4)
				{
					OrderLine = OrderLine.Mid(DotIdx + 1).TrimStartAndEnd();
				}
			}

			// Strip trailing parenthetical "(create)", "(modify)"
			int32 ParenIdx = INDEX_NONE;
			OrderLine.FindChar('(', ParenIdx);
			if (ParenIdx != INDEX_NONE)
			{
				OrderLine = OrderLine.Left(ParenIdx).TrimStartAndEnd();
			}

			if (!OrderLine.IsEmpty())
			{
				OutResult.AssetOrder.Add(OrderLine);
			}
			continue;
		}

		// Skip if no current asset block
		if (CurrentAsset.IsEmpty())
		{
			continue;
		}

		// Check for indented continuation lines (multi-line list items).
		// These are "  - VarName (Class)" lines that belong to the current list section.
		// Detection: the raw line starts with whitespace, and the trimmed line starts with "- ".
		const bool bIsContinuationLine = (Line.Len() > 0 && (Line[0] == ' ' || Line[0] == '\t'))
			&& TrimLine.StartsWith(TEXT("- "));

		if (bIsContinuationLine && CurrentListSection != EListSection::None)
		{
			switch (CurrentListSection)
			{
			case EListSection::Components:
				ParseComponentEntry(TrimLine);
				break;
			case EListSection::Interfaces:
				ParseInterfaceEntry(TrimLine);
				break;
			case EListSection::Variables:
				// Variables section: no structured extraction needed for Validator currently,
				// but we consume the line to avoid it being misinterpreted.
				break;
			default:
				break;
			}
			continue;
		}

		// A new top-level "- **" marker resets the list section
		if (TrimLine.StartsWith(TEXT("- **")))
		{
			CurrentListSection = EListSection::None;
		}

		// Parse "- **Parent Class**: ClassName"
		if (TrimLine.Contains(TEXT("**Parent Class**")))
		{
			CurrentListSection = EListSection::None;
			int32 ColonIdx = TrimLine.Find(TEXT(":"), ESearchCase::IgnoreCase,
				ESearchDir::FromStart, TrimLine.Find(TEXT("**Parent Class**")) + 16);
			if (ColonIdx != INDEX_NONE)
			{
				FString ClassName = TrimLine.Mid(ColonIdx + 1).TrimStartAndEnd();
				// Remove any trailing text after the class name (comments, etc.)
				for (int32 i = 0; i < ClassName.Len(); i++)
				{
					if (ClassName[i] == '(' || ClassName[i] == '-' || ClassName[i] == ',')
					{
						ClassName = ClassName.Left(i).TrimStartAndEnd();
						break;
					}
				}
				if (!ClassName.IsEmpty())
				{
					OutResult.ParentClasses.Add(CurrentAsset, ClassName);
				}
			}
			continue;
		}

		// Parse "- **Components**: VarName (ClassName) -- purpose"
		// Handles both single-line comma-separated and multi-line lists.
		if (TrimLine.Contains(TEXT("**Components**")))
		{
			int32 ColonIdx = TrimLine.Find(TEXT(":"), ESearchCase::IgnoreCase,
				ESearchDir::FromStart, TrimLine.Find(TEXT("**Components**")) + 14);
			if (ColonIdx != INDEX_NONE)
			{
				FString CompStr = TrimLine.Mid(ColonIdx + 1).TrimStartAndEnd();

				if (CompStr.IsEmpty())
				{
					// Multi-line format: "**Components**:" header only, items follow as indented bullets
					CurrentListSection = EListSection::Components;
				}
				else
				{
					// Single-line comma-separated list: "VarA (ClassA), VarB (ClassB)"
					CurrentListSection = EListSection::Components;
					TArray<FString> CompParts;
					CompStr.ParseIntoArray(CompParts, TEXT(","), true);

					for (const FString& CompPart : CompParts)
					{
						ParseComponentEntry(CompPart);
					}
				}
			}
			continue;
		}

		// Parse "- **Interfaces**: InterfaceName -- purpose"
		// Handles both single-line comma-separated and multi-line lists.
		if (TrimLine.Contains(TEXT("**Interfaces**")))
		{
			int32 ColonIdx = TrimLine.Find(TEXT(":"), ESearchCase::IgnoreCase,
				ESearchDir::FromStart, TrimLine.Find(TEXT("**Interfaces**")) + 14);
			if (ColonIdx != INDEX_NONE)
			{
				FString IntStr = TrimLine.Mid(ColonIdx + 1).TrimStartAndEnd();

				if (IntStr.IsEmpty())
				{
					// Multi-line format: header only, items follow as indented bullets
					CurrentListSection = EListSection::Interfaces;
				}
				else
				{
					// Single-line comma-separated
					CurrentListSection = EListSection::Interfaces;
					TArray<FString> IntParts;
					IntStr.ParseIntoArray(IntParts, TEXT(","), true);

					for (const FString& IntPart : IntParts)
					{
						ParseInterfaceEntry(IntPart);
					}
				}
			}
			continue;
		}

		// Parse "- **Variables**: ..." — track section for multi-line consumption
		if (TrimLine.Contains(TEXT("**Variables**")))
		{
			CurrentListSection = EListSection::Variables;
			continue;
		}

		// Other section headers (Functions, Events, Dispatchers, Action, etc.)
		// reset the list section so continuation lines are not misattributed.
		if (TrimLine.StartsWith(TEXT("- **")))
		{
			CurrentListSection = EListSection::None;
		}
	}
}

// ---------------------------------------------------------------------------
// BuildComponentAPIMap -- compact API reference for Builder prompt
// ---------------------------------------------------------------------------

FString FOliveAgentPipeline::BuildComponentAPIMap(const FOliveArchitectResult& ArchResult)
{
	// Budget cap -- keeps the API map to ~750-800 tokens in the Builder prompt
	static constexpr int32 API_MAP_BUDGET = 3000;

	// Collect unique component class names across all assets
	TSet<FString> UniqueClassNames;
	for (const auto& AssetPair : ArchResult.Components)
	{
		for (const TPair<FString, FString>& CompPair : AssetPair.Value)
		{
			const FString& ClassName = CompPair.Value;
			if (!ClassName.IsEmpty())
			{
				UniqueClassNames.Add(ClassName);
			}
		}
	}

	if (UniqueClassNames.Num() == 0)
	{
		return FString();
	}

	// Resolve each unique class name to a UClass* and format its API summary
	TArray<TPair<FString, FString>> ClassSummaries; // (DisplayName, FormattedSummary)
	for (const FString& ClassName : UniqueClassNames)
	{
		// Try multiple resolution strategies: exact, U-prefix, stripped-U
		UClass* ResolvedClass = FindFirstObjectSafe<UClass>(*ClassName);
		if (!ResolvedClass)
		{
			ResolvedClass = FindFirstObjectSafe<UClass>(*(TEXT("U") + ClassName));
		}
		if (!ResolvedClass && ClassName.StartsWith(TEXT("U")))
		{
			ResolvedClass = FindFirstObjectSafe<UClass>(*ClassName.Mid(1));
		}

		if (!ResolvedClass)
		{
			UE_LOG(LogOliveAgentPipeline, Verbose,
				TEXT("BuildComponentAPIMap: could not resolve class '%s', skipping"), *ClassName);
			continue;
		}

		FString Summary = FOliveClassAPIHelper::FormatCompactAPISummary(ResolvedClass);
		if (!Summary.IsEmpty())
		{
			// Use the short class name without prefix for display
			FString DisplayName = ResolvedClass->GetName();
			if (DisplayName.StartsWith(TEXT("U")))
			{
				DisplayName = DisplayName.Mid(1);
			}
			ClassSummaries.Add(TPair<FString, FString>(DisplayName, Summary));
		}
	}

	if (ClassSummaries.Num() == 0)
	{
		return FString();
	}

	// Build the output with budget tracking
	FString Output;
	Output.Reserve(API_MAP_BUDGET + 256);
	Output += TEXT("## Component API Reference\n\n");
	Output += TEXT("Use set_var/get_var for properties, call for functions.\n");

	int32 CurrentLen = Output.Len();
	int32 ClassesIncluded = 0;

	for (const TPair<FString, FString>& Entry : ClassSummaries)
	{
		// FormatCompactAPISummary already includes the "### ClassName" header
		FString ClassBlock = TEXT("\n") + Entry.Value;

		if (CurrentLen + ClassBlock.Len() > API_MAP_BUDGET && ClassesIncluded > 0)
		{
			// Over budget -- report how many classes were omitted
			int32 Remaining = ClassSummaries.Num() - ClassesIncluded;
			if (Remaining > 0)
			{
				Output += FString::Printf(TEXT("\n... and %d more component type(s) omitted.\n"), Remaining);
			}
			break;
		}

		Output += ClassBlock;
		CurrentLen += ClassBlock.Len();
		ClassesIncluded++;
	}

	UE_LOG(LogOliveAgentPipeline, Log,
		TEXT("BuildComponentAPIMap: %d/%d component classes resolved, %d chars"),
		ClassesIncluded, UniqueClassNames.Num(), Output.Len());

	return Output;
}

// ---------------------------------------------------------------------------
// Validator Helpers
// ---------------------------------------------------------------------------

UClass* FOliveAgentPipeline::TryResolveClass(const FString& ClassName)
{
	if (ClassName.IsEmpty())
	{
		return nullptr;
	}

	// 1. Exact match
	UClass* Found = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
	if (Found)
	{
		return Found;
	}

	// 2. Try with A-prefix (for Actor classes)
	if (ClassName.Len() < 2 || ClassName[0] != 'A' || !FChar::IsUpper(ClassName[1]))
	{
		Found = FindFirstObject<UClass>(*(TEXT("A") + ClassName), EFindFirstObjectOptions::NativeFirst);
		if (Found)
		{
			return Found;
		}
	}

	// 3. Try with U-prefix (for UObject-derived classes)
	if (ClassName.Len() < 2 || ClassName[0] != 'U' || !FChar::IsUpper(ClassName[1]))
	{
		Found = FindFirstObject<UClass>(*(TEXT("U") + ClassName), EFindFirstObjectOptions::NativeFirst);
		if (Found)
		{
			return Found;
		}
	}

	// 4. Short-name alias table
	const TMap<FString, FString>& Aliases = GetClassAliasMap();
	const FString* AliasTarget = Aliases.Find(ClassName);
	if (AliasTarget)
	{
		Found = FindFirstObject<UClass>(**AliasTarget, EFindFirstObjectOptions::NativeFirst);
		if (Found)
		{
			return Found;
		}
	}

	// 5. Try stripping prefix and using alias (e.g., "UActor" -> strip U -> "Actor" -> alias)
	if (ClassName.Len() > 1 && (ClassName[0] == 'U' || ClassName[0] == 'A'))
	{
		FString Stripped = ClassName.Mid(1);
		AliasTarget = Aliases.Find(Stripped);
		if (AliasTarget)
		{
			Found = FindFirstObject<UClass>(**AliasTarget, EFindFirstObjectOptions::NativeFirst);
			if (Found)
			{
				return Found;
			}
		}
	}

	return nullptr;
}

UClass* FOliveAgentPipeline::TryResolveComponentClass(const FString& ClassName)
{
	UClass* Resolved = TryResolveClass(ClassName);
	if (Resolved && Resolved->IsChildOf(UActorComponent::StaticClass()))
	{
		return Resolved;
	}
	return nullptr;
}

bool FOliveAgentPipeline::IsValidInterface(const FString& InterfaceName)
{
	if (InterfaceName.IsEmpty())
	{
		return false;
	}

	// 1. Check for native C++ interface (exact name)
	UClass* Found = FindFirstObject<UClass>(*InterfaceName, EFindFirstObjectOptions::NativeFirst);
	if (Found && Found->IsChildOf(UInterface::StaticClass()))
	{
		return true;
	}

	// Try with U prefix (e.g., "Interface" -> "UInterface")
	if (!InterfaceName.StartsWith(TEXT("U")))
	{
		Found = FindFirstObject<UClass>(*(TEXT("U") + InterfaceName), EFindFirstObjectOptions::NativeFirst);
		if (Found && Found->IsChildOf(UInterface::StaticClass()))
		{
			return true;
		}
	}

	// 2. Search project index for Blueprint Interface assets.
	// This handles both BPI_ prefixed names and I-prefixed names via fuzzy search.
	// We search with the original name and common prefix variants.
	TArray<FString> SearchTerms;
	SearchTerms.Add(InterfaceName);

	// Strip common prefixes for additional search terms
	if (InterfaceName.StartsWith(TEXT("BPI_")))
	{
		SearchTerms.Add(InterfaceName.Mid(4));
	}
	else if (InterfaceName.StartsWith(TEXT("I")) && InterfaceName.Len() > 1 && FChar::IsUpper(InterfaceName[1]))
	{
		// "IInteractable" -> also search "Interactable"
		SearchTerms.Add(InterfaceName.Mid(1));
	}

	for (const FString& SearchTerm : SearchTerms)
	{
		TArray<FOliveAssetInfo> ProjectResults = FOliveProjectIndex::Get().SearchAssets(SearchTerm, 5);
		for (const FOliveAssetInfo& Result : ProjectResults)
		{
			if (Result.Name.Contains(InterfaceName) || InterfaceName.Contains(Result.Name)
				|| Result.Name.Contains(SearchTerm) || SearchTerm.Contains(Result.Name))
			{
				return true;
			}
		}
	}

	return false;
}

// ---------------------------------------------------------------------------
// BuildAssetStateSummary -- For Reviewer
// ---------------------------------------------------------------------------

namespace
{

FString BuildAssetStateSummary(const TArray<FString>& AssetPaths)
{
	FString Output;
	Output.Reserve(2048);

	const int32 MaxAssets = FMath::Min(AssetPaths.Num(), MAX_REVIEWER_ASSETS);

	for (int32 i = 0; i < MaxAssets; i++)
	{
		const FString& AssetPath = AssetPaths[i];

		TOptional<FOliveIRBlueprint> IR = FOliveBlueprintReader::Get().ReadBlueprintSummary(AssetPath);
		if (!IR.IsSet())
		{
			Output += TEXT("### ") + AssetPath + TEXT(" -- FAILED TO LOAD\n\n");
			continue;
		}

		const FOliveIRBlueprint& BP = IR.GetValue();

		Output += TEXT("### ") + BP.Path + TEXT("\n");
		Output += TEXT("- **Parent**: ") + BP.ParentClass.Name + TEXT("\n");

		// Compile status
		const TCHAR* StatusStr =
			BP.CompileStatus == EOliveIRCompileStatus::UpToDate ? TEXT("OK") :
			BP.CompileStatus == EOliveIRCompileStatus::Error ? TEXT("Error") :
			BP.CompileStatus == EOliveIRCompileStatus::Dirty ? TEXT("Dirty") :
			BP.CompileStatus == EOliveIRCompileStatus::Warning ? TEXT("Warning") :
			TEXT("Unknown");
		Output += TEXT("- **Status**: ");
		Output += StatusStr;
		Output += TEXT("\n");

		// Components (skip DefaultSceneRoot)
		{
			TArray<FString> CompEntries;
			for (const FOliveIRComponent& Comp : BP.Components)
			{
				if (Comp.Name == TEXT("DefaultSceneRoot"))
				{
					continue;
				}
				CompEntries.Add(Comp.Name + TEXT(" (") + Comp.ComponentClass + TEXT(")"));
			}
			if (CompEntries.Num() > 0)
			{
				Output += TEXT("- **Components**: ") + FString::Join(CompEntries, TEXT(", ")) + TEXT("\n");
			}
		}

		// Variables (skip DefaultComponent_*)
		{
			TArray<FString> VarEntries;
			for (const FOliveIRVariable& Var : BP.Variables)
			{
				if (Var.Name.StartsWith(TEXT("DefaultComponent_")))
				{
					continue;
				}
				FString Entry = Var.Name + TEXT(" (") + Var.Type.GetDisplayName();
				if (!Var.DefaultValue.IsEmpty())
				{
					Entry += TEXT(", default: ") + Var.DefaultValue;
				}
				Entry += TEXT(")");
				VarEntries.Add(Entry);
			}
			if (VarEntries.Num() > 0)
			{
				Output += TEXT("- **Variables**: ") + FString::Join(VarEntries, TEXT(", ")) + TEXT("\n");
			}
		}

		// Event dispatchers
		if (BP.EventDispatchers.Num() > 0)
		{
			TArray<FString> DispEntries;
			for (const FOliveIREventDispatcher& Disp : BP.EventDispatchers)
			{
				FString Entry = Disp.Name;
				if (Disp.Parameters.Num() > 0)
				{
					Entry += TEXT("(");
					for (int32 j = 0; j < Disp.Parameters.Num(); j++)
					{
						if (j > 0)
						{
							Entry += TEXT(", ");
						}
						Entry += Disp.Parameters[j].Type.GetDisplayName();
					}
					Entry += TEXT(")");
				}
				DispEntries.Add(Entry);
			}
			Output += TEXT("- **Dispatchers**: ") + FString::Join(DispEntries, TEXT(", ")) + TEXT("\n");
		}

		// Interfaces
		if (BP.Interfaces.Num() > 0)
		{
			TArray<FString> IntEntries;
			for (const FOliveIRInterfaceRef& Intf : BP.Interfaces)
			{
				IntEntries.Add(Intf.Name);
			}
			Output += TEXT("- **Interfaces**: ") + FString::Join(IntEntries, TEXT(", ")) + TEXT("\n");
		}

		// Functions (with node counts from summaries, excluding internal graphs)
		{
			TArray<FString> FuncEntries;
			for (const FOliveIRGraphSummary& FuncSummary : BP.FunctionSummaries)
			{
				// Exclude internal graphs
				if (FuncSummary.Name == TEXT("ConstructionScript")
					|| FuncSummary.Name == TEXT("UserConstructionScript")
					|| FuncSummary.Name.StartsWith(TEXT("ExecuteUbergraph_")))
				{
					continue;
				}
				FuncEntries.Add(FString::Printf(TEXT("%s (%d nodes)"),
					*FuncSummary.Name, FuncSummary.NodeCount));
			}
			// Fallback to FunctionNames if no summaries
			if (FuncEntries.Num() == 0)
			{
				for (const FString& FuncName : BP.FunctionNames)
				{
					if (FuncName == TEXT("ConstructionScript")
						|| FuncName == TEXT("UserConstructionScript")
						|| FuncName.StartsWith(TEXT("ExecuteUbergraph_")))
					{
						continue;
					}
					FuncEntries.Add(FuncName);
				}
			}
			if (FuncEntries.Num() > 0)
			{
				Output += TEXT("- **Functions**: ") + FString::Join(FuncEntries, TEXT(", ")) + TEXT("\n");
			}
		}

		// Event graphs (list event entry point names)
		if (BP.EventGraphNames.Num() > 0)
		{
			TArray<FString> EventEntries;
			for (const FString& EventName : BP.EventGraphNames)
			{
				EventEntries.Add(EventName);
			}
			if (EventEntries.Num() > 0)
			{
				Output += TEXT("- **Event Graphs**: ") + FString::Join(EventEntries, TEXT(", ")) + TEXT("\n");
			}
		}

		Output += TEXT("\n");
	}

	return Output;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Per-Agent Configuration
// ---------------------------------------------------------------------------

float FOliveAgentPipeline::GetTemperature(EOliveAgentRole Role)
{
	switch (Role)
	{
	case EOliveAgentRole::Router:     return 0.0f;
	case EOliveAgentRole::Scout:      return 0.0f;
	case EOliveAgentRole::Researcher: return 0.2f;
	case EOliveAgentRole::Architect:  return 0.2f;
	case EOliveAgentRole::Reviewer:   return 0.0f;
	default:                          return 0.0f;
	}
}

int32 FOliveAgentPipeline::GetMaxTokens(EOliveAgentRole Role)
{
	switch (Role)
	{
	case EOliveAgentRole::Router:     return 64;
	case EOliveAgentRole::Scout:      return 256;
	case EOliveAgentRole::Researcher: return 512;
	case EOliveAgentRole::Architect:  return 2048;
	case EOliveAgentRole::Reviewer:   return 512;
	default:                          return 256;
	}
}

float FOliveAgentPipeline::GetTimeout(EOliveAgentRole Role)
{
	switch (Role)
	{
	case EOliveAgentRole::Router:     return 30.0f;
	case EOliveAgentRole::Scout:      return 30.0f;
	case EOliveAgentRole::Researcher: return 60.0f;
	case EOliveAgentRole::Architect:  return 120.0f;
	case EOliveAgentRole::Reviewer:   return 60.0f;
	default:                          return 60.0f;
	}
}
