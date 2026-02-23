// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "OliveBlueprintWriter.h"

// Forward declarations
class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UK2Node;
class UFunction;
class UClass;
class UScriptStruct;

/**
 * Standard node type names for Blueprint graph nodes.
 * Use these constants when creating nodes via OliveNodeFactory.
 */
namespace OliveNodeTypes
{
	// Control Flow
	const FString Branch = TEXT("Branch");
	const FString Sequence = TEXT("Sequence");
	const FString ForLoop = TEXT("ForLoop");
	const FString ForEachLoop = TEXT("ForEachLoop");
	const FString WhileLoop = TEXT("WhileLoop");
	const FString DoOnce = TEXT("DoOnce");
	const FString FlipFlop = TEXT("FlipFlop");
	const FString Gate = TEXT("Gate");
	const FString Delay = TEXT("Delay");

	// Function/Event
	const FString CallFunction = TEXT("CallFunction");
	const FString CallParentFunction = TEXT("CallParentFunction");
	const FString GetVariable = TEXT("GetVariable");
	const FString SetVariable = TEXT("SetVariable");
	const FString Event = TEXT("Event");
	const FString CustomEvent = TEXT("CustomEvent");

	// Casting/Type
	const FString Cast = TEXT("Cast");
	const FString IsValid = TEXT("IsValid");

	// Object Creation
	const FString SpawnActor = TEXT("SpawnActor");

	// Struct Operations
	const FString MakeStruct = TEXT("MakeStruct");
	const FString BreakStruct = TEXT("BreakStruct");

	// Input
	const FString InputKey = TEXT("InputKey");

	// Utility
	const FString PrintString = TEXT("PrintString");
	const FString Comment = TEXT("Comment");
	const FString Reroute = TEXT("Reroute");
}

/**
 * FOliveNodeFactory
 *
 * Factory class for creating Blueprint graph nodes.
 * Provides a unified interface for creating various node types
 * with proper initialization and positioning.
 *
 * Supports all common K2 node types including:
 * - Control flow (Branch, Sequence, Loops)
 * - Function calls and events
 * - Variable access (Get/Set)
 * - Type casting and validation
 * - Struct operations
 * - Utility nodes (Comments, Reroutes)
 *
 * Usage:
 *   FOliveNodeFactory& Factory = FOliveNodeFactory::Get();
 *   TMap<FString, FString> Props;
 *   Props.Add(TEXT("function_name"), TEXT("PrintString"));
 *   UEdGraphNode* Node = Factory.CreateNode(Blueprint, Graph, OliveNodeTypes::CallFunction, Props, 100, 200);
 */
class OLIVEAIEDITOR_API FOliveNodeFactory
{
public:
	/**
	 * Get the singleton instance
	 * @return Reference to the node factory singleton
	 */
	static FOliveNodeFactory& Get();

	// ============================================================================
	// Node Creation
	// ============================================================================

	/**
	 * Create a Blueprint graph node of the specified type
	 * @param Blueprint The Blueprint containing the graph
	 * @param Graph The graph to add the node to
	 * @param NodeType Type of node to create (use OliveNodeTypes constants)
	 * @param Properties Type-specific properties for node configuration
	 * @param PosX X position in the graph
	 * @param PosY Y position in the graph
	 * @return The created node, or nullptr if creation failed
	 */
	UEdGraphNode* CreateNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const FString& NodeType,
		const TMap<FString, FString>& Properties,
		int32 PosX,
		int32 PosY);

	/**
	 * Check if a node type is supported
	 * @param NodeType The node type to check
	 * @return True if the node type can be created
	 */
	bool IsNodeTypeSupported(const FString& NodeType) const;

	/**
	 * Get all supported node types
	 * @return Array of supported node type names
	 */
	TArray<FString> GetSupportedNodeTypes() const;

	/**
	 * Get required properties for a node type
	 * @param NodeType The node type to query
	 * @return Map of property names to descriptions (empty for unknown types)
	 */
	TMap<FString, FString> GetRequiredProperties(const FString& NodeType) const;

	/**
	 * Validate whether a node type string is recognized.
	 * Checks both the factory's built-in node creator map and
	 * deeper resolution for types that require property-based lookup
	 * (e.g., CallFunction resolves function_name, Event resolves event_name).
	 *
	 * Unlike IsNodeTypeSupported() which only checks the NodeCreators map,
	 * this method also validates that property-dependent types can actually
	 * resolve their targets.
	 *
	 * @param NodeType  The type string to validate (use OliveNodeTypes constants)
	 * @param Properties  The node properties (needed for CallFunction/Event resolution)
	 * @return True if the node type can be created with the given properties
	 */
	bool ValidateNodeType(const FString& NodeType, const TMap<FString, FString>& Properties) const;

	/**
	 * Get the last error message from a failed operation
	 * @return Error message string
	 */
	FString GetLastError() const { return LastError; }

private:
	FOliveNodeFactory();
	~FOliveNodeFactory() = default;

	// Non-copyable
	FOliveNodeFactory(const FOliveNodeFactory&) = delete;
	FOliveNodeFactory& operator=(const FOliveNodeFactory&) = delete;

	// ============================================================================
	// Type-Specific Node Creators
	// ============================================================================

	/**
	 * Create a Call Function node
	 * Required properties: "function_name"
	 * Optional properties: "target_class"
	 */
	UK2Node* CreateCallFunctionNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TMap<FString, FString>& Properties);

	/**
	 * Create a Get Variable node
	 * Required properties: "variable_name"
	 */
	UK2Node* CreateVariableGetNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TMap<FString, FString>& Properties);

	/**
	 * Create a Set Variable node
	 * Required properties: "variable_name"
	 */
	UK2Node* CreateVariableSetNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TMap<FString, FString>& Properties);

	/**
	 * Create an Event node (override event)
	 * Required properties: "event_name"
	 */
	UK2Node* CreateEventNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TMap<FString, FString>& Properties);

	/**
	 * Create a Custom Event node
	 * Required properties: "event_name"
	 */
	UK2Node* CreateCustomEventNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TMap<FString, FString>& Properties);

	/**
	 * Create a Branch (If) node
	 * No required properties
	 */
	UK2Node* CreateBranchNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TMap<FString, FString>& Properties);

	/**
	 * Create a Sequence node
	 * Optional properties: "num_outputs" (default 2)
	 */
	UK2Node* CreateSequenceNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TMap<FString, FString>& Properties);

	/**
	 * Create a Cast node
	 * Required properties: "target_class"
	 */
	UK2Node* CreateCastNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TMap<FString, FString>& Properties);

	/**
	 * Create a Spawn Actor node
	 * Required properties: "actor_class"
	 */
	UK2Node* CreateSpawnActorNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TMap<FString, FString>& Properties);

	/**
	 * Create a Make Struct node
	 * Required properties: "struct_type"
	 */
	UK2Node* CreateMakeStructNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TMap<FString, FString>& Properties);

	/**
	 * Create a Break Struct node
	 * Required properties: "struct_type"
	 */
	UK2Node* CreateBreakStructNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TMap<FString, FString>& Properties);

	/**
	 * Create a For Loop node
	 * No required properties
	 */
	UK2Node* CreateForLoopNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TMap<FString, FString>& Properties);

	/**
	 * Create a For Each Loop node
	 * No required properties
	 */
	UK2Node* CreateForEachLoopNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TMap<FString, FString>& Properties);

	/**
	 * Create a Delay node
	 * No required properties
	 */
	UK2Node* CreateDelayNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TMap<FString, FString>& Properties);

	/**
	 * Create an Is Valid node
	 * No required properties
	 */
	UK2Node* CreateIsValidNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TMap<FString, FString>& Properties);

	/**
	 * Create a Print String node
	 * No required properties
	 */
	UK2Node* CreatePrintStringNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TMap<FString, FString>& Properties);

	/**
	 * Create a Comment node
	 * Optional properties: "text", "width", "height"
	 */
	UEdGraphNode* CreateCommentNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TMap<FString, FString>& Properties);

	/**
	 * Create a Reroute node
	 * No required properties
	 */
	UK2Node* CreateRerouteNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TMap<FString, FString>& Properties);

	/**
	 * Create an InputKey node (keyboard/gamepad key binding)
	 * Required properties: "key" (e.g., "E", "SpaceBar", "Gamepad_FaceButton_Bottom")
	 */
	UK2Node* CreateInputKeyNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TMap<FString, FString>& Properties);

	// ============================================================================
	// Helper Methods
	// ============================================================================

	/**
	 * Set the position of a node in the graph
	 * @param Node The node to position
	 * @param PosX X position
	 * @param PosY Y position
	 */
	void SetNodePosition(UEdGraphNode* Node, int32 PosX, int32 PosY);

	/**
	 * Find a class by name or path
	 * @param ClassName Class name (with or without prefix) or full path
	 * @return The class if found, nullptr otherwise
	 */
	UClass* FindClass(const FString& ClassName);

	/**
	 * Find a function by name, optionally within a specific class
	 * @param FunctionName Name of the function to find
	 * @param ClassName Optional class to search in
	 * @return The function if found, nullptr otherwise
	 */
	UFunction* FindFunction(const FString& FunctionName, const FString& ClassName = TEXT(""));

	/**
	 * Find a struct by name or path
	 * @param StructName Struct name or full path
	 * @return The struct if found, nullptr otherwise
	 */
	UScriptStruct* FindStruct(const FString& StructName);

	/**
	 * Initialize the node creator function map
	 */
	void InitializeNodeCreators();

	// ============================================================================
	// Members
	// ============================================================================

	/** Function type for node creators */
	using FNodeCreator = TFunction<UEdGraphNode*(UBlueprint*, UEdGraph*, const TMap<FString, FString>&)>;

	/** Map of node type names to creator functions */
	TMap<FString, FNodeCreator> NodeCreators;

	/** Map of node type names to required property descriptions */
	TMap<FString, TMap<FString, FString>> RequiredPropertiesMap;

	/** Last error message */
	FString LastError;
};
