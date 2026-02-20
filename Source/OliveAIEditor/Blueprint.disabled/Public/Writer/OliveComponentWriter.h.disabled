// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "OliveBlueprintWriter.h"

// Forward declarations
class UBlueprint;
class UClass;
class USCS_Node;
class USimpleConstructionScript;

/**
 * FOliveComponentWriter
 *
 * Handles component-level write operations for Blueprints.
 * All operations use FScopedTransaction for undo support.
 *
 * This class provides methods to:
 * - Add new components to a Blueprint's component hierarchy
 * - Remove existing components
 * - Modify component properties
 * - Reparent components within the hierarchy
 *
 * Key Responsibilities:
 * - Validate component operations against Blueprint type constraints
 * - Manage the Simple Construction Script (SCS) for component changes
 * - Handle parent-child relationships in the component tree
 * - Support all common component types (Static Mesh, Skeletal Mesh, etc.)
 *
 * Usage:
 *   FOliveComponentWriter& Writer = FOliveComponentWriter::Get();
 *   auto Result = Writer.AddComponent("/Game/BP_Player", "UStaticMeshComponent", "MeshComp", "");
 *   if (Result.bSuccess) { ... }
 */
class OLIVEAIEDITOR_API FOliveComponentWriter
{
public:
	/**
	 * Get the singleton instance
	 * @return Reference to the component writer singleton
	 */
	static FOliveComponentWriter& Get();

	// ============================================================================
	// Component Operations
	// ============================================================================

	/**
	 * Add a new component to a Blueprint
	 * @param AssetPath Path to the Blueprint to modify
	 * @param ComponentClass Class name of the component to add (e.g., "UStaticMeshComponent")
	 * @param ComponentName Variable name for the new component
	 * @param ParentComponentName Name of the parent component (empty for root attachment)
	 * @return Result with component name on success
	 */
	FOliveBlueprintWriteResult AddComponent(
		const FString& AssetPath,
		const FString& ComponentClass,
		const FString& ComponentName,
		const FString& ParentComponentName = TEXT(""));

	/**
	 * Remove a component from a Blueprint
	 * @param AssetPath Path to the Blueprint to modify
	 * @param ComponentName Name of the component to remove
	 * @return Result indicating success or failure
	 */
	FOliveBlueprintWriteResult RemoveComponent(
		const FString& AssetPath,
		const FString& ComponentName);

	/**
	 * Modify properties of an existing component
	 * @param AssetPath Path to the Blueprint to modify
	 * @param ComponentName Name of the component to modify
	 * @param Properties Map of property names to string values
	 *        Supports common properties like "RelativeLocation", "RelativeRotation",
	 *        "RelativeScale3D", "bVisible", "bHiddenInGame", etc.
	 * @return Result indicating success or failure
	 */
	FOliveBlueprintWriteResult ModifyComponent(
		const FString& AssetPath,
		const FString& ComponentName,
		const TMap<FString, FString>& Properties);

	/**
	 * Change the parent of a component in the hierarchy
	 * @param AssetPath Path to the Blueprint to modify
	 * @param ComponentName Name of the component to reparent
	 * @param NewParentName Name of the new parent component (empty for root)
	 * @return Result indicating success or failure
	 */
	FOliveBlueprintWriteResult ReparentComponent(
		const FString& AssetPath,
		const FString& ComponentName,
		const FString& NewParentName);

	/**
	 * Set the root component of a Blueprint
	 * @param AssetPath Path to the Blueprint to modify
	 * @param ComponentName Name of the component to set as root
	 * @return Result indicating success or failure
	 */
	FOliveBlueprintWriteResult SetRootComponent(
		const FString& AssetPath,
		const FString& ComponentName);

	/**
	 * Rename a component
	 * @param AssetPath Path to the Blueprint to modify
	 * @param OldName Current name of the component
	 * @param NewName New name for the component
	 * @return Result with new name on success
	 */
	FOliveBlueprintWriteResult RenameComponent(
		const FString& AssetPath,
		const FString& OldName,
		const FString& NewName);

private:
	FOliveComponentWriter() = default;
	~FOliveComponentWriter() = default;

	// Non-copyable
	FOliveComponentWriter(const FOliveComponentWriter&) = delete;
	FOliveComponentWriter& operator=(const FOliveComponentWriter&) = delete;

	// ============================================================================
	// Private Helper Methods
	// ============================================================================

	/**
	 * Find an SCS node by component name
	 * @param Blueprint The Blueprint to search
	 * @param ComponentName Name of the component to find
	 * @return The SCS node if found, nullptr otherwise
	 */
	USCS_Node* FindSCSNode(const UBlueprint* Blueprint, const FString& ComponentName);

	/**
	 * Find a component class by name
	 * @param ClassName Class name (with or without U prefix)
	 * @return The class if found, nullptr otherwise
	 */
	UClass* FindComponentClass(const FString& ClassName);

	/**
	 * Validate that a Blueprint can have components
	 * @param Blueprint The Blueprint to validate
	 * @param OutError Error message if validation fails
	 * @return True if the Blueprint supports components
	 */
	bool ValidateCanHaveComponents(const UBlueprint* Blueprint, FString& OutError);

	/**
	 * Generate a unique component name
	 * @param Blueprint The Blueprint to check for conflicts
	 * @param BaseName Desired base name
	 * @return A unique name derived from BaseName
	 */
	FString GenerateUniqueComponentName(const UBlueprint* Blueprint, const FString& BaseName);

	/**
	 * Set a property on a component template by name
	 * @param Component The component template to modify
	 * @param PropertyName Name of the property
	 * @param Value String value to set
	 * @param OutError Error message if setting fails
	 * @return True if the property was set successfully
	 */
	bool SetComponentProperty(
		UActorComponent* Component,
		const FString& PropertyName,
		const FString& Value,
		FString& OutError);

	/**
	 * Check if Play-In-Editor is currently active
	 * @return True if PIE is active
	 */
	bool IsPIEActive() const;

	/**
	 * Load a Blueprint for editing with validation
	 * @param AssetPath Path to the Blueprint
	 * @param OutError Error message if loading fails
	 * @return The Blueprint if successful, nullptr otherwise
	 */
	UBlueprint* LoadBlueprintForEditing(const FString& AssetPath, FString& OutError);
};
