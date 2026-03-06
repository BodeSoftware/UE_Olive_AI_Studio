// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

// Forward declarations
class FOliveLoopDetector;
struct FOliveRetryPolicy;

/**
 * What action the self-correction policy recommends
 */
enum class EOliveCorrectionAction : uint8
{
	Continue,           // Tool succeeded, keep going
	FeedBackErrors,     // Tool/compile failed, feed enriched errors back to model
	StopWorker          // Loop detected, stop with report
};

/**
 * Decision from the self-correction policy
 */
struct OLIVEAIEDITOR_API FOliveCorrectionDecision
{
	/** The recommended action */
	EOliveCorrectionAction Action = EOliveCorrectionAction::Continue;

	/** Enriched message to send back to the model (for FeedBackErrors) */
	FString EnrichedMessage;

	/** Loop report (for StopWorker) */
	FString LoopReport;

	/** Current attempt number */
	int32 AttemptNumber = 0;

	/** Max attempts before stop */
	int32 MaxAttempts = 3;
};

/**
 * Error classification for self-correction routing.
 * Determines whether an error is worth retrying.
 */
enum class EOliveErrorCategory : uint8
{
	FixableMistake,       // Category A: retry with guidance
	UnsupportedFeature,   // Category B: do NOT retry, suggest alternative
	Ambiguous             // Category C: retry once, then escalate
};

/**
 * Self-Correction Policy
 *
 * Evaluates tool results and compile outcomes to decide whether to
 * continue, retry with enriched errors, or stop.
 * Replaces the ad-hoc CompileRetryCount in ConversationManager.
 */
class OLIVEAIEDITOR_API FOliveSelfCorrectionPolicy
{
public:
	FOliveSelfCorrectionPolicy();

	/**
	 * Evaluate a tool result and decide the correction action.
	 *
	 * @param ToolName The tool that was executed
	 * @param ResultJson The JSON string of the tool result
	 * @param LoopDetector The loop detector to check for loops
	 * @param Policy The retry policy configuration
	 * @param AssetContext Asset path context for per-asset signature tracking
	 * @param ToolCallArgs Original tool call arguments (used for plan dedup)
	 * @return Decision with action and enriched message
	 */
	FOliveCorrectionDecision Evaluate(
		const FString& ToolName,
		const FString& ResultJson,
		FOliveLoopDetector& LoopDetector,
		const FOliveRetryPolicy& Policy,
		const FString& AssetContext = TEXT(""),
		const TSharedPtr<FJsonObject>& ToolCallArgs = nullptr
	);

	/** Reset state for a new run */
	void Reset();

private:
	/**
	 * Check if the result contains a compile failure.
	 * Also detects stale errors (compile errors NOT caused by the current plan).
	 *
	 * @param ResultJson      The tool result JSON string
	 * @param OutErrors       Extracted compile error text
	 * @param OutAssetPath    Blueprint asset path from the result
	 * @param OutHasStaleErrors  Set to true if compile errors do NOT reference any class/function
	 *                           from the current plan's resolved steps (meaning errors are stale,
	 *                           caused by pre-existing issues or previous operations).
	 *                           Only set to true when plan metadata is available; defaults to false.
	 * @param OutRolledBackNodeCount  Number of nodes rolled back by the executor (from "rolled_back_nodes"
	 *                                field in result data). 0 if not present.
	 * @return true if compile_result.success == false
	 */
	bool HasCompileFailure(const FString& ResultJson, FString& OutErrors, FString& OutAssetPath, bool& OutHasStaleErrors, int32& OutRolledBackNodeCount) const;

	/**
	 * Check if the result is a tool execution failure.
	 * @return true if the tool returned an error
	 */
	bool HasToolFailure(const FString& ResultJson, FString& OutErrorCode, FString& OutErrorMessage) const;

	/**
	 * Build enriched error message for compile failures.
	 */
	FString BuildCompileErrorMessage(
		const FString& ToolName,
		const FString& Errors,
		int32 AttemptNum,
		int32 MaxAttempts
	) const;

	/**
	 * Build enriched error message for tool failures.
	 *
	 * @param ToolName The tool that failed
	 * @param ErrorCode The error code (e.g., "ASSET_NOT_FOUND")
	 * @param ErrorMessage The full error message
	 * @param AttemptNum Current retry attempt number
	 * @param MaxAttempts Maximum retries allowed
	 * @param AssetContext Asset path context, used for auto-search on ASSET_NOT_FOUND
	 */
	FString BuildToolErrorMessage(
		const FString& ToolName,
		const FString& ErrorCode,
		const FString& ErrorMessage,
		int32 AttemptNum,
		int32 MaxAttempts,
		const FString& AssetContext = TEXT("")
	) const;

	/**
	 * Classify an error code into a correction category.
	 * Determines whether the error is worth retrying (Category A),
	 * represents an unsupported feature (Category B), or is ambiguous (Category C).
	 *
	 * @param ErrorCode The tool error code (e.g., "NODE_TYPE_UNKNOWN", "BP_ADD_NODE_FAILED")
	 * @param ErrorMessage The full error message (for secondary pattern matching)
	 * @return Category determining retry behavior
	 */
	static EOliveErrorCategory ClassifyErrorCode(
		const FString& ErrorCode,
		const FString& ErrorMessage);

	/**
	 * Build a stable hash of plan content from tool call arguments.
	 * Returns empty string if args don't contain a plan object.
	 */
	FString BuildPlanHash(const FString& ToolName, const TSharedPtr<FJsonObject>& ToolCallArgs) const;

	/**
	 * Build message for plan failures where rollback happened but retries remain.
	 * Tells AI to resubmit corrected plan, NOT to reference deleted node IDs.
	 */
	FString BuildRollbackAwareMessage(
		const FString& ToolName,
		const FString& Errors,
		int32 AttemptNum,
		int32 MaxAttempts,
		int32 RolledBackNodeCount) const;

	/**
	 * Build message for forced switch from plan mode to granular step-by-step mode.
	 * This is mandatory -- the AI MUST use granular tools after this message.
	 */
	FString BuildGranularFallbackMessage(
		const FString& ToolName,
		const FString& Errors,
		const FString& AssetPath,
		int32 RolledBackNodeCount) const;

	/** Map of plan content hash -> submission count. Reset per turn. */
	TMap<FString, int32> PreviousPlanHashes;

	/** Whether we have switched to granular fallback mode for this turn */
	bool bIsInGranularFallback = false;

	/** The last plan failure error text (so granular mode knows what to avoid) */
	FString LastPlanFailureReason;

	/** The asset currently being worked on — tracks for premature switching detection */
	FString CurrentWorkingAsset;

	/** Whether the current asset has unresolved failures */
	bool bCurrentAssetHasFailure = false;

	/** Extract asset path from a tool result JSON string */
	FString ExtractAssetFromResult(const FString& ResultJson) const;

};
