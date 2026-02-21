// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Brain/OliveBrainState.h"

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnOliveBrainStateChanged, EOliveBrainState /* OldState */, EOliveBrainState /* NewState */);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnOliveWorkerPhaseChanged, EOliveWorkerPhase /* NewPhase */);

/**
 * Brain Layer
 *
 * Sits between the Conversation Manager and tool execution.
 * Manages state machine, operation history, prompt distillation,
 * self-correction, and loop detection.
 *
 * Phase E ships the shell + state machine + integration hooks.
 * Phase E2 adds orchestrator/worker agent architecture.
 */
class OLIVEAIEDITOR_API FOliveBrainLayer : public TSharedFromThis<FOliveBrainLayer>
{
public:
	FOliveBrainLayer();
	~FOliveBrainLayer();

	// ==========================================
	// State Access
	// ==========================================

	/** Get current brain state */
	EOliveBrainState GetState() const { return CurrentState; }

	/** Get current worker phase (only meaningful when WorkerActive) */
	EOliveWorkerPhase GetWorkerPhase() const { return CurrentWorkerPhase; }

	/** Get the current run ID (unique per user message -> completion cycle) */
	const FString& GetCurrentRunId() const { return CurrentRunId; }

	/** Check if the brain is in an active (non-idle) state */
	bool IsActive() const;

	// ==========================================
	// State Control
	// ==========================================

	/** Request cancellation of the current operation */
	void RequestCancel();

	/** Reset to idle state (after completion, error, or cancel) */
	void ResetToIdle();

	/** Transition to a new state (validates transition) */
	bool TransitionTo(EOliveBrainState NewState);

	/** Set the worker phase (only valid when in WorkerActive state) */
	void SetWorkerPhase(EOliveWorkerPhase NewPhase);

	// ==========================================
	// Run Management
	// ==========================================

	/** Begin a new run (generates RunId, transitions to WorkerActive) */
	FString BeginRun();

	/** Complete the current run with an outcome */
	void CompleteRun(EOliveRunOutcome Outcome);

	// ==========================================
	// Events
	// ==========================================

	/** Fired when brain state changes */
	FOnOliveBrainStateChanged OnStateChanged;

	/** Fired when worker phase changes within WorkerActive */
	FOnOliveWorkerPhaseChanged OnWorkerPhaseChanged;

private:
	/** Validate that a state transition is allowed */
	bool IsValidTransition(EOliveBrainState From, EOliveBrainState To) const;

	/** Current brain state */
	EOliveBrainState CurrentState = EOliveBrainState::Idle;

	/** Current worker phase */
	EOliveWorkerPhase CurrentWorkerPhase = EOliveWorkerPhase::Streaming;

	/** Current run identifier */
	FString CurrentRunId;

	/** Outcome of the last completed run */
	EOliveRunOutcome LastOutcome = EOliveRunOutcome::Completed;
};
