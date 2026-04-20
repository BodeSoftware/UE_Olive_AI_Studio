// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Cross-System Tool Schema Builder
 *
 * Provides JSON Schema Draft 7 definitions for all Cross-System MCP tools.
 * Includes bulk operations, multi-asset workflows, and snapshot/rollback.
 * Reuses common helpers from OliveBlueprintSchemas.
 */
namespace OliveCrossSystemSchemas
{
	// ============================================================================
	// Bulk Operations
	// ============================================================================

	/** Schema for project.bulk_read: {paths: string[], read_mode?: "summary"|"full"} */
	TSharedPtr<FJsonObject> ProjectBulkRead();

	/** Schema for project.implement_interface: {paths: string[], interface: string} */
	TSharedPtr<FJsonObject> ProjectImplementInterface();

	/** Schema for project.refactor_rename: {asset_path: string, new_name: string, update_references?: bool} */
	TSharedPtr<FJsonObject> ProjectRefactorRename();

	// ============================================================================
	// Index / Context Operations
	// ============================================================================

	/** Schema for project.index_build: {force?: bool} */
	TSharedPtr<FJsonObject> ProjectIndexBuild();

	/** Schema for project.index_status: no params */
	TSharedPtr<FJsonObject> ProjectIndexStatus();

	/** Schema for project.get_relevant_context: {query: string, max_assets?: int, kinds?: string[]} */
	TSharedPtr<FJsonObject> ProjectGetRelevantContext();

	// ============================================================================
	// Recipe Operations
	// ============================================================================

	/** Schema for olive.get_recipe: {query: string} -- free-text keyword search */
	TSharedPtr<FJsonObject> RecipeGetRecipe();

}
