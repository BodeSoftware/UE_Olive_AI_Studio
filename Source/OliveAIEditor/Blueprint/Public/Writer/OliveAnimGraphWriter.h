// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IR/BlueprintIR.h"

// Forward declarations
class UAnimBlueprint;
class UAnimGraphNode_StateMachine;
class UAnimationStateMachineGraph;
class UAnimStateNodeBase;
class UAnimStateTransitionNode;
class UEdGraph;

DECLARE_LOG_CATEGORY_EXTERN(LogOliveAnimWriter, Log, All);

/**
 * Result of an AnimGraph write operation
 */
struct OLIVEAIEDITOR_API FOliveAnimGraphWriteResult
{
	/** Whether the operation succeeded */
	bool bSuccess = false;

	/** Error messages if the operation failed */
	TArray<FString> Errors;

	/** Warning messages generated during the operation */
	TArray<FString> Warnings;

	/** Created state machine name (for AddStateMachine) */
	FString CreatedStateMachineName;

	/** Created state name (for AddState) */
	FString CreatedStateName;

	/** Created transition ID (for AddTransition) */
	FString CreatedTransitionId;

	/**
	 * Create a successful result
	 * @param ItemName Optional name of created item
	 * @return Success result
	 */
	static FOliveAnimGraphWriteResult Success(const FString& ItemName = TEXT(""));

	/**
	 * Create an error result
	 * @param ErrorMessage The error message
	 * @return Error result
	 */
	static FOliveAnimGraphWriteResult Error(const FString& ErrorMessage);

	/**
	 * Add a warning to this result
	 * @param Warning The warning message
	 */
	void AddWarning(const FString& Warning);

	/**
	 * Add an error to this result
	 * @param ErrorMessage The error message
	 */
	void AddError(const FString& ErrorMessage);

	/** Returns the first non-empty error string, or DefaultMsg if none. */
	FString GetFirstError(const FString& DefaultMsg = TEXT("Operation failed")) const
	{
		for (const FString& E : Errors)
		{
			if (!E.IsEmpty()) return E;
		}
		return DefaultMsg;
	}
};

/**
 * FOliveAnimGraphWriter
 *
 * Specialized writer for Animation Blueprint state machines and transitions.
 * Handles creation and modification of state machines, states, and transitions
 * in Animation Blueprints.
 *
 * Key Responsibilities:
 * - Create state machines in AnimGraph
 * - Add states to existing state machines
 * - Create transitions between states
 * - Set transition rules and conditions
 *
 * All operations use FScopedTransaction for undo support and are validated
 * before execution.
 *
 * Usage:
 *   FOliveAnimGraphWriter& Writer = FOliveAnimGraphWriter::Get();
 *   auto Result = Writer.AddStateMachine(AnimBP, "Locomotion");
 *   if (Result.bSuccess) { ... }
 */
class OLIVEAIEDITOR_API FOliveAnimGraphWriter
{
public:
	/**
	 * Get the singleton instance
	 * @return Reference to the AnimGraph writer singleton
	 */
	static FOliveAnimGraphWriter& Get();

	// ============================================================================
	// State Machine Operations
	// ============================================================================

	/**
	 * Add a new state machine to an Animation Blueprint's AnimGraph
	 * @param AnimBlueprint The Animation Blueprint to modify
	 * @param StateMachineName Name for the new state machine
	 * @return Result with state machine name on success
	 */
	FOliveAnimGraphWriteResult AddStateMachine(
		UAnimBlueprint* AnimBlueprint,
		const FString& StateMachineName);

	/**
	 * Add a state to an existing state machine
	 * @param AnimBlueprint The Animation Blueprint containing the state machine
	 * @param StateMachineName Name of the state machine to add the state to
	 * @param StateName Name for the new state
	 * @param AnimationAsset Optional animation asset path to use in the state
	 * @return Result with state name on success
	 */
	FOliveAnimGraphWriteResult AddState(
		UAnimBlueprint* AnimBlueprint,
		const FString& StateMachineName,
		const FString& StateName,
		const FString& AnimationAsset = TEXT(""));

	/**
	 * Add a transition between two states in a state machine
	 * @param AnimBlueprint The Animation Blueprint containing the state machine
	 * @param StateMachineName Name of the state machine
	 * @param FromStateName Source state for the transition
	 * @param ToStateName Destination state for the transition
	 * @return Result with transition ID on success
	 */
	FOliveAnimGraphWriteResult AddTransition(
		UAnimBlueprint* AnimBlueprint,
		const FString& StateMachineName,
		const FString& FromStateName,
		const FString& ToStateName);

	/**
	 * Set the rule/condition for a transition
	 * @param AnimBlueprint The Animation Blueprint containing the state machine
	 * @param StateMachineName Name of the state machine
	 * @param FromStateName Source state of the transition
	 * @param ToStateName Destination state of the transition
	 * @param RuleExpression The transition rule expression (simplified for now)
	 * @return Result indicating success or failure
	 */
	FOliveAnimGraphWriteResult SetTransitionRule(
		UAnimBlueprint* AnimBlueprint,
		const FString& StateMachineName,
		const FString& FromStateName,
		const FString& ToStateName,
		const TSharedPtr<FJsonObject>& RuleExpression);

private:
	FOliveAnimGraphWriter() = default;
	~FOliveAnimGraphWriter() = default;

	// Non-copyable
	FOliveAnimGraphWriter(const FOliveAnimGraphWriter&) = delete;
	FOliveAnimGraphWriter& operator=(const FOliveAnimGraphWriter&) = delete;

	// ============================================================================
	// Helper Methods
	// ============================================================================

	/**
	 * Find the AnimGraph in an Animation Blueprint
	 * @param AnimBlueprint The Animation Blueprint
	 * @return The AnimGraph, or nullptr if not found
	 */
	UEdGraph* FindAnimGraph(UAnimBlueprint* AnimBlueprint) const;

	/**
	 * Find a state machine node by name in the AnimGraph
	 * @param AnimBlueprint The Animation Blueprint
	 * @param StateMachineName Name of the state machine to find
	 * @return The state machine node, or nullptr if not found
	 */
	UAnimGraphNode_StateMachine* FindStateMachine(
		UAnimBlueprint* AnimBlueprint,
		const FString& StateMachineName) const;

	/**
	 * Find a state node by name in a state machine
	 * @param StateMachineGraph The state machine graph
	 * @param StateName Name of the state to find
	 * @return The state node, or nullptr if not found
	 */
	UAnimStateNodeBase* FindState(
		UAnimationStateMachineGraph* StateMachineGraph,
		const FString& StateName) const;

	/**
	 * Find a transition between two states
	 * @param StateMachineGraph The state machine graph
	 * @param FromStateName Source state
	 * @param ToStateName Destination state
	 * @return The transition node, or nullptr if not found
	 */
	UAnimStateTransitionNode* FindTransition(
		UAnimationStateMachineGraph* StateMachineGraph,
		const FString& FromStateName,
		const FString& ToStateName) const;

	/**
	 * Mark an Animation Blueprint as modified for undo and save tracking
	 * @param AnimBlueprint The Animation Blueprint to mark dirty
	 */
	void MarkDirty(UAnimBlueprint* AnimBlueprint);

	/**
	 * Check if Play-In-Editor is currently active
	 * @return True if PIE is active
	 */
	bool IsPIEActive() const;

	/**
	 * Generate a unique name for a state machine node
	 * @param AnimGraph The AnimGraph to check for existing names
	 * @param BaseName Base name for the state machine
	 * @return Unique name (BaseName or BaseName_N)
	 */
	FString GenerateUniqueStateMachineName(
		UEdGraph* AnimGraph,
		const FString& BaseName) const;

	/**
	 * Generate a unique name for a state
	 * @param StateMachineGraph The state machine graph to check
	 * @param BaseName Base name for the state
	 * @return Unique name (BaseName or BaseName_N)
	 */
	FString GenerateUniqueStateName(
		UAnimationStateMachineGraph* StateMachineGraph,
		const FString& BaseName) const;
};
