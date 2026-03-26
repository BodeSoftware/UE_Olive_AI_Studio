// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Brain/OliveBrainState.h"

/**
 * Prompt Assembler
 *
 * Assembles system prompts from templates and context.
 * Handles token estimation and context truncation.
 * Uses EOliveChatMode (Code/Plan/Ask) to append mode-specific behavioral suffixes.
 */
class OLIVEAIEDITOR_API FOlivePromptAssembler
{
public:
	/** Get singleton instance */
	static FOlivePromptAssembler& Get();

	/** Initialize by loading prompt templates */
	void Initialize();

	// ==========================================
	// Prompt Assembly
	// ==========================================

	/**
	 * Assemble the full system prompt
	 * @param Mode Active chat mode (Code, Plan, or Ask)
	 * @param ContextAssetPaths Assets in context
	 * @param MaxTokens Maximum tokens for context
	 * @return Assembled system prompt
	 */
	FString AssembleSystemPrompt(
		EOliveChatMode Mode,
		const TArray<FString>& ContextAssetPaths,
		int32 MaxTokens = 4000
	);

	/**
	 * Assemble full prompt with a base override.
	 * @param BasePromptOverride Prompt text to use as the base component
	 * @param Mode Active chat mode (Code, Plan, or Ask)
	 * @param ContextAssetPaths Assets in context
	 * @param MaxTokens Maximum tokens for context
	 * @return Assembled system prompt
	 */
	FString AssembleSystemPromptWithBase(
		const FString& BasePromptOverride,
		EOliveChatMode Mode,
		const TArray<FString>& ContextAssetPaths,
		int32 MaxTokens = 4000
	);

	// ==========================================
	// Components
	// ==========================================

	/** Get the base system prompt */
	const FString& GetBasePrompt() const { return BasePromptTemplate; }

	/** Get project context as string */
	FString GetProjectContext() const;

	/** Get policy context as string */
	FString GetPolicyContext() const;

	/** Get active asset context as string */
	FString GetActiveContext(const TArray<FString>& AssetPaths, int32 MaxTokens) const;

	/** Get the layer decision policy text for C++/BP hybrid profiles */
	FString GetLayerDecisionPolicy() const;

	/** Get all capability knowledge packs concatenated */
	FString GetCapabilityKnowledge() const;

	/**
	 * Returns shared preamble text that ALL provider paths should include.
	 * Contains project context, policy context, and capability knowledge packs
	 * (recipe routing, blueprint authoring rules, etc.).
	 *
	 * Claude Code provider, future CLI providers, etc. call this to stay
	 * in sync with the knowledge packs that API providers get automatically
	 * via AssembleSystemPromptInternal().
	 *
	 * @return Assembled preamble text, or empty string if called before Initialize()
	 */
	FString BuildSharedSystemPreamble() const;

	/** Get a single knowledge pack by ID (e.g. "recipe_routing", "blueprint_authoring") */
	FString GetKnowledgePackById(const FString& PackId) const;

	/**
	 * Build a compact context block for a Blueprint asset showing its
	 * components and variables. Returns empty string if asset is not
	 * a Blueprint or cannot be loaded.
	 *
	 * @param AssetPath Package path to the Blueprint asset
	 * @return Indented context block, or empty string
	 */
	FString BuildBlueprintContextBlock(const FString& AssetPath) const;

	/**
	 * Assemble a worker-specific prompt for the Brain Layer.
	 * Loads domain template, substitutes variables, appends base rules.
	 *
	 * @param WorkerDomain Domain name (e.g., "blueprint", "behaviortree")
	 * @param TaskDescription What the worker should accomplish
	 * @param PreviousStepContext Summary from previous worker steps (empty if first step)
	 * @param ProjectRules User-configured project rules (empty if none)
	 * @return Assembled worker prompt
	 */
	FString AssembleWorkerPrompt(
		const FString& WorkerDomain,
		const FString& TaskDescription,
		const FString& PreviousStepContext,
		const FString& ProjectRules
	);

	/** Get project rules from settings */
	FString GetProjectRules() const;

	// ==========================================
	// Token Estimation
	// ==========================================

	/**
	 * Estimate token count for text
	 * Rough approximation: ~4 characters per token for English
	 */
	int32 EstimateTokenCount(const FString& Text) const;

	// ==========================================
	// Template Management
	// ==========================================

	/** Set base prompt template */
	void SetBasePrompt(const FString& Prompt);

	/** Reload templates from files */
	void ReloadTemplates();

	/** Substitute engine/project variables (e.g., {ENGINE_VERSION}) in template text */
	FString SubstituteVariables(const FString& Template) const;

private:
	FOlivePromptAssembler() = default;

	/** Shared internal prompt assembly implementation */
	FString AssembleSystemPromptInternal(
		const FString& BasePrompt,
		EOliveChatMode Mode,
		const TArray<FString>& ContextAssetPaths,
		int32 MaxTokens
	);

	/** Load prompt templates from Content folder */
	void LoadPromptTemplates();

	/**
	 * Returns the mode-specific behavioral suffix appended as the last paragraph
	 * of the system prompt. ~50 tokens per mode.
	 *
	 * @param Mode Active chat mode
	 * @return Mode suffix text
	 */
	FString GetModeSuffix(EOliveChatMode Mode) const;

	/** Base system prompt template */
	FString BasePromptTemplate;

	/** Rough estimate: characters per token */
	static constexpr float CharsPerToken = 4.0f;

	/** Token budget constants */
	static constexpr int32 WorkerSystemPromptBudget = 1500;
	static constexpr int32 WorkerContextBudget = 1000;
	static constexpr int32 ResponseBudget = 4096;

	/** Worker domain -> template file mapping */
	TMap<FString, FString> WorkerTemplates;

	/** Base rules text (loaded from Base.txt) */
	FString BaseRulesText;

	/** Capability knowledge pack id -> text */
	TMap<FString, FString> CapabilityKnowledgePacks;
};
