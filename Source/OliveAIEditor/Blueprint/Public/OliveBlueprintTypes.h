// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IR/BlueprintIR.h"
#include "Services/OliveValidationEngine.h"
#include "OliveBlueprintTypes.generated.h"

// Forward declarations
class UBlueprint;
class UAnimBlueprint;
class UWidgetBlueprint;
class UEditorUtilityWidgetBlueprint;

/**
 * Extended Blueprint type enum with more specific types.
 * More granular than the IR version for editor-side type detection.
 */
UENUM()
enum class EOliveBlueprintType : uint8
{
	// Standard K2 types
	Normal,              // Regular Blueprint (Actor, Pawn, etc.)
	Interface,           // Blueprint Interface
	FunctionLibrary,     // Blueprint Function Library
	MacroLibrary,        // Blueprint Macro Library
	ActorComponent,      // Actor Component Blueprint
	AnimNotify,          // Anim Notify Blueprint
	AnimNotifyState,     // Anim Notify State Blueprint
	EditorUtility,       // Editor Utility Blueprint (Blutility)
	EditorUtilityWidget, // Editor Utility Widget Blueprint (separate from EditorUtility)
	GameplayAbility,     // Gameplay Ability Blueprint (GAS)

	// Extended systems
	AnimationBlueprint,  // Animation Blueprint (Anim Instance)
	WidgetBlueprint,     // Widget Blueprint (UMG)
	ControlRigBlueprint, // Control Rig Blueprint

	// Level
	LevelScript,         // Level Blueprint

	Unknown              // Unrecognized Blueprint type
};

/**
 * Blueprint capabilities - what features a Blueprint type supports.
 * Used to validate operations before attempting them.
 */
USTRUCT()
struct OLIVEAIEDITOR_API FOliveBlueprintCapabilities
{
	GENERATED_BODY()

	/** Whether this Blueprint type has an event graph */
	bool bHasEventGraph = true;

	/** Whether this Blueprint type can have user-defined functions */
	bool bHasFunctions = true;

	/** Whether this Blueprint type can have member variables */
	bool bHasVariables = true;

	/** Whether this Blueprint type can have components (SCS) */
	bool bHasComponents = true;

	/** Whether this Blueprint type can have macros */
	bool bHasMacros = true;

	/** Whether functions can have local variables */
	bool bCanHaveLocalVariables = true;

	/** Whether all functions must be static (e.g., Function Libraries) */
	bool bFunctionsMustBeStatic = false;

	/** Whether all functions must be public (e.g., Interfaces) */
	bool bFunctionsMustBePublic = false;

	/** Whether this Blueprint has an animation graph */
	bool bHasAnimGraph = false;

	/** Whether this Blueprint has a widget tree (UMG) */
	bool bHasWidgetTree = false;

	/** Whether this Blueprint has state machines (e.g., Animation BPs) */
	bool bHasStateMachines = false;

	/** Whether this Component BP inherits from SceneComponent (can have SCS/children) */
	bool bIsSceneComponent = false;

	/** Whether event graph can be written (Tier 2 partial support) */
	bool bCanWriteEventGraph = true;

	/** Whether anim graph can be written (Tier 2 partial support) */
	bool bCanWriteAnimGraph = false;

	/** Whether state machines can be written (Tier 2 partial support) */
	bool bCanWriteStateMachine = false;

	/** Whether widget tree can be written (Tier 2 partial support) */
	bool bCanWriteWidgetTree = false;

	/** Whether this Blueprint is editor-only */
	bool bEditorOnly = false;
};

/**
 * Detects Blueprint types and provides capability information.
 * Used to determine what operations are valid for a given Blueprint.
 */
class OLIVEAIEDITOR_API FOliveBlueprintTypeDetector
{
public:
	/**
	 * Detect the type of a Blueprint
	 * @param Blueprint The Blueprint to analyze
	 * @return The detected type
	 */
	static EOliveBlueprintType DetectType(const UBlueprint* Blueprint);

	/**
	 * Get capabilities for a Blueprint type
	 * @param Type The Blueprint type
	 * @return Capabilities struct describing what the type supports
	 */
	static FOliveBlueprintCapabilities GetCapabilities(EOliveBlueprintType Type);

	/**
	 * Get capabilities directly from a Blueprint
	 * @param Blueprint The Blueprint to analyze
	 * @return Capabilities struct describing what the Blueprint supports
	 */
	static FOliveBlueprintCapabilities GetCapabilities(const UBlueprint* Blueprint);

	/**
	 * Convert type enum to display string
	 * @param Type The type to convert
	 * @return Human-readable type name
	 */
	static FString TypeToString(EOliveBlueprintType Type);

	/**
	 * Convert IR enum to internal enum
	 * @param IRType The IR Blueprint type
	 * @return Equivalent internal type
	 */
	static EOliveBlueprintType FromIRType(EOliveIRBlueprintType IRType);

	/**
	 * Convert internal enum to IR enum
	 * @param Type The internal Blueprint type
	 * @return Equivalent IR type
	 */
	static EOliveIRBlueprintType ToIRType(EOliveBlueprintType Type);

	// ============================================================================
	// Capability Queries
	// ============================================================================

	/**
	 * Check if a Blueprint type can have variables added
	 * @param Type The Blueprint type
	 * @return True if variables can be added
	 */
	static bool CanAddVariable(EOliveBlueprintType Type);

	/**
	 * Check if a Blueprint type can have components added
	 * @param Type The Blueprint type
	 * @return True if components can be added
	 */
	static bool CanAddComponent(EOliveBlueprintType Type);

	/**
	 * Check if a Blueprint type can have an event graph
	 * @param Type The Blueprint type
	 * @return True if event graph is supported
	 */
	static bool CanHaveEventGraph(EOliveBlueprintType Type);

	/**
	 * Check if a Blueprint type can have a function with given attributes
	 * @param Type The Blueprint type
	 * @param bStatic Whether the function is static
	 * @param bPublic Whether the function is public
	 * @return True if such a function is allowed
	 */
	static bool CanHaveFunction(EOliveBlueprintType Type, bool bStatic, bool bPublic);

	/**
	 * Check if a Blueprint type can have macros
	 * @param Type The Blueprint type
	 * @return True if macros are supported
	 */
	static bool CanHaveMacros(EOliveBlueprintType Type);
};

/**
 * Validates operations against Blueprint type constraints.
 * Returns structured validation results with suggestions.
 */
class OLIVEAIEDITOR_API FOliveBlueprintConstraints
{
public:
	/**
	 * Validate adding a variable to a Blueprint
	 * @param Type The Blueprint type
	 * @param VarName The proposed variable name
	 * @return Validation result with errors/warnings if invalid
	 */
	static FOliveValidationResult ValidateAddVariable(
		EOliveBlueprintType Type,
		const FString& VarName);

	/**
	 * Validate adding a component to a Blueprint
	 * @param Type The Blueprint type
	 * @param ComponentClass The component class name
	 * @return Validation result with errors/warnings if invalid
	 */
	static FOliveValidationResult ValidateAddComponent(
		EOliveBlueprintType Type,
		const FString& ComponentClass);

	/**
	 * Validate adding a function to a Blueprint
	 * @param Type The Blueprint type
	 * @param bIsStatic Whether the function is static
	 * @param bIsPublic Whether the function is public
	 * @return Validation result with errors/warnings if invalid
	 */
	static FOliveValidationResult ValidateAddFunction(
		EOliveBlueprintType Type,
		bool bIsStatic,
		bool bIsPublic);

	/**
	 * Validate adding an event graph to a Blueprint
	 * @param Type The Blueprint type
	 * @return Validation result with errors/warnings if invalid
	 */
	static FOliveValidationResult ValidateAddEventGraph(
		EOliveBlueprintType Type);

	/**
	 * Validate adding a child component to an Actor Component Blueprint
	 * @param Blueprint The ActorComponent Blueprint
	 * @param ComponentClass The child component class name
	 * @return Validation result with errors if the Blueprint is not a SceneComponent
	 */
	static FOliveValidationResult ValidateAddComponentToComponent(
		const UBlueprint* Blueprint,
		const FString& ComponentClass);

	/**
	 * Get constraint error message for a specific constraint ID
	 * @param ConstraintId The constraint identifier
	 * @return Human-readable error message
	 */
	static FString GetConstraintMessage(const FString& ConstraintId);

	/**
	 * Get suggestion for how to resolve a constraint violation
	 * @param ConstraintId The constraint identifier
	 * @return Suggestion text
	 */
	static FString GetConstraintSuggestion(const FString& ConstraintId);
};

/**
 * Constraint error identifiers.
 * Used for consistent error codes across the system.
 */
namespace OliveBPConstraints
{
	/** Interface Blueprints cannot have variables */
	const FString InterfaceNoVariables = TEXT("BP_CONSTRAINT_INTERFACE_NO_VARS");

	/** Interface Blueprints cannot have components */
	const FString InterfaceNoComponents = TEXT("BP_CONSTRAINT_INTERFACE_NO_COMPONENTS");

	/** Interface Blueprints cannot have event graphs */
	const FString InterfaceNoEventGraph = TEXT("BP_CONSTRAINT_INTERFACE_NO_EVENT_GRAPH");

	/** Function Library Blueprints cannot have event graphs */
	const FString FunctionLibraryNoEventGraph = TEXT("BP_CONSTRAINT_FUNCLIB_NO_EVENT_GRAPH");

	/** Function Library functions must be static */
	const FString FunctionLibraryMustBeStatic = TEXT("BP_CONSTRAINT_FUNCLIB_STATIC");

	/** Function Library Blueprints cannot have variables */
	const FString FunctionLibraryNoVariables = TEXT("BP_CONSTRAINT_FUNCLIB_NO_VARS");

	/** Function Library Blueprints cannot have components */
	const FString FunctionLibraryNoComponents = TEXT("BP_CONSTRAINT_FUNCLIB_NO_COMPONENTS");

	/** Macro Library Blueprints can only have macros */
	const FString MacroLibraryOnlyMacros = TEXT("BP_CONSTRAINT_MACROLIB_ONLY_MACROS");

	/** Animation Blueprints cannot have components */
	const FString AnimBPNoComponents = TEXT("BP_CONSTRAINT_ANIMBP_NO_COMPONENTS");

	/** Level Script Blueprints cannot have components */
	const FString LevelScriptNoComponents = TEXT("BP_CONSTRAINT_LEVELSCRIPT_NO_COMPONENTS");

	/** Variable name is invalid */
	const FString InvalidVariableName = TEXT("BP_CONSTRAINT_INVALID_VAR_NAME");

	/** Variable name conflicts with existing member */
	const FString VariableNameConflict = TEXT("BP_CONSTRAINT_VAR_NAME_CONFLICT");

	/** Component class not found */
	const FString ComponentClassNotFound = TEXT("BP_CONSTRAINT_COMPONENT_NOT_FOUND");

	/** Non-scene Actor Components cannot have child components */
	const FString ActorComponentNoSCS = TEXT("BP_CONSTRAINT_ACTORCOMP_NO_SCS");
}
