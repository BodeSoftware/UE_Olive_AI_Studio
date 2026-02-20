// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IR/BlueprintIR.h"

// Forward declarations
class UAnimBlueprint;
class UAnimGraphNode_Base;
class UAnimGraphNode_StateMachine;
class UAnimStateNodeBase;
class UAnimStateTransitionNode;
class UEdGraph;
class UAnimationStateMachineGraph;
class FOliveNodeSerializer;

DECLARE_LOG_CATEGORY_EXTERN(LogOliveAnimSerializer, Log, All);

/**
 * AnimGraph Serializer
 *
 * Specialized serializer for Animation Blueprint structures.
 * Handles:
 * - AnimGraph root and sub-graphs
 * - State machine hierarchies
 * - States (including conduits)
 * - Transitions with rules
 * - Animation node properties
 *
 * This serializer produces dedicated structured payloads rather than
 * falling back to generic graph serialization, enabling AI agents to
 * understand animation flow and state machine structure.
 */
class OLIVEAIEDITOR_API FOliveAnimGraphSerializer
{
public:
	FOliveAnimGraphSerializer();
	~FOliveAnimGraphSerializer() = default;

	// ============================================================================
	// High-Level Read Methods
	// ============================================================================

	/**
	 * Read all state machines from an Animation Blueprint
	 * @param AnimBlueprint The Animation Blueprint to read from
	 * @return Array of state machine IR structures
	 */
	TArray<FOliveIRAnimStateMachine> ReadStateMachines(const UAnimBlueprint* AnimBlueprint);

	/**
	 * Read a specific state machine by name
	 * @param AnimBlueprint The Animation Blueprint
	 * @param StateMachineName Name of the state machine
	 * @return The state machine IR, or empty optional if not found
	 */
	TOptional<FOliveIRAnimStateMachine> ReadStateMachine(
		const UAnimBlueprint* AnimBlueprint,
		const FString& StateMachineName);

	/**
	 * Read the AnimGraph as a simplified IR (without full node data)
	 * @param AnimBlueprint The Animation Blueprint
	 * @return AnimGraph summary with state machine references
	 */
	TSharedPtr<FJsonObject> ReadAnimGraphSummary(const UAnimBlueprint* AnimBlueprint);

	/**
	 * Read full AnimGraph with all node data
	 * @param AnimBlueprint The Animation Blueprint
	 * @return Full AnimGraph IR including state machine details
	 */
	FOliveIRGraph ReadAnimGraphFull(const UAnimBlueprint* AnimBlueprint);

private:
	// ============================================================================
	// State Machine Reading
	// ============================================================================

	/**
	 * Serialize a state machine graph to IR
	 */
	FOliveIRAnimStateMachine SerializeStateMachine(
		const UAnimationStateMachineGraph* StateMachineGraph,
		const UAnimBlueprint* OwningBlueprint);

	/**
	 * Find all state machine nodes in the AnimGraph
	 */
	TArray<UAnimGraphNode_StateMachine*> FindStateMachineNodes(const UAnimBlueprint* AnimBlueprint);

	// ============================================================================
	// State Reading
	// ============================================================================

	/**
	 * Serialize a single state to IR
	 */
	FOliveIRAnimState SerializeState(
		const UAnimStateNodeBase* StateNode,
		const UAnimationStateMachineGraph* OwningGraph);

	/**
	 * Get the animation asset referenced by a state (if any)
	 */
	FString GetStateAnimationAsset(const UAnimStateNodeBase* StateNode);

	/**
	 * Check if a state is a conduit
	 */
	bool IsConduitState(const UAnimStateNodeBase* StateNode) const;

	// ============================================================================
	// Transition Reading
	// ============================================================================

	/**
	 * Get all transitions into a state
	 */
	TArray<FString> GetTransitionsIn(
		const UAnimStateNodeBase* StateNode,
		const UAnimationStateMachineGraph* Graph);

	/**
	 * Get all transitions out of a state
	 */
	TArray<FString> GetTransitionsOut(
		const UAnimStateNodeBase* StateNode,
		const UAnimationStateMachineGraph* Graph);

	/**
	 * Serialize a transition rule to a description string
	 */
	FString SerializeTransitionRule(const UAnimStateTransitionNode* TransitionNode);

	// ============================================================================
	// AnimGraph Node Reading
	// ============================================================================

	/**
	 * Serialize an AnimGraph node to generic IR node format
	 */
	FOliveIRNode SerializeAnimNode(const UAnimGraphNode_Base* AnimNode);

	/**
	 * Extract animation-specific properties from a node
	 */
	TMap<FString, FString> ExtractAnimNodeProperties(const UAnimGraphNode_Base* AnimNode);

	// ============================================================================
	// Helper Methods
	// ============================================================================

	/**
	 * Find the AnimGraph in an Animation Blueprint
	 */
	UEdGraph* FindAnimGraph(const UAnimBlueprint* AnimBlueprint) const;

	/**
	 * Get the entry state of a state machine
	 */
	FString GetEntryStateName(const UAnimationStateMachineGraph* StateMachineGraph) const;

	/**
	 * Get the bound graph for a state (the graph inside the state)
	 */
	UEdGraph* GetStateBoundGraph(const UAnimStateNodeBase* StateNode) const;

	// ============================================================================
	// Member Variables
	// ============================================================================

	/** Node serializer for generic node conversion (shared with main Blueprint reader) */
	TSharedPtr<FOliveNodeSerializer> NodeSerializer;
};
