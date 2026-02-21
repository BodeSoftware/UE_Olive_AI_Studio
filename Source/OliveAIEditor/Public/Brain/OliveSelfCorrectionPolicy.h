// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

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
	 * @return Decision with action and enriched message
	 */
	FOliveCorrectionDecision Evaluate(
		const FString& ToolName,
		const FString& ResultJson,
		FOliveLoopDetector& LoopDetector,
		const FOliveRetryPolicy& Policy
	);

	/** Reset state for a new run */
	void Reset();

private:
	/**
	 * Check if the result contains a compile failure.
	 * @return true if compile_result.success == false
	 */
	bool HasCompileFailure(const FString& ResultJson, FString& OutErrors, FString& OutAssetPath) const;

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
	 */
	FString BuildToolErrorMessage(
		const FString& ToolName,
		const FString& ErrorCode,
		const FString& ErrorMessage,
		int32 AttemptNum,
		int32 MaxAttempts
	) const;

	/** Current attempt count within this run */
	int32 CurrentAttemptCount = 0;
};
