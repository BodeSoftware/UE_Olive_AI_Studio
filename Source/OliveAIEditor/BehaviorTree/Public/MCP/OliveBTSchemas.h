// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * BT/Blackboard Tool Schema Builder
 *
 * Provides JSON Schema Draft 7 definitions for all Behavior Tree and
 * Blackboard MCP tools. Reuses common helpers from OliveBlueprintSchemas.
 */
namespace OliveBTSchemas
{
	// ============================================================================
	// Blackboard Tool Schemas
	// ============================================================================

	/** Schema for blackboard.create: {path: string, parent?: string} */
	TSharedPtr<FJsonObject> BlackboardCreate();

	/** Schema for blackboard.read: {path: string, include_inherited?: bool} */
	TSharedPtr<FJsonObject> BlackboardRead();

	/**
	 * Schema for blackboard.add_key (upsert): {path, name, key_type, base_class?, enum_type?,
	 * instance_synced?, description?, new_name?}
	 * If the key already exists, modifies it instead of erroring.
	 * new_name is accepted for rename-on-modify (forwarded from old blackboard.modify_key).
	 */
	TSharedPtr<FJsonObject> BlackboardAddKey();

	/** Schema for blackboard.remove_key: {path: string, name: string} */
	TSharedPtr<FJsonObject> BlackboardRemoveKey();

	/** Schema for blackboard.set_parent: {path: string, parent_path: string} */
	TSharedPtr<FJsonObject> BlackboardSetParent();

	// ============================================================================
	// Behavior Tree Tool Schemas
	// ============================================================================

	/** Schema for behaviortree.create: {path: string, blackboard?: string} */
	TSharedPtr<FJsonObject> BehaviorTreeCreate();

	/** Schema for behaviortree.read: {path: string, include_blackboard?: bool} */
	TSharedPtr<FJsonObject> BehaviorTreeRead();

	/** Schema for behaviortree.set_blackboard: {path: string, blackboard: string} */
	TSharedPtr<FJsonObject> BehaviorTreeSetBlackboard();

	/**
	 * Schema for behaviortree.add_node: unified tool for adding any BT node type.
	 * {path, node_kind, parent_node_id?, node_id?, class?, composite_type?, child_index?, properties?}
	 * node_kind routes to composite/task/decorator/service logic.
	 */
	TSharedPtr<FJsonObject> BehaviorTreeAddNode();

	/** Schema for behaviortree.remove_node: {path: string, node_id: string} */
	TSharedPtr<FJsonObject> BehaviorTreeRemoveNode();

	/** Schema for behaviortree.move_node: {path, node_id, new_parent_id, child_index?} */
	TSharedPtr<FJsonObject> BehaviorTreeMoveNode();

	/** Schema for behaviortree.set_node_property: {path, node_id, property, value} */
	TSharedPtr<FJsonObject> BehaviorTreeSetNodeProperty();
}
