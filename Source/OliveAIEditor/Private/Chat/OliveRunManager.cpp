// Copyright Bode Software. All Rights Reserved.

#include "Chat/OliveRunManager.h"
#include "OliveSnapshotManager.h"

DEFINE_LOG_CATEGORY(LogOliveRunManager);

FOliveRunManager& FOliveRunManager::Get()
{
	static FOliveRunManager Instance;
	return Instance;
}

FGuid FOliveRunManager::StartRun(const FString& Name)
{
	if (ActiveRun.IsSet())
	{
		UE_LOG(LogOliveRunManager, Warning, TEXT("Completing previous run '%s' before starting new one"), *ActiveRun->Name);
		CompleteRun();
	}

	FOliveRun NewRun;
	NewRun.RunId = FGuid::NewGuid();
	NewRun.Name = Name;
	NewRun.Status = EOliveRunStatus::Running;
	NewRun.StartTime = FDateTime::UtcNow();
	NewRun.CurrentStepIndex = -1;

	ActiveRun = NewRun;

	UE_LOG(LogOliveRunManager, Log, TEXT("Started run '%s' (ID: %s)"), *Name, *NewRun.RunId.ToString());
	OnRunStatusChanged.Broadcast(ActiveRun.GetValue());
	return NewRun.RunId;
}

const FOliveRun* FOliveRunManager::GetActiveRun() const
{
	return ActiveRun.IsSet() ? &ActiveRun.GetValue() : nullptr;
}

bool FOliveRunManager::HasActiveRun() const
{
	return ActiveRun.IsSet() && ActiveRun->Status == EOliveRunStatus::Running;
}

void FOliveRunManager::CompleteRun()
{
	if (!ActiveRun.IsSet()) return;

	ActiveRun->Status = EOliveRunStatus::Completed;
	ActiveRun->EndTime = FDateTime::UtcNow();

	UE_LOG(LogOliveRunManager, Log, TEXT("Completed run '%s' (%d steps)"),
		*ActiveRun->Name, ActiveRun->Steps.Num());

	OnRunStatusChanged.Broadcast(ActiveRun.GetValue());

	ActiveRun.Reset();
}

int32 FOliveRunManager::BeginStep(const FString& Description)
{
	if (!ActiveRun.IsSet())
	{
		UE_LOG(LogOliveRunManager, Warning, TEXT("BeginStep called with no active run"));
		return -1;
	}

	// Complete previous step if still running
	if (ActiveRun->CurrentStepIndex >= 0 && ActiveRun->CurrentStepIndex < ActiveRun->Steps.Num())
	{
		FOliveRunStep& PrevStep = ActiveRun->Steps[ActiveRun->CurrentStepIndex];
		if (PrevStep.Status == EOliveRunStepStatus::Running)
		{
			PrevStep.Status = EOliveRunStepStatus::Completed;
			PrevStep.EndTime = FDateTime::UtcNow();
			PrevStep.DurationMs = (PrevStep.EndTime - PrevStep.StartTime).GetTotalMilliseconds();
		}
	}

	FOliveRunStep NewStep;
	NewStep.StepIndex = ActiveRun->Steps.Num();
	NewStep.Status = EOliveRunStepStatus::Running;
	NewStep.Description = Description;
	NewStep.StartTime = FDateTime::UtcNow();

	ActiveRun->Steps.Add(NewStep);
	ActiveRun->CurrentStepIndex = NewStep.StepIndex;

	UE_LOG(LogOliveRunManager, Log, TEXT("Step %d: %s"), NewStep.StepIndex, *Description);
	OnRunStepChanged.Broadcast(ActiveRun.GetValue(), NewStep.StepIndex);
	return NewStep.StepIndex;
}

void FOliveRunManager::RecordToolCall(const FString& ToolName, const FString& ToolCallId,
	bool bSuccess, const FString& Summary, double DurationMs,
	const TSharedPtr<FJsonObject>& ResultData)
{
	if (!ActiveRun.IsSet() || ActiveRun->CurrentStepIndex < 0 ||
		ActiveRun->CurrentStepIndex >= ActiveRun->Steps.Num())
	{
		return;
	}

	FOliveRunToolCall ToolCall;
	ToolCall.ToolName = ToolName;
	ToolCall.ToolCallId = ToolCallId;
	ToolCall.bSuccess = bSuccess;
	ToolCall.ResultSummary = Summary;
	ToolCall.DurationMs = DurationMs;
	ToolCall.ResultData = ResultData;

	ActiveRun->Steps[ActiveRun->CurrentStepIndex].ToolCalls.Add(ToolCall);
}

void FOliveRunManager::CompleteStep(bool bSuccess)
{
	if (!ActiveRun.IsSet() || ActiveRun->CurrentStepIndex < 0 ||
		ActiveRun->CurrentStepIndex >= ActiveRun->Steps.Num())
	{
		return;
	}

	FOliveRunStep& Step = ActiveRun->Steps[ActiveRun->CurrentStepIndex];
	Step.Status = bSuccess ? EOliveRunStepStatus::Completed : EOliveRunStepStatus::Failed;
	Step.EndTime = FDateTime::UtcNow();
	Step.DurationMs = (Step.EndTime - Step.StartTime).GetTotalMilliseconds();

	OnRunStepChanged.Broadcast(ActiveRun.GetValue(), ActiveRun->CurrentStepIndex);
}

void FOliveRunManager::PauseRun()
{
	if (!ActiveRun.IsSet() || ActiveRun->Status != EOliveRunStatus::Running) return;
	ActiveRun->Status = EOliveRunStatus::Paused;
	UE_LOG(LogOliveRunManager, Log, TEXT("Paused run '%s'"), *ActiveRun->Name);
	OnRunStatusChanged.Broadcast(ActiveRun.GetValue());
}

void FOliveRunManager::ResumeRun()
{
	if (!ActiveRun.IsSet() || ActiveRun->Status != EOliveRunStatus::Paused) return;
	ActiveRun->Status = EOliveRunStatus::Running;
	UE_LOG(LogOliveRunManager, Log, TEXT("Resumed run '%s'"), *ActiveRun->Name);
	OnRunStatusChanged.Broadcast(ActiveRun.GetValue());
}

void FOliveRunManager::CancelRun()
{
	if (!ActiveRun.IsSet()) return;
	ActiveRun->Status = EOliveRunStatus::Cancelled;
	ActiveRun->EndTime = FDateTime::UtcNow();
	UE_LOG(LogOliveRunManager, Log, TEXT("Cancelled run '%s'"), *ActiveRun->Name);
	OnRunStatusChanged.Broadcast(ActiveRun.GetValue());
	ActiveRun.Reset();
}

void FOliveRunManager::RetryStep(int32 StepIndex)
{
	if (!ActiveRun.IsSet() || StepIndex < 0 || StepIndex >= ActiveRun->Steps.Num()) return;
	ActiveRun->Steps[StepIndex].Status = EOliveRunStepStatus::Pending;
	ActiveRun->Steps[StepIndex].ToolCalls.Empty();
	UE_LOG(LogOliveRunManager, Log, TEXT("Retrying step %d"), StepIndex);
	OnRunStepChanged.Broadcast(ActiveRun.GetValue(), StepIndex);
}

void FOliveRunManager::SkipStep(int32 StepIndex)
{
	if (!ActiveRun.IsSet() || StepIndex < 0 || StepIndex >= ActiveRun->Steps.Num()) return;
	ActiveRun->Steps[StepIndex].Status = EOliveRunStepStatus::Skipped;
	ActiveRun->Steps[StepIndex].EndTime = FDateTime::UtcNow();
	UE_LOG(LogOliveRunManager, Log, TEXT("Skipped step %d"), StepIndex);
	OnRunStepChanged.Broadcast(ActiveRun.GetValue(), StepIndex);
}

void FOliveRunManager::RollbackToCheckpoint(const FString& SnapshotId)
{
	if (SnapshotId.IsEmpty()) return;
	UE_LOG(LogOliveRunManager, Log, TEXT("Rolling back to checkpoint: %s"), *SnapshotId);
	FOliveSnapshotManager::Get().RollbackSnapshot(SnapshotId, {}, false, TEXT(""));
	if (ActiveRun.IsSet())
	{
		ActiveRun->Status = EOliveRunStatus::RolledBack;
		ActiveRun->EndTime = FDateTime::UtcNow();
		OnRunStatusChanged.Broadcast(ActiveRun.GetValue());
		ActiveRun.Reset();
	}
}
