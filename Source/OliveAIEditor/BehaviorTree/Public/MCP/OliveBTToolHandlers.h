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
 * Registered tools (P5 consolidation):
 * - behaviortree.create, read, add, modify, remove, move (6 tools)
 * - blackboard.modify (1 tool)
 *
 * Legacy tool names (add_composite, add_task, add_node, set_blackboard,
 * remove_node, move_node, set_node_property, blackboard.create, add_key,
 * remove_key, set_parent, read, modify_key, etc.) continue to work via the
 * alias map in OliveToolRegistry.cpp::GetToolAliases().
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

	// --- Consolidated dispatchers (P5) ---

	/** blackboard.modify: routes on 'action' to one of the Blackboard sub-handlers. */
	FOliveToolResult HandleBlackboardModify(const TSharedPtr<FJsonObject>& Params);

	/** behaviortree.add: routes on 'node_type' (or legacy 'node_kind') to composite/task/decorator/service. */
	FOliveToolResult HandleBehaviorTreeAdd(const TSharedPtr<FJsonObject>& Params);

	/** behaviortree.modify: routes on 'entity' (node|decorator|blackboard_ref). */
	FOliveToolResult HandleBehaviorTreeModify(const TSharedPtr<FJsonObject>& Params);

	// --- Internal Blackboard handlers ---
	FOliveToolResult HandleBlackboardCreate(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBlackboardRead(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBlackboardAddKey(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBlackboardRemoveKey(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBlackboardModifyKey(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBlackboardSetParent(const TSharedPtr<FJsonObject>& Params);

	// --- Internal Behavior Tree handlers ---
	FOliveToolResult HandleBehaviorTreeCreate(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBehaviorTreeRead(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBehaviorTreeSetBlackboard(const TSharedPtr<FJsonObject>& Params);

	/** Legacy add_node handler — still used by HandleBehaviorTreeAdd after node_type normalization. */
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
