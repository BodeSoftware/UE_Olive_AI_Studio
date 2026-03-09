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

	/**
	 * Post-process the Build Plan text to produce a compact "Function Pin Reference"
	 * section. Extracts function names via regex, resolves each to UFunction* via
	 * FindFunctionEx, and formats exact parameter signatures. Also detects UPROPERTY
	 * matches from FindFunctionEx's SearchedLocations for property-vs-function guidance.
	 *
	 * Injected as Section 3.25 in FormatForPromptInjection(), between Build Plan
	 * and Component API Map.
	 *
	 * @param PipelineResult  The full pipeline result (provides Build Plan text and ParentClasses)
	 * @param ContextBlueprint  Optional Blueprint for class-specific function resolution (may be nullptr)
	 * @return Formatted markdown block, or empty string if no functions resolved
	 */
	static FString BuildFunctionPinReference(
		const FOliveAgentPipelineResult& PipelineResult,
		UBlueprint* ContextBlueprint);

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
	// CLI-Optimized Pipeline (2-call path)
	// ==========================================

	/**
	 * Detect whether all agent LLM calls would fall through to CLI --print.
	 * Returns true when:
	 * - Main provider is ClaudeCode or Codex
	 * - No per-agent API model overrides are configured
	 * This triggers the optimized 2-call path to minimize CLI cold-start overhead.
	 */
	bool IsCLIOnlyMode() const;

	/**
	 * CLI-optimized pipeline: pure-C++ Scout + single Planner LLM call + Validator.
	 * Reduces 5 sequential CLI invocations to 1, saving ~16-20s of cold-start overhead.
	 *
	 * @param UserMessage         The user's original task description
	 * @param ContextAssetPaths   @-mentioned asset paths from the chat context bar
	 * @return Pipeline result with all sub-agent outputs
	 */
	FOliveAgentPipelineResult ExecuteCLIPath(
		const FString& UserMessage,
		const TArray<FString>& ContextAssetPaths);

	/**
	 * Combined Researcher+Architect in one LLM call (Planner).
	 * Receives Scout discovery + inline Blueprint IR data and produces the Build Plan directly.
	 * For CLI path only -- API path uses separate Researcher + Architect stages.
	 *
	 * @param UserMessage         The user's original task description
	 * @param ScoutResult         Discovery and asset results from the pure-C++ Scout
	 * @param ContextAssetPaths   @-mentioned asset paths for inline IR loading
	 * @return Architect result with Build Plan
	 */
	FOliveArchitectResult RunPlanner(
		const FString& UserMessage,
		const FOliveScoutResult& ScoutResult,
		const TArray<FString>& ContextAssetPaths);

	/**
	 * CLI-optimized pipeline: Planner with MCP tool access.
	 * Launches claude --print with --max-turns 15 and a filtered tool set
	 * (blueprint.get_template, blueprint.list_templates, blueprint.describe).
	 * The Planner reads template data on demand instead of receiving it all upfront.
	 *
	 * Falls back to RunPlanner() (no tools) if:
	 * - MCP server is not running
	 * - Claude CLI is not installed
	 * - Process spawn fails
	 *
	 * @param UserMessage         The user's original task description
	 * @param ScoutResult         Discovery and asset results from the pure-C++ Scout
	 * @param ContextAssetPaths   @-mentioned asset paths for inline IR loading
	 * @return Architect result with Build Plan
	 */
	FOliveArchitectResult RunPlannerWithTools(
		const FString& UserMessage,
		const FOliveScoutResult& ScoutResult,
		const TArray<FString>& ContextAssetPaths);

	/**
	 * Set up a minimal sandbox directory for the Planner's CLI process.
	 * Creates {ProjectDir}/Saved/OliveAI/PlannerSandbox/ with:
	 * - .mcp.json (MCP server connection via mcp-bridge.js)
	 * - CLAUDE.md (minimal Planner-specific instructions)
	 *
	 * Reuses the same directory across runs (no cleanup needed -- files are overwritten).
	 *
	 * @return Absolute path to the sandbox directory, or empty string on failure
	 */
	static FString SetupPlannerSandbox();

	/**
	 * Extract the final text content from stream-json output.
	 * Parses each line as JSON. Collects text from "assistant" type messages
	 * (content[].type=="text"), ignoring tool_use, tool_result, and system events.
	 * Falls back to treating the entire output as plain text if no JSON is found.
	 *
	 * @param StreamOutput  Raw stdout from the CLI process (newline-delimited JSON)
	 * @return The extracted text response (Build Plan)
	 */
	static FString ParseStreamJsonFinalText(const FString& StreamOutput);

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

	/** Build the Planner's system prompt (merged Researcher + Architect for CLI path). */
	static FString BuildPlannerSystemPrompt();

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

	/**
	 * Build a compact API reference string for components mentioned in the Build Plan.
	 * Resolves each unique component class name to a UClass, then formats callable
	 * functions and visible properties via FOliveClassAPIHelper. Caps output at
	 * ~3000 chars to keep the Builder prompt within token budget.
	 *
	 * @param ArchResult  The Architect result containing the Components map
	 * @return Formatted markdown block, or empty string if no components resolved
	 */
	static FString BuildComponentAPIMap(const FOliveArchitectResult& ArchResult);

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
