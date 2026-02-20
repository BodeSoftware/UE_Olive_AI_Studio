// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CommonIR.h"
#include "BlueprintIR.generated.h"

/**
 * Blueprint type enum for IR serialization.
 * Maps to EOliveBlueprintType in editor module.
 */
UENUM(BlueprintType)
enum class EOliveIRBlueprintType : uint8
{
	// Standard K2 types (Tier 1 - full read/write)
	Normal,              // Standard Blueprint (Actor, Pawn, etc.) + AnimNotify, AnimNotifyState, GameplayAbility
	Interface,           // Blueprint Interface
	FunctionLibrary,     // Blueprint Function Library
	MacroLibrary,        // Blueprint Macro Library
	LevelScript,         // Level Blueprint
	ActorComponent,      // Actor Component Blueprint
	EditorUtility,       // Editor Utility Blueprint (Blutility)
	EditorUtilityWidget, // Editor Utility Widget Blueprint

	// Extended systems (Tier 2 - read full, write partial)
	AnimationBlueprint,  // Animation Blueprint (event graph writable, anim graph read-only)
	WidgetBlueprint,     // Widget Blueprint (event graph writable, widget tree read-only)
	ControlRigBlueprint, // Control Rig Blueprint (read-only for Phase 1)

	Unknown
};

/**
 * Compilation status
 */
UENUM(BlueprintType)
enum class EOliveIRCompileStatus : uint8
{
	Unknown,
	UpToDate,
	Dirty,
	Error,
	Warning
};

/**
 * Blueprint capabilities (what features it supports)
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRBlueprintCapabilities
{
	GENERATED_BODY()

	UPROPERTY()
	bool bHasEventGraph = true;

	UPROPERTY()
	bool bHasFunctions = true;

	UPROPERTY()
	bool bHasVariables = true;

	UPROPERTY()
	bool bHasComponents = true;

	UPROPERTY()
	bool bHasMacros = true;

	UPROPERTY()
	bool bHasAnimGraph = false;

	UPROPERTY()
	bool bHasWidgetTree = false;

	UPROPERTY()
	bool bHasStateMachine = false;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRBlueprintCapabilities FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * Interface reference
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRInterfaceRef
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString Path;

	/** Functions required by this interface */
	UPROPERTY()
	TArray<FString> RequiredFunctions;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRInterfaceRef FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * Class reference (for parent class)
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRClassRef
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	/** "cpp" for native C++, "blueprint" for BP parent */
	UPROPERTY()
	FString Source;

	/** Path if it's a Blueprint class */
	UPROPERTY()
	FString Path;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRClassRef FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * Event dispatcher definition
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIREventDispatcher
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	UPROPERTY()
	TArray<FOliveIRPin> Parameters;

	UPROPERTY()
	FString Description;

	UPROPERTY()
	FString Category;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIREventDispatcher FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * Blueprint-level IR
 *
 * This is the top-level representation of a Blueprint asset.
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRBlueprint
{
	GENERATED_BODY()

	/** IR schema version for compatibility checking */
	UPROPERTY()
	FString SchemaVersion = TEXT("1.0");

	/** Blueprint name */
	UPROPERTY()
	FString Name;

	/** Full asset path */
	UPROPERTY()
	FString Path;

	/** Blueprint type */
	UPROPERTY()
	EOliveIRBlueprintType Type = EOliveIRBlueprintType::Normal;

	/** Parent class */
	UPROPERTY()
	FOliveIRClassRef ParentClass;

	/** What this Blueprint can do */
	UPROPERTY()
	FOliveIRBlueprintCapabilities Capabilities;

	/** Implemented interfaces */
	UPROPERTY()
	TArray<FOliveIRInterfaceRef> Interfaces;

	/** Current compilation status */
	UPROPERTY()
	EOliveIRCompileStatus CompileStatus = EOliveIRCompileStatus::Unknown;

	/** Compilation errors/warnings */
	UPROPERTY()
	TArray<FOliveIRMessage> CompileMessages;

	/** Variables defined in this Blueprint */
	UPROPERTY()
	TArray<FOliveIRVariable> Variables;

	/** Component tree (root + children) */
	UPROPERTY()
	TArray<FOliveIRComponent> Components;

	/** Name of the root component */
	UPROPERTY()
	FString RootComponentName;

	/** Event graphs */
	UPROPERTY()
	TArray<FString> EventGraphNames;

	/** User-defined functions */
	UPROPERTY()
	TArray<FString> FunctionNames;

	/** Macros */
	UPROPERTY()
	TArray<FString> MacroNames;

	/** Event dispatchers */
	UPROPERTY()
	TArray<FOliveIREventDispatcher> EventDispatchers;

	/** Whether the asset has unsaved changes */
	UPROPERTY()
	bool bIsDirty = false;

	/** Whether someone is currently editing this in the editor */
	UPROPERTY()
	bool bIsBeingEdited = false;

	/** Full graph data (optional, for detailed read) */
	TArray<FOliveIRGraph> Graphs;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRBlueprint FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * Widget tree node for Widget Blueprints
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRWidgetNode
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString WidgetClass;

	/** Child widgets (not reflected due to recursion) */
	TArray<FOliveIRWidgetNode> Children;

	UPROPERTY()
	TMap<FString, FString> Properties;

	UPROPERTY()
	FString SlotType;  // Canvas slot, horizontal box slot, etc.

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRWidgetNode FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * Animation state for Animation Blueprints
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRAnimState
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	UPROPERTY()
	bool bIsConduit = false;

	UPROPERTY()
	TArray<FString> TransitionsIn;

	UPROPERTY()
	TArray<FString> TransitionsOut;

	/** Animation asset or blend space reference */
	UPROPERTY()
	FString AnimationAsset;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRAnimState FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * Animation state machine
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRAnimStateMachine
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	UPROPERTY()
	TArray<FOliveIRAnimState> States;

	UPROPERTY()
	FString EntryState;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRAnimStateMachine FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};
