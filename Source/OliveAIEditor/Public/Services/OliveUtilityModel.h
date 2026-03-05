// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// Forward declarations
enum class EOliveAIProvider : uint8;

/**
 * FOliveUtilityModel
 *
 * Lightweight wrapper for quick single-shot LLM completions.
 * Used for keyword expansion, error classification, and other
 * sub-second utility tasks that don't need streaming or tool calling.
 *
 * NOT a singleton -- stateless helper class with static methods.
 * Each call creates a transient provider, sends one request, and waits.
 *
 * Thread safety: Must be called from the game thread (creates HTTP requests
 * via FHttpModule which requires game thread for delegate dispatch).
 */
class OLIVEAIEDITOR_API FOliveUtilityModel
{
public:
	/**
	 * Send a simple completion request to the configured utility model.
	 * Blocks the calling thread (game thread) until the response arrives
	 * or the timeout expires.
	 *
	 * 2-tier fallback:
	 * 1. Try the configured utility model provider
	 * 2. Try the main chat provider (if it supports HTTP completions)
	 *
	 * @param SystemPrompt   System-level instructions for the model
	 * @param UserPrompt     The user-facing query
	 * @param OutResponse    Filled with the model's text response on success
	 * @param OutError       Filled with error message on failure
	 * @return True if the completion succeeded, false on timeout/error/unconfigured
	 */
	static bool SendSimpleCompletion(
		const FString& SystemPrompt,
		const FString& UserPrompt,
		FString& OutResponse,
		FString& OutError
	);

	/**
	 * Check if the utility model is configured and available.
	 * Returns false if the provider has no API key, the setting is disabled,
	 * or the provider is ClaudeCode (CLI providers can't do simple completions).
	 */
	static bool IsAvailable();

	/**
	 * Extract search keywords from a user message using the utility model.
	 * 3-tier fallback: utility model -> main provider -> basic tokenizer.
	 * ALWAYS returns non-empty (basic tokenizer is deterministic).
	 *
	 * @param UserMessage    The user's task description
	 * @param MaxKeywords    Maximum number of keywords to return
	 * @return Array of lowercase search terms (always non-empty -- fallback guarantees output)
	 */
	static TArray<FString> ExtractSearchKeywords(
		const FString& UserMessage,
		int32 MaxKeywords = 12
	);

private:
	/**
	 * Basic keyword extraction using the library index tokenizer.
	 * Strips @-mentions, removes action verbs and noise words, returns tokens.
	 * This is the fallback when the utility model is unavailable.
	 */
	static TArray<FString> ExtractKeywordsBasic(
		const FString& UserMessage,
		int32 MaxKeywords = 12
	);

	/**
	 * Build the system prompt for the keyword expansion LLM call.
	 */
	static FString BuildKeywordExpansionPrompt();

	/** Action verbs and noise words to strip from user messages (not useful as search terms). */
	static const TSet<FString>& GetActionVerbStopWords();

	/**
	 * Try to send a completion via Claude Code CLI's --print mode.
	 * Spawns `claude --print --output-format text` as a subprocess,
	 * captures stdout, and returns the text response.
	 * Used as fallback when the main provider is ClaudeCode (CLI-based, not HTTP).
	 *
	 * @param TimeoutSeconds   Max seconds to wait (currently informational; ExecProcess blocks)
	 * @param SystemPrompt     System-level instructions for the model
	 * @param UserPrompt       The user-facing query
	 * @param OutResponse      Filled with the model's text response on success
	 * @param OutError         Filled with error message on failure
	 * @return True if the completion succeeded, false on any failure
	 */
	static bool TrySendCompletionViaCLI(
		int32 TimeoutSeconds,
		const FString& SystemPrompt,
		const FString& UserPrompt,
		FString& OutResponse,
		FString& OutError
	);

	/**
	 * Try to send a completion using a specific provider enum + model + key.
	 * Creates a transient provider, configures it, sends one message, and
	 * blocks with tick-pumping until the response or timeout.
	 *
	 * @param ProviderType     Which provider to create
	 * @param ModelId          Model identifier for the request
	 * @param ApiKey           API key (empty is ok for Ollama)
	 * @param BaseUrl          Base URL override (for Ollama/OpenAICompatible)
	 * @param TimeoutSeconds   Max seconds to wait
	 * @param SystemPrompt     System message content
	 * @param UserPrompt       User message content
	 * @param OutResponse      Filled with the model's text response on success
	 * @param OutError         Filled with error message on failure
	 * @return True on success, false on any failure
	 */
	static bool TrySendCompletion(
		EOliveAIProvider ProviderType,
		const FString& ModelId,
		const FString& ApiKey,
		const FString& BaseUrl,
		int32 TimeoutSeconds,
		const FString& SystemPrompt,
		const FString& UserPrompt,
		FString& OutResponse,
		FString& OutError
	);

	/** Map EOliveAIProvider enum to the string name FOliveProviderFactory::CreateProvider expects. */
	static FString ProviderEnumToName(EOliveAIProvider Provider);
};
