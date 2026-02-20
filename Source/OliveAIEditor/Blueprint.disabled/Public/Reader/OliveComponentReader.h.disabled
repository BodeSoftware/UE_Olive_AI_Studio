// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IR/CommonIR.h"

// Forward declarations
class UBlueprint;
class USCS_Node;
class USimpleConstructionScript;
class UActorComponent;
class UClass;

/**
 * FOliveComponentReader
 *
 * Reads component hierarchy from Blueprints and converts to IR format.
 *
 * This class handles reading the Simple Construction Script (SCS) from a Blueprint,
 * which contains all the components added in the Blueprint editor. It traverses
 * the component tree recursively and serializes each component with its properties.
 *
 * Key Responsibilities:
 * - Extract component tree from USimpleConstructionScript
 * - Recursively build component hierarchy
 * - Read component properties that differ from defaults
 * - Determine root component and component parent relationships
 *
 * Usage:
 *   FOliveComponentReader Reader;
 *   TArray<FOliveIRComponent> Components = Reader.ReadComponents(Blueprint);
 */
class OLIVEAIEDITOR_API FOliveComponentReader
{
public:
	FOliveComponentReader() = default;
	~FOliveComponentReader() = default;

	// ============================================================================
	// Main Public Methods
	// ============================================================================

	/**
	 * Read all components from a Blueprint's component hierarchy
	 * @param Blueprint The Blueprint to read components from
	 * @return Array of components in IR format (root components at top level)
	 */
	TArray<FOliveIRComponent> ReadComponents(const UBlueprint* Blueprint) const;

	/**
	 * Get the name of the root component in a Blueprint
	 * @param Blueprint The Blueprint to query
	 * @return Name of the root component, or empty string if none
	 */
	FString GetRootComponentName(const UBlueprint* Blueprint) const;

	/**
	 * Read a single USCS_Node and convert to IR format
	 * Does not include children - use BuildComponentTree for recursive reading
	 * @param Node The SCS node to read
	 * @return The component in IR format
	 */
	FOliveIRComponent ReadComponentNode(const USCS_Node* Node) const;

	/**
	 * Check if a Blueprint has any components in its SCS
	 * @param Blueprint The Blueprint to check
	 * @return True if the Blueprint has at least one component
	 */
	bool HasComponents(const UBlueprint* Blueprint) const;

private:
	// ============================================================================
	// Tree Building Methods
	// ============================================================================

	/**
	 * Recursively build component tree from an SCS node
	 * @param Node The current SCS node to process
	 * @param OutComponents Array to append components to (at current level)
	 * @param ParentName Name of the parent component (empty for root nodes)
	 */
	void BuildComponentTree(
		const USCS_Node* Node,
		TArray<FOliveIRComponent>& OutComponents,
		const FString& ParentName = TEXT("")) const;

	/**
	 * Read properties from a component node that differ from class defaults
	 * @param Node The SCS node containing the component template
	 * @return Map of property name to string value
	 */
	TMap<FString, FString> ReadComponentProperties(const USCS_Node* Node) const;

	/**
	 * Get a clean class name without the U prefix for display
	 * @param Class The class to get name from
	 * @return Class name without U prefix (e.g., "StaticMeshComponent")
	 */
	FString GetCleanClassName(const UClass* Class) const;

	/**
	 * Serialize a single property value to string
	 * @param Property The property to serialize
	 * @param ValuePtr Pointer to the property value
	 * @return String representation of the value
	 */
	FString SerializePropertyValue(const FProperty* Property, const void* ValuePtr) const;

	/**
	 * Check if a property value differs from its class default
	 * @param Component The component instance
	 * @param Property The property to check
	 * @return True if the property value differs from CDO
	 */
	bool IsPropertyModified(const UActorComponent* Component, const FProperty* Property) const;
};
