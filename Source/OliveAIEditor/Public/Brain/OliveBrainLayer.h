// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Brain/OliveBrainState.h"

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnOliveBrainStateChanged, EOliveBrainState /* OldState */, EOliveBrainState /* NewState */);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnOliveWorkerPhaseChanged, EOliveWorkerPhase /* NewPhase */);

/**
 * Brain Layer -- 3-state state machine (Idle / Active / Cancelling).
 *
 * Sits between the Conversation Manager and tool execution.
 * Manages state machine, operation history, prompt distillation,
 * self-correction, and loop detection.
 *
 * State transitions:
 *   Idle -> Active      (BeginRun)
 *   Active -> Idle      (CompleteRun -- any outcome)
 *   Active -> Cancelling (RequestCancel)
 *   Cancelling -> Idle  (cancel drained)
 *
 * Worker phases (Streaming, ExecutingTools, Compiling, SelfCorrecting, Complete)
 * are metadata on the Active state for UI progress display.
 */
class OLIVEAIEDITOR_API FOliveBrainLayer : public TSharedFromThis<FOliveBrainLayer>
{
public:
	FOliveBrainLayer();
	~FOliveBrainLayer();

	// ==========================================
	// State Access
	// ==========================================

	/** Get current brain state (Idle, Active, or Cancelling) */
	EOliveBrainState GetState() const { return CurrentState; }

	/** Get current worker phase (only meaningful when Active) */
	EOliveWorkerPhase GetWorkerPhase() const { return CurrentWorkerPhase; }

	/** Get the current run ID (unique per user message -> completion cycle) */
	const FString& GetCurrentRunId() const { return CurrentRunId; }

	/** Get the outcome of the last completed run (stored when transitioning Active->Idle) */
	EOliveRunOutcome GetLastOutcome() const { return LastOutcome; }

	/** Check if the brain is in an active (non-idle) state */
	bool IsActive() const;

	// ==========================================
	// State Control
	// ==========================================

	/** Request cancellation of the current operation. Transitions Active -> Cancelling. */
	void RequestCancel();

	/** Safety reset -- forces any state to Idle. Use after error recovery or abnormal termination. */
	void ResetToIdle();

	/** Transition to a new state (validates transition against 3-state model) */
	bool TransitionTo(EOliveBrainState NewState);

	/** Set the worker phase (only valid when in Active state). Used for UI progress display. */
	void SetWorkerPhase(EOliveWorkerPhase NewPhase);

	// ==========================================
	// Run Management
	// ==========================================

	/** Begin a new run (generates RunId, transitions Idle -> Active) */
	FString BeginRun();

	/** Complete the current run with an outcome. Always transitions to Idle. */
	void CompleteRun(EOliveRunOutcome Outcome);

	// ==========================================
	// Events
	// ==========================================

	/** Fired when brain state changes */
	FOnOliveBrainStateChanged OnStateChanged;

	/** Fired when worker phase changes within Active state */
	FOnOliveWorkerPhaseChanged OnWorkerPhaseChanged;

private:
	/** Validate that a state transition is allowed by the 3-state model */
	bool IsValidTransition(EOliveBrainState From, EOliveBrainState To) const;

	/** Current brain state */
	EOliveBrainState CurrentState = EOliveBrainState::Idle;

	/** Current worker phase (metadata on Active state) */
	EOliveWorkerPhase CurrentWorkerPhase = EOliveWorkerPhase::Streaming;

	/** Current run identifier */
	FString CurrentRunId;

	/** Outcome of the last completed run -- stored when transitioning Active -> Idle */
	EOliveRunOutcome LastOutcome = EOliveRunOutcome::Completed;
};
