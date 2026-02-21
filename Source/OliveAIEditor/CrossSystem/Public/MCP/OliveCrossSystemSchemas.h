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

	/** Schema for project.create_ai_character: {name, path, parent_class?, blackboard_keys?, behavior_tree_root?} */
	TSharedPtr<FJsonObject> ProjectCreateAICharacter();

	/** Schema for project.move_to_cpp: {asset_path, module_name, target_class_name, parent_class?, create_wrapper_blueprint?, compile_after?} */
	TSharedPtr<FJsonObject> ProjectMoveToCpp();

	// ============================================================================
	// Snapshot Operations
	// ============================================================================

	/** Schema for project.snapshot: {name: string, paths: string[], description?: string} */
	TSharedPtr<FJsonObject> ProjectSnapshot();

	/** Schema for project.list_snapshots: {asset_filter?: string} */
	TSharedPtr<FJsonObject> ProjectListSnapshots();

	/** Schema for project.rollback: {snapshot_id: string, paths?: string[], preview_only?: bool, confirmation_token?: string} */
	TSharedPtr<FJsonObject> ProjectRollback();

	/** Schema for project.diff: {snapshot_id: string, paths?: string[]} */
	TSharedPtr<FJsonObject> ProjectDiff();
}
