// Copyright Bode Software. All Rights Reserved.

/**
 * OliveWiringDiagnostic.h
 *
 * Structured diagnostic types for pin wiring failures.
 * Extracted into its own header to avoid circular dependency between
 * OlivePinConnector.h and OliveBlueprintWriter.h.
 *
 * Produced by FOlivePinConnector::Connect() on failure.
 * Consumed by PlanExecutor, connect_pins tool handler, and self-correction policy.
 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * Categorizes the reason a pin connection failed.
 * Used to select the appropriate alternative suggestion strategy.
 */
enum class EOliveWiringFailureReason : uint8
{
	/** Pin types are completely incompatible (e.g., Exec -> Float) */
	TypesIncompatible,

	/** Struct output -> scalar input; needs decomposition (BreakVector, SplitPin) */
	StructToScalar,

	/** Scalar output -> struct input; needs composition (MakeVector) */
	ScalarToStruct,

	/** Object type mismatch; needs explicit cast */
	ObjectCastRequired,

	/** Container type mismatch (Array vs single, Set vs Array) */
	ContainerMismatch,

	/** Direction mismatch (output -> output, input -> input) */
	DirectionMismatch,

	/** Same-node connection attempt */
	SameNode,

	/** Pin is already connected and does not allow multiple */
	AlreadyConnected,

	/** Unknown / uncategorized failure */
	Unknown,
};

/**
 * A single actionable alternative the AI can try to resolve a wiring failure.
 */
struct OLIVEAIEDITOR_API FOliveWiringAlternative
{
	/** Brief label for what this alternative does (e.g., "Use break_struct op") */
	FString Label;

	/** Specific action the AI should take, in tool-call terms */
	FString Action;

	/** Confidence: "high" if this is the standard fix, "medium" if it may work,
	 *  "low" if it is a fallback. Helps the AI prioritize. */
	FString Confidence;
};

/**
 * Structured diagnostic for a pin wiring failure.
 * Produced by FOlivePinConnector::Connect() on failure.
 * Contains: what went wrong, why, and what to try instead.
 */
struct OLIVEAIEDITOR_API FOliveWiringDiagnostic
{
	/** Categorized failure reason */
	EOliveWiringFailureReason Reason = EOliveWiringFailureReason::Unknown;

	/** Human-readable source pin type (e.g., "Vector", "Float", "Actor Object Reference") */
	FString SourceTypeName;

	/** Human-readable target pin type */
	FString TargetTypeName;

	/** Source pin name */
	FString SourcePinName;

	/** Target pin name */
	FString TargetPinName;

	/** The raw schema response message (preserved for logging) */
	FString SchemaMessage;

	/** Why the specific auto-fix paths failed (e.g., "No autocast function for Vector -> Float",
	 *  "Pin is not splittable (MD_NativeDisableSplitPin)") */
	FString WhyAutoFixFailed;

	/** Ordered list of alternatives the AI can try */
	TArray<FOliveWiringAlternative> Alternatives;

	/** Serialize to JSON for inclusion in tool results */
	TSharedPtr<FJsonObject> ToJson() const;

	/** Format as a single human-readable string (for log and existing string-based paths) */
	FString ToHumanReadable() const;

	/**
	 * Get a string representation of the failure reason enum value.
	 * @return String like "StructToScalar", "ObjectCastRequired", etc.
	 */
	static FString ReasonToString(EOliveWiringFailureReason InReason);
};
