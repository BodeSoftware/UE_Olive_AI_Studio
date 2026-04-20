// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/OliveToolRegistry.h"
#include "IR/BehaviorTreeIR.h"

class UBehaviorTree;
class UBlackboardData;

DECLARE_LOG_CATEGORY_EXTERN(LogOliveBTTools, Log, All);

/**
 * FOliveBTToolHandlers
 *
 * Registers and handles all Behavior Tree and Blackboard MCP tools.
 * Acts as a bridge between the MCP tool registry and the BT/BB
 * reader/writer infrastructure.
 *
 * Tool Categories:
 * - Blackboard: blackboard.create, read, add_key (upsert), remove_key, set_parent
 * - BehaviorTree: behaviortree.create, read, set_blackboard, add_node (unified),
 *                 remove_node, move_node, set_node_property
 *
 * AI Freedom Phase 2 changes:
 * - behaviortree.add_composite/add_task/add_decorator/add_service consolidated
 *   into behaviortree.add_node with node_kind parameter
 * - blackboard.modify_key merged into blackboard.add_key (upsert semantics)
 */
class OLIVEAIEDITOR_API FOliveBTToolHandlers
{
public:
	/** Get singleton instance */
	static FOliveBTToolHandlers& Get();

	/** Register all BT/BB tools with the tool registry */
	void RegisterAllTools();

	/** Unregister all BT/BB tools */
	void UnregisterAllTools();

private:
	FOliveBTToolHandlers() = default;

	FOliveBTToolHandlers(const FOliveBTToolHandlers&) = delete;
	FOliveBTToolHandlers& operator=(const FOliveBTToolHandlers&) = delete;

	// Registration helpers
	void RegisterBlackboardTools();
	void RegisterBehaviorTreeTools();

	// Blackboard handlers
	FOliveToolResult HandleBlackboardCreate(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBlackboardRead(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBlackboardAddKey(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBlackboardRemoveKey(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBlackboardModifyKey(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBlackboardSetParent(const TSharedPtr<FJsonObject>& Params);

	// Behavior Tree handlers
	FOliveToolResult HandleBehaviorTreeCreate(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBehaviorTreeRead(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBehaviorTreeSetBlackboard(const TSharedPtr<FJsonObject>& Params);

	/** Unified add_node handler. Routes to the appropriate internal handler based on node_kind. */
	FOliveToolResult HandleBehaviorTreeAddNode(const TSharedPtr<FJsonObject>& Params);

	// Internal add helpers (used by HandleBehaviorTreeAddNode)
	FOliveToolResult HandleBehaviorTreeAddComposite(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBehaviorTreeAddTask(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBehaviorTreeAddDecorator(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBehaviorTreeAddService(const TSharedPtr<FJsonObject>& Params);

	FOliveToolResult HandleBehaviorTreeRemoveNode(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBehaviorTreeMoveNode(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBehaviorTreeSetNodeProperty(const TSharedPtr<FJsonObject>& Params);

	// Helpers
	bool LoadBlackboardFromParams(const TSharedPtr<FJsonObject>& Params,
		UBlackboardData*& OutBB, FOliveToolResult& OutError);
	bool LoadBehaviorTreeFromParams(const TSharedPtr<FJsonObject>& Params,
		UBehaviorTree*& OutBT, FOliveToolResult& OutError);
	EOliveIRBlackboardKeyType ParseKeyType(const FString& InTypeStr);

	TArray<FString> RegisteredToolNames;
};
