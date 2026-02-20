// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IR/CommonIR.h"

// Forward declarations
class UEdGraphNode;
class UEdGraphPin;
class UK2Node;
class UK2Node_CallFunction;
class UK2Node_VariableGet;
class UK2Node_VariableSet;
class UK2Node_Event;
class UK2Node_CustomEvent;
class UK2Node_FunctionEntry;
class UK2Node_FunctionResult;
class UK2Node_IfThenElse;
class UK2Node_ExecutionSequence;
class UK2Node_DynamicCast;
class UK2Node_MacroInstance;
class UK2Node_Timeline;
class UK2Node_Knot;
class UEdGraphNode_Comment;
class FOlivePinSerializer;

/**
 * Namespace containing supported node class names for quick lookup
 */
namespace OliveSupportedNodes
{
	/**
	 * Check if a node class name is supported by the specialized serializers
	 * @param ClassName The class name to check (e.g., "UK2Node_CallFunction")
	 * @return True if the node type has a specialized serializer
	 */
	bool IsSupported(FName ClassName);

	/**
	 * Get all supported node class names
	 * @return Array of all class names with specialized serializers
	 */
	TArray<FName> GetAllSupported();
}

/**
 * FOliveNodeSerializer
 *
 * Serializes UEdGraphNode instances to FOliveIRNode intermediate representation.
 * Provides specialized serialization for common K2 node types with fallback
 * to generic serialization for unknown node types.
 *
 * This class is responsible for:
 * - Identifying node types and dispatching to appropriate serializers
 * - Extracting node metadata (title, category, properties)
 * - Serializing all pins on a node
 * - Handling specialized data for function calls, variables, events, etc.
 *
 * Supported Node Types:
 * - UK2Node_CallFunction: Function calls with target and parameter info
 * - UK2Node_VariableGet/Set: Variable access with variable name and scope
 * - UK2Node_Event: Native event handlers (BeginPlay, Tick, etc.)
 * - UK2Node_CustomEvent: User-defined events
 * - UK2Node_FunctionEntry/Result: Function graph entry/exit
 * - UK2Node_IfThenElse: Branch nodes
 * - UK2Node_ExecutionSequence: Sequence nodes
 * - UK2Node_DynamicCast: Cast nodes
 * - UK2Node_MacroInstance: Macro instances
 * - UK2Node_Timeline: Timeline nodes
 * - UK2Node_Knot: Reroute/knot nodes
 * - UEdGraphNode_Comment: Comment nodes
 *
 * Usage:
 *   TSharedRef<FOliveNodeSerializer> NodeSerializer = MakeShared<FOliveNodeSerializer>();
 *   TMap<const UEdGraphNode*, FString> NodeIdMap;  // Pre-populated with node->id mappings
 *   FOliveIRNode IRNode = NodeSerializer->SerializeNode(Node, NodeIdMap);
 */
class OLIVEAIEDITOR_API FOliveNodeSerializer
{
public:
	FOliveNodeSerializer();
	~FOliveNodeSerializer() = default;

	// ============================================================================
	// Main Serialization Methods
	// ============================================================================

	/**
	 * Serialize a UEdGraphNode to IR format
	 * Automatically dispatches to specialized serializers based on node type
	 * @param Node The node to serialize
	 * @param NodeIdMap Map of nodes to their IR IDs (for connection resolution)
	 * @return The serialized node in IR format
	 */
	FOliveIRNode SerializeNode(
		const UEdGraphNode* Node,
		const TMap<const UEdGraphNode*, FString>& NodeIdMap) const;

	/**
	 * Check if a node has a specialized serializer
	 * @param Node The node to check
	 * @return True if the node type has specialized handling
	 */
	bool HasSpecializedSerializer(const UEdGraphNode* Node) const;

	/**
	 * Get list of node classes with specialized serializers
	 * @return Array of class names (e.g., "UK2Node_CallFunction")
	 */
	static TArray<FName> GetSupportedNodeClasses();

private:
	// ============================================================================
	// Specialized Serializers
	// ============================================================================

	/**
	 * Serialize a function call node
	 * Extracts function name, owning class, and whether it's a pure function
	 */
	FOliveIRNode SerializeCallFunction(
		const UK2Node_CallFunction* Node,
		const TMap<const UEdGraphNode*, FString>& NodeIdMap) const;

	/**
	 * Serialize a variable get node
	 * Extracts variable name and property scope
	 */
	FOliveIRNode SerializeVariableGet(
		const UK2Node_VariableGet* Node,
		const TMap<const UEdGraphNode*, FString>& NodeIdMap) const;

	/**
	 * Serialize a variable set node
	 * Extracts variable name and property scope
	 */
	FOliveIRNode SerializeVariableSet(
		const UK2Node_VariableSet* Node,
		const TMap<const UEdGraphNode*, FString>& NodeIdMap) const;

	/**
	 * Serialize a native event node (BeginPlay, Tick, etc.)
	 */
	FOliveIRNode SerializeEvent(
		const UK2Node_Event* Node,
		const TMap<const UEdGraphNode*, FString>& NodeIdMap) const;

	/**
	 * Serialize a custom event node
	 */
	FOliveIRNode SerializeCustomEvent(
		const UK2Node_CustomEvent* Node,
		const TMap<const UEdGraphNode*, FString>& NodeIdMap) const;

	/**
	 * Serialize a function entry node (beginning of function graph)
	 */
	FOliveIRNode SerializeFunctionEntry(
		const UK2Node_FunctionEntry* Node,
		const TMap<const UEdGraphNode*, FString>& NodeIdMap) const;

	/**
	 * Serialize a function result node (return from function)
	 */
	FOliveIRNode SerializeFunctionResult(
		const UK2Node_FunctionResult* Node,
		const TMap<const UEdGraphNode*, FString>& NodeIdMap) const;

	/**
	 * Serialize a branch node (if/then/else)
	 */
	FOliveIRNode SerializeBranch(
		const UK2Node_IfThenElse* Node,
		const TMap<const UEdGraphNode*, FString>& NodeIdMap) const;

	/**
	 * Serialize a sequence node (execute in order)
	 */
	FOliveIRNode SerializeSequence(
		const UK2Node_ExecutionSequence* Node,
		const TMap<const UEdGraphNode*, FString>& NodeIdMap) const;

	/**
	 * Serialize a cast node (dynamic cast)
	 */
	FOliveIRNode SerializeCast(
		const UK2Node_DynamicCast* Node,
		const TMap<const UEdGraphNode*, FString>& NodeIdMap) const;

	/**
	 * Serialize a macro instance node
	 */
	FOliveIRNode SerializeMacroInstance(
		const UK2Node_MacroInstance* Node,
		const TMap<const UEdGraphNode*, FString>& NodeIdMap) const;

	/**
	 * Serialize a timeline node
	 */
	FOliveIRNode SerializeTimeline(
		const UK2Node_Timeline* Node,
		const TMap<const UEdGraphNode*, FString>& NodeIdMap) const;

	/**
	 * Serialize a knot (reroute) node
	 */
	FOliveIRNode SerializeKnot(
		const UK2Node_Knot* Node,
		const TMap<const UEdGraphNode*, FString>& NodeIdMap) const;

	/**
	 * Serialize a comment node
	 */
	FOliveIRNode SerializeComment(
		const UEdGraphNode_Comment* Node) const;

	/**
	 * Fallback serializer for unknown node types
	 * Extracts basic node info and all pins
	 */
	FOliveIRNode SerializeGenericNode(
		const UEdGraphNode* Node,
		const TMap<const UEdGraphNode*, FString>& NodeIdMap) const;

	// ============================================================================
	// Helper Methods
	// ============================================================================

	/**
	 * Determine the category for a node
	 * @param Node The node to categorize
	 * @return The node category enum value
	 */
	EOliveIRNodeCategory DetermineCategory(const UEdGraphNode* Node) const;

	/**
	 * Serialize all pins on a node into the IR node structure
	 * @param Node The source node
	 * @param OutIR The IR node to populate with pins
	 * @param NodeIdMap Map of nodes to their IR IDs
	 */
	void SerializePins(
		const UEdGraphNode* Node,
		FOliveIRNode& OutIR,
		const TMap<const UEdGraphNode*, FString>& NodeIdMap) const;

	/**
	 * Get the node ID from the map, generating one if not found
	 * @param Node The node to get ID for
	 * @param NodeIdMap The ID map
	 * @return The node ID string
	 */
	FString GetNodeId(
		const UEdGraphNode* Node,
		const TMap<const UEdGraphNode*, FString>& NodeIdMap) const;

	/**
	 * Extract common node properties into the IR node
	 * @param Node The source node
	 * @param OutIR The IR node to populate
	 * @param NodeIdMap The node ID map
	 */
	void PopulateCommonProperties(
		const UEdGraphNode* Node,
		FOliveIRNode& OutIR,
		const TMap<const UEdGraphNode*, FString>& NodeIdMap) const;

private:
	/** Pin serializer for handling all pin serialization */
	TSharedPtr<FOlivePinSerializer> PinSerializer;
};
