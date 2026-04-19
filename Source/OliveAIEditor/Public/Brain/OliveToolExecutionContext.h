// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Origin of a tool call
 */
enum class EOliveToolCallOrigin : uint8
{
	EditorChat,    // Built-in chat panel
	MCP            // External agent via MCP server
};

/**
 * Context for the current tool execution
 */
struct FOliveToolCallContext
{
	EOliveToolCallOrigin Origin = EOliveToolCallOrigin::EditorChat;
	FString SessionId;
	FString RunId;
	FName ActiveWorkerDomain;
	bool bRunModeActive = false;
};

/**
 * Thread-local singleton providing the current tool execution context.
 * Use FOliveToolExecutionContextScope (RAII) to set context for a block of code.
 */
class OLIVEAIEDITOR_API FOliveToolExecutionContext
{
public:
	/** Get the current context, or nullptr if none is active */
	static const FOliveToolCallContext* Get();

	/** Check if currently executing from MCP origin */
	static bool IsFromMCP();

private:
	friend class FOliveToolExecutionContextScope;
	/** Stored in .cpp as file-scoped thread_local to avoid C2492 with dllexport */
	static const FOliveToolCallContext*& GetCurrentContextRef();
};

/**
 * RAII scope that sets the tool execution context for its lifetime.
 * Restores the previous context on destruction (supports nesting).
 */
class OLIVEAIEDITOR_API FOliveToolExecutionContextScope
{
public:
	explicit FOliveToolExecutionContextScope(const FOliveToolCallContext& InContext);
	~FOliveToolExecutionContextScope();

	// Non-copyable, non-movable
	FOliveToolExecutionContextScope(const FOliveToolExecutionContextScope&) = delete;
	FOliveToolExecutionContextScope& operator=(const FOliveToolExecutionContextScope&) = delete;

private:
	const FOliveToolCallContext* PreviousContext;
	FOliveToolCallContext StoredContext;
};
