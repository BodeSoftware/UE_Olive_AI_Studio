// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IR/CommonIR.h"
#include "IR/BlueprintIR.h"
#include "OliveBlueprintTypes.h"
#include "Services/OliveValidationEngine.h"
#include "OliveBlueprintWriter.generated.h"

// Forward declarations
class UBlueprint;
class UClass;
class UEdGraph;
struct FEdGraphPinType;
struct FBPVariableDescription;

DECLARE_LOG_CATEGORY_EXTERN(LogOliveBPWriter, Log, All);

/**
 * Result of a Blueprint write operation.
 * Contains success status, warnings, errors, and created item information.
 */
USTRUCT()
struct OLIVEAIEDITOR_API FOliveBlueprintWriteResult
{
	GENERATED_BODY()

	/** Whether the operation succeeded */
	UPROPERTY()
	bool bSuccess = false;

	/** Asset path of the modified Blueprint */
	UPROPERTY()
	FString AssetPath;

	/** Warning messages generated during the operation */
	UPROPERTY()
	TArray<FString> Warnings;

	/** Error messages if the operation failed */
	UPROPERTY()
	TArray<FString> Errors;

	/** ID of a created node (for graph operations) */
	UPROPERTY()
	FString CreatedNodeId;

	/** IDs of multiple created nodes (for batch operations) */
	UPROPERTY()
	TArray<FString> CreatedNodeIds;

	/** Name of a created item (variable, function, component, etc.) */
	UPROPERTY()
	FString CreatedItemName;

	/** Whether compilation succeeded (if Compile was called) */
	UPROPERTY()
	bool bCompileSuccess = false;

	/** Compile errors if compilation failed */
	UPROPERTY()
	TArray<FString> CompileErrors;

	// ============================================================================
	// Factory Methods
	// ============================================================================

	/**
	 * Create a successful result
	 * @param InAssetPath Path of the modified asset
	 * @param InCreatedItemName Optional name of created item
	 * @return Success result
	 */
	static FOliveBlueprintWriteResult Success(const FString& InAssetPath, const FString& InCreatedItemName = TEXT(""));

	/**
	 * Create a successful result with created node ID
	 * @param InAssetPath Path of the modified asset
	 * @param InNodeId ID of the created node
	 * @return Success result
	 */
	static FOliveBlueprintWriteResult SuccessWithNode(const FString& InAssetPath, const FString& InNodeId);

	/**
	 * Create an error result
	 * @param ErrorMessage The error message
	 * @param InAssetPath Optional asset path
	 * @return Error result
	 */
	static FOliveBlueprintWriteResult Error(const FString& ErrorMessage, const FString& InAssetPath = TEXT(""));

	/**
	 * Create an error result from validation result
	 * @param ValidationResult The validation result containing errors
	 * @param InAssetPath Optional asset path
	 * @return Error result
	 */
	static FOliveBlueprintWriteResult FromValidation(const FOliveValidationResult& ValidationResult, const FString& InAssetPath = TEXT(""));

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

	/**
	 * Convert to JSON representation
	 * @return JSON object containing result data
	 */
	TSharedPtr<FJsonObject> ToJson() const;
};

/**
 * FOliveBlueprintWriter
 *
 * Main entry point for writing/modifying Blueprint assets.
 * Handles asset-level operations (create, delete, duplicate, reparent),
 * variable operations (add, remove, modify), function operations,
 * and compilation.
 *
 * This is a singleton class that provides transactional, validated
 * write operations for Blueprints. All modifications use FScopedTransaction
 * for undo support.
 *
 * Key Responsibilities:
 * - Create, delete, and duplicate Blueprint assets
 * - Modify Blueprint parent class and interfaces
 * - Add, remove, and modify variables
 * - Add and remove functions, events, and event dispatchers
 * - Compile and save Blueprints
 * - Validate operations before execution
 *
 * Usage:
 *   FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();
 *   auto Result = Writer.AddVariable("/Game/BP_Player", VariableIR);
 *   if (Result.bSuccess) { ... }
 */
class OLIVEAIEDITOR_API FOliveBlueprintWriter
{
public:
	/**
	 * Get the singleton instance
	 * @return Reference to the Blueprint writer singleton
	 */
	static FOliveBlueprintWriter& Get();

	// ============================================================================
	// Asset-Level Operations
	// ============================================================================

	/**
	 * Create a new Blueprint asset
	 * @param AssetPath Full asset path (e.g., "/Game/Blueprints/BP_NewActor")
	 * @param ParentClass Name or path of the parent class (e.g., "Actor", "/Game/BP_Base")
	 * @param Type Blueprint type to create
	 * @return Result with asset path on success
	 */
	FOliveBlueprintWriteResult CreateBlueprint(
		const FString& AssetPath,
		const FString& ParentClass,
		EOliveBlueprintType Type = EOliveBlueprintType::Normal);

	/**
	 * Delete a Blueprint asset
	 * @param AssetPath Path to the Blueprint to delete
	 * @return Result indicating success or failure
	 */
	FOliveBlueprintWriteResult DeleteBlueprint(const FString& AssetPath);

	/**
	 * Duplicate a Blueprint asset
	 * @param SourcePath Path to the source Blueprint
	 * @param DestPath Path for the duplicated Blueprint
	 * @return Result with new asset path on success
	 */
	FOliveBlueprintWriteResult DuplicateBlueprint(
		const FString& SourcePath,
		const FString& DestPath);

	/**
	 * Change the parent class of a Blueprint
	 * @param AssetPath Path to the Blueprint to modify
	 * @param NewParentClass Name or path of the new parent class
	 * @return Result indicating success or failure
	 */
	FOliveBlueprintWriteResult SetParentClass(
		const FString& AssetPath,
		const FString& NewParentClass);

	/**
	 * Add an interface to a Blueprint
	 * @param AssetPath Path to the Blueprint to modify
	 * @param InterfacePath Path to the interface class/Blueprint
	 * @return Result indicating success or failure
	 */
	FOliveBlueprintWriteResult AddInterface(
		const FString& AssetPath,
		const FString& InterfacePath);

	/**
	 * Remove an interface from a Blueprint
	 * @param AssetPath Path to the Blueprint to modify
	 * @param InterfacePath Path to the interface to remove
	 * @return Result indicating success or failure
	 */
	FOliveBlueprintWriteResult RemoveInterface(
		const FString& AssetPath,
		const FString& InterfacePath);

	// ============================================================================
	// Variable Operations
	// ============================================================================

	/**
	 * Add a variable to a Blueprint
	 * @param AssetPath Path to the Blueprint to modify
	 * @param Variable Variable definition in IR format
	 * @return Result with variable name on success
	 */
	FOliveBlueprintWriteResult AddVariable(
		const FString& AssetPath,
		const FOliveIRVariable& Variable);

	/**
	 * Remove a variable from a Blueprint
	 * @param AssetPath Path to the Blueprint to modify
	 * @param VariableName Name of the variable to remove
	 * @return Result indicating success or failure
	 */
	FOliveBlueprintWriteResult RemoveVariable(
		const FString& AssetPath,
		const FString& VariableName);

	/**
	 * Modify properties of an existing variable
	 * @param AssetPath Path to the Blueprint to modify
	 * @param VariableName Name of the variable to modify
	 * @param Modifications Map of property names to new values
	 *        Supported keys: "Category", "DefaultValue", "Description",
	 *        "bBlueprintReadWrite", "bExposeOnSpawn", "bReplicated"
	 * @return Result indicating success or failure
	 */
	FOliveBlueprintWriteResult ModifyVariable(
		const FString& AssetPath,
		const FString& VariableName,
		const TMap<FString, FString>& Modifications);

	// ============================================================================
	// Function Operations
	// ============================================================================

	/**
	 * Add a user-defined function to a Blueprint
	 * @param AssetPath Path to the Blueprint to modify
	 * @param Signature Function signature definition
	 * @return Result with function name on success
	 */
	FOliveBlueprintWriteResult AddFunction(
		const FString& AssetPath,
		const FOliveIRFunctionSignature& Signature);

	/**
	 * Remove a function from a Blueprint
	 * @param AssetPath Path to the Blueprint to modify
	 * @param FunctionName Name of the function to remove
	 * @return Result indicating success or failure
	 */
	FOliveBlueprintWriteResult RemoveFunction(
		const FString& AssetPath,
		const FString& FunctionName);

	/**
	 * Override a parent class function in the Blueprint
	 * @param AssetPath Path to the Blueprint to modify
	 * @param FunctionName Name of the function to override
	 * @return Result with function name on success
	 */
	FOliveBlueprintWriteResult OverrideFunction(
		const FString& AssetPath,
		const FString& FunctionName);

	/**
	 * Add a custom event to the Blueprint's event graph
	 * @param AssetPath Path to the Blueprint to modify
	 * @param EventName Name of the custom event
	 * @param Params Parameters for the event
	 * @return Result with event name on success
	 */
	FOliveBlueprintWriteResult AddCustomEvent(
		const FString& AssetPath,
		const FString& EventName,
		const TArray<FOliveIRFunctionParam>& Params = TArray<FOliveIRFunctionParam>());

	/**
	 * Add an event dispatcher (multicast delegate) to the Blueprint
	 * @param AssetPath Path to the Blueprint to modify
	 * @param DispatcherName Name of the event dispatcher
	 * @param Params Parameters for the dispatcher
	 * @return Result with dispatcher name on success
	 */
	FOliveBlueprintWriteResult AddEventDispatcher(
		const FString& AssetPath,
		const FString& DispatcherName,
		const TArray<FOliveIRFunctionParam>& Params = TArray<FOliveIRFunctionParam>());

	// ============================================================================
	// Compilation and Saving
	// ============================================================================

	/**
	 * Compile a Blueprint
	 * @param AssetPath Path to the Blueprint to compile
	 * @return Result with compile status and any errors
	 */
	FOliveBlueprintWriteResult Compile(const FString& AssetPath);

	/**
	 * Compile and save a Blueprint
	 * @param AssetPath Path to the Blueprint to compile and save
	 * @return Result with compile status and any errors
	 */
	FOliveBlueprintWriteResult CompileAndSave(const FString& AssetPath);

	/**
	 * Save a Blueprint without compiling
	 * @param AssetPath Path to the Blueprint to save
	 * @return Result indicating success or failure
	 */
	FOliveBlueprintWriteResult Save(const FString& AssetPath);

private:
	enum class EOliveVariableCorrectionAction : uint8
	{
		Continue,
		RouteToDispatcher,
		Reject
	};

	struct FOliveVariableCorrectionDecision
	{
		EOliveVariableCorrectionAction Action = EOliveVariableCorrectionAction::Continue;
		FString RuleId;
		FString Message;
	};

	FOliveBlueprintWriter() = default;
	~FOliveBlueprintWriter() = default;

	// Non-copyable
	FOliveBlueprintWriter(const FOliveBlueprintWriter&) = delete;
	FOliveBlueprintWriter& operator=(const FOliveBlueprintWriter&) = delete;

	// ============================================================================
	// Private Helper Methods
	// ============================================================================

	/**
	 * Load a Blueprint for editing with PIE check
	 * @param AssetPath Path to the Blueprint to load
	 * @param OutError Error message if loading fails
	 * @return The Blueprint if successful, nullptr otherwise
	 */
	UBlueprint* LoadBlueprintForEditing(const FString& AssetPath, FString& OutError);

	/**
	 * Mark a Blueprint as modified for undo and save tracking
	 * @param Blueprint The Blueprint to mark dirty
	 */
	void MarkDirty(UBlueprint* Blueprint);

	/**
	 * Validate an operation before execution
	 * @param Operation Name of the operation for logging
	 * @param Blueprint The Blueprint being modified
	 * @param Params Optional parameters for validation
	 * @return Validation result
	 */
	FOliveValidationResult ValidateOperation(
		const FString& Operation,
		const UBlueprint* Blueprint,
		const TSharedPtr<FJsonObject>& Params = nullptr);

	/**
	 * Convert an IR type to UE FEdGraphPinType
	 * @param IRType The IR type to convert
	 * @return Equivalent UE pin type
	 */
	FEdGraphPinType ConvertIRType(const FOliveIRType& IRType);

	/**
	 * Parse a nested IR type from a JSON string payload.
	 * Supports full JSON object payloads and simple category literals.
	 * @param JsonString Serialized nested type payload
	 * @param OutType Parsed IR type
	 * @return True if parse produced a known type
	 */
	bool ParseNestedIRType(const FString& JsonString, FOliveIRType& OutType) const;

	/**
	 * Validate whether a variable type is safe/resolved for creation.
	 * @param Variable The variable being added
	 * @param PinType Resolved pin type
	 * @param OutError Validation error if invalid
	 * @return True if variable creation should proceed
	 */
	bool ValidateVariableTypeForCreation(
		const FOliveIRVariable& Variable,
		const FEdGraphPinType& PinType,
		FString& OutError) const;

	/**
	 * Apply registered variable correction rules before creation.
	 * This is the single extension point for known bad variable patterns.
	 */
	FOliveVariableCorrectionDecision ApplyVariableCorrectionRules(FOliveIRVariable& Variable) const;

	/**
	 * Find a parent class by name or path
	 * @param ClassName Class name (e.g., "Actor") or path (e.g., "/Game/BP_Base")
	 * @return The class if found, nullptr otherwise
	 */
	UClass* FindParentClass(const FString& ClassName);

	/**
	 * Find an interface class by path
	 * @param InterfacePath Path to the interface
	 * @return The interface class if found, nullptr otherwise
	 */
	UClass* FindInterfaceClass(const FString& InterfacePath);

	/**
	 * Find a variable in a Blueprint by name
	 * @param Blueprint The Blueprint to search
	 * @param VariableName Name of the variable to find
	 * @return Index in NewVariables array, or INDEX_NONE if not found
	 */
	int32 FindVariableIndex(const UBlueprint* Blueprint, const FString& VariableName);

	/**
	 * Create pin type from IR function parameter
	 * @param Param The IR parameter definition
	 * @return UE pin type for the parameter
	 */
	FEdGraphPinType CreatePinTypeFromParam(const FOliveIRFunctionParam& Param);

	/**
	 * Check if Play-In-Editor is currently active
	 * @return True if PIE is active
	 */
	bool IsPIEActive() const;

	/**
	 * Get the default Blueprint type for a given engine Blueprint type
	 * @param BPType The EOliveBlueprintType to map
	 * @return UE EBlueprintType value
	 */
	EBlueprintType GetUEBlueprintType(EOliveBlueprintType BPType) const;
};
