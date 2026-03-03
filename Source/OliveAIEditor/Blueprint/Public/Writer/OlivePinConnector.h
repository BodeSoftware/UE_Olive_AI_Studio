// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "OliveBlueprintWriter.h"
#include "OliveWiringDiagnostic.h"
#include "EdGraph/EdGraphPin.h"

// Forward declarations
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class UEdGraphSchema_K2;
struct FEdGraphPinType;
struct FPinConnectionResponse;

/**
 * FOlivePinConnector
 *
 * Handles pin connections between Blueprint graph nodes.
 * Provides validation, connection, and automatic conversion insertion.
 *
 * Key Features:
 * - Validates pin compatibility before connection
 * - Reports detailed reasons for connection failures
 * - Identifies available type conversion options
 * - Can insert conversion nodes for incompatible but convertible types
 *
 * All connections use the K2 schema for proper Blueprint semantics,
 * ensuring connections follow UE Blueprint rules (exec flow, type compatibility, etc.)
 *
 * Usage:
 *   FOlivePinConnector& Connector = FOlivePinConnector::Get();
 *
 *   FString Reason;
 *   if (Connector.CanConnect(SourcePin, TargetPin, Reason))
 *   {
 *       auto Result = Connector.Connect(SourcePin, TargetPin);
 *   }
 */
class OLIVEAIEDITOR_API FOlivePinConnector
{
public:
	/**
	 * Get the singleton instance
	 * @return Reference to the pin connector singleton
	 */
	static FOlivePinConnector& Get();

	// ============================================================================
	// Connection Operations
	// ============================================================================

	/**
	 * Connect two pins
	 * @param SourcePin The output pin to connect from
	 * @param TargetPin The input pin to connect to
	 * @param bAllowConversion If true, will try to insert conversion nodes for type mismatches
	 * @return Result indicating success or failure with details
	 */
	FOliveBlueprintWriteResult Connect(
		UEdGraphPin* SourcePin,
		UEdGraphPin* TargetPin,
		bool bAllowConversion = false);

	/**
	 * Check if two pins can be connected
	 * @param SourcePin The output pin to connect from
	 * @param TargetPin The input pin to connect to
	 * @param OutReason Output parameter with reason if connection is not possible
	 * @return True if the pins can be connected directly
	 */
	bool CanConnect(
		const UEdGraphPin* SourcePin,
		const UEdGraphPin* TargetPin,
		FString& OutReason) const;

	// ============================================================================
	// Batch Operations
	// ============================================================================

	/**
	 * Disconnect a pin from all its connections
	 * @param Pin The pin to disconnect
	 * @return Result indicating success or failure
	 */
	FOliveBlueprintWriteResult DisconnectAll(UEdGraphPin* Pin);

	/**
	 * Disconnect two specific pins
	 * @param PinA First pin
	 * @param PinB Second pin
	 * @return Result indicating success or failure
	 */
	FOliveBlueprintWriteResult Disconnect(
		UEdGraphPin* PinA,
		UEdGraphPin* PinB);

	// ============================================================================
	// Pin Queries
	// ============================================================================

	/**
	 * Get all pins connected to a specific pin
	 * @param Pin The pin to query
	 * @return Array of connected pins
	 */
	TArray<UEdGraphPin*> GetConnectedPins(const UEdGraphPin* Pin) const;

	/**
	 * Check if a pin has any connections
	 * @param Pin The pin to check
	 * @return True if the pin has at least one connection
	 */
	bool HasConnections(const UEdGraphPin* Pin) const;

	/**
	 * Get the number of connections on a pin
	 * @param Pin The pin to query
	 * @return Number of connections
	 */
	int32 GetConnectionCount(const UEdGraphPin* Pin) const;

private:
	FOlivePinConnector() = default;
	~FOlivePinConnector() = default;

	// Non-copyable
	FOlivePinConnector(const FOlivePinConnector&) = delete;
	FOlivePinConnector& operator=(const FOlivePinConnector&) = delete;

	// ============================================================================
	// Private Helper Methods
	// ============================================================================

	/**
	 * Get the K2 schema for Blueprint graphs
	 * @return Pointer to the K2 schema singleton
	 */
	const UEdGraphSchema_K2* GetK2Schema() const;

	/**
	 * Get a human-readable description of a pin type
	 * @param PinType The pin type to describe
	 * @return Type description string
	 */
	FString GetPinTypeDescription(const FEdGraphPinType& PinType) const;

	/**
	 * Validate that a pin is valid for connection operations
	 * @param Pin The pin to validate
	 * @param OutError Error message if validation fails
	 * @return True if the pin is valid
	 */
	bool ValidatePin(const UEdGraphPin* Pin, FString& OutError) const;

	/**
	 * Get the owning Blueprint for a pin
	 * @param Pin The pin to query
	 * @return The owning Blueprint, or nullptr
	 */
	UBlueprint* GetOwningBlueprint(const UEdGraphPin* Pin) const;

	/**
	 * Get the asset path for a pin's Blueprint (for result reporting)
	 * @param Pin The pin to query
	 * @return Asset path string
	 */
	FString GetAssetPathForPin(const UEdGraphPin* Pin) const;

	// ============================================================================
	// Wiring Diagnostic Methods
	// ============================================================================

	/**
	 * Build a structured wiring diagnostic for a failed connection.
	 * Called when TryCreateConnection returns false AND CanCreateConnection
	 * returns CONNECT_RESPONSE_DISALLOW.
	 *
	 * Categorizes the failure reason based on pin types and probes for
	 * available auto-fix paths (autocast, split pin) to explain why they failed.
	 *
	 * @param SourcePin The output pin that was being connected
	 * @param TargetPin The input pin that was the target
	 * @param Response The schema's connection response with message
	 * @return Structured diagnostic with reason, type info, and alternatives
	 */
	FOliveWiringDiagnostic BuildWiringDiagnostic(
		const UEdGraphPin* SourcePin,
		const UEdGraphPin* TargetPin,
		const FPinConnectionResponse& Response) const;

	/**
	 * Given source and target pin types, suggest ordered alternatives.
	 * This is the "alternative suggestion engine" -- produces actionable
	 * alternatives based on the categorized failure reason.
	 *
	 * Does NOT call any UE graph mutation APIs. May call Schema probes
	 * (CanSplitStructPin, SearchForAutocastFunction) for diagnostic info only.
	 *
	 * @param SourcePin The output pin
	 * @param TargetPin The input pin
	 * @param Reason The categorized failure reason
	 * @return Ordered array of alternatives from highest to lowest confidence
	 */
	TArray<FOliveWiringAlternative> SuggestAlternatives(
		const UEdGraphPin* SourcePin,
		const UEdGraphPin* TargetPin,
		EOliveWiringFailureReason Reason) const;
};
