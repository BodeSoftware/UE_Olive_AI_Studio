// Copyright Bode Software. All Rights Reserved.

#include "Brain/OliveBrainLayer.h"
#include "MCP/OliveToolRegistry.h"
#include "OliveAIEditorModule.h"
#include "Misc/Guid.h"

FOliveBrainLayer::FOliveBrainLayer()
{
	UE_LOG(LogOliveAI, Log, TEXT("Brain Layer initialized"));
}

FOliveBrainLayer::~FOliveBrainLayer()
{
}

// ==========================================
// State Access
// ==========================================

bool FOliveBrainLayer::IsActive() const
{
	return CurrentState != EOliveBrainState::Idle
		&& CurrentState != EOliveBrainState::Completed
		&& CurrentState != EOliveBrainState::Error;
}

// ==========================================
// State Control
// ==========================================

void FOliveBrainLayer::RequestCancel()
{
	if (IsActive())
	{
		UE_LOG(LogOliveAI, Log, TEXT("Brain: Cancel requested from state %s"), LexToString(CurrentState));
		TransitionTo(EOliveBrainState::Cancelling);
	}
}

void FOliveBrainLayer::ResetToIdle()
{
	if (CurrentState != EOliveBrainState::Idle)
	{
		UE_LOG(LogOliveAI, Log, TEXT("Brain: Reset to Idle from state %s"), LexToString(CurrentState));
		TransitionTo(EOliveBrainState::Idle);
	}
}

bool FOliveBrainLayer::TransitionTo(EOliveBrainState NewState)
{
	if (CurrentState == NewState)
	{
		return true;
	}

	if (!IsValidTransition(CurrentState, NewState))
	{
		UE_LOG(LogOliveAI, Warning, TEXT("Brain: Invalid state transition %s -> %s"),
			LexToString(CurrentState), LexToString(NewState));
		return false;
	}

	const EOliveBrainState OldState = CurrentState;
	CurrentState = NewState;

	UE_LOG(LogOliveAI, Log, TEXT("Brain: State %s -> %s"), LexToString(OldState), LexToString(NewState));

	OnStateChanged.Broadcast(OldState, NewState);

	// Reset worker phase when entering WorkerActive
	if (NewState == EOliveBrainState::WorkerActive)
	{
		SetWorkerPhase(EOliveWorkerPhase::Streaming);
	}

	return true;
}

void FOliveBrainLayer::SetWorkerPhase(EOliveWorkerPhase NewPhase)
{
	if (CurrentState != EOliveBrainState::WorkerActive)
	{
		return;
	}

	if (CurrentWorkerPhase != NewPhase)
	{
		CurrentWorkerPhase = NewPhase;
		OnWorkerPhaseChanged.Broadcast(NewPhase);
	}
}

// ==========================================
// Run Management
// ==========================================

FString FOliveBrainLayer::BeginRun()
{
	CurrentRunId = FGuid::NewGuid().ToString();
	TransitionTo(EOliveBrainState::WorkerActive);
	UE_LOG(LogOliveAI, Log, TEXT("Brain: Run started [%s]"), *CurrentRunId);
	return CurrentRunId;
}

void FOliveBrainLayer::CompleteRun(EOliveRunOutcome Outcome)
{
	LastOutcome = Outcome;
	UE_LOG(LogOliveAI, Log, TEXT("Brain: Run completed [%s] outcome=%d"),
		*CurrentRunId, static_cast<int32>(Outcome));

	// Clean up routing stats for this run
	FOliveToolRegistry::Get().ClearBlueprintRoutingStats(
		FString::Printf(TEXT("run:%s"), *CurrentRunId));

	if (Outcome == EOliveRunOutcome::Failed)
	{
		TransitionTo(EOliveBrainState::Error);
	}
	else if (Outcome == EOliveRunOutcome::Cancelled)
	{
		TransitionTo(EOliveBrainState::Idle);
	}
	else
	{
		TransitionTo(EOliveBrainState::Completed);
	}
}

// ==========================================
// Transition Validation
// ==========================================

bool FOliveBrainLayer::IsValidTransition(EOliveBrainState From, EOliveBrainState To) const
{
	// Cancelling is allowed from any active state
	if (To == EOliveBrainState::Cancelling)
	{
		return From == EOliveBrainState::Planning
			|| From == EOliveBrainState::WorkerActive
			|| From == EOliveBrainState::AwaitingConfirmation;
	}

	// Idle is the reset state — allowed from terminal states and Cancelling
	if (To == EOliveBrainState::Idle)
	{
		return From == EOliveBrainState::Completed
			|| From == EOliveBrainState::Error
			|| From == EOliveBrainState::Cancelling
			|| From == EOliveBrainState::AwaitingConfirmation;
	}

	switch (From)
	{
	case EOliveBrainState::Idle:
		return To == EOliveBrainState::Planning
			|| To == EOliveBrainState::WorkerActive;

	case EOliveBrainState::Planning:
		return To == EOliveBrainState::WorkerActive
			|| To == EOliveBrainState::Error;

	case EOliveBrainState::WorkerActive:
		return To == EOliveBrainState::WorkerActive  // next step
			|| To == EOliveBrainState::AwaitingConfirmation
			|| To == EOliveBrainState::Completed
			|| To == EOliveBrainState::Error;

	case EOliveBrainState::AwaitingConfirmation:
		return To == EOliveBrainState::WorkerActive;

	case EOliveBrainState::Completed:
	case EOliveBrainState::Error:
		return false; // Must go through Idle

	default:
		return false;
	}
}
