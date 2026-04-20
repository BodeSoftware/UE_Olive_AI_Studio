// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Providers/IOliveAIProvider.h"

/**
 * Result metadata from prompt distillation.
 * Used to inject truncation notes into model-visible context so the LLM
 * knows that earlier messages were summarized and may request re-provision.
 */
struct OLIVEAIEDITOR_API FOliveDistillationResult
{
	/** Number of messages that were summarized */
	int32 MessagesSummarized = 0;

	/** Number of tool results truncated (subset of summarized that were oversized) */
	int32 ToolResultsTruncated = 0;

	/** Estimated tokens saved by distillation */
	int32 TokensSaved = 0;

	/** Whether any truncation occurred */
	bool DidTruncate() const
	{
		return MessagesSummarized > 0 || ToolResultsTruncated > 0;
	}
};

/**
 * Configuration for prompt distillation
 */
struct OLIVEAIEDITOR_API FOliveDistillationConfig
{
	/** Number of recent tool call/result pairs to keep verbatim */
	int32 RecentPairsToKeep = 2;

	/** Max chars for a single tool result before forced summarization */
	int32 MaxResultChars = 4000;

	/** Maximum total characters for all tool result messages combined.
	  * When exceeded, older tool results are progressively summarized.
	  * 0 = no limit. Default 80000 (~20K tokens). */
	int32 MaxTotalResultChars = 80000;

	/** Max chars for a single assistant message before truncation.
	  * Only applied to non-recent assistant messages. 0 = no limit. */
	int32 MaxAssistantChars = 4000;

	/** Target token budget for the distilled conversation (0 = no limit, just apply rules) */
	int32 TargetTokenBudget = 0;
};

/**
 * Prompt Distiller
 *
 * Enforces the distillation contract on conversation history before
 * sending to the provider. Summarizes older tool results to save tokens
 * while keeping recent results verbatim for context continuity.
 */
class OLIVEAIEDITOR_API FOlivePromptDistiller
{
public:
	FOlivePromptDistiller();

	/**
	 * Distill a message array in-place.
	 * - Identifies tool result messages (Role == Tool)
	 * - Keeps the last RecentPairsToKeep tool results verbatim (unless oversized)
	 * - Summarizes all older tool results to one-line summaries
	 * - Never touches System or User messages
	 *
	 * @param Messages The message array to distill (modified in-place)
	 * @param Config Distillation configuration
	 */
	FOliveDistillationResult Distill(TArray<FOliveChatMessage>& Messages, const FOliveDistillationConfig& Config) const;

	/**
	 * Summarize a single tool result to a one-line string.
	 * Parses JSON, extracts key fields, graceful fallback to truncation.
	 *
	 * @param ToolName The tool that produced this result
	 * @param ResultContent The full JSON result string
	 * @return One-line summary like "[blueprint.add_variable] SUCCESS: Added Health (float) to BP_Enemy"
	 */
	FString SummarizeToolResult(const FString& ToolName, const FString& ResultContent) const;

	/**
	 * Estimate total tokens in a message array.
	 */
	int32 EstimateTokenCount(const TArray<FOliveChatMessage>& Messages) const;

private:
	/** Estimate tokens for a single string */
	int32 EstimateTokens(const FString& Text) const;

	/** Extract a brief description from a JSON result */
	FString ExtractBrief(const FString& JsonContent) const;

	/** Check if a JSON result indicates success */
	bool IsSuccessResult(const FString& JsonContent) const;

	static constexpr float CharsPerToken = 4.0f;
};
