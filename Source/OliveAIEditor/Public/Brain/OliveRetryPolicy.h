// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Retry policy configuration
 */
struct OLIVEAIEDITOR_API FOliveRetryPolicy
{
	/** Max retries for the same error signature within one worker */
	int32 MaxRetriesPerError = 3;

	/** Total retry budget per worker */
	int32 MaxCorrectionCyclesPerWorker = 5;

	/** How many workers can fail before stopping the run */
	int32 MaxWorkerFailures = 2;

	/** Delay between retries (0 = immediate) */
	float RetryDelaySeconds = 0.0f;
};

/**
 * Tracks error patterns and detects loops/oscillations.
 * Reset per-worker invocation.
 */
class OLIVEAIEDITOR_API FOliveLoopDetector
{
public:
	/**
	 * Record an error attempt with its signature and the fix that was tried.
	 * @param ErrorSignature Stable identifier for the error (see BuildErrorSignature)
	 * @param AttemptedFix Description of what was tried to fix it
	 */
	void RecordAttempt(const FString& ErrorSignature, const FString& AttemptedFix);

	/**
	 * Check if the same error with similar fixes has been tried too many times.
	 * @param ErrorSignature The error to check
	 * @param Policy Retry policy to check against
	 * @return true if loop detected (exceeded MaxRetriesPerError)
	 */
	bool IsLooping(const FString& ErrorSignature, const FOliveRetryPolicy& Policy) const;

	/**
	 * Check if errors are oscillating (cycling between different signatures).
	 * Detects A->B->C->A patterns within a sliding window.
	 * @return true if oscillation detected
	 */
	bool IsOscillating() const;

	/** Get total number of correction attempts across all signatures */
	int32 GetTotalAttempts() const { return TotalAttempts; }

	/** Get the attempt count for a specific error signature */
	int32 GetAttemptCount(const FString& ErrorSignature) const;

	/** Check if total correction budget is exceeded */
	bool IsBudgetExhausted(const FOliveRetryPolicy& Policy) const;

	/** Reset state (called at start of each worker) */
	void Reset();

	// ==========================================
	// Error Signature Builders
	// ==========================================

	/**
	 * Build error signature for a tool failure.
	 * Format: {tool_name}:{error_code}:{asset_path}
	 */
	static FString BuildToolErrorSignature(
		const FString& ToolName,
		const FString& ErrorCode,
		const FString& AssetPath
	);

	/**
	 * Build error signature for a compile failure.
	 * Format: {asset_path}:{error_hash}:{message_hash}
	 */
	static FString BuildCompileErrorSignature(
		const FString& AssetPath,
		const FString& TopErrorMessage
	);

	/**
	 * Build a human-readable loop report.
	 * Includes: what was tried, the stable error, and 2-3 next-step suggestions.
	 */
	FString BuildLoopReport() const;

	/** Simple CRC32-based string hash. Returns 8-char hex string. */
	static FString HashString(const FString& Input);

private:
	/** Map of error signature -> list of attempted fixes */
	TMap<FString, TArray<FString>> AttemptHistory;

	/** Recent error signatures in order (for oscillation detection) */
	TArray<FString> RecentErrors;

	/** Total correction attempts */
	int32 TotalAttempts = 0;

	/** Window size for oscillation detection */
	static constexpr int32 OscillationWindowSize = 6;

	/** Number of cycles required to declare oscillation */
	static constexpr int32 OscillationThreshold = 3;

	/** Get suggestions based on error code pattern */
	static TArray<FString> GetSuggestionsForError(const FString& ErrorSignature);
};
