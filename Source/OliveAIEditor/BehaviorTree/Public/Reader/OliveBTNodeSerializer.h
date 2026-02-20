// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IR/BehaviorTreeIR.h"

class UBehaviorTree;
class UBTNode;
class UBTCompositeNode;
class UBTTaskNode;
class UBTDecorator;
class UBTService;

/**
 * FOliveBTNodeSerializer
 *
 * Recursively serializes a UBehaviorTree node hierarchy into
 * FOliveIRBTNode IR format. Extracts node properties via UE reflection
 * and detects blackboard key references via FBlackboardKeySelector properties.
 *
 * Usage:
 *   FOliveBTNodeSerializer& Serializer = FOliveBTNodeSerializer::Get();
 *   FOliveIRBTNode Root = Serializer.SerializeTree(BehaviorTree);
 */
class OLIVEAIEDITOR_API FOliveBTNodeSerializer
{
public:
	/** Get the singleton instance */
	static FOliveBTNodeSerializer& Get();

	/**
	 * Serialize entire behavior tree starting from root
	 * @param BehaviorTree The behavior tree asset
	 * @return Root IR node containing the full tree hierarchy
	 */
	FOliveIRBTNode SerializeTree(const UBehaviorTree* BehaviorTree);

private:
	FOliveBTNodeSerializer() = default;
	~FOliveBTNodeSerializer() = default;

	FOliveBTNodeSerializer(const FOliveBTNodeSerializer&) = delete;
	FOliveBTNodeSerializer& operator=(const FOliveBTNodeSerializer&) = delete;

	/** Serialize a composite node and all its children recursively */
	FOliveIRBTNode SerializeComposite(const UBTCompositeNode* CompositeNode);

	/** Serialize a task node */
	FOliveIRBTNode SerializeTask(const UBTTaskNode* TaskNode);

	/** Serialize a decorator node */
	FOliveIRBTNode SerializeDecorator(const UBTDecorator* DecoratorNode);

	/** Serialize a service node */
	FOliveIRBTNode SerializeService(const UBTService* ServiceNode);

	/** Read editable UPROPERTYs from a node via reflection */
	TMap<FString, FString> ReadNodeProperties(const UBTNode* Node);

	/** Extract blackboard key references (FBlackboardKeySelector properties) */
	TArray<FString> ExtractBlackboardKeyReferences(const UBTNode* Node);

	/** Map a composite node to its composite type enum */
	EOliveIRBTCompositeType MapCompositeType(const UBTCompositeNode* CompositeNode);

	/** Generate next sequential node ID */
	FString NextNodeId();

	/** Node ID counter, reset per SerializeTree call */
	int32 NodeCounter = 0;
};
