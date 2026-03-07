// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// Forward declaration for EOliveAIProvider used by FOliveAgentModelConfig
enum class EOliveAIProvider : uint8;

/**
 * Roles for sub-agents in the pipeline.
 * Each role has its own system prompt, temperature, and token budget.
 * The Builder is NOT included -- it is the main provider (Claude Code / API).
 */
enum class EOliveAgentRole : uint8
{
	Router,      // Classifies task complexity
	Scout,       // Discovers existing assets and templates
	Researcher,  // Analyzes architecture of existing assets
	Architect,   // Produces the Build Plan
	Reviewer     // Reviews Builder's output against the Build Plan
};

/**
 * Task complexity classification from the Router agent.
 */
enum class EOliveTaskComplexity : uint8
{
	Simple,   // Single asset, < 3 functions, no interactions (skip Researcher)
	Moderate, // 2-3 assets OR complex single asset
	Complex   // 4+ assets OR cross-system interactions
};

// ---------------------------------------------------------------------------
// Per-agent result structs
// ---------------------------------------------------------------------------

/**
 * Router output: task complexity + reasoning.
 */
struct OLIVEAIEDITOR_API FOliveRouterResult
{
	EOliveTaskComplexity Complexity = EOliveTaskComplexity::Simple;

	/** One-line reasoning for the classification. Logged but not shown to user. */
	FString Reasoning;

	/** Whether the LLM call succeeded. On failure, defaults to Moderate. */
	bool bSuccess = false;

	/** Agent elapsed time in seconds */
	double ElapsedSeconds = 0.0;
};

/**
 * Scout output: existing assets and templates relevant to the task.
 */
struct OLIVEAIEDITOR_API FOliveScoutResult
{
	/**
	 * Existing project assets found by keyword search.
	 * Each entry: { "path": "/Game/...", "class": "Blueprint", "relevance": "brief reason" }
	 */
	struct FAssetEntry
	{
		FString Path;
		FString AssetClass;
		FString Relevance;   // One-line reason why this is relevant
	};
	TArray<FAssetEntry> RelevantAssets;

	/**
	 * Template discovery results (library, factory, community).
	 * Reuses the existing FOliveDiscoveryResult from OliveUtilityModel.h.
	 */
	FString DiscoveryBlock;   // Pre-formatted markdown from FormatDiscoveryForPrompt

	/**
	 * Implementation reference content auto-loaded from top library template matches.
	 * Contains actual function graph data (nodes, connections, pin values) for
	 * 1-2 key functions from the most relevant library templates.
	 * Pure C++ operation (no LLM call). Typically 200-800 tokens.
	 * Empty if no library templates matched or discovery was disabled.
	 */
	FString TemplateContent;

	/** Whether the LLM call succeeded (for relevance ranking). */
	bool bSuccess = false;

	double ElapsedSeconds = 0.0;
};

/**
 * Researcher output: architectural analysis of existing assets.
 */
struct OLIVEAIEDITOR_API FOliveResearcherResult
{
	/**
	 * Per-asset analysis: parent class, key components, public interface
	 * (functions, dispatchers, variables). Free-form markdown.
	 */
	FString ArchitecturalAnalysis;

	/** Asset paths that were analyzed */
	TArray<FString> AnalyzedAssets;

	bool bSuccess = false;
	double ElapsedSeconds = 0.0;
};

/**
 * Architect output: the Build Plan.
 * This is the core deliverable of the pipeline.
 */
struct OLIVEAIEDITOR_API FOliveArchitectResult
{
	/** The full Build Plan markdown, following the schema defined in the Architect's prompt. */
	FString BuildPlan;

	/** Ordered list of asset names extracted from the plan (e.g., "BP_Gun", "@BP_Character") */
	TArray<FString> AssetOrder;

	/** Per-asset parent class extracted from the plan (for Validator) */
	TMap<FString, FString> ParentClasses;

	/** Per-asset component list: asset -> [(VarName, ClassName)] */
	TMap<FString, TArray<TPair<FString, FString>>> Components;

	/** Per-asset interface list: asset -> [InterfaceName] */
	TMap<FString, TArray<FString>> Interfaces;

	bool bSuccess = false;
	double ElapsedSeconds = 0.0;
};

/**
 * Validator output: C++ verification of the Architect's plan.
 * No LLM call -- pure engine reflection.
 */
struct OLIVEAIEDITOR_API FOliveValidatorResult
{
	struct FValidationIssue
	{
		/** Asset name from the plan (e.g., "BP_Gun") */
		FString AssetName;

		/** What was checked (e.g., "Parent Class", "Component", "Interface") */
		FString Category;

		/** The value that failed (e.g., "AWeaponBase") */
		FString Value;

		/** Human-readable diagnostic */
		FString Message;

		/** Suggested fix (e.g., "Did you mean AActor?") */
		FString Suggestion;
	};
	TArray<FValidationIssue> Issues;

	bool bHasBlockingIssues = false;
	double ElapsedSeconds = 0.0;
};

/**
 * Reviewer output: comparison of Builder's result against the Build Plan.
 */
struct OLIVEAIEDITOR_API FOliveReviewerResult
{
	/** Whether the Build Plan was fully satisfied */
	bool bPlanSatisfied = false;

	/** List of unimplemented items from the plan */
	TArray<FString> MissingItems;

	/** List of deviations (implemented differently than planned) */
	TArray<FString> Deviations;

	/** Correction directive for the Builder (empty if plan satisfied) */
	FString CorrectionDirective;

	bool bSuccess = false;
	double ElapsedSeconds = 0.0;
};

// ---------------------------------------------------------------------------
// Composite pipeline result
// ---------------------------------------------------------------------------

/**
 * The full pipeline result, cached for the duration of the Builder's run.
 * FormatForPromptInjection() produces the markdown block that goes into
 * the Builder's stdin (autonomous) or system message (orchestrated).
 */
struct OLIVEAIEDITOR_API FOliveAgentPipelineResult
{
	FOliveRouterResult Router;
	FOliveScoutResult Scout;
	FOliveResearcherResult Researcher;   // Empty if Simple
	FOliveArchitectResult Architect;
	FOliveValidatorResult Validator;

	/** Total pipeline elapsed time */
	double TotalElapsedSeconds = 0.0;

	/** Whether the pipeline ran successfully enough to produce useful context */
	bool bValid = false;

	/**
	 * Format the entire pipeline result as a markdown block for injection
	 * into the Builder's prompt. This is the primary output of the pipeline.
	 *
	 * Sections included:
	 * - Task Analysis (complexity + reasoning)
	 * - Reference Templates Found (discovery block)
	 * - Build Plan (the Architect's output, with validator warnings inline)
	 * - Existing Asset Context (Scout's relevant assets, optionally with Researcher analysis)
	 *
	 * @return Formatted markdown string (typically 800-2000 tokens)
	 */
	FString FormatForPromptInjection() const;
};

// ---------------------------------------------------------------------------
// Per-agent model configuration helper
// ---------------------------------------------------------------------------

/**
 * Resolved model configuration for a specific agent role.
 * Produced by UOliveAISettings::GetAgentModelConfig().
 */
struct OLIVEAIEDITOR_API FOliveAgentModelConfig
{
	/** The resolved provider. Default-initialized to the first enum value (ClaudeCode).
	 *  Always set explicitly by GetAgentModelConfig() before use. */
	EOliveAIProvider Provider{};
	FString ModelId;
	FString ApiKey;
	FString BaseUrl;

	/** Whether this resolved to a valid configuration */
	bool bIsValid = false;

	/** Whether this falls back to CLI --print mode */
	bool bIsCLIFallback = false;
};
