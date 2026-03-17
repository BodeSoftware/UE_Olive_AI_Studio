// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Status of an operation record
 */
enum class EOliveOperationStatus : uint8
{
	Success,
	Failed,
	Skipped,
	Cancelled,
	RequiresConfirmation
};

/**
 * A single tool call operation record
 */
struct OLIVEAIEDITOR_API FOliveOperationRecord
{
	/** Monotonic sequence number */
	int32 Sequence = 0;

	/** Groups ops within a single user request cycle */
	FString RunId;

	/** Which worker/domain executed this */
	FString WorkerDomain;

	/** Step index in orchestrator plan (0 if no orchestrator) */
	int32 StepIndex = 0;

	/** Tool name (e.g. "blueprint.add_variable") */
	FString ToolName;

	/** Tool call parameters */
	TSharedPtr<FJsonObject> Params;

	/** Tool result data */
	TSharedPtr<FJsonObject> Result;

	/** Operation outcome */
	EOliveOperationStatus Status = EOliveOperationStatus::Success;

	/** Error message if failed */
	FString ErrorMessage;

	/** When the operation was executed */
	FDateTime Timestamp;

	/** Asset paths affected by this operation */
	TArray<FString> AffectedAssets;
};

/**
 * Operation History Store
 *
 * Per-session log of all tool calls. Provides prompt summarization
 * at multiple budget tiers for token-efficient context passing.
 */
class OLIVEAIEDITOR_API FOliveOperationHistoryStore
{
public:
	FOliveOperationHistoryStore();

	// ==========================================
	// Recording
	// ==========================================

	/** Record an operation. Sequence is auto-assigned. */
	void RecordOperation(FOliveOperationRecord& Record);

	// ==========================================
	// Summarization
	// ==========================================

	/**
	 * Build a prompt summary at the given token budget.
	 * - >2000 tokens: per-operation detail with params and results
	 * - 500-2000 tokens: grouped by asset with outcome summary
	 * - <500 tokens: one-line session summary
	 */
	FString BuildPromptSummary(int32 TokenBudget) const;

	/**
	 * Build compact context summary for inter-worker handoff.
	 * Summarizes operations up to a given step in a specific run.
	 */
	FString BuildWorkerContext(const FString& RunId, int32 UpToStep) const;

	/**
	 * Build a run report with stats (successes, failures, skips).
	 */
	FString BuildRunReport(const FString& RunId) const;

	/**
	 * Build a distilled context string for model consumption with 3-tier detail.
	 *
	 * Tier 1: Previous completed runs -> compressed one-liners
	 * Tier 2: Current run, older ops -> one-line summaries
	 * Tier 3: Current run, last N (RawResultCount) -> full detail
	 *
	 * @param TokenBudget Maximum estimated tokens for the output (~4 chars per token)
	 * @param RawResultCount Number of most recent operations to include at full detail
	 * @return Distilled context string, empty if no operations recorded
	 */
	FString BuildModelContext(int32 TokenBudget, int32 RawResultCount = 2) const;

	// ==========================================
	// Queries
	// ==========================================

	/** Get all records for a run */
	TArray<FOliveOperationRecord> GetRunHistory(const FString& RunId) const;

	/** Get records for a specific step in a run */
	TArray<FOliveOperationRecord> GetStepHistory(const FString& RunId, int32 StepIndex) const;

	/** Get total record count */
	int32 GetTotalRecordCount() const { return Records.Num(); }

	/** Get run stats */
	void GetRunStats(const FString& RunId,
		int32& OutSucceeded, int32& OutFailed, int32& OutSkipped) const;

	/** Clear all history */
	void Clear();

private:
	/** Estimate tokens for a string (~4 chars per token) */
	int32 EstimateTokens(const FString& Text) const;

	/** Summarize a single record to one line */
	FString SummarizeRecord(const FOliveOperationRecord& Record) const;

	/** Summarize a single record with detail */
	FString DetailRecord(const FOliveOperationRecord& Record) const;

	/** All recorded operations */
	TArray<FOliveOperationRecord> Records;

	/** Next sequence number */
	int32 NextSequence = 1;
};
