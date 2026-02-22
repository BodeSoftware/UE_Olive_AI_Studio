// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/OliveToolRegistry.h"

DECLARE_LOG_CATEGORY_EXTERN(LogOliveCrossSystemTools, Log, All);

/**
 * FOliveCrossSystemToolHandlers
 *
 * Registers and handles all cross-system MCP tools.
 * Bridges bulk operations, snapshots, and composite creation.
 *
 * Tool Categories:
 * - Bulk: project.bulk_read, implement_interface, refactor_rename, create_ai_character, move_to_cpp
 * - Snapshot: project.snapshot, list_snapshots, rollback, diff
 */
class OLIVEAIEDITOR_API FOliveCrossSystemToolHandlers
{
public:
	/** Get singleton instance */
	static FOliveCrossSystemToolHandlers& Get();

	/** Register all cross-system tools with the tool registry */
	void RegisterAllTools();

	/** Unregister all cross-system tools */
	void UnregisterAllTools();

private:
	FOliveCrossSystemToolHandlers() = default;

	FOliveCrossSystemToolHandlers(const FOliveCrossSystemToolHandlers&) = delete;
	FOliveCrossSystemToolHandlers& operator=(const FOliveCrossSystemToolHandlers&) = delete;

	// Registration helpers
	void RegisterBulkTools();
	void RegisterBatchTools();
	void RegisterSnapshotTools();
	void RegisterIndexTools();

	// Bulk operation handlers
	FOliveToolResult HandleBulkRead(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleImplementInterface(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleRefactorRename(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleCreateAICharacter(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleMoveToCpp(const TSharedPtr<FJsonObject>& Params);

	// Batch write handler
	FOliveToolResult HandleBatchWrite(const TSharedPtr<FJsonObject>& Params);

	// Snapshot handlers
	FOliveToolResult HandleSnapshot(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleListSnapshots(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleRollback(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleDiff(const TSharedPtr<FJsonObject>& Params);

	// Index / context handlers
	FOliveToolResult HandleIndexBuild(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleIndexStatus(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleGetRelevantContext(const TSharedPtr<FJsonObject>& Params);

	TArray<FString> RegisteredToolNames;
};
