// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/OliveToolRegistry.h"

/**
 * Minimal self-correction policy: retry exactly once on transient errors.
 *
 * Transient: TIMEOUT, RATE_LIMIT, HTTP_5xx, anything carrying "TRANSIENT".
 * Non-transient (validation, execution, compile): pass through. The LLM
 * decides what to do from the 3-part error (code + message + suggestion).
 */
class OLIVEAIEDITOR_API FOliveSelfCorrectionPolicy
{
public:
	FOliveSelfCorrectionPolicy() = default;

	/** Returns true iff this tool result should be retried. Attempt is 1-based. */
	bool ShouldRetry(const FOliveToolResult& Result, int32 Attempt) const;

private:
	static bool IsTransient(const FString& ErrorCode);
};
