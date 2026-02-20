// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IR/CommonIR.h"
#include "IR/OliveIRTypes.h"

// Forward declarations
class UEdGraphPin;
class UEdGraphNode;
class UStruct;
class UEnum;
class UClass;
class UObject;
struct FEdGraphPinType;

/**
 * FOlivePinSerializer
 *
 * Serializes UEdGraphPin instances to FOliveIRPin intermediate representation.
 * Handles pin type mapping, default values, and connection resolution.
 *
 * This class is responsible for:
 * - Converting UE pin types to IR type categories
 * - Extracting default values from pins
 * - Resolving pin connections to node_id.pin_name format
 * - Handling all container types (arrays, sets, maps)
 * - Handling object/class/struct/enum types with full path resolution
 *
 * Usage:
 *   TSharedRef<FOlivePinSerializer> PinSerializer = MakeShared<FOlivePinSerializer>();
 *   TMap<const UEdGraphNode*, FString> NodeIdMap;  // Pre-populated with node->id mappings
 *   FOliveIRPin IRPin = PinSerializer->SerializePin(Pin, NodeIdMap);
 */
class OLIVEAIEDITOR_API FOlivePinSerializer
{
public:
	FOlivePinSerializer() = default;
	~FOlivePinSerializer() = default;

	// ============================================================================
	// Main Serialization Methods
	// ============================================================================

	/**
	 * Serialize a UEdGraphPin to IR format
	 * @param Pin The pin to serialize
	 * @param NodeIdMap Map of nodes to their IR IDs (for connection resolution)
	 * @return The serialized pin in IR format
	 */
	FOliveIRPin SerializePin(
		const UEdGraphPin* Pin,
		const TMap<const UEdGraphNode*, FString>& NodeIdMap) const;

	/**
	 * Serialize a pin type to IR format
	 * @param PinType The UE pin type to serialize
	 * @return The serialized type in IR format
	 */
	FOliveIRType SerializePinType(const FEdGraphPinType& PinType) const;

	/**
	 * Extract default value from a pin
	 * @param Pin The pin to get default value from
	 * @return The default value as a string
	 */
	FString SerializeDefaultValue(const UEdGraphPin* Pin) const;

	/**
	 * Resolve a single connection from a pin
	 * Returns the first connection for input pins
	 * @param Pin The pin to resolve connections from
	 * @param NodeIdMap Map of nodes to their IR IDs
	 * @return Connection string in "node_id.pin_name" format, or empty if not connected
	 */
	FString ResolveConnection(
		const UEdGraphPin* Pin,
		const TMap<const UEdGraphNode*, FString>& NodeIdMap) const;

	/**
	 * Resolve all connections from a pin
	 * Used for output data pins which can have multiple connections
	 * @param Pin The pin to resolve connections from
	 * @param NodeIdMap Map of nodes to their IR IDs
	 * @return Array of connection strings in "node_id.pin_name" format
	 */
	TArray<FString> ResolveAllConnections(
		const UEdGraphPin* Pin,
		const TMap<const UEdGraphNode*, FString>& NodeIdMap) const;

private:
	// ============================================================================
	// Type Mapping Helpers
	// ============================================================================

	/**
	 * Map a UE pin category name to IR type category
	 * @param CategoryName The UE pin category (e.g., "bool", "int", "struct")
	 * @return The corresponding IR type category
	 */
	EOliveIRTypeCategory MapPinCategory(const FName& CategoryName) const;

	/**
	 * Resolve the full path of a UObject (for type references)
	 * @param Object The object to get path for
	 * @return The full object path, or empty string if null
	 */
	FString ResolveObjectPath(const UObject* Object) const;

	/**
	 * Format a struct name for IR output
	 * @param Struct The struct to format
	 * @return The formatted struct name (e.g., "FVector", "FTransform")
	 */
	FString FormatTypeName(const UStruct* Struct) const;

	/**
	 * Format an enum name for IR output
	 * @param Enum The enum to format
	 * @return The formatted enum name
	 */
	FString FormatTypeName(const UEnum* Enum) const;

	/**
	 * Format a class name for IR output
	 * @param Class The class to format
	 * @return The formatted class name (e.g., "AActor", "UStaticMeshComponent")
	 */
	FString FormatTypeName(const UClass* Class) const;

	/**
	 * Serialize inner type for containers (array, set, map)
	 * @param PinType The container pin type
	 * @return JSON string representation of inner type
	 */
	FString SerializeInnerType(const FEdGraphPinType& PinType) const;

	/**
	 * Get the subcategory object and format appropriately
	 * @param PinType The pin type
	 * @return The formatted subcategory name (struct/class/enum name)
	 */
	FString GetSubcategoryName(const FEdGraphPinType& PinType) const;
};
