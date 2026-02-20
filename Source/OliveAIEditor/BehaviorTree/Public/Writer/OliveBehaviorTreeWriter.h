// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UBehaviorTree;
class UBTCompositeNode;
class UBTTaskNode;
class UBTDecorator;
class UBTService;
class UBTNode;
class UBlackboardData;

/**
 * FOliveBehaviorTreeWriter
 *
 * Creates and modifies UBehaviorTree assets. Handles node creation,
 * attachment, removal, and property setting. Uses node ID-based
 * addressing (node_0, node_1, etc.) matching the serializer output.
 *
 * All write operations use FScopedTransaction for undo support.
 *
 * Usage:
 *   FOliveBehaviorTreeWriter& Writer = FOliveBehaviorTreeWriter::Get();
 *   UBehaviorTree* BT = Writer.CreateBehaviorTree("/Game/AI/BT_Enemy");
 *   Writer.AddComposite(BT, "node_1", "Sequence");
 *   Writer.AddTask(BT, "node_2", "BTTask_MoveTo");
 */
class OLIVEAIEDITOR_API FOliveBehaviorTreeWriter
{
public:
	/** Get the singleton instance */
	static FOliveBehaviorTreeWriter& Get();

	/**
	 * Create a new Behavior Tree asset with a default root Selector
	 * @param AssetPath Content path for the new BT
	 * @return Created behavior tree, or nullptr on failure
	 */
	UBehaviorTree* CreateBehaviorTree(const FString& AssetPath);

	/**
	 * Set the associated Blackboard for a Behavior Tree
	 * @param BehaviorTree Target BT
	 * @param BlackboardPath Asset path to the Blackboard
	 * @return True if blackboard was set successfully
	 */
	bool SetBlackboard(UBehaviorTree* BehaviorTree, const FString& BlackboardPath);

	/**
	 * Add a composite node as a child of the specified parent
	 * @param BehaviorTree Target BT
	 * @param ParentNodeId Node ID of the parent composite
	 * @param CompositeType "Selector", "Sequence", or "SimpleParallel"
	 * @param ChildIndex Insert position (-1 for end)
	 * @return Node ID of the new node, or empty on failure
	 */
	FString AddComposite(UBehaviorTree* BehaviorTree, const FString& ParentNodeId,
		const FString& CompositeType, int32 ChildIndex = -1);

	/**
	 * Add a task node as a child of the specified parent
	 * @param BehaviorTree Target BT
	 * @param ParentNodeId Node ID of the parent composite
	 * @param TaskClass Task class name
	 * @param ChildIndex Insert position (-1 for end)
	 * @param Properties Optional initial property values
	 * @return Node ID of the new node, or empty on failure
	 */
	FString AddTask(UBehaviorTree* BehaviorTree, const FString& ParentNodeId,
		const FString& TaskClass, int32 ChildIndex = -1,
		const TMap<FString, FString>& Properties = {});

	/**
	 * Add a decorator to a node
	 * @param BehaviorTree Target BT
	 * @param NodeId Node ID to attach the decorator to
	 * @param DecoratorClass Decorator class name
	 * @param Properties Optional initial property values
	 * @return Node ID of the new decorator, or empty on failure
	 */
	FString AddDecorator(UBehaviorTree* BehaviorTree, const FString& NodeId,
		const FString& DecoratorClass, const TMap<FString, FString>& Properties = {});

	/**
	 * Add a service to a node
	 * @param BehaviorTree Target BT
	 * @param NodeId Node ID to attach the service to
	 * @param ServiceClass Service class name
	 * @param Properties Optional initial property values
	 * @return Node ID of the new service, or empty on failure
	 */
	FString AddService(UBehaviorTree* BehaviorTree, const FString& NodeId,
		const FString& ServiceClass, const TMap<FString, FString>& Properties = {});

	/**
	 * Remove a node from the tree
	 * @param BehaviorTree Target BT
	 * @param NodeId Node ID to remove
	 * @return True if node was found and removed
	 */
	bool RemoveNode(UBehaviorTree* BehaviorTree, const FString& NodeId);

	/**
	 * Move a node to a new parent
	 * @param BehaviorTree Target BT
	 * @param NodeId Node ID to move
	 * @param NewParentId Node ID of the new parent composite
	 * @param ChildIndex Insert position in new parent (-1 for end)
	 * @return True if node was moved successfully
	 */
	bool MoveNode(UBehaviorTree* BehaviorTree, const FString& NodeId,
		const FString& NewParentId, int32 ChildIndex = -1);

	/**
	 * Set a UPROPERTY value on a node using reflection
	 * @param BehaviorTree Target BT
	 * @param NodeId Target node ID
	 * @param PropertyName UPROPERTY name
	 * @param Value Value as string (UE ImportText format)
	 * @return True if property was set successfully
	 */
	bool SetNodeProperty(UBehaviorTree* BehaviorTree, const FString& NodeId,
		const FString& PropertyName, const FString& Value);

private:
	FOliveBehaviorTreeWriter() = default;
	~FOliveBehaviorTreeWriter() = default;

	FOliveBehaviorTreeWriter(const FOliveBehaviorTreeWriter&) = delete;
	FOliveBehaviorTreeWriter& operator=(const FOliveBehaviorTreeWriter&) = delete;

	/**
	 * Build a cache mapping node IDs to UBTNode pointers
	 * Node IDs are assigned in the same order as the serializer (DFS traversal)
	 * @param BehaviorTree The behavior tree to cache
	 */
	void BuildNodeCache(const UBehaviorTree* BehaviorTree);

	/**
	 * Walk the tree recursively assigning node IDs
	 */
	void WalkTreeForCache(const UBTCompositeNode* Node);

	/**
	 * Find a node by ID in the cache
	 * @param NodeId The node ID to find
	 * @return The node, or nullptr if not found
	 */
	UBTNode* FindNodeById(const FString& NodeId) const;

	/**
	 * Find the parent composite of a node
	 * @param BehaviorTree The behavior tree
	 * @param TargetNode The node to find the parent of
	 * @param OutChildIndex Output: index of the node in parent's Children array
	 * @return Parent composite, or nullptr if not found
	 */
	UBTCompositeNode* FindParentComposite(const UBehaviorTree* BehaviorTree,
		const UBTNode* TargetNode, int32& OutChildIndex) const;

	/**
	 * Apply properties to a node via reflection
	 */
	void ApplyProperties(UBTNode* Node, const TMap<FString, FString>& Properties);

	/**
	 * Load a UBehaviorTree by path
	 */
	UBehaviorTree* LoadBehaviorTree(const FString& AssetPath) const;

	/** Cached node ID -> UBTNode mapping */
	TMap<FString, UBTNode*> NodeCache;

	/** Counter for assigning node IDs during cache build */
	int32 CacheCounter = 0;
};
