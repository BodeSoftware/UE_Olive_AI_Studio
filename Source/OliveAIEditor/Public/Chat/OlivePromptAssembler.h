// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Prompt Assembler
 *
 * Assembles system prompts from templates and context.
 * Handles token estimation and context truncation.
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
	 * @param FocusProfileName Active focus profile
	 * @param ContextAssetPaths Assets in context
	 * @param MaxTokens Maximum tokens for context
	 * @return Assembled system prompt
	 */
	FString AssembleSystemPrompt(
		const FString& FocusProfileName,
		const TArray<FString>& ContextAssetPaths,
		int32 MaxTokens = 4000
	);

	/**
	 * Assemble full prompt with a base override.
	 * @param BasePromptOverride Prompt text to use as the base component
	 * @param FocusProfileName Active focus profile
	 * @param ContextAssetPaths Assets in context
	 * @param MaxTokens Maximum tokens for context
	 * @return Assembled system prompt
	 */
	FString AssembleSystemPromptWithBase(
		const FString& BasePromptOverride,
		const FString& FocusProfileName,
		const TArray<FString>& ContextAssetPaths,
		int32 MaxTokens = 4000
	);

	// ==========================================
	// Components
	// ==========================================

	/** Get the base system prompt */
	const FString& GetBasePrompt() const { return BasePromptTemplate; }

	/** Get profile-specific prompt addition */
	FString GetProfilePromptAddition(const FString& ProfileName) const;

	/** Get project context as string */
	FString GetProjectContext() const;

	/** Get policy context as string */
	FString GetPolicyContext() const;

	/** Get active asset context as string */
	FString GetActiveContext(const TArray<FString>& AssetPaths, int32 MaxTokens) const;

	/** Get the layer decision policy text for C++/BP hybrid profiles */
	FString GetLayerDecisionPolicy() const;

	/** Get capability knowledge packs for the active profile */
	FString GetCapabilityKnowledge(const FString& ProfileName) const;

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

private:
	FOlivePromptAssembler() = default;

	/** Shared internal prompt assembly implementation */
	FString AssembleSystemPromptInternal(
		const FString& BasePrompt,
		const FString& FocusProfileName,
		const TArray<FString>& ContextAssetPaths,
		int32 MaxTokens
	);

	/** Load prompt templates from Content folder */
	void LoadPromptTemplates();

	/** Substitute variables in template */
	FString SubstituteVariables(const FString& Template) const;

	/** Base system prompt template */
	FString BasePromptTemplate;

	/** Profile-specific prompts */
	TMap<FString, FString> ProfilePrompts;

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

	/** Focus profile -> capability pack ids */
	TMap<FString, TArray<FString>> ProfileCapabilityPackIds;
};
