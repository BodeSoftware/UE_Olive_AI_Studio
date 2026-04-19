// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Top-level Brain state -- simplified 3-state model.
 * Detailed worker progress is tracked via EOliveWorkerPhase (telemetry only).
 * Terminal outcomes (success/failure) are available via GetLastOutcome() on the brain layer.
 */
enum class EOliveBrainState : uint8
{
	Idle,           // No active operation -- last run outcome available via GetLastOutcome()
	Active,         // AI is working (streaming, tools, compiling, self-correcting)
	Cancelling      // User hit Stop -- draining in-flight ops
};

/**
 * Sub-state within Active -- used for telemetry and progress display only.
 * Does not affect state machine transitions.
 */
enum class EOliveWorkerPhase : uint8
{
	Streaming,          // Receiving model response
	ExecutingTools,     // Running tool calls
	Compiling,          // Auto-compile in progress
	SelfCorrecting,     // Feeding errors back to model
	Complete            // Worker finished its task
};

/**
 * Outcome of a completed run -- stored on the brain layer after transitioning back to Idle.
 */
enum class EOliveRunOutcome : uint8
{
	Completed,          // All steps done successfully
	PartialSuccess,     // Some steps completed, some failed
	Failed,             // All/critical steps failed
	Cancelled           // User cancelled
};

/** Convert state enum to string for logging */
inline const TCHAR* LexToString(EOliveBrainState State)
{
	switch (State)
	{
	case EOliveBrainState::Idle:       return TEXT("Idle");
	case EOliveBrainState::Active:     return TEXT("Active");
	case EOliveBrainState::Cancelling: return TEXT("Cancelling");
	default: return TEXT("Unknown");
	}
}

inline const TCHAR* LexToString(EOliveWorkerPhase Phase)
{
	switch (Phase)
	{
	case EOliveWorkerPhase::Streaming:        return TEXT("Streaming");
	case EOliveWorkerPhase::ExecutingTools:   return TEXT("ExecutingTools");
	case EOliveWorkerPhase::Compiling:        return TEXT("Compiling");
	case EOliveWorkerPhase::SelfCorrecting:   return TEXT("SelfCorrecting");
	case EOliveWorkerPhase::Complete:         return TEXT("Complete");
	default: return TEXT("Unknown");
	}
}

inline const TCHAR* LexToString(EOliveRunOutcome Outcome)
{
	switch (Outcome)
	{
	case EOliveRunOutcome::Completed:      return TEXT("Completed");
	case EOliveRunOutcome::PartialSuccess: return TEXT("PartialSuccess");
	case EOliveRunOutcome::Failed:         return TEXT("Failed");
	case EOliveRunOutcome::Cancelled:      return TEXT("Cancelled");
	default: return TEXT("Unknown");
	}
}

