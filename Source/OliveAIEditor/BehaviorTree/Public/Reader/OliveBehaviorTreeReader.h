// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IR/BehaviorTreeIR.h"

class UBehaviorTree;

/**
 * Result of reading a Behavior Tree
 * Contains the BT IR and optionally the associated Blackboard IR
 */
struct OLIVEAIEDITOR_API FBTReadResult
{
	/** The behavior tree IR */
	FOliveIRBehaviorTree BehaviorTree;

	/** Optional associated blackboard IR (if BT has a blackboard set) */
	TOptional<FOliveIRBlackboard> Blackboard;

	/** Convert to JSON for MCP responses */
	TSharedPtr<FJsonObject> ToJson() const;
};

/**
 * FOliveBehaviorTreeReader
 *
 * Main entry point for reading Behavior Tree assets into IR format.
 * Coordinates reading of the BT node hierarchy and optionally the
 * associated Blackboard data.
 *
 * Usage:
 *   FOliveBehaviorTreeReader& Reader = FOliveBehaviorTreeReader::Get();
 *   TOptional<FBTReadResult> Result = Reader.ReadBehaviorTree("/Game/AI/BT_Enemy");
 */
class OLIVEAIEDITOR_API FOliveBehaviorTreeReader
{
public:
	/** Get the singleton instance */
	static FOliveBehaviorTreeReader& Get();

	/**
	 * Read a behavior tree by asset path
	 * Auto-reads associated blackboard if one is set
	 * @param AssetPath Content path (e.g., "/Game/AI/BT_Enemy")
	 * @param bIncludeBlackboard Whether to include the associated blackboard
	 * @return Read result with BT IR and optional BB IR
	 */
	TOptional<FBTReadResult> ReadBehaviorTree(const FString& AssetPath, bool bIncludeBlackboard = true);

	/**
	 * Read a behavior tree from an already-loaded asset
	 * @param BehaviorTree The behavior tree asset
	 * @param bIncludeBlackboard Whether to include the associated blackboard
	 * @return Read result with BT IR and optional BB IR
	 */
	TOptional<FBTReadResult> ReadBehaviorTree(const UBehaviorTree* BehaviorTree, bool bIncludeBlackboard = true);

	/**
	 * Collect all unique class names used in the tree
	 * @param RootNode The root IR node
	 * @param OutTasks Task class names
	 * @param OutDecorators Decorator class names
	 * @param OutServices Service class names
	 */
	void CollectUsedClasses(const FOliveIRBTNode& RootNode,
		TArray<FString>& OutTasks, TArray<FString>& OutDecorators, TArray<FString>& OutServices);

private:
	FOliveBehaviorTreeReader() = default;
	~FOliveBehaviorTreeReader() = default;

	FOliveBehaviorTreeReader(const FOliveBehaviorTreeReader&) = delete;
	FOliveBehaviorTreeReader& operator=(const FOliveBehaviorTreeReader&) = delete;

	/** Load a UBehaviorTree by asset path */
	UBehaviorTree* LoadBehaviorTree(const FString& AssetPath) const;
};
