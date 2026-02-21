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
};
