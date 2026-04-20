// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

enum class EOliveAIProvider : uint8;

/**
 * Utility Model — Lightweight Static Helper for Quick LLM Completions
 *
 * Provides single-shot, blocking LLM completions for sub-second tasks like
 * keyword expansion, error classification, and search enhancement. All methods
 * are static; no singleton, no state, no registration needed.
 *
 * 3-tier fallback for SendSimpleCompletion():
 *   1. Configured utility model provider (cheap/fast, e.g., Haiku via OpenRouter)
 *   2. Main chat provider (if not ClaudeCode — CLI providers can't do HTTP completions)
 *   3. Claude Code CLI via `--print` mode (slowest, always available if installed)
 */
class OLIVEAIEDITOR_API FOliveUtilityModel
{
public:
	/**
	 * Send a single-shot blocking LLM completion using the 3-tier fallback chain.
	 *
	 * @param SystemPrompt  System-level instructions for the model
	 * @param UserPrompt    The user-facing prompt / query
	 * @param OutResponse   Populated with the model's text response on success
	 * @param OutError      Populated with error details on failure
	 * @return true if a response was obtained from any tier
	 */
	static bool SendSimpleCompletion(
		const FString& SystemPrompt,
		const FString& UserPrompt,
		FString& OutResponse,
		FString& OutError);

	/**
	 * Check whether at least one completion tier is available.
	 * Returns true if the utility provider has an API key, the main provider
	 * is an HTTP provider, or Claude Code CLI is installed.
	 */
	static bool IsAvailable();

	/**
	 * Extract search keywords from a user message for template pre-search.
	 * Uses LLM for synonym expansion when available, falls back to basic tokenizer.
	 *
	 * @param UserMessage  The user's natural-language task description
	 * @param MaxKeywords  Maximum number of keywords to return (default 12)
	 * @return Array of lowercase keyword strings
	 */
	static TArray<FString> ExtractSearchKeywords(const FString& UserMessage, int32 MaxKeywords = 12);

private:
	/**
	 * Attempt a completion via a specific HTTP-based AI provider.
	 *
	 * @param ProviderType  Which provider enum to instantiate
	 * @param ModelId       Model identifier (e.g., "anthropic/claude-3-5-haiku-latest")
	 * @param ApiKey        API key for the provider
	 * @param BaseUrl       Base URL for the provider's API endpoint
	 * @param Timeout       Timeout in seconds
	 * @param SystemPrompt  System-level instructions
	 * @param UserPrompt    User query
	 * @param OutResponse   Populated on success
	 * @param OutError      Populated on failure
	 * @return true if the completion succeeded
	 */
	static bool TrySendCompletion(
		EOliveAIProvider ProviderType,
		const FString& ModelId,
		const FString& ApiKey,
		const FString& BaseUrl,
		float Timeout,
		const FString& SystemPrompt,
		const FString& UserPrompt,
		FString& OutResponse,
		FString& OutError);

	/**
	 * Attempt a completion via Claude Code CLI's --print mode.
	 * This is the slowest tier but requires no API key if the user has Claude Max.
	 *
	 * @param Timeout       Timeout in seconds
	 * @param SystemPrompt  System-level instructions
	 * @param UserPrompt    User query
	 * @param OutResponse   Populated on success
	 * @param OutError      Populated on failure
	 * @return true if the CLI process completed successfully
	 */
	static bool TrySendCompletionViaCLI(
		float Timeout,
		const FString& SystemPrompt,
		const FString& UserPrompt,
		FString& OutResponse,
		FString& OutError);

	/**
	 * Basic keyword extraction using tokenizer and stop-word filtering.
	 * No LLM call — pure string processing.
	 *
	 * @param UserMessage  The user's message to extract keywords from
	 * @param MaxKeywords  Maximum keywords to return
	 * @return Array of lowercase keyword strings
	 */
	static TArray<FString> ExtractKeywordsBasic(const FString& UserMessage, int32 MaxKeywords = 12);

	/** Build the system prompt used for LLM keyword expansion. */
	static FString BuildKeywordExpansionPrompt();

	/** Get the set of action-verb stop words filtered from basic tokenizer output. */
	static const TSet<FString>& GetActionVerbStopWords();

	/** Convert an EOliveAIProvider enum value to a string name for FOliveProviderFactory::CreateProvider(). */
	static FString ProviderEnumToName(EOliveAIProvider Provider);
};
