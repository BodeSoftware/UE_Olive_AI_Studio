// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Top-level Brain state
 */
enum class EOliveBrainState : uint8
{
	Idle,                   // No active operation
	Planning,               // Orchestrator is generating a plan (Phase E2)
	WorkerActive,           // A worker/agentic loop is executing
	AwaitingConfirmation,   // Waiting for user approval (Tier 2/3)
	Cancelling,             // User hit Stop — draining in-flight ops
	Completed,              // Run finished successfully
	Error                   // Run failed (after exhausting retries)
};

/**
 * Sub-state within WorkerActive
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
 * Outcome of a run
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
	case EOliveBrainState::Idle: return TEXT("Idle");
	case EOliveBrainState::Planning: return TEXT("Planning");
	case EOliveBrainState::WorkerActive: return TEXT("WorkerActive");
	case EOliveBrainState::AwaitingConfirmation: return TEXT("AwaitingConfirmation");
	case EOliveBrainState::Cancelling: return TEXT("Cancelling");
	case EOliveBrainState::Completed: return TEXT("Completed");
	case EOliveBrainState::Error: return TEXT("Error");
	default: return TEXT("Unknown");
	}
}

inline const TCHAR* LexToString(EOliveWorkerPhase Phase)
{
	switch (Phase)
	{
	case EOliveWorkerPhase::Streaming: return TEXT("Streaming");
	case EOliveWorkerPhase::ExecutingTools: return TEXT("ExecutingTools");
	case EOliveWorkerPhase::Compiling: return TEXT("Compiling");
	case EOliveWorkerPhase::SelfCorrecting: return TEXT("SelfCorrecting");
	case EOliveWorkerPhase::Complete: return TEXT("Complete");
	default: return TEXT("Unknown");
	}
}
