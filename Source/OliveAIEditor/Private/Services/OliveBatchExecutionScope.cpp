// Copyright Olive AI Studio. All Rights Reserved.

#include "Services/OliveBatchExecutionScope.h"

// File-scoped thread_local avoids MSVC C2492 with __declspec(dllexport)
static thread_local bool bBatchExecutionActive = false;

FOliveBatchExecutionScope::FOliveBatchExecutionScope()
	: bPreviousState(bBatchExecutionActive)
{
	bBatchExecutionActive = true;
}

FOliveBatchExecutionScope::~FOliveBatchExecutionScope()
{
	bBatchExecutionActive = bPreviousState;
}

bool FOliveBatchExecutionScope::IsActive()
{
	return bBatchExecutionActive;
}
