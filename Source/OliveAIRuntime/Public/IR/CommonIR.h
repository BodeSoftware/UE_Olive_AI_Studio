// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "OliveIRTypes.h"
#include "CommonIR.generated.h"

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

	/** Node category (for catalog/filtering) */
	UPROPERTY()
	FString Category;

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

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRGraph FromJson(const TSharedPtr<FJsonObject>& JsonObject);
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
