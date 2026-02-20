// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "OliveIRTypes.h"
#include "CommonIR.generated.h"

/**
 * Node category for AI understanding and catalog organization.
 * Used to classify Blueprint nodes by their purpose and behavior.
 */
UENUM(BlueprintType)
enum class EOliveIRNodeCategory : uint8
{
	// Events
	Event,              // BeginPlay, Tick, etc.
	CustomEvent,        // User-defined custom events
	FunctionEntry,      // Entry point of a function graph
	FunctionResult,     // Return node of a function graph

	// Flow Control
	Branch,             // If/else branching
	Sequence,           // Execute multiple outputs in order
	ForLoop,            // For loop with index
	ForEachLoop,        // Iterate over array/collection
	WhileLoop,          // While loop
	Switch,             // Switch on value (enum, int, string, name)
	Select,             // Select value based on condition
	Gate,               // Flow control gate (open/close)
	DoOnce,             // Execute only once
	FlipFlop,           // Alternate between two outputs
	Delay,              // Delayed execution

	// Calls
	CallFunction,       // Call a function
	CallParentFunction, // Call parent class implementation

	// Variables
	VariableGet,        // Get variable value
	VariableSet,        // Set variable value
	LocalVariable,      // Local variable in function

	// Object
	Cast,               // Cast to type
	IsValid,            // Check if object is valid
	SpawnActor,         // Spawn an actor

	// Struct
	MakeStruct,         // Construct a struct
	BreakStruct,        // Decompose a struct
	SetMember,          // Set struct member

	// Array/Container
	ArrayOperation,     // Array operations (add, remove, find, etc.)

	// Delegate
	CreateDelegate,     // Create a delegate
	BindDelegate,       // Bind a delegate
	CallDelegate,       // Call/broadcast a delegate

	// Macro
	MacroInstance,      // Instance of a macro

	// Utility
	Comment,            // Comment node
	Reroute,            // Reroute/knot node
	Timeline,           // Timeline node
	Literal,            // Literal value (make literal int, float, etc.)

	// Math (pure)
	MathExpression,     // Math expression node
	Comparison,         // Comparison operations (==, !=, <, >, etc.)
	BooleanOp,          // Boolean operations (AND, OR, NOT, etc.)

	Unknown             // Unknown or unclassified node type
};

/**
 * Asset reference in IR format
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRAssetRef
{
	GENERATED_BODY()

	/** Asset name without path */
	UPROPERTY()
	FString Name;

	/** Full asset path (e.g., /Game/Blueprints/BP_Player) */
	UPROPERTY()
	FString Path;

	/** Asset class name (e.g., Blueprint, BehaviorTree) */
	UPROPERTY()
	FString AssetClass;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRAssetRef FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * Pin connection reference
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRPinRef
{
	GENERATED_BODY()

	/** Node ID that owns this pin */
	UPROPERTY()
	FString NodeId;

	/** Pin name */
	UPROPERTY()
	FString PinName;

	/** Format: "node_id.pin_name" */
	FString ToConnectionString() const;
	static FOliveIRPinRef FromConnectionString(const FString& ConnectionString);

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRPinRef FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * A pin on a node
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRPin
{
	GENERATED_BODY()

	/** Pin name (unique within the node) */
	UPROPERTY()
	FString Name;

	/** Display name for UI */
	UPROPERTY()
	FString DisplayName;

	/** Pin type */
	UPROPERTY()
	FOliveIRType Type;

	/** Whether this is an input or output pin */
	UPROPERTY()
	bool bIsInput = true;

	/** Whether this is an exec (flow control) pin */
	UPROPERTY()
	bool bIsExec = false;

	/** Default value if not connected (for input pins) */
	UPROPERTY()
	FString DefaultValue;

	/** Connection (if connected) */
	UPROPERTY()
	FString Connection;  // Format: "node_id.pin_name"

	/** Multiple connections (for output data pins) */
	UPROPERTY()
	TArray<FString> Connections;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRPin FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * Function parameter definition.
 * Used for function inputs/outputs and event parameters.
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRFunctionParam
{
	GENERATED_BODY()

	/** Parameter name */
	UPROPERTY()
	FString Name;

	/** Parameter type */
	UPROPERTY()
	FOliveIRType Type;

	/** Default value as string (empty if no default) */
	UPROPERTY()
	FString DefaultValue;

	/** Whether this is an output parameter (passed by reference for output) */
	UPROPERTY()
	bool bIsOutParam = false;

	/** Whether this parameter is passed by reference */
	UPROPERTY()
	bool bIsReference = false;

	/**
	 * Convert to JSON representation
	 * @return JSON object containing parameter definition
	 */
	TSharedPtr<FJsonObject> ToJson() const;

	/**
	 * Parse from JSON representation
	 * @param JsonObject The JSON to parse
	 * @return Parsed function parameter
	 */
	static FOliveIRFunctionParam FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * Function signature definition.
 * Contains all metadata about a function including inputs, outputs, and flags.
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRFunctionSignature
{
	GENERATED_BODY()

	/** Function name */
	UPROPERTY()
	FString Name;

	/** Input parameters */
	UPROPERTY()
	TArray<FOliveIRFunctionParam> Inputs;

	/** Output parameters (return values) */
	UPROPERTY()
	TArray<FOliveIRFunctionParam> Outputs;

	/** Whether this is a static function */
	UPROPERTY()
	bool bIsStatic = false;

	/** Whether this is a pure function (no side effects, no exec pins) */
	UPROPERTY()
	bool bIsPure = false;

	/** Whether this is a const function */
	UPROPERTY()
	bool bIsConst = false;

	/** Whether this function is publicly accessible */
	UPROPERTY()
	bool bIsPublic = true;

	/** Whether this function can be called in editor (CallInEditor) */
	UPROPERTY()
	bool bCallInEditor = false;

	/** Whether this function overrides a parent function */
	UPROPERTY()
	bool bIsOverride = false;

	/** Whether this is an event (has special execution semantics) */
	UPROPERTY()
	bool bIsEvent = false;

	/** Category for organization in the editor */
	UPROPERTY()
	FString Category;

	/** Description/tooltip for the function */
	UPROPERTY()
	FString Description;

	/** Keywords for search */
	UPROPERTY()
	FString Keywords;

	/** Where this function is defined: "self" or parent class name if inherited */
	UPROPERTY()
	FString DefinedIn;

	/**
	 * Convert to JSON representation
	 * @return JSON object containing function signature
	 */
	TSharedPtr<FJsonObject> ToJson() const;

	/**
	 * Parse from JSON representation
	 * @param JsonObject The JSON to parse
	 * @return Parsed function signature
	 */
	static FOliveIRFunctionSignature FromJson(const TSharedPtr<FJsonObject>& JsonObject);

	/**
	 * Get the number of input parameters
	 */
	int32 GetInputCount() const { return Inputs.Num(); }

	/**
	 * Get the number of output parameters
	 */
	int32 GetOutputCount() const { return Outputs.Num(); }

	/**
	 * Check if function has return value
	 */
	bool HasReturnValue() const { return Outputs.Num() > 0; }
};

/**
 * A node in a graph
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRNode
{
	GENERATED_BODY()

	/** Simple node ID (e.g., "node_1", "node_2") */
	UPROPERTY()
	FString Id;

	/** Node type (e.g., "CallFunction", "Branch", "VariableGet") */
	UPROPERTY()
	FString Type;

	/** Human-readable title */
	UPROPERTY()
	FString Title;

	/** For CallFunction: the function name */
	UPROPERTY()
	FString FunctionName;

	/** For CallFunction: the owning class */
	UPROPERTY()
	FString OwningClass;

	/** For Variable nodes: the variable name */
	UPROPERTY()
	FString VariableName;

	/** Node category as string (for catalog/filtering) */
	UPROPERTY()
	FString Category;

	/** Node category enum (for programmatic use) */
	UPROPERTY()
	EOliveIRNodeCategory NodeCategory = EOliveIRNodeCategory::Unknown;

	/** Input pins */
	UPROPERTY()
	TArray<FOliveIRPin> InputPins;

	/** Output pins */
	UPROPERTY()
	TArray<FOliveIRPin> OutputPins;

	/** Additional node-specific properties */
	UPROPERTY()
	TMap<FString, FString> Properties;

	/** Comments/notes on this node */
	UPROPERTY()
	FString Comment;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRNode FromJson(const TSharedPtr<FJsonObject>& JsonObject);

	/**
	 * Get node category as string
	 * @return Category enum value as string
	 */
	static FString NodeCategoryToString(EOliveIRNodeCategory InCategory);

	/**
	 * Parse node category from string
	 * @param InString Category string
	 * @return Parsed category enum value
	 */
	static EOliveIRNodeCategory StringToNodeCategory(const FString& InString);
};

/**
 * A graph (function, event graph, macro, etc.)
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRGraph
{
	GENERATED_BODY()

	/** Graph name */
	UPROPERTY()
	FString Name;

	/** Graph type: "EventGraph", "Function", "Macro", "AnimGraph", etc. */
	UPROPERTY()
	FString GraphType;

	/** For functions: input parameters */
	UPROPERTY()
	TArray<FOliveIRPin> Inputs;

	/** For functions: output parameters */
	UPROPERTY()
	TArray<FOliveIRPin> Outputs;

	/** Access level: "public", "protected", "private" */
	UPROPERTY()
	FString Access;

	/** Whether this is a pure function (no side effects) */
	UPROPERTY()
	bool bIsPure = false;

	/** Whether this is a static function */
	UPROPERTY()
	bool bIsStatic = false;

	/** Whether this is a const function */
	UPROPERTY()
	bool bIsConst = false;

	/** All nodes in this graph */
	UPROPERTY()
	TArray<FOliveIRNode> Nodes;

	/** Description/tooltip */
	UPROPERTY()
	FString Description;

	/** Category for organization */
	UPROPERTY()
	FString Category;

	/** Keywords for search */
	UPROPERTY()
	TArray<FString> Keywords;

	/** Total number of nodes in this graph (for statistics) */
	UPROPERTY()
	int32 NodeCount = 0;

	/** Total number of pin connections in this graph (for statistics) */
	UPROPERTY()
	int32 ConnectionCount = 0;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRGraph FromJson(const TSharedPtr<FJsonObject>& JsonObject);

	/**
	 * Calculate and update statistics (NodeCount, ConnectionCount)
	 */
	void UpdateStatistics();
};

/**
 * A variable definition
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRVariable
{
	GENERATED_BODY()

	/** Variable name */
	UPROPERTY()
	FString Name;

	/** Variable type */
	UPROPERTY()
	FOliveIRType Type;

	/** Default value (as string) */
	UPROPERTY()
	FString DefaultValue;

	/** Category for organization */
	UPROPERTY()
	FString Category;

	/** Tooltip description */
	UPROPERTY()
	FString Description;

	/** Where this variable is defined: "self", class name for inherited */
	UPROPERTY()
	FString DefinedIn;

	/** Property flags */
	UPROPERTY()
	bool bBlueprintReadWrite = true;

	UPROPERTY()
	bool bExposeOnSpawn = false;

	UPROPERTY()
	bool bReplicated = false;

	UPROPERTY()
	bool bSaveGame = false;

	UPROPERTY()
	bool bEditAnywhere = true;

	UPROPERTY()
	bool bBlueprintVisible = true;

	/** Replication condition if replicated */
	UPROPERTY()
	FString ReplicationCondition;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRVariable FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * A component in the component hierarchy
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRComponent
{
	GENERATED_BODY()

	/** Component variable name */
	UPROPERTY()
	FString Name;

	/** Component class name (e.g., UStaticMeshComponent) */
	UPROPERTY()
	FString ComponentClass;

	/** Whether this is the root component */
	UPROPERTY()
	bool bIsRoot = false;

	/** Child components (not reflected due to recursion) */
	TArray<FOliveIRComponent> Children;

	/** Component properties that were modified from defaults */
	UPROPERTY()
	TMap<FString, FString> Properties;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRComponent FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};
