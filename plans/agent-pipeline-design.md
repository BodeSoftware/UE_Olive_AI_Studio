# Agent Pipeline Design

**Status:** Approved for implementation
**Author:** Architect Agent
**Date:** 2026-03-06

## Overview

Always-on agent pipeline that runs before the Builder (Claude Code / API provider) receives the user's message. The pipeline assembles structured context -- task complexity, existing asset state, validated architectural plan -- and injects it into the Builder's prompt. This replaces the existing discovery pass + decomposition directive in `OliveCLIProviderBase::SendMessageAutonomous()` (lines 570-621).

Pipeline stages: **Router -> Scout -> [Researcher if not Simple] -> Architect -> Validator (C++) -> Builder -> Reviewer**

Sub-agents (Router, Scout, Researcher, Architect, Reviewer) are single-shot blocking LLM calls via `SendAgentCompletion()`, which reuses the tick-pump pattern from `FOliveUtilityModel::TrySendCompletion()`.

---

## 1. File Structure

### New Files

```
Source/OliveAIEditor/Public/Brain/OliveAgentConfig.h      -- Enums, result structs, agent role config
Source/OliveAIEditor/Public/Brain/OliveAgentPipeline.h     -- Pipeline class declaration
Source/OliveAIEditor/Private/Brain/OliveAgentPipeline.cpp  -- Pipeline implementation
```

### Modified Files

```
Source/OliveAIEditor/Public/Settings/OliveAISettings.h     -- Add "Agent Pipeline" category UPROPERTYs
Source/OliveAIEditor/Private/Settings/OliveAISettings.cpp  -- Constructor defaults for new settings
Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp  -- Replace lines 570-621
Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp   -- Inject pipeline into orchestrated path
Source/OliveAIEditor/Public/Chat/OliveConversationManager.h      -- Store cached pipeline result
```

### Dependencies

`OliveAgentPipeline` depends on:
- `OliveAISettings.h` (read per-agent model config)
- `IOliveAIProvider.h` (FOliveProviderConfig, FOliveProviderFactory, FOliveChatMessage)
- `OliveUtilityModel.h` (reuses ProviderEnumToName, discovery pass)
- `OliveProjectIndex.h` (SearchAssets for Scout)
- `OliveNodeFactory.h` (FindFunction for Validator)
- `OliveBlueprintReader.h` (ReadBlueprint for Scout)
- `OliveToolRegistry.h` (tool names for Scout)

Nothing depends on `OliveAgentPipeline` except the two integration points (CLIProviderBase and ConversationManager).

---

## 2. OliveAgentConfig.h -- Complete Header

```cpp
// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

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
struct FOliveRouterResult
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
struct FOliveScoutResult
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

    /** Whether the LLM call succeeded (for relevance ranking). */
    bool bSuccess = false;

    double ElapsedSeconds = 0.0;
};

/**
 * Researcher output: architectural analysis of existing assets.
 */
struct FOliveResearcherResult
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
struct FOliveArchitectResult
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
struct FOliveValidatorResult
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
struct FOliveReviewerResult
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
struct FOliveAgentModelConfig
{
    EOliveAIProvider Provider = EOliveAIProvider::OpenRouter;
    FString ModelId;
    FString ApiKey;
    FString BaseUrl;

    /** Whether this resolved to a valid configuration */
    bool bIsValid = false;

    /** Whether this falls back to CLI --print mode */
    bool bIsCLIFallback = false;
};
```

---

## 3. OliveAgentPipeline.h -- Complete Header

```cpp
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
```

---

## 4. Settings UPROPERTYs

Add the following to `UOliveAISettings` after the "Utility Model Settings" category:

```cpp
// ==========================================
// Agent Pipeline Settings
// ==========================================

/** Customize models for individual pipeline agents.
 *  When disabled, all agents use your main provider and model.
 *  When enabled, per-agent provider and model selectors appear below. */
UPROPERTY(Config, EditAnywhere, Category="Agent Pipeline",
    meta=(DisplayName="Customize Agent Models"))
bool bCustomizeAgentModels = false;

// --- Router Agent ---

/** Provider for the Router agent (classifies task complexity).
 *  Should be fast and cheap -- this is a single classification call. */
UPROPERTY(Config, EditAnywhere, Category="Agent Pipeline",
    meta=(DisplayName="Router: Provider",
          EditCondition="bCustomizeAgentModels",
          EditConditionHides,
          ToolTip="Provider for task complexity classification. Recommended: fast/cheap model."))
EOliveAIProvider RouterProvider = EOliveAIProvider::OpenRouter;

/** Model for the Router agent */
UPROPERTY(Config, EditAnywhere, Category="Agent Pipeline",
    meta=(DisplayName="Router: Model",
          EditCondition="bCustomizeAgentModels",
          EditConditionHides))
FString RouterModel = TEXT("anthropic/claude-3-5-haiku-latest");

// --- Scout Agent ---

/** Provider for the Scout agent (discovers relevant assets and templates). */
UPROPERTY(Config, EditAnywhere, Category="Agent Pipeline",
    meta=(DisplayName="Scout: Provider",
          EditCondition="bCustomizeAgentModels",
          EditConditionHides,
          ToolTip="Provider for asset/template discovery. Recommended: fast/cheap model."))
EOliveAIProvider ScoutProvider = EOliveAIProvider::OpenRouter;

/** Model for the Scout agent */
UPROPERTY(Config, EditAnywhere, Category="Agent Pipeline",
    meta=(DisplayName="Scout: Model",
          EditCondition="bCustomizeAgentModels",
          EditConditionHides))
FString ScoutModel = TEXT("anthropic/claude-3-5-haiku-latest");

// --- Researcher Agent ---

/** Provider for the Researcher agent (analyzes architecture of existing assets).
 *  Only runs for Moderate/Complex tasks. */
UPROPERTY(Config, EditAnywhere, Category="Agent Pipeline",
    meta=(DisplayName="Researcher: Provider",
          EditCondition="bCustomizeAgentModels",
          EditConditionHides,
          ToolTip="Provider for existing asset analysis. Runs only on Moderate/Complex tasks."))
EOliveAIProvider ResearcherProvider = EOliveAIProvider::OpenRouter;

/** Model for the Researcher agent */
UPROPERTY(Config, EditAnywhere, Category="Agent Pipeline",
    meta=(DisplayName="Researcher: Model",
          EditCondition="bCustomizeAgentModels",
          EditConditionHides))
FString ResearcherModel = TEXT("anthropic/claude-3-5-haiku-latest");

// --- Architect Agent ---

/** Provider for the Architect agent (produces the Build Plan).
 *  This is the most important sub-agent -- use a capable model. */
UPROPERTY(Config, EditAnywhere, Category="Agent Pipeline",
    meta=(DisplayName="Architect: Provider",
          EditCondition="bCustomizeAgentModels",
          EditConditionHides,
          ToolTip="Provider for Build Plan generation. Recommended: capable reasoning model (Sonnet/GPT-4o)."))
EOliveAIProvider ArchitectProvider = EOliveAIProvider::OpenRouter;

/** Model for the Architect agent */
UPROPERTY(Config, EditAnywhere, Category="Agent Pipeline",
    meta=(DisplayName="Architect: Model",
          EditCondition="bCustomizeAgentModels",
          EditConditionHides))
FString ArchitectModel = TEXT("anthropic/claude-sonnet-4");

// --- Reviewer Agent ---

/** Provider for the Reviewer agent (checks Builder output against Build Plan).
 *  Runs after the Builder completes. */
UPROPERTY(Config, EditAnywhere, Category="Agent Pipeline",
    meta=(DisplayName="Reviewer: Provider",
          EditCondition="bCustomizeAgentModels",
          EditConditionHides,
          ToolTip="Provider for post-build review. Recommended: same tier as Architect."))
EOliveAIProvider ReviewerProvider = EOliveAIProvider::OpenRouter;

/** Model for the Reviewer agent */
UPROPERTY(Config, EditAnywhere, Category="Agent Pipeline",
    meta=(DisplayName="Reviewer: Model",
          EditCondition="bCustomizeAgentModels",
          EditConditionHides))
FString ReviewerModel = TEXT("anthropic/claude-sonnet-4");
```

### Helper Method

Add to `UOliveAISettings` public interface:

```cpp
/**
 * Resolve the provider configuration for a specific agent role.
 * When bCustomizeAgentModels is false, returns the main provider+model config.
 * When true, returns the per-agent config.
 * Falls through to CLI --print if no API key is available.
 *
 * @param Role  The agent role to resolve
 * @return Resolved model configuration (check bIsValid)
 */
FOliveAgentModelConfig GetAgentModelConfig(EOliveAgentRole Role) const;
```

Implementation approach for `GetAgentModelConfig`:

```cpp
FOliveAgentModelConfig UOliveAISettings::GetAgentModelConfig(EOliveAgentRole Role) const
{
    FOliveAgentModelConfig Config;

    // Determine provider + model for this role
    EOliveAIProvider TargetProvider;
    FString TargetModel;

    if (!bCustomizeAgentModels)
    {
        // Use main provider + model for all agents (NOT utility model)
        TargetProvider = AIProvider;
        TargetModel = ModelId;
    }
    else
    {
        // Per-agent config
        switch (Role)
        {
        case EOliveAgentRole::Router:
            TargetProvider = RouterProvider;
            TargetModel = RouterModel;
            break;
        case EOliveAgentRole::Scout:
            TargetProvider = ScoutProvider;
            TargetModel = ScoutModel;
            break;
        case EOliveAgentRole::Researcher:
            TargetProvider = ResearcherProvider;
            TargetModel = ResearcherModel;
            break;
        case EOliveAgentRole::Architect:
            TargetProvider = ArchitectProvider;
            TargetModel = ArchitectModel;
            break;
        case EOliveAgentRole::Reviewer:
            TargetProvider = ReviewerProvider;
            TargetModel = ReviewerModel;
            break;
        default:
            TargetProvider = AIProvider;
            TargetModel = ModelId;
            break;
        }
    }

    // Skip CLI providers (handled as fallback tier)
    if (TargetProvider == EOliveAIProvider::ClaudeCode
        || TargetProvider == EOliveAIProvider::Codex)
    {
        Config.bIsCLIFallback = true;
        return Config;
    }

    Config.Provider = TargetProvider;
    Config.ModelId = TargetModel;

    // Resolve API key: per-provider field (main provider key) -> utility model key -> empty
    Config.ApiKey = GetApiKeyForProvider(TargetProvider);
    if (Config.ApiKey.IsEmpty())
    {
        Config.ApiKey = UtilityModelApiKey; // Fallback for custom agent models using utility key
    }

    // Resolve base URL
    Config.BaseUrl = GetBaseUrlForProvider(TargetProvider);

    const bool bNeedsKey = TargetProvider != EOliveAIProvider::Ollama
                        && TargetProvider != EOliveAIProvider::OpenAICompatible;
    Config.bIsValid = !TargetModel.IsEmpty() && (!bNeedsKey || !Config.ApiKey.IsEmpty());

    if (!Config.bIsValid)
    {
        Config.bIsCLIFallback = true; // Will fall through to CLI --print
    }

    return Config;
}
```

---

## 5. System Prompt Text for Each Agent

### 5.1 Router System Prompt (~180 tokens)

```
You classify Unreal Engine Blueprint tasks by complexity.

SIMPLE: Single asset, 1-3 functions, no inter-asset communication.
  Examples: a health pickup, a spinning actor, a door with timeline.

MODERATE: 2-3 assets OR single complex asset (5+ functions, components with state).
  Examples: a weapon with projectile, a door with key, AI character with behavior tree.

COMPLEX: 4+ assets, cross-system interactions, or requires event dispatchers between assets.
  Examples: inventory system, full combat suite with melee/ranged/projectile/damage.

Respond with EXACTLY one line:
COMPLEXITY: [SIMPLE|MODERATE|COMPLEX]

Then a second line with a brief reason (max 20 words).

No other text.
```

### 5.2 Scout System Prompt (~140 tokens)

```
You rank Unreal Engine project assets by relevance to a task.

Given a list of existing assets (path + class) and a task description, output the TOP 5 most relevant assets. For each, explain in <10 words WHY it is relevant (e.g., "parent class for weapon actors", "existing projectile to reference").

Format:
1. /Game/Path/AssetName (ClassName) -- reason
2. ...

If none are relevant, output: NONE

No other text.
```

### 5.3 Researcher System Prompt (~280 tokens)

```
You analyze Unreal Engine Blueprint architecture for an AI Builder agent.

Given Blueprint IR data (variables, components, functions, interfaces, dispatchers), produce a concise architectural summary that helps the Builder understand:

1. The asset's role (what it does, its parent class)
2. Public interface: functions other assets call, event dispatchers they bind to
3. Component architecture: which components carry state, which handle collision
4. Integration points: interfaces implemented, dispatch events exposed

Format per asset:

### AssetName
- **Role**: One sentence
- **Parent**: ClassName
- **Public API**: Function1(params), Function2(params)
- **Dispatchers**: OnDamage(float), OnDeath()
- **Components**: CompName (UClass) -- purpose
- **Interfaces**: IInterface1, IInterface2

Keep each asset summary under 150 words. Focus on information the Builder needs to CREATE NEW assets that interact with these existing ones.
```

### 5.4 Architect System Prompt (~400 tokens)

```
You are an Unreal Engine Blueprint architect. Given a task description, existing asset context, and template references, produce a Build Plan.

## Build Plan Format

### Order
1. BP_Name (create)
2. @ExistingBP (modify)

### BP_Name
- **Action**: create
- **Parent Class**: <real UE class, e.g., AActor, APawn, ACharacter, UActorComponent>
- **Components**: VarName (UComponentClass) -- purpose
- **Variables**: VarName (Type, default: value) -- purpose
- **Event Dispatchers**: Name(ParamType Param, ...) -- purpose
- **Interfaces**: UInterfaceName -- purpose
- **Functions**: Name(Params) -> ReturnType -- natural language description of logic
- **Events**: EventName [ComponentName if delegate] -- what happens

### @ExistingBP
- **Action**: modify
- **Add Variables**: ...
- **Add Functions**: ...
- **Add Events**: ...

### Interactions
- How assets communicate (dispatchers, interfaces, direct calls)

## Rules
- Order is mandatory: assets must be listed in dependency order (referenced before referencing)
- Use UE class names. Short names are fine (Actor, Character, SphereComponent) -- the Validator normalizes them
- Function logic is natural language (the Builder translates to plan_json)
- For delegate events, specify which component they belong to (e.g., OnComponentBeginOverlap [BoxComp])
- Modify blocks (@prefix) only list CHANGES, not the full asset
- If a function needs latent operations (Delay, Timeline), note it must be a Custom Event, not a Function
- Keep function descriptions to 1-2 sentences
```

### 5.5 Reviewer System Prompt (~200 tokens)

```
You review the output of an Unreal Engine Blueprint Builder against a Build Plan.

Given:
1. The original Build Plan (from the Architect)
2. The current state of each asset (variables, components, functions, dispatchers, compile status)

Compare the actual state to the plan. Report:

SATISFIED: (if everything in the plan exists in the assets)

OR:

MISSING:
- BP_Name: MissingFunction(params), MissingVariable, etc.
- @ExistingBP: MissingEvent, etc.

DEVIATIONS:
- BP_Name: FunctionX has wrong signature (planned: X, actual: Y)

CORRECTION:
One paragraph telling the Builder exactly what to do next to complete the plan.

Be precise. Only report genuinely missing items -- do not flag cosmetic differences (naming case, extra helper functions).
```

---

## 6. SendAgentCompletion Implementation

### Provider Resolution

```
For a given EOliveAgentRole:
1. Call UOliveAISettings::Get()->GetAgentModelConfig(Role)
2. If Config.bIsValid:
   a. Create transient provider via FOliveProviderFactory::CreateProvider(ProviderEnumToName(Config.Provider))
   b. Configure with Config fields + role-specific Temperature/MaxTokens
   c. Call SendMessage with [SystemMsg, UserMsg], empty tools array
   d. Tick-pump until completion (identical to FOliveUtilityModel::TrySendCompletion)
3. If Config.bIsCLIFallback OR step 2 failed:
   a. Try main provider (if HTTP-based, same as utility model tier 2)
   b. Try claude --print fallback (same as utility model tier 3)
```

### Per-Agent Parameters

| Role       | Temperature | MaxTokens | Timeout (s) |
|------------|-------------|-----------|-------------|
| Router     | 0.0         | 64        | 10          |
| Scout      | 0.0         | 256       | 10          |
| Researcher | 0.2         | 512       | 15          |
| Architect  | 0.2         | 2048      | 30          |
| Reviewer   | 0.0         | 512       | 15          |

Router gets 0.0 temperature because it is pure classification. Scout gets 0.0 for deterministic ranking. Architect and Researcher get 0.2 for light creativity in plan generation. Reviewer gets 0.0 for strict comparison.

### Tick-Pump Pattern

Identical to `FOliveUtilityModel::TrySendCompletion()` lines 291-351. The key implementation is:

```cpp
// Pump the ticker until completion or timeout
const double StartTime = FPlatformTime::Seconds();
while (!bCompleted)
{
    if (FPlatformTime::Seconds() - StartTime >= TimeoutLimit)
    {
        Provider->CancelRequest();
        OutError = FString::Printf(TEXT("Agent %s timed out after %.1fs"),
            *RoleToString(Role), TimeoutLimit);
        return false;
    }
    FTSTicker::GetCoreTicker().Tick(0.01f);
    FPlatformProcess::Sleep(0.01f);
}
```

### CLI --print Path

For users on Claude Max/Codex with no API key, the fallback uses `FPlatformProcess::ExecProcess` exactly as `FOliveUtilityModel::TrySendCompletionViaCLI()`. The system prompt and user prompt are concatenated into a single `--print` argument.

---

## 7. FormatForPromptInjection

This is what the Builder actually receives. It replaces the old "Required: Asset Decomposition" block.

```cpp
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
    Output += FString::Printf(TEXT("**Complexity**: %s"),
        Router.Complexity == EOliveTaskComplexity::Simple ? TEXT("Simple") :
        Router.Complexity == EOliveTaskComplexity::Moderate ? TEXT("Moderate") :
        TEXT("Complex"));
    if (!Router.Reasoning.IsEmpty())
    {
        Output += TEXT(" -- ") + Router.Reasoning;
    }
    Output += TEXT("\n\n");

    // Section 2: Reference Templates (from Scout's discovery)
    if (!Scout.DiscoveryBlock.IsEmpty())
    {
        Output += Scout.DiscoveryBlock;
        Output += TEXT("\n\n");
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
                Output += FString::Printf(TEXT("- **%s** (%s): %s"),
                    *Issue.AssetName, *Issue.Category, *Issue.Message);
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
            Output += FString::Printf(TEXT("- %s (%s) -- %s\n"),
                *Asset.Path, *Asset.AssetClass, *Asset.Relevance);
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
    Output += TEXT("2. Write ALL graph logic with apply_plan_json for every function/event\n");
    Output += TEXT("3. Compile to 0 errors before moving to the next asset\n");
    Output += TEXT("Do not stop until every asset in the plan is fully built and compiled.\n");

    return Output;
}
```

---

## 8. Integration Points

### 8.1 CLIProviderBase::SendMessageAutonomous -- Replace lines 570-621

The current code at lines 570-621 does:
1. Template discovery pass (lines 570-598) -- `FOliveUtilityModel::RunDiscoveryPass`
2. Decomposition directive (lines 600-621) -- hardcoded "Required: Asset Decomposition" text

Replace with:

```cpp
// Agent pipeline: runs Router -> Scout -> Researcher -> Architect -> Validator.
// Produces a Build Plan with validated class names that replaces the old
// discovery pass + decomposition directive.
if (!IsContinuationMessage(UserMessage))
{
    FOliveAgentPipeline Pipeline;
    CachedPipelineResult = Pipeline.Execute(EffectiveMessage, InitialContextAssetPaths);

    if (CachedPipelineResult.bValid)
    {
        FString PipelineBlock = CachedPipelineResult.FormatForPromptInjection();
        if (!PipelineBlock.IsEmpty())
        {
            EffectiveMessage += TEXT("\n\n");
            EffectiveMessage += PipelineBlock;

            UE_LOG(LogOliveCLIProvider, Log,
                TEXT("Agent pipeline: %s complexity, %d assets planned, %.1fs total"),
                CachedPipelineResult.Router.Complexity == EOliveTaskComplexity::Simple ? TEXT("Simple") :
                CachedPipelineResult.Router.Complexity == EOliveTaskComplexity::Moderate ? TEXT("Moderate") :
                TEXT("Complex"),
                CachedPipelineResult.Architect.AssetOrder.Num(),
                CachedPipelineResult.TotalElapsedSeconds);
        }
    }
}
```

The "Tool Execution Requirement" guardrail (lines 623-629) is retained as-is.

**New member on `FOliveCLIProviderBase`:**
```cpp
/** Cached pipeline result from the most recent run. Used by Reviewer on completion. */
FOliveAgentPipelineResult CachedPipelineResult;
```

### 8.2 CLIProviderBase::HandleResponseCompleteAutonomous -- Reviewer integration

After the existing auto-continue logic, before marking the run as complete:

```cpp
// Reviewer pass: compare Builder output against the Build Plan.
// Only runs if:
//   1. The pipeline produced a valid Build Plan
//   2. The Builder modified at least one asset
//   3. This is not already a correction pass (prevent infinite loop)
if (CachedPipelineResult.bValid
    && CachedPipelineResult.Architect.bSuccess
    && LastRunContext.ModifiedAssetPaths.Num() > 0
    && !bIsReviewerCorrectionPass)
{
    FOliveAgentPipeline Pipeline;
    FOliveReviewerResult Review = Pipeline.RunReviewer(
        CachedPipelineResult, LastRunContext.ModifiedAssetPaths);

    if (Review.bSuccess && !Review.bPlanSatisfied && !Review.CorrectionDirective.IsEmpty())
    {
        UE_LOG(LogOliveCLIProvider, Log,
            TEXT("Reviewer found %d missing items, %d deviations. Triggering correction pass."),
            Review.MissingItems.Num(), Review.Deviations.Num());

        // Trigger one correction pass by feeding findings back to the Builder.
        bIsReviewerCorrectionPass = true;
        bIsAutoContinuation = true;

        FString CorrectionMessage = TEXT("## Review Findings\n\n");
        CorrectionMessage += Review.CorrectionDirective;
        CorrectionMessage += TEXT("\n\nComplete these remaining items now.");

        AsyncTask(ENamedThreads::GameThread, [this, CorrectionMessage]()
        {
            if (*AliveGuard)
            {
                SendMessageAutonomous(
                    CorrectionMessage,
                    CurrentOnChunk,
                    CurrentOnComplete,
                    CurrentOnError);
            }
        });
        return; // Do not signal completion yet
    }
}

// Reset reviewer flag for next run
bIsReviewerCorrectionPass = false;
```

**New members on `FOliveCLIProviderBase`:**
```cpp
/** Whether the current run is a Reviewer-triggered correction pass.
 *  Prevents infinite review -> correct -> review loop. */
bool bIsReviewerCorrectionPass = false;
```

### 8.3 ConversationManager -- Orchestrated Path (API providers)

In `SendUserMessage()`, after `Brain->BeginRun()` (line ~475) and before `SendToProvider()` (line ~488):

```cpp
// Run agent pipeline for non-trivial tasks (orchestrated providers).
// Pipeline result is cached for potential Reviewer pass on HandleComplete.
if (bTurnHasExplicitWriteIntent)
{
    FOliveAgentPipeline Pipeline;
    CachedPipelineResult = Pipeline.Execute(Message, ActiveContextPaths);
}
else
{
    CachedPipelineResult = FOliveAgentPipelineResult(); // Reset
}
```

In `BuildSystemMessage()`, after the operation history injection (line ~633):

```cpp
// Inject agent pipeline context if available
if (CachedPipelineResult.bValid)
{
    FString PipelineContext = CachedPipelineResult.FormatForPromptInjection();
    if (!PipelineContext.IsEmpty())
    {
        SystemMessage.Content += TEXT("\n\n") + PipelineContext;
    }
}
```

In `HandleComplete()`, in the no-tool-calls branch after determining `FinalOutcome` (around line ~931), add the Reviewer:

```cpp
// Reviewer pass for orchestrated providers
if (CachedPipelineResult.bValid
    && CachedPipelineResult.Architect.bSuccess
    && !bIsReviewerCorrectionPass)
{
    // Collect modified assets from operation history
    TArray<FString> ModifiedAssets;
    // ... extract from HistoryStore or ActiveContextPaths ...

    if (ModifiedAssets.Num() > 0)
    {
        FOliveAgentPipeline ReviewPipeline;
        FOliveReviewerResult Review = ReviewPipeline.RunReviewer(
            CachedPipelineResult, ModifiedAssets);

        if (Review.bSuccess && !Review.bPlanSatisfied)
        {
            bIsReviewerCorrectionPass = true;

            FOliveChatMessage CorrectionMsg;
            CorrectionMsg.Role = EOliveChatRole::User;
            CorrectionMsg.Content = TEXT("## Review Findings\n\n")
                + Review.CorrectionDirective
                + TEXT("\n\nComplete these remaining items.");
            CorrectionMsg.Timestamp = FDateTime::UtcNow();
            AddMessage(CorrectionMsg);

            SendToProvider();
            return; // Re-enter agentic loop for one correction pass
        }
    }
}
bIsReviewerCorrectionPass = false;
```

**New members on `FOliveConversationManager`:**
```cpp
/** Cached agent pipeline result for the current turn */
FOliveAgentPipelineResult CachedPipelineResult;

/** Whether we are in a Reviewer-triggered correction pass */
bool bIsReviewerCorrectionPass = false;
```

---

## 9. Validator Implementation Detail

The Validator is pure C++ -- no LLM call. It parses the Architect's Build Plan text and checks each claim against engine reflection.

### Parsing the Build Plan

`ParseBuildPlan()` uses line-by-line scanning with regex-like matching:

```
For each line:
  "### BP_Name" or "### @ExistingBP" -> new asset block, extract name
  "- **Parent Class**: X"    -> extract X into ParentClasses["BP_Name"]
  "- **Components**: V (C)"  -> extract VarName, ClassName into Components["BP_Name"]
  "- **Interfaces**: X"      -> extract X into Interfaces["BP_Name"]
  "### Order" -> parse numbered list into AssetOrder
```

Implementation note: Use `FRegexMatcher` with patterns like:
```
TEXT("^-\\s*\\*\\*Parent Class\\*\\*:\\s*(.+)$")
TEXT("^-\\s*\\*\\*Components\\*\\*:\\s*(\\w+)\\s*\\(([^)]+)\\)")
```

### Validation Checks

For each asset in the parsed plan:

1. **Parent Class**: `TryResolveClass(ClassName)` normalizes short names to full UE names:
   - `FindFirstObjectSafe<UClass>(Name)` -- exact match (handles both `AActor` and `Actor`)
   - Try with `U` prefix, then `A` prefix
   - Short-name aliases: `Actor`→`AActor`, `Pawn`→`APawn`, `Character`→`ACharacter`, `PlayerController`→`APlayerController`, `GameModeBase`→`AGameModeBase`, `ActorComponent`→`UActorComponent`, `SceneComponent`→`USceneComponent`
   - Component short names: `SphereComponent`→`USphereComponent`, `BoxComponent`→`UBoxComponent`, `StaticMeshComponent`→`UStaticMeshComponent`, `SkeletalMeshComponent`→`USkeletalMeshComponent`, `CapsuleComponent`→`UCapsuleComponent`, `ProjectileMovementComponent`→`UProjectileMovementComponent`, `FloatingPawnMovement`→`UFloatingPawnMovement`, `CharacterMovementComponent`→`UCharacterMovementComponent`
   - If resolved: validator issues carry the **normalized** name so the Builder sees corrected output
   - If not found: issue warning with "Class not found" + closest alias suggestion

2. **Components**: `TryResolveComponentClass(ClassName)` via:
   - Same resolution as parent class (including short-name aliases), but verify `->IsChildOf(UActorComponent::StaticClass())`
   - If class found but not a component: "X is not a UActorComponent subclass"

3. **Interfaces**: `IsValidInterface(Name)` via:
   - `FindFirstObjectSafe<UClass>(Name)` where class `->IsChildOf(UInterface::StaticClass())`
   - Asset registry search for Blueprint Interface assets with matching name
   - If not found: warning (not blocking -- interface might be created during the run)

4. **Modify targets**: For `@ExistingBP` entries, verify the asset exists via `FOliveProjectIndex::Get().FindAsset(Path)`. If not found: blocking issue.

### Blocking vs Non-Blocking

Only truly fatal issues block the pipeline:
- `@ExistingBP` references an asset that does not exist (blocking)
- All other issues are warnings surfaced inline in the Builder's prompt

The Validator sets `bHasBlockingIssues` only for the first category. The pipeline still produces a result even with non-blocking issues -- they appear as "Validator Warnings" in the Builder's prompt.

---

## 10. Reviewer Integration Detail

### When to Use LLM Review vs C++-Only

The Reviewer ALWAYS uses an LLM call. The input it receives is:

1. The original Build Plan (from `CachedPipelineResult.Architect.BuildPlan`)
2. Current asset state snapshots (from `BuildAssetStateSummary(ModifiedAssets)`)

The LLM compares these two and produces structured output (SATISFIED / MISSING / DEVIATIONS / CORRECTION).

### One Correction Pass Limit

The `bIsReviewerCorrectionPass` flag ensures exactly one correction pass:
- First Builder run: `bIsReviewerCorrectionPass = false`. On completion, Reviewer runs. If issues found, set flag to true and re-launch Builder with correction directive.
- Correction run: `bIsReviewerCorrectionPass = true`. On completion, Reviewer is skipped. Flag reset to false.

This prevents infinite review loops while giving the Builder one chance to fix omissions.

### Reviewer User Prompt Assembly

```cpp
FString BuildReviewerUserPrompt(
    const FOliveArchitectResult& Architect,
    const TArray<FString>& ModifiedAssets)
{
    FString Prompt;

    Prompt += TEXT("## Original Build Plan\n\n");
    Prompt += Architect.BuildPlan;
    Prompt += TEXT("\n\n## Current Asset State\n\n");

    // Use BuildAssetStateSummary to get live Blueprint state
    // This loads each Blueprint and reports: components, variables,
    // functions (with node counts), dispatchers, compile status
    Prompt += BuildAssetStateSummary(ModifiedAssets);

    return Prompt;
}
```

### BuildAssetStateSummary() Specification

Loads each modified Blueprint and produces a structured markdown summary for the Reviewer LLM to compare against the Build Plan.

**Signature:**
```cpp
static FString BuildAssetStateSummary(const TArray<FString>& AssetPaths);
```

**Per-Asset Fields (in order):**
1. **Asset path** — `/Game/...` path
2. **Parent class** — native parent's `GetName()` (e.g., `Actor`, `Character`)
3. **Compile status** — `OK`, `Error`, `Unknown` (from `Blueprint->Status`)
4. **Components** — SCS component list: `VarName (UClassName)`
5. **Variables** — `NewVariables` list: `VarName (TypeName, default: DefaultValue)`. Skip `DefaultComponent_*` vars (SCS-internal)
6. **Event Dispatchers** — from `NewVariables` where `VarProperty` is `FMulticastDelegateProperty`: `Name(ParamTypes)`
7. **Interfaces** — `ImplementedInterfaces` list: `InterfaceName`
8. **Functions** — `FunctionGraphs` list: `FunctionName (N nodes)`. Excludes internal graphs (`ConstructionScript`, `UserConstructionScript`, `ExecuteUbergraph_*`)
9. **Event Graphs** — Events found in `UbergraphPages`: list entry point node names (e.g., `BeginPlay`, `Tick`, `OnComponentBeginOverlap [CompName]`)

**Output Format:**
```markdown
### /Game/Blueprints/BP_ArrowProjectile
- **Parent**: Actor
- **Status**: OK
- **Components**: CollisionSphere (USphereComponent), ArrowMesh (UStaticMeshComponent), ProjectileMovement (UProjectileMovementComponent)
- **Variables**: Damage (Float, default: 25.0), ArrowSpeed (Float, default: 4000.0), bHasHit (Bool, default: false)
- **Dispatchers**: OnArrowHit(AActor*, Float)
- **Interfaces**: (none)
- **Functions**: LaunchArrow (4 nodes)
- **Events**: BeginPlay, OnComponentBeginOverlap [CollisionSphere]
```

**Token Budget:** Target ~150 tokens per asset. For a 4-asset build, ~600 tokens total. Max 8 assets reported (if more modified, report the ones matching `Architect.AssetOrder` first). If an asset can't be loaded, emit one line: `### /Game/Path — FAILED TO LOAD`.

**Implementation Notes:**
- Use `FOliveBlueprintReader::ReadBlueprint()` (already exists) to get `FOliveBlueprintIR` — this has components, variables, functions, interfaces already parsed
- For node counts: `Graph->Nodes.Num()` minus comment/reroute nodes
- For event entry points: scan `UbergraphPages` for `UK2Node_Event` subclasses, extract `EventReference.GetMemberName()`
- For compile status: read `Blueprint->Status` directly (`BS_UpToDate`, `BS_Dirty`, `BS_Error`)
- Skip `DefaultSceneRoot` component (always present, not architecturally relevant)
- Omit sections that are empty (no "Interfaces: (none)" — just skip the line)

### Reviewer Response Parsing

```
If response starts with "SATISFIED" -> bPlanSatisfied = true, done.
Otherwise:
  Lines between "MISSING:" and "DEVIATIONS:" -> MissingItems
  Lines between "DEVIATIONS:" and "CORRECTION:" -> Deviations
  Lines after "CORRECTION:" -> CorrectionDirective
```

---

## 11. Data Flow Diagram

```
User Message
    |
    v
[Router] -- "COMPLEX" -- reasoning
    |
    v
[Scout]  -- keyword search + discovery pass
    |        |
    |        +-- project index search -> relevant assets
    |        +-- template discovery -> library/factory/community matches
    |
    v
[Researcher] (skipped if Simple)
    |        |
    |        +-- loads Blueprint IR for Scout's relevant assets
    |        +-- produces architectural analysis
    |
    v
[Architect] -- receives: user message + complexity + relevant assets
    |              + researcher analysis + template references
    |           -- produces: Build Plan (markdown)
    |
    v
[Validator] (C++ only, no LLM)
    |        |
    |        +-- parses Build Plan for class names
    |        +-- resolves via FindFirstObjectSafe / asset registry
    |        +-- produces: validation issues with suggestions
    |
    v
FOliveAgentPipelineResult
    |
    v
FormatForPromptInjection()
    |
    v
Injected into Builder's stdin (autonomous) or system message (orchestrated)
    |
    v
[Builder runs...] -- creates/modifies Blueprints via MCP tools
    |
    v
[Reviewer] -- compares live asset state vs Build Plan
    |
    +-- SATISFIED -> done
    +-- MISSING   -> one correction pass -> Builder re-runs -> done
```

---

## 12. Edge Cases & Error Handling

### Agent LLM Call Failures

Each stage handles failure gracefully:

| Stage      | On Failure                                                |
|------------|-----------------------------------------------------------|
| Router     | Default to `Moderate` (safe middle ground)                 |
| Scout      | Still runs discovery pass; skip LLM relevance ranking      |
| Researcher | Skip entirely; Architect works without analysis            |
| Architect  | Fall back to old decomposition directive text               |
| Validator  | Skip validation; plan goes to Builder unvalidated          |
| Reviewer   | Skip review; Builder output accepted as-is                 |

### Continuation Messages

The pipeline does NOT run on continuation messages (detected by `IsContinuationMessage()`). Continuations use the cached `CachedPipelineResult` from the initial run. If the user sends a genuinely new task, the cache is overwritten.

### Read-Only / Question Messages

The pipeline only runs when `bTurnHasExplicitWriteIntent` is true (detected by `DetectWriteIntent()` in ConversationManager, or `MessageImpliesMutation()` in CLIProviderBase). For read-only queries ("read BP_Gun", "what does this do"), no pipeline stages execute.

### Empty / Trivial Projects

If the project has no existing assets (new project), Scout returns empty results, Researcher is skipped, and Architect produces a plan purely from the user's description. This is the expected path for greenfield projects.

### Pipeline Timeout

Total pipeline budget: ~60 seconds worst case (Router 10s + Scout 10s + Researcher 15s + Architect 30s). Each stage has its own timeout. If the total exceeds 60s, remaining stages are skipped and partial results are used.

---

## 13. Implementation Order

### Phase 1: Foundation (OliveAgentConfig.h + Settings)

**Files:** `OliveAgentConfig.h`, `OliveAISettings.h`, `OliveAISettings.cpp`

1. Create `OliveAgentConfig.h` with all enums and structs
2. Add `Agent Pipeline` category UPROPERTYs to `OliveAISettings.h`
3. Implement `GetAgentModelConfig()` in `OliveAISettings.cpp`
4. Build and verify no compile errors

### Phase 2: Pipeline Core (SendAgentCompletion + Router)

**Files:** `OliveAgentPipeline.h`, `OliveAgentPipeline.cpp`

1. Create `OliveAgentPipeline.h` with the full class declaration
2. Implement `SendAgentCompletion()` -- copy tick-pump from `FOliveUtilityModel::TrySendCompletion()`, add role-based model resolution via `GetAgentModelConfig()`, add CLI `--print` fallback
3. Implement `BuildRouterSystemPrompt()` and `RunRouter()` + `ParseRouterResponse()`
4. Write a minimal `Execute()` that only runs Router and returns
5. Manually test by calling `Execute()` from a temporary tool handler or test

### Phase 3: Scout + Discovery

1. Implement `BuildScoutSystemPrompt()` and `RunScout()`
2. Scout user prompt: extract keywords from user message, run `FOliveProjectIndex::Get().SearchAssets()`, format raw asset list, call LLM to rank, parse response
3. Integrate existing `FOliveUtilityModel::RunDiscoveryPass()` for template discovery (reuse, do not duplicate)
4. Add Scout stage to `Execute()`

### Phase 4: Researcher

1. Implement `BuildResearcherSystemPrompt()` and `RunResearcher()`
2. Researcher user prompt: for each relevant asset from Scout, load Blueprint IR via `FOliveBlueprintReader::Get().ReadBlueprint()`, format as structured text
3. Only runs when Router.Complexity != Simple
4. Add Researcher stage to `Execute()`

### Phase 5: Architect + ParseBuildPlan

1. Implement `BuildArchitectSystemPrompt()` and `RunArchitect()`
2. Architect user prompt: user message + complexity + relevant assets + researcher analysis + discovery block
3. Implement `ParseBuildPlan()` -- line-by-line parser extracting asset order, parent classes, components, interfaces
4. Add Architect stage to `Execute()`

### Phase 6: Validator

1. Implement `TryResolveClass()`, `TryResolveComponentClass()`, `IsValidInterface()`
2. Implement `RunValidator()` -- iterate parsed plan data, call resolve helpers, collect issues
3. Add Validator stage to `Execute()`
4. Implement `FormatForPromptInjection()` on `FOliveAgentPipelineResult`

### Phase 7: CLIProviderBase Integration

1. Replace lines 570-621 in `SendMessageAutonomous()` with pipeline call
2. Add `CachedPipelineResult` member to `FOliveCLIProviderBase`
3. Verify autonomous mode works end-to-end with pipeline context

### Phase 8: ConversationManager Integration (Orchestrated Path)

1. Add `CachedPipelineResult` and `bIsReviewerCorrectionPass` members
2. Insert pipeline call in `SendUserMessage()` before `SendToProvider()`
3. Insert pipeline context injection in `BuildSystemMessage()`
4. Verify orchestrated path works with API providers

### Phase 9: Reviewer

1. Implement `BuildReviewerSystemPrompt()` and `RunReviewer()`
2. Reviewer user prompt: Build Plan + `BuildAssetStateSummary(ModifiedAssets)`
3. Parse reviewer response (SATISFIED / MISSING / DEVIATIONS / CORRECTION)
4. Integrate into `HandleResponseCompleteAutonomous()` (autonomous path)
5. Integrate into `HandleComplete()` (orchestrated path)
6. Add `bIsReviewerCorrectionPass` flag to both paths
7. Test: create a task that deliberately omits one planned function, verify Reviewer catches it

### Phase 10: Polish & Testing

1. Add `DEFINE_LOG_CATEGORY_STATIC(LogOliveAgentPipeline, Log, All)` with structured logging at each stage
2. Verify continuation messages bypass pipeline correctly
3. Verify read-only queries bypass pipeline correctly
4. Test with no API key (CLI --print fallback)
5. Test with bCustomizeAgentModels = true (per-agent model selection)
6. Performance check: measure total pipeline latency on Simple/Moderate/Complex tasks

---

## 14. Key Design Decisions & Rationale

### Why not a singleton?

The pipeline is instantiated per run. It has no persistent state between runs (unlike FOliveNodeCatalog or FOliveToolRegistry). The `CachedPipelineResult` lives on the caller (CLIProviderBase or ConversationManager), not on the pipeline itself.

### Why blocking (tick-pump) instead of async?

The pipeline must complete before the Builder starts. Making it async would require complex state management to hold the Builder's launch until all stages finish. The tick-pump pattern (proven in FOliveUtilityModel) blocks the game thread while keeping the engine responsive for HTTP callbacks. Each stage takes 1-5 seconds typically.

### Why not reuse FOliveUtilityModel::SendSimpleCompletion directly?

`SendSimpleCompletion` uses the utility model config for all calls. The agent pipeline needs per-role model resolution (Router uses Haiku, Architect uses Sonnet). `SendAgentCompletion` adds this role-based resolution layer while reusing the same tick-pump mechanics.

### Why parse the Build Plan with regex instead of structured JSON output?

Markdown is more natural for LLMs to produce and more readable in the Builder's prompt. JSON would require strict format enforcement and error-prone parsing of LLM output. The regex parser only needs to extract class names for validation -- the full markdown plan is passed through verbatim to the Builder.

### Why one correction pass, not two?

Diminishing returns. The first correction pass catches 80% of omissions. A second pass risks the Builder producing garbage if the Reviewer's feedback is ambiguous. One pass is the sweet spot between thoroughness and reliability.
