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
 * How a function was matched during FindFunction() resolution.
 * Used by the resolver to determine if a UK2Node_Message (interface call)
 * should be created instead of a regular UK2Node_CallFunction.
 */
enum class EOliveFunctionMatchMethod : uint8
{
	/** Not yet matched or match failed */
	None,
	/** Direct name match on a class (exact or K2_ prefix) */
	ExactName,
	/** Matched via the alias map */
	AliasMap,
	/** Found in the Blueprint's GeneratedClass */
	GeneratedClass,
	/** Found via Blueprint FunctionGraphs + SkeletonGeneratedClass */
	FunctionGraph,
	/** Found in a parent class in the hierarchy */
	ParentClassSearch,
	/** Found on an SCS component class */
	ComponentClassSearch,
	/** Found on an implemented interface class -- requires UK2Node_Message */
	InterfaceSearch,
	/** Found in a common library class (KismetSystemLibrary, GameplayStatics, etc.) */
	LibrarySearch,
	/** Found via universal search across all UBlueprintFunctionLibrary subclasses */
	UniversalLibrarySearch,
	/** Found via K2_ prefix fuzzy matching across previously searched classes */
	FuzzyK2Match,
};

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
	const FString EnhancedInputAction = TEXT("EnhancedInputAction");

	// Utility
	const FString PrintString = TEXT("PrintString");
	const FString Comment = TEXT("Comment");
	const FString Reroute = TEXT("Reroute");

	// Delegate
	const FString CallDelegate = TEXT("CallDelegate");
	const FString BindDelegate = TEXT("BindDelegate");

	// Component Events
	const FString ComponentBoundEvent = TEXT("ComponentBoundEvent");

	// Timeline (requires blueprint.create_timeline -- cannot be created via add_node)
	const FString Timeline = TEXT("Timeline");

	// Function Parameter (virtual -- maps to existing FunctionEntry/FunctionResult nodes)
	const FString FunctionInput = TEXT("FunctionInput");
	const FString FunctionOutput = TEXT("FunctionOutput");
}

/**
 * Extended result from FindFunction with search history for error messages.
 * When FindFunction fails, this struct provides a human-readable trail of
 * every location that was searched, enabling actionable error messages.
 */
struct FOliveFunctionSearchResult
{
	/** The resolved function, or nullptr if not found */
	UFunction* Function = nullptr;

	/** How the function was matched (None if not found) */
	EOliveFunctionMatchMethod MatchMethod = EOliveFunctionMatchMethod::None;

	/** Name of the class where the function was found (empty if not found) */
	FString MatchedClassName;

	/** Human-readable descriptions of every location searched (populated only on failure) */
	TArray<FString> SearchedLocations;

	/** Whether the function was found */
	bool IsValid() const { return Function != nullptr; }

	/**
	 * Build a comma-separated string of all searched locations for error messages.
	 * @return Joined string of SearchedLocations
	 */
	FString BuildSearchedLocationsString() const
	{
		return FString::Join(SearchedLocations, TEXT(", "));
	}
};

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
	 * @param Blueprint  Optional Blueprint for searching Blueprint-defined functions (may be nullptr for pre-checks)
	 * @return True if the node type can be created with the given properties
	 */
	bool ValidateNodeType(const FString& NodeType, const TMap<FString, FString>& Properties, UBlueprint* Blueprint = nullptr) const;

	/**
	 * Create a node by resolving the type string as a UK2Node subclass name.
	 * This is the universal fallback -- used when the type is not in the curated
	 * NodeCreators map. Properties are set via reflection BEFORE AllocateDefaultPins
	 * (critical for nodes whose pins depend on property values).
	 *
	 * @param Blueprint The Blueprint containing the graph
	 * @param Graph The graph to add the node to
	 * @param ClassName The UK2Node subclass name (e.g., "K2Node_ComponentBoundEvent")
	 * @param Properties UPROPERTY field name -> string value pairs to set via reflection
	 * @return The created node, or nullptr (with LastError set)
	 */
	UEdGraphNode* CreateNodeByClass(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const FString& ClassName,
		const TMap<FString, FString>& Properties);

	/**
	 * Get the function name alias map.
	 * Maps common AI-provided names to actual UE function names.
	 * Used as the first step in FindFunction() resolution.
	 * @return Static alias map (case-insensitive lookup recommended)
	 */
	static const TMap<FString, FString>& GetAliasMap();

	/**
	 * Find a function by name, optionally within a specific class.
	 * Searches (in order): alias map -> specified class -> Blueprint's GeneratedClass ->
	 * Blueprint's FunctionGraphs via SkeletonGeneratedClass (catches uncompiled user functions) ->
	 * Blueprint parent class hierarchy -> Blueprint SCS component classes ->
	 * Blueprint implemented interfaces -> common library classes.
	 * Each class is tried with exact name first, then K2_ prefix variant.
	 * @param FunctionName Name of the function to find (may be an alias or approximate name)
	 * @param ClassName Optional class to search in first
	 * @param Blueprint Optional Blueprint for class hierarchy, SCS, FunctionGraphs, interfaces, and GeneratedClass search
	 * @param OutMatchMethod Optional output: how the function was matched (for interface call detection)
	 * @return The function if found, nullptr otherwise
	 */
	UFunction* FindFunction(const FString& FunctionName, const FString& ClassName = TEXT(""), UBlueprint* Blueprint = nullptr, EOliveFunctionMatchMethod* OutMatchMethod = nullptr, bool bSkipAliasMap = false);

	/**
	 * Extended FindFunction that collects search history for error messages.
	 * Returns the same result as FindFunction, plus a list of every location
	 * searched (populated only when the function is NOT found).
	 * @param FunctionName Name of the function to find (may be an alias or approximate name)
	 * @param ClassName Optional class to search in first
	 * @param Blueprint Optional Blueprint for class hierarchy, SCS, FunctionGraphs, interfaces, and GeneratedClass search
	 * @return FOliveFunctionSearchResult with Function, MatchMethod, MatchedClassName, and SearchedLocations
	 */
	FOliveFunctionSearchResult FindFunctionEx(
		const FString& FunctionName,
		const FString& ClassName = TEXT(""),
		UBlueprint* Blueprint = nullptr,
		bool bSkipAliasMap = false);

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

	/**
	 * Create a Call Delegate (broadcast event dispatcher) node.
	 * Required properties: "delegate_name" -- the name of the multicast delegate
	 * property on the Blueprint (e.g., "OnFired").
	 * Searches SkeletonGeneratedClass (fallback GeneratedClass) for an
	 * FMulticastDelegateProperty matching the name, then creates a
	 * UK2Node_CallDelegate with SetFromProperty + AllocateDefaultPins.
	 *
	 * @param Blueprint The Blueprint containing the graph
	 * @param Graph The target graph
	 * @param Properties Must contain "delegate_name"
	 * @return The created CallDelegate node, or nullptr if the delegate was not found
	 */
	UK2Node* CreateCallDelegateNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TMap<FString, FString>& Properties);

	/**
	 * Create a Bind Delegate (bind event to dispatcher) node.
	 * Required properties: "delegate_name" -- the name of the multicast delegate
	 * property on the Blueprint (e.g., "OnFired").
	 * Creates a UK2Node_AddDelegate that binds a custom event to the dispatcher.
	 * Searches SkeletonGeneratedClass (fallback GeneratedClass) for an
	 * FMulticastDelegateProperty matching the name, then creates a
	 * UK2Node_AddDelegate with SetFromProperty + AllocateDefaultPins.
	 *
	 * @param Blueprint The Blueprint containing the graph
	 * @param Graph The target graph
	 * @param Properties Must contain "delegate_name"
	 * @return The created AddDelegate node, or nullptr if the delegate was not found
	 */
	UK2Node* CreateBindDelegateNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TMap<FString, FString>& Properties);

	/**
	 * Create a UK2Node_ComponentBoundEvent for component delegate events
	 * (OnComponentBeginOverlap, OnComponentHit, OnComponentEndOverlap, etc.).
	 *
	 * Required properties:
	 *   "delegate_name"   -- the FMulticastDelegateProperty name on the component
	 *                        class (e.g., "OnComponentBeginOverlap")
	 *   "component_name"  -- the SCS variable name of the component in the Blueprint
	 *                        (e.g., "CollisionComp", "MeshComp")
	 *
	 * Searches the Blueprint's SCS for the matching component, finds the
	 * FObjectProperty on the GeneratedClass, then uses
	 * InitializeComponentBoundEventParams() to configure the node.
	 *
	 * @param Blueprint The Blueprint containing the graph
	 * @param Graph The target graph
	 * @param Properties Must contain "delegate_name" and "component_name"
	 * @return The created ComponentBoundEvent node, or nullptr on failure
	 */
	UK2Node* CreateComponentBoundEventNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TMap<FString, FString>& Properties);

	/**
	 * Create an Enhanced Input Action event node (UK2Node_EnhancedInputAction).
	 * Required properties: "input_action_name" -- the name of the UInputAction asset
	 * (e.g., "IA_Interact", "IA_Jump", "IA_Fire").
	 * Searches the project asset registry for a matching UInputAction data asset,
	 * creates the K2Node, sets the InputAction property, then allocates pins
	 * (Triggered, Started, Completed, Canceled, Ongoing, plus ActionValue).
	 *
	 * @param Blueprint The Blueprint containing the graph
	 * @param Graph The target graph
	 * @param Properties Must contain "input_action_name"
	 * @return The created EnhancedInputAction node, or nullptr if the asset was not found
	 */
	UK2Node* CreateEnhancedInputActionNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TMap<FString, FString>& Properties);

	/**
	 * Create a macro instance node from the StandardMacros library
	 * @param Blueprint The Blueprint containing the graph
	 * @param Graph The target graph
	 * @param MacroName Exact name of the macro in StandardMacros (e.g., "WhileLoop", "DoOnce")
	 * @return The created macro instance node, or nullptr if creation failed
	 */
	UK2Node* CreateMacroInstanceNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const FString& MacroName);

	// ============================================================================
	// Universal Node Creation Helpers
	// ============================================================================

	/**
	 * Resolve a UK2Node subclass by name. Searches across multiple engine module packages.
	 * Multi-strategy lookup: FindFirstObject -> prefix variants -> U-prefix strip -> StaticLoadClass.
	 * @param ClassName Short class name (e.g., "K2Node_ComponentBoundEvent") or prefixed name
	 * @return The UClass if found and is a subclass of UK2Node, nullptr otherwise
	 */
	UClass* FindK2NodeClass(const FString& ClassName) const;

	/**
	 * Set UPROPERTY fields on a node via reflection.
	 * Uses the same type-dispatch pattern as OliveGraphWriter::SetNodeProperty.
	 * Type-specific fast paths for bool, int, float, double, string, name, text, object.
	 * Generic ImportText_Direct fallback for enums, structs, FKey, etc.
	 *
	 * @param Node The node to set properties on
	 * @param Properties Key-value pairs (UPROPERTY name -> string value)
	 * @param OutSetProperties Names of properties that were successfully set
	 * @param OutSkippedProperties Names of properties that could not be set (with reason)
	 * @return Number of properties successfully set
	 */
	int32 SetNodePropertiesViaReflection(
		UEdGraphNode* Node,
		const TMap<FString, FString>& Properties,
		TArray<FString>& OutSetProperties,
		TMap<FString, FString>& OutSkippedProperties);

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

	/** Fuzzy suggestions from the most recent failed FindFunction call.
	 *  Each entry is "FunctionName (ClassName)". Populated on failure, cleared on success. */
	TArray<FString> LastFuzzySuggestions;
};
