// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/OliveToolRegistry.h"

/**
 * Batch tool executor -- executes multiple tool operations in a single MCP call.
 *
 * Claude Code CLI generates a full build plan during planning, then calls
 * olive.build once with all operations. The plugin executes them in-process
 * on the game thread and returns aggregated results.
 *
 * Features:
 * - Pre-validates all steps (tool existence) before executing any
 * - Optional snapshot creation for one-click rollback
 * - Configurable error handling: stop on first failure or continue remaining
 * - Recursion guard to prevent olive.build from being called inside olive.build
 * - Aggregated result with per-step timing and status
 */
class FOliveBuildTool
{
public:
	/** Register olive.build with the tool registry */
	static void RegisterTool();

private:
	/**
	 * Handler for olive.build
	 *
	 * @param Params JSON parameters containing description, snapshot, steps[], on_error
	 * @return Aggregated result with per-step outcomes
	 */
	static FOliveToolResult HandleBuild(const TSharedPtr<FJsonObject>& Params);

	/** Recursion guard -- true while HandleBuild is executing */
	static bool bIsExecuting;
};
