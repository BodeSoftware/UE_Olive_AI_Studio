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

	/** Schema for blackboard.add_key: {path, name, key_type, base_class?, enum_type?, instance_synced?, description?} */
	TSharedPtr<FJsonObject> BlackboardAddKey();

	/** Schema for blackboard.remove_key: {path: string, name: string} */
	TSharedPtr<FJsonObject> BlackboardRemoveKey();

	/** Schema for blackboard.modify_key: {path, name, new_name?, instance_synced?, description?} */
	TSharedPtr<FJsonObject> BlackboardModifyKey();

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

	/** Schema for behaviortree.add_composite: {path, parent_node_id, composite_type, child_index?} */
	TSharedPtr<FJsonObject> BehaviorTreeAddComposite();

	/** Schema for behaviortree.add_task: {path, parent_node_id, task_class, child_index?, properties?} */
	TSharedPtr<FJsonObject> BehaviorTreeAddTask();

	/** Schema for behaviortree.add_decorator: {path, node_id, decorator_class, properties?} */
	TSharedPtr<FJsonObject> BehaviorTreeAddDecorator();

	/** Schema for behaviortree.add_service: {path, node_id, service_class, properties?} */
	TSharedPtr<FJsonObject> BehaviorTreeAddService();

	/** Schema for behaviortree.remove_node: {path: string, node_id: string} */
	TSharedPtr<FJsonObject> BehaviorTreeRemoveNode();

	/** Schema for behaviortree.move_node: {path, node_id, new_parent_id, child_index?} */
	TSharedPtr<FJsonObject> BehaviorTreeMoveNode();

	/** Schema for behaviortree.set_node_property: {path, node_id, property, value} */
	TSharedPtr<FJsonObject> BehaviorTreeSetNodeProperty();
}
