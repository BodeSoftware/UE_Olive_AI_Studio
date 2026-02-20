// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "OliveIRTypes.h"

/**
 * IR Schema version constants and validation utilities.
 *
 * The IR schema defines the structure and rules for Blueprint intermediate representation.
 * Version 1.0 is locked for Phase 1 and should not be changed without a major version bump.
 */
namespace OliveIR
{
	/** Current IR schema major version */
	constexpr int32 SchemaVersionMajor = 1;

	/** Current IR schema minor version */
	constexpr int32 SchemaVersionMinor = 0;

	/** Schema version string for JSON serialization */
	const FString SchemaVersion = TEXT("1.0");

	/** Minimum supported major version for reading IR */
	constexpr int32 MinSupportedMajor = 1;

	/** Minimum supported minor version for reading IR */
	constexpr int32 MinSupportedMinor = 0;

	// ============================================================================
	// Locked IR Design Rules (Phase 1)
	// ============================================================================
	// R1: Node IDs are simple strings ("node_1", "node_2", "entry")
	// R2: Connections use "node_id.pin_name" format
	// R3: Pin types use EOliveIRTypeCategory
	// R4: Positions are omitted (auto-layout on write)
	// R5: Inherited members have "defined_in" field
	// R6: Empty/null values are omitted from JSON
	// R7: Arrays serialize even if empty
	// R8: Schema version is included in root
	// ============================================================================
}

/**
 * Validates IR JSON against schema rules.
 * Used to ensure IR data conforms to the locked schema specification.
 *
 * Phase 1 uses STRICT validation by default - all locked rules are enforced.
 */
class OLIVEAIRUNTIME_API FOliveIRValidator
{
public:
	// ============================================================================
	// Primary Validation Methods (Strict by default for Phase 1)
	// ============================================================================

	/**
	 * Validate a Blueprint IR JSON object
	 * @param Json The JSON to validate
	 * @param bStrict If true (default), enforces all locked rules including forbidden fields
	 * @return Validation result with any errors
	 */
	static FOliveIRResult ValidateBlueprintIR(const TSharedPtr<FJsonObject>& Json, bool bStrict = true);

	/**
	 * Validate a Graph IR JSON object
	 * @param Json The JSON to validate
	 * @param bStrict If true (default), enforces all locked rules including forbidden fields
	 * @return Validation result with any errors
	 */
	static FOliveIRResult ValidateGraphIR(const TSharedPtr<FJsonObject>& Json, bool bStrict = true);

	/**
	 * Validate a Node IR JSON object
	 * @param Json The JSON to validate
	 * @param bStrict If true (default), enforces node ID format and rejects forbidden fields
	 * @return Validation result with any errors
	 */
	static FOliveIRResult ValidateNodeIR(const TSharedPtr<FJsonObject>& Json, bool bStrict = true);

	/**
	 * Validate a Variable IR JSON object
	 * @param Json The JSON to validate
	 * @param bStrict If true (default), requires defined_in field
	 * @return Validation result with any errors
	 */
	static FOliveIRResult ValidateVariableIR(const TSharedPtr<FJsonObject>& Json, bool bStrict = true);

	// ============================================================================
	// Node ID Validation (Rule R1)
	// ============================================================================

	/**
	 * Check if a node ID follows the allowed format.
	 * Allowed formats: "entry", "result", "result_<suffix>", "node_<number>"
	 * GUID-style IDs are rejected.
	 * @param NodeId The node ID to validate
	 * @return True if valid format
	 */
	static bool IsValidNodeId(const FString& NodeId);

	/**
	 * Check if a string looks like a GUID (8-4-4-4-12 hex format)
	 * @param Str The string to check
	 * @return True if it matches GUID pattern
	 */
	static bool IsGuidFormat(const FString& Str);

	// ============================================================================
	// Connection Validation (Rule R2)
	// ============================================================================

	/**
	 * Check if a connection string is valid format (node_id.pin_name)
	 * @param Connection The connection string to validate
	 * @return True if valid format
	 */
	static bool IsValidConnectionString(const FString& Connection);

	/**
	 * Parse a connection string into node ID and pin name
	 * @param Connection The connection string (e.g., "node_1.ReturnValue")
	 * @param OutNodeId Output: the node ID portion
	 * @param OutPinName Output: the pin name portion
	 * @return True if parsed successfully
	 */
	static bool ParseConnectionString(
		const FString& Connection,
		FString& OutNodeId,
		FString& OutPinName);

	// ============================================================================
	// Forbidden Fields Validation (Rule R4)
	// ============================================================================

	/**
	 * Check if JSON contains any forbidden layout/position fields.
	 * Forbidden: position_x, position_y, node_pos_x, node_pos_y, pos_x, pos_y,
	 *            location_x, location_y, x, y (when appearing to be coordinates)
	 * @param Json The JSON object to check
	 * @param OutForbiddenField Output: name of first forbidden field found
	 * @return True if forbidden fields are present
	 */
	static bool HasForbiddenFields(const TSharedPtr<FJsonObject>& Json, FString& OutForbiddenField);

	/**
	 * Get list of all forbidden field names
	 * @return Array of forbidden field names
	 */
	static const TArray<FString>& GetForbiddenFieldNames();

	// ============================================================================
	// Schema Version (Rule R8)
	// ============================================================================

	/**
	 * Check schema version compatibility
	 * @param Version The version string to check (e.g., "1.0")
	 * @return True if compatible with current schema
	 */
	static bool IsSchemaVersionCompatible(const FString& Version);

	/**
	 * Parse a version string into major and minor components
	 * @param Version The version string (e.g., "1.0")
	 * @param OutMajor Output: major version number
	 * @param OutMinor Output: minor version number
	 * @return True if parsed successfully
	 */
	static bool ParseVersionString(
		const FString& Version,
		int32& OutMajor,
		int32& OutMinor);
};
