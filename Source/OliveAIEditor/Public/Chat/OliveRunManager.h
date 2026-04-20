// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

DECLARE_LOG_CATEGORY_EXTERN(LogOliveRunManager, Log, All);

/** Status of an individual run step */
enum class EOliveRunStepStatus : uint8
{
	Pending,
	Running,
	Completed,
	Failed,
	Skipped,
	RolledBack
};

/** Status of an entire run */
enum class EOliveRunStatus : uint8
{
	Running,
	Paused,
	Completed,
	Failed,
	Cancelled,
	RolledBack
};

/** A single tool call within a run step */
struct FOliveRunToolCall
{
	FString ToolName;
	FString ToolCallId;
	bool bSuccess = false;
	FString ResultSummary;
	double DurationMs = 0.0;
	TSharedPtr<FJsonObject> ResultData;
};

/** A single step in a run (one agentic loop iteration) */
struct FOliveRunStep
{
	int32 StepIndex = 0;
	EOliveRunStepStatus Status = EOliveRunStepStatus::Pending;
	FString Description;
	FDateTime StartTime;
	FDateTime EndTime;
	double DurationMs = 0.0;
	TArray<FOliveRunToolCall> ToolCalls;
	FString SnapshotId;
};

/** A complete multi-step run */
struct FOliveRun
{
	FGuid RunId;
	FString Name;
	EOliveRunStatus Status = EOliveRunStatus::Running;
	FDateTime StartTime;
	FDateTime EndTime;
	TArray<FOliveRunStep> Steps;
	TArray<FString> CheckpointSnapshotIds;
	int32 CurrentStepIndex = -1;

	int32 GetCompletedStepCount() const
	{
		int32 Count = 0;
		for (const FOliveRunStep& Step : Steps)
		{
			if (Step.Status == EOliveRunStepStatus::Completed || Step.Status == EOliveRunStepStatus::Skipped)
			{
				Count++;
			}
		}
		return Count;
	}
};

DECLARE_MULTICAST_DELEGATE_OneParam(FOnOliveRunStatusChanged, const FOliveRun&);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnOliveRunStepChanged, const FOliveRun&, int32);

/**
 * FOliveRunManager
 *
 * Manages multi-step "runs" — sequences of agentic tool call iterations
 * with checkpointing, pause/resume, retry, and rollback capabilities.
 */
class OLIVEAIEDITOR_API FOliveRunManager
{
public:
	static FOliveRunManager& Get();

	FGuid StartRun(const FString& Name);
	const FOliveRun* GetActiveRun() const;
	bool HasActiveRun() const;
	void CompleteRun();

	int32 BeginStep(const FString& Description);
	void RecordToolCall(const FString& ToolName, const FString& ToolCallId,
		bool bSuccess, const FString& Summary, double DurationMs,
		const TSharedPtr<FJsonObject>& ResultData);
	void CompleteStep(bool bSuccess);

	FString CreateCheckpoint(const TArray<FString>& AssetPaths);
	bool ShouldCheckpoint() const;
	int32 GetCheckpointInterval() const;

	void PauseRun();
	void ResumeRun();
	void CancelRun();
	void RetryStep(int32 StepIndex);
	void SkipStep(int32 StepIndex);
	void RollbackToCheckpoint(const FString& SnapshotId);

	FOnOliveRunStatusChanged OnRunStatusChanged;
	FOnOliveRunStepChanged OnRunStepChanged;

private:
	FOliveRunManager() = default;

	TOptional<FOliveRun> ActiveRun;
	int32 StepsSinceLastCheckpoint = 0;
};
