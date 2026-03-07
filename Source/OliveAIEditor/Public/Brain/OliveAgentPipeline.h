// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Brain/OliveAgentConfig.h"

class IOliveAIProvider;

/**
 * Agent Pipeline
 *
 * Runs a sequence of lightweight LLM sub-agents to analyze the user's task
 * and produce structured context for the Builder. This is NOT a singleton --
 * instantiate per run. The pipeline is synchronous (blocking with tick-pump)
 * and must be called on the game thread.
 *
 * Usage:
 *   FOliveAgentPipeline Pipeline;
 *   FOliveAgentPipelineResult Result = Pipeline.Execute(UserMessage, ActiveContextPaths);
 *   if (Result.bValid)
 *   {
 *       FString ContextBlock = Result.FormatForPromptInjection();
 *       // Inject into Builder's prompt...
 *   }
 *
 * Lifecycle: Execute() runs Router -> Scout -> [Researcher] -> Architect -> Validator.
 * The Reviewer runs separately after the Builder completes, via RunReviewer().
 */
class OLIVEAIEDITOR_API FOliveAgentPipeline
{
public:
	FOliveAgentPipeline() = default;

	/**
	 * Execute the pre-Builder pipeline stages.
	 * Blocking call that pumps the game thread ticker while waiting for LLM responses.
	 *
	 * @param UserMessage         The user's original task description
	 * @param ContextAssetPaths   @-mentioned asset paths from the chat context bar
	 * @return Pipeline result with all sub-agent outputs
	 */
	FOliveAgentPipelineResult Execute(
		const FString& UserMessage,
		const TArray<FString>& ContextAssetPaths);

	/**
	 * Run the Reviewer stage after the Builder completes.
	 * Compares the current state of built assets against the cached Build Plan.
	 *
	 * @param PipelineResult   The result from Execute() (provides the Build Plan)
	 * @param ModifiedAssets   Asset paths modified during the Builder's run
	 * @return Reviewer result with correction directive if needed
	 */
	FOliveReviewerResult RunReviewer(
		const FOliveAgentPipelineResult& PipelineResult,
		const TArray<FString>& ModifiedAssets);

private:
	// ==========================================
	// Pipeline Stages
	// ==========================================

	/** Stage 1: Classify task complexity. */
	FOliveRouterResult RunRouter(const FString& UserMessage);

	/** Stage 2: Discover relevant assets and templates. */
	FOliveScoutResult RunScout(
		const FString& UserMessage,
		const TArray<FString>& ContextAssetPaths);

	/**
	 * Stage 3: Analyze architecture of existing assets.
	 * Only runs for Moderate/Complex tasks.
	 */
	FOliveResearcherResult RunResearcher(
		const FString& UserMessage,
		const FOliveScoutResult& ScoutResult);

	/**
	 * Stage 4: Produce the Build Plan.
	 * Receives all prior context to inform the plan.
	 */
	FOliveArchitectResult RunArchitect(
		const FString& UserMessage,
		const FOliveRouterResult& RouterResult,
		const FOliveScoutResult& ScoutResult,
		const FOliveResearcherResult& ResearcherResult);

	/**
	 * Stage 5: C++ validation of the Build Plan.
	 * No LLM call -- uses engine reflection to verify classes, components, interfaces.
	 */
	FOliveValidatorResult RunValidator(const FOliveArchitectResult& ArchitectResult);

	// ==========================================
	// LLM Communication
	// ==========================================

	/**
	 * Send a single-shot blocking LLM completion for a specific agent role.
	 * Resolves provider/model via UOliveAISettings::GetAgentModelConfig(),
	 * then follows the 3-tier fallback: custom agent model -> main provider -> CLI --print.
	 *
	 * Reuses the tick-pump pattern from FOliveUtilityModel::TrySendCompletion().
	 *
	 * @param Role          Agent role (determines model config, temperature, max tokens)
	 * @param SystemPrompt  System-level instructions for this agent
	 * @param UserPrompt    The assembled prompt for this agent
	 * @param OutResponse   Populated with the agent's response on success
	 * @param OutError      Populated with error details on failure
	 * @return true if a response was obtained
	 */
	bool SendAgentCompletion(
		EOliveAgentRole Role,
		const FString& SystemPrompt,
		const FString& UserPrompt,
		FString& OutResponse,
		FString& OutError);

	// ==========================================
	// Prompt Builders (static, pure functions)
	// ==========================================

	/** Build the Router's system prompt. */
	static FString BuildRouterSystemPrompt();

	/** Build the Scout's system prompt. */
	static FString BuildScoutSystemPrompt();

	/** Build the Researcher's system prompt. */
	static FString BuildResearcherSystemPrompt();

	/** Build the Architect's system prompt. */
	static FString BuildArchitectSystemPrompt();

	/** Build the Reviewer's system prompt. */
	static FString BuildReviewerSystemPrompt();

	// ==========================================
	// Parse Helpers
	// ==========================================

	/** Parse Router response ("SIMPLE", "MODERATE", "COMPLEX" + reasoning). */
	static FOliveRouterResult ParseRouterResponse(const FString& Response);

	/** Parse Scout response into relevance-ranked asset entries. */
	static TArray<FOliveScoutResult::FAssetEntry> ParseScoutResponse(
		const FString& Response,
		const TArray<FOliveScoutResult::FAssetEntry>& RawAssets);

	/**
	 * Parse the Architect's Build Plan to extract structured data for the Validator.
	 * Extracts: asset order, parent classes, components, interfaces.
	 */
	static void ParseBuildPlan(
		const FString& BuildPlan,
		FOliveArchitectResult& OutResult);

	// ==========================================
	// Validator Helpers (C++ engine checks)
	// ==========================================

	/**
	 * Attempt to resolve a class name to a UClass*.
	 * Tries: exact match, U-prefix, A-prefix, common aliases (Actor, Pawn, Character, etc.).
	 * @return UClass* or nullptr if not found
	 */
	static UClass* TryResolveClass(const FString& ClassName);

	/**
	 * Check if a class name is a valid UActorComponent subclass.
	 * @return UClass* of the component, or nullptr
	 */
	static UClass* TryResolveComponentClass(const FString& ClassName);

	/**
	 * Check if an interface name resolves to a valid Blueprint Interface.
	 * Searches both native C++ interfaces and Blueprint Interface assets.
	 * @return true if the interface exists
	 */
	static bool IsValidInterface(const FString& InterfaceName);

	// ==========================================
	// Per-Agent Configuration
	// ==========================================

	/** Get temperature for this agent role */
	static float GetTemperature(EOliveAgentRole Role);

	/** Get max tokens for this agent role */
	static int32 GetMaxTokens(EOliveAgentRole Role);

	/** Get timeout in seconds for this agent role */
	static float GetTimeout(EOliveAgentRole Role);
};
