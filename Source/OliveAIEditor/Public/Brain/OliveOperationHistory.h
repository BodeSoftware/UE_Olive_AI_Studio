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
 * Per-session log of all tool calls. After the P3 makeover the store
 * exposes only the three operations the conversation manager actually
 * needs: append a new record, ask how many records exist, and build a
 * model-facing context string. The previous prompt-summary/run-report/
 * worker-handoff helpers were removed alongside the self-correction
 * and prompt-distillation machinery they served.
 */
class OLIVEAIEDITOR_API FOliveOperationHistoryStore
{
public:
	FOliveOperationHistoryStore() = default;

	// ==========================================
	// Recording
	// ==========================================

	/** Record an operation. Sequence is auto-assigned. */
	void RecordOperation(FOliveOperationRecord& Record);

	// ==========================================
	// Context
	// ==========================================

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

	/** Get total record count */
	int32 GetTotalRecordCount() const { return Records.Num(); }

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
