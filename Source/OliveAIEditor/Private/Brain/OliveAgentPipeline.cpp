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
#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"
#include "Components/ActorComponent.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogOliveAgentPipeline, Log, All);

// ---------------------------------------------------------------------------
// Anonymous namespace helpers
// ---------------------------------------------------------------------------

namespace
{

// Forward declaration -- defined later in file after all class methods
FString BuildAssetStateSummary(const TArray<FString>& AssetPaths);

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
static constexpr double PIPELINE_TOTAL_TIMEOUT = 60.0;

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

	// Section 2.5: Implementation Reference (from Scout's auto-loaded template content)
	if (!Scout.TemplateContent.IsEmpty() && !bWantsSimpleLogic)
	{
		Output += Scout.TemplateContent;
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
	Output += TEXT("1. Create structure (components, variables, interfaces, dispatchers)\n");

	if (bWantsSimpleLogic)
	{
		Output += TEXT("2. Write graph logic as described in the plan. The user wants ");
		Output += TEXT("placeholder/simple logic -- PrintString stubs are acceptable.\n");
	}
	else
	{
		Output += TEXT("2. For each function/event:\n");
		Output += TEXT("   a. If library templates were referenced above, use `blueprint.get_template(template_id, pattern=\"FunctionName\")` ");
		Output += TEXT("to study how similar functions are built. Base your plan_json on real node patterns.\n");
		Output += TEXT("   b. Write graph logic with apply_plan_json. Use the actual node types, function calls, ");
		Output += TEXT("and wiring patterns from the reference -- do not simplify to PrintString.\n");
	}

	Output += TEXT("3. Compile to 0 errors before moving to the next asset\n");
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

	// --- Tier 3: Claude Code CLI --print ---
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

				// Escape double quotes for command line
				FString EscapedPrompt = CombinedPrompt.Replace(TEXT("\""), TEXT("\\\""));

				const bool bIsJs = ClaudePath.EndsWith(TEXT(".js")) || ClaudePath.EndsWith(TEXT(".mjs"));

				FString StdOut;
				FString StdErr;
				int32 ReturnCode = -1;

				const FString Args = FString::Printf(
					TEXT("--print --output-format text --max-turns 1 \"%s\""),
					*EscapedPrompt);

				bool bLaunched = false;
				if (bIsJs)
				{
					const FString FullArgs = FString::Printf(TEXT("\"%s\" %s"), *ClaudePath, *Args);
					bLaunched = FPlatformProcess::ExecProcess(
						TEXT("node"), *FullArgs, &ReturnCode, &StdOut, &StdErr);
				}
				else
				{
					bLaunched = FPlatformProcess::ExecProcess(
						*ClaudePath, *Args, &ReturnCode, &StdOut, &StdErr);
				}

				if (bLaunched && ReturnCode == 0)
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
					TEXT("[%s] Tier 3 (Claude CLI) failed: code=%d, err=%s"),
					*RoleToString(Role), ReturnCode,
					StdErr.IsEmpty() ? TEXT("(no stderr)") : *StdErr.Left(200));
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

	// Part 1.5: Auto-load key function content from top library template matches.
	// This is a pure C++ operation (lazy-load JSON + format) -- no LLM call, ~0ms.
	{
		const FOliveLibraryIndex& LibIndex = FOliveTemplateSystem::Get().GetLibraryIndex();

		if (LibIndex.IsInitialized())
		{
			// Budget: max 2 templates, max 2 functions each, to stay under ~800 tokens
			static constexpr int32 MAX_TEMPLATE_CONTENT_TEMPLATES = 2;
			static constexpr int32 MAX_TEMPLATE_CONTENT_FUNCTIONS = 2;

			int32 TemplatesLoaded = 0;
			FString ContentBlock;

			for (const FOliveDiscoveryEntry& Entry : DiscoveryResult.Entries)
			{
				if (TemplatesLoaded >= MAX_TEMPLATE_CONTENT_TEMPLATES)
				{
					break;
				}

				// Only auto-load library templates (factory/reference don't have node data)
				if (Entry.SourceType != TEXT("library"))
				{
					continue;
				}

				// Only load if there are matched functions (means the search was targeted)
				if (Entry.MatchedFunctions.Num() == 0)
				{
					continue;
				}

				// Load top matched functions
				int32 FunctionsLoaded = 0;
				for (const FString& FuncName : Entry.MatchedFunctions)
				{
					if (FunctionsLoaded >= MAX_TEMPLATE_CONTENT_FUNCTIONS)
					{
						break;
					}

					FString FuncContent = LibIndex.GetFunctionContent(Entry.TemplateId, FuncName);

					// Skip not-found results
					if (FuncContent.IsEmpty() || FuncContent.StartsWith(FOliveLibraryIndex::GetFuncNotFoundSentinel()))
					{
						continue;
					}

					if (ContentBlock.IsEmpty())
					{
						ContentBlock += TEXT("## Implementation Reference\n\n");
						ContentBlock += TEXT("These are real function implementations from library templates. ");
						ContentBlock += TEXT("Study the node patterns, not just the names.\n\n");
					}

					ContentBlock += FuncContent;
					ContentBlock += TEXT("\n\n");
					FunctionsLoaded++;
				}

				if (FunctionsLoaded > 0)
				{
					TemplatesLoaded++;
				}
			}

			Result.TemplateContent = ContentBlock;

			if (!ContentBlock.IsEmpty())
			{
				UE_LOG(LogOliveAgentPipeline, Log,
					TEXT("  Scout: auto-loaded %d template function(s), %d chars"),
					TemplatesLoaded, ContentBlock.Len());
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

	// Implementation reference content (actual function graphs from library templates)
	if (!ScoutResult.TemplateContent.IsEmpty())
	{
		ArchitectUserPrompt += TEXT("\n");
		ArchitectUserPrompt += ScoutResult.TemplateContent;
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
	case EOliveAgentRole::Router:     return 10.0f;
	case EOliveAgentRole::Scout:      return 10.0f;
	case EOliveAgentRole::Researcher: return 15.0f;
	case EOliveAgentRole::Architect:  return 30.0f;
	case EOliveAgentRole::Reviewer:   return 15.0f;
	default:                          return 15.0f;
	}
}
