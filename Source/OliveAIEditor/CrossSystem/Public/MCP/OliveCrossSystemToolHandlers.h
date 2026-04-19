// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/OliveToolRegistry.h"

DECLARE_LOG_CATEGORY_EXTERN(LogOliveCrossSystemTools, Log, All);

class FSQLiteDatabase;

/**
 * FOliveCrossSystemToolHandlers
 *
 * Registers and handles all cross-system MCP tools.
 * Bridges bulk operations, snapshots, and composite creation.
 *
 * Tool Categories (post-P5 consolidation):
 * - Read: project.read (include-array dispatch), project.refactor_rename
 * - Snapshot: project.snapshot (action-dispatch, subsumes list), project.rollback, project.diff
 * - Index: project.index (action-dispatch, subsumes build/status)
 * - Search: project.search (alias for get_relevant_context)
 * - Recipe: olive.get_recipe, olive.search_community_blueprints
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
	void RegisterReadTools();
	void RegisterSnapshotTools();
	void RegisterIndexTools();

	// Consolidated P5 read dispatcher — dispatches on `include` array; merges
	// results from multiple read-family handlers into a single response.
	FOliveToolResult HandleProjectRead(const TSharedPtr<FJsonObject>& Params);

	// Consolidated P5 index dispatcher — dispatches on `action` ("build" | "status").
	FOliveToolResult HandleProjectIndex(const TSharedPtr<FJsonObject>& Params);

	// Sub-handlers used by HandleProjectRead (kept private to this module)
	FOliveToolResult HandleBulkRead(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleGetRelevantContext(const TSharedPtr<FJsonObject>& Params);

	// Refactor handler (survives)
	FOliveToolResult HandleRefactorRename(const TSharedPtr<FJsonObject>& Params);

	// Snapshot handlers
	FOliveToolResult HandleSnapshot(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleListSnapshots(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleRollback(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleDiff(const TSharedPtr<FJsonObject>& Params);

	// Index sub-handlers used by HandleProjectIndex
	FOliveToolResult HandleIndexBuild(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleIndexStatus(const TSharedPtr<FJsonObject>& Params);

	// Recipe system
	void RegisterRecipeTools();
	void LoadRecipeLibrary();
	FOliveToolResult HandleGetRecipe(const TSharedPtr<FJsonObject>& Params);

	/** Loaded recipes: Key = "category/name", Value = file content */
	TMap<FString, FString> RecipeLibrary;

	/** Manifest data: Key = "category/name", Value = description */
	TMap<FString, FString> RecipeDescriptions;

	/** Categories discovered from manifest */
	TArray<FString> RecipeCategories;

	/** Tags per recipe entry for keyword search. Key = "category/name" */
	TMap<FString, TArray<FString>> RecipeTags;

	// Community Blueprint search
	void RegisterCommunityTools();
	void OpenCommunityDatabase();
	void CloseCommunityDatabase();
	FOliveToolResult HandleSearchCommunityBlueprints(const TSharedPtr<FJsonObject>& Params);

	/** SQLite connection for community blueprint database (lazy init, read-only) */
	TSharedPtr<FSQLiteDatabase> CommunityDb;

	/** Whether we've attempted to open the db (avoid retrying on missing file) */
	bool bCommunityDbInitAttempted = false;

	TArray<FString> RegisteredToolNames;
};
