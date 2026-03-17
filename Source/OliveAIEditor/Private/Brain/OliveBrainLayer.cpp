// Copyright Bode Software. All Rights Reserved.

#include "Brain/OliveBrainLayer.h"
#include "OliveAIEditorModule.h"
#include "Misc/Guid.h"

FOliveBrainLayer::FOliveBrainLayer()
{
	UE_LOG(LogOliveAI, Log, TEXT("Brain Layer initialized (3-state model)"));
}

FOliveBrainLayer::~FOliveBrainLayer()
{
}

// ==========================================
// State Access
// ==========================================

bool FOliveBrainLayer::IsActive() const
{
	return CurrentState != EOliveBrainState::Idle;
}

// ==========================================
// State Control
// ==========================================

void FOliveBrainLayer::RequestCancel()
{
	if (CurrentState == EOliveBrainState::Active)
	{
		UE_LOG(LogOliveAI, Log, TEXT("Brain: Cancel requested from Active state"));
		TransitionTo(EOliveBrainState::Cancelling);
	}
	else if (CurrentState == EOliveBrainState::Cancelling)
	{
		UE_LOG(LogOliveAI, Verbose, TEXT("Brain: Cancel requested but already Cancelling"));
	}
	else
	{
		UE_LOG(LogOliveAI, Verbose, TEXT("Brain: Cancel requested but state is Idle -- nothing to cancel"));
	}
}

void FOliveBrainLayer::ResetToIdle()
{
	if (CurrentState != EOliveBrainState::Idle)
	{
		UE_LOG(LogOliveAI, Log, TEXT("Brain: Safety reset to Idle from state %s"), LexToString(CurrentState));

		const EOliveBrainState OldState = CurrentState;
		CurrentState = EOliveBrainState::Idle;

		// Bypass IsValidTransition -- ResetToIdle is an unconditional safety escape
		OnStateChanged.Broadcast(OldState, EOliveBrainState::Idle);
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

	// Reset worker phase when entering Active
	if (NewState == EOliveBrainState::Active)
	{
		SetWorkerPhase(EOliveWorkerPhase::Streaming);
	}

	return true;
}

void FOliveBrainLayer::SetWorkerPhase(EOliveWorkerPhase NewPhase)
{
	if (CurrentState != EOliveBrainState::Active)
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
	TransitionTo(EOliveBrainState::Active);
	UE_LOG(LogOliveAI, Log, TEXT("Brain: Run started [%s]"), *CurrentRunId);
	return CurrentRunId;
}

void FOliveBrainLayer::CompleteRun(EOliveRunOutcome Outcome)
{
	LastOutcome = Outcome;
	UE_LOG(LogOliveAI, Log, TEXT("Brain: Run completed [%s] outcome=%s"),
		*CurrentRunId, LexToString(Outcome));

	// All outcomes transition to Idle -- no terminal states
	TransitionTo(EOliveBrainState::Idle);
}

// ==========================================
// Transition Validation
// ==========================================

bool FOliveBrainLayer::IsValidTransition(EOliveBrainState From, EOliveBrainState To) const
{
	switch (From)
	{
	case EOliveBrainState::Idle:
		return To == EOliveBrainState::Active;

	case EOliveBrainState::Active:
		return To == EOliveBrainState::Idle
			|| To == EOliveBrainState::Cancelling;

	case EOliveBrainState::Cancelling:
		return To == EOliveBrainState::Idle;

	default:
		return false;
	}
}
