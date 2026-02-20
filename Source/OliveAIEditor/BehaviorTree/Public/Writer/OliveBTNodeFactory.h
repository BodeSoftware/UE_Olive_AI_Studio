// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UBTCompositeNode;
class UBTTaskNode;
class UBTDecorator;
class UBTService;
class UBTNode;

DECLARE_LOG_CATEGORY_EXTERN(LogOliveBTWriter, Log, All);

/**
 * FOliveBTNodeFactory
 *
 * Creates Behavior Tree node instances by class name.
 * Uses multi-strategy class resolution to find node classes
 * from various name formats (BTTask_MoveTo, MoveTo, UBTTask_MoveTo).
 *
 * All nodes are created with RF_Transactional flag for undo support.
 *
 * Usage:
 *   FOliveBTNodeFactory& Factory = FOliveBTNodeFactory::Get();
 *   UBTCompositeNode* Selector = Factory.CreateComposite(Outer, "Selector");
 *   UBTTaskNode* MoveToTask = Factory.CreateTask(Outer, "BTTask_MoveTo");
 */
class OLIVEAIEDITOR_API FOliveBTNodeFactory
{
public:
	/** Get the singleton instance */
	static FOliveBTNodeFactory& Get();

	/**
	 * Create a composite node by type name
	 * @param Outer The outer object (typically the BehaviorTree)
	 * @param CompositeType "Selector", "Sequence", or "SimpleParallel" (case-insensitive)
	 * @return Created composite node, or nullptr if type is unknown
	 */
	UBTCompositeNode* CreateComposite(UObject* Outer, const FString& CompositeType);

	/**
	 * Create a task node by class name
	 * @param Outer The outer object
	 * @param ClassName Class name (e.g., "BTTask_MoveTo", "MoveTo")
	 * @return Created task node, or nullptr if class not found
	 */
	UBTTaskNode* CreateTask(UObject* Outer, const FString& ClassName);

	/**
	 * Create a decorator by class name
	 * @param Outer The outer object
	 * @param ClassName Class name (e.g., "BTDecorator_Blackboard", "Blackboard")
	 * @return Created decorator, or nullptr if class not found
	 */
	UBTDecorator* CreateDecorator(UObject* Outer, const FString& ClassName);

	/**
	 * Create a service by class name
	 * @param Outer The outer object
	 * @param ClassName Class name (e.g., "BTService_DefaultFocus", "DefaultFocus")
	 * @return Created service, or nullptr if class not found
	 */
	UBTService* CreateService(UObject* Outer, const FString& ClassName);

	/**
	 * Resolve a node class by name with multiple strategies
	 * @param ClassName The class name to resolve
	 * @param BaseClass Required base class (UBTTaskNode, UBTDecorator, etc.)
	 * @return The resolved UClass, or nullptr if not found
	 */
	UClass* ResolveNodeClass(const FString& ClassName, UClass* BaseClass);

private:
	FOliveBTNodeFactory() = default;
	~FOliveBTNodeFactory() = default;

	FOliveBTNodeFactory(const FOliveBTNodeFactory&) = delete;
	FOliveBTNodeFactory& operator=(const FOliveBTNodeFactory&) = delete;
};
