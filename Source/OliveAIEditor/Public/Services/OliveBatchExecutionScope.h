// Copyright Olive AI Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * RAII scope that sets a thread-local flag indicating batch execution is active.
 * When active, OLIVE_SCOPED_TRANSACTION skips creating inner transactions,
 * allowing a single outer transaction to wrap the entire batch.
 * Supports nesting — destructor restores the previous state.
 */
class FOliveBatchExecutionScope
{
public:
	FOliveBatchExecutionScope();
	~FOliveBatchExecutionScope();

	/** Check whether batch execution is currently active on this thread */
	static bool IsActive();

	// Non-copyable, non-movable
	FOliveBatchExecutionScope(const FOliveBatchExecutionScope&) = delete;
	FOliveBatchExecutionScope& operator=(const FOliveBatchExecutionScope&) = delete;
	FOliveBatchExecutionScope(FOliveBatchExecutionScope&&) = delete;
	FOliveBatchExecutionScope& operator=(FOliveBatchExecutionScope&&) = delete;

private:
	bool bPreviousState;
};
