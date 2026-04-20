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

/**
 * Chat interaction mode -- controls tool access and AI behavior.
 * Modeled after Claude Code CLI's /code, /plan, /ask commands.
 */
enum class EOliveChatMode : uint8
{
	Code,   // Full autonomous execution -- all tools, no confirmation except destructive ops
	Plan,   // Read + plan -- write tools return PLAN_MODE error, preview allowed
	Ask     // Read-only -- write tools return ASK_MODE error
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

inline const TCHAR* LexToString(EOliveChatMode Mode)
{
	switch (Mode)
	{
	case EOliveChatMode::Code: return TEXT("Code");
	case EOliveChatMode::Plan: return TEXT("Plan");
	case EOliveChatMode::Ask:  return TEXT("Ask");
	default: return TEXT("Unknown");
	}
}

// Forward declaration -- full UENUM definition lives in OliveAISettings.h (Task 4).
// The enums are value-identical (Code=0, Plan=1, Ask=2), so static_cast is safe.
enum class EOliveChatModeConfig : uint8;

/**
 * Converts the settings-serializable EOliveChatModeConfig to the runtime EOliveChatMode.
 * Both enums share the same ordinal layout so a static_cast is sufficient.
 *
 * NOTE: This function cannot be defined inline here because EOliveChatModeConfig is only
 * forward-declared. Include OliveAISettings.h before calling this, or use the definition
 * provided in OliveAISettings.h after EOliveChatModeConfig is fully defined there.
 */
inline EOliveChatMode ChatModeFromConfig(EOliveChatModeConfig C)
{
	return static_cast<EOliveChatMode>(static_cast<uint8>(C));
}
