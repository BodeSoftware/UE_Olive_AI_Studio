// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IR/OliveIRTypes.h"  // EOliveIRTypeCategory

// Forward declarations
class UEdGraphNode;
class UEdGraphPin;
class FJsonObject;
class FJsonValue;
struct FEdGraphPinType;

DECLARE_LOG_CATEGORY_EXTERN(LogOlivePinManifest, Log, All);

/**
 * Introspected pin data from a real UEdGraphNode after creation.
 * This is ground truth -- these are the ACTUAL pin names, not guesses.
 */
struct OLIVEAIEDITOR_API FOlivePinManifestEntry
{
    /** Exact internal pin name as returned by UEdGraphPin::GetName() */
    FString PinName;

    /** Display name as returned by UEdGraphPin::GetDisplayName().ToString() */
    FString DisplayName;

    /** Pin direction: true = input, false = output */
    bool bIsInput = true;

    /** Pin category from UEdGraphPin::PinType (e.g., "exec", "bool", "float", "object") */
    FString PinCategory;

    /** Pin subcategory (class name for objects, struct name for structs) */
    FString PinSubCategory;

    /** Full type string for AI-readable description (e.g., "bool", "Actor Object Reference") */
    FString TypeDisplayString;

    /** Whether this is an execution (flow control) pin */
    bool bIsExec = false;

    /** Whether this pin is hidden (e.g., self pin on static functions) */
    bool bIsHidden = false;

    /** Whether this pin has a default value */
    bool bHasDefaultValue = false;

    /** Current default value (for input pins) */
    FString DefaultValue;

    /** Whether this pin is currently connected */
    bool bIsConnected = false;

    /** Whether this pin is required for the node to function correctly.
     *  true for: exec input on non-event impure nodes, self/Target on non-static member functions.
     *  Surfaced in tool results so the AI knows which pins must be wired. */
    bool bIsRequired = false;

    /** IR type category for type-compatibility matching */
    EOliveIRTypeCategory IRTypeCategory = EOliveIRTypeCategory::Unknown;
};

/**
 * Complete pin manifest for a single created node.
 * Built by introspecting the real UEdGraphNode* after CreateNode().
 *
 * This is the contract between node creation (Phase 1) and
 * wiring (Phases 3-5). All pin references in wiring phases
 * resolve against this manifest, never against AI-guessed names.
 */
struct OLIVEAIEDITOR_API FOlivePinManifest
{
    /** The step ID from the plan that created this node */
    FString StepId;

    /** The node ID assigned by GraphWriter (e.g., "node_0") */
    FString NodeId;

    /** The node type (e.g., "CallFunction", "Branch") */
    FString NodeType;

    /** For CallFunction: the resolved function name */
    FString ResolvedFunctionName;

    /** Whether this node has exec flow (has at least one exec pin) */
    bool bHasExecPins = false;

    /** Whether this is a pure node (no exec pins) */
    bool bIsPure = false;

    /** All pins on this node */
    TArray<FOlivePinManifestEntry> Pins;

    // ====================================================================
    // Query Methods
    // ====================================================================

    /**
     * Find the primary exec input pin (the "execute" pin).
     * For most nodes, there is exactly one. Returns nullptr if pure node.
     */
    const FOlivePinManifestEntry* FindExecInput() const;

    /**
     * Find the primary exec output pin (the "then" pin).
     * For most nodes, there is exactly one. Returns nullptr if pure node.
     */
    const FOlivePinManifestEntry* FindExecOutput() const;

    /**
     * Find all exec output pins (e.g., Branch has "True" and "False").
     * @return Array of pointers to exec output pin entries
     */
    TArray<const FOlivePinManifestEntry*> FindAllExecOutputs() const;

    /**
     * Find a data input pin by exact name.
     * @param Name Pin name to search for
     * @return Pointer to the pin entry, or nullptr
     */
    const FOlivePinManifestEntry* FindDataInputByName(const FString& Name) const;

    /**
     * Find a data output pin by exact name.
     * @param Name Pin name to search for
     * @return Pointer to the pin entry, or nullptr
     */
    const FOlivePinManifestEntry* FindDataOutputByName(const FString& Name) const;

    /**
     * Find a pin using the smart fallback chain:
     * exact name -> display name -> case-insensitive -> fuzzy -> type-match.
     *
     * @param Hint The name hint from the plan (may be approximate)
     * @param bIsInput Whether to search input or output pins
     * @param TypeHint Optional: expected type category for type-based fallback
     * @param OutMatchMethod Set to the method that matched (for diagnostics)
     * @return Pointer to the best matching pin entry, or nullptr
     */
    const FOlivePinManifestEntry* FindPinSmart(
        const FString& Hint,
        bool bIsInput,
        EOliveIRTypeCategory TypeHint = EOliveIRTypeCategory::Unknown,
        FString* OutMatchMethod = nullptr) const;

    /**
     * Get all non-hidden, non-exec data pins in a given direction.
     * @param bInput True for input pins, false for output pins
     * @return Array of pointers to data pin entries
     */
    TArray<const FOlivePinManifestEntry*> GetDataPins(bool bInput) const;

    /**
     * Serialize manifest to JSON for inclusion in apply result.
     * Useful for debugging and for the AI to understand what was created.
     */
    TSharedPtr<FJsonObject> ToJson() const;

    // ====================================================================
    // Static Builder
    // ====================================================================

    /**
     * Build a manifest by introspecting a real UEdGraphNode.
     * This is the core factory method -- it reads every pin on the node
     * and populates the manifest with ground-truth data.
     *
     * @param Node The created node to introspect
     * @param StepId The plan step ID that created this node
     * @param NodeId The GraphWriter node ID assigned to this node
     * @param NodeType The OliveNodeTypes constant
     * @return Populated manifest
     */
    static FOlivePinManifest Build(
        UEdGraphNode* Node,
        const FString& StepId,
        const FString& NodeId,
        const FString& NodeType);

    // ====================================================================
    // Type Conversion Utilities
    // ====================================================================

    /**
     * Convert a UE FEdGraphPinType to our EOliveIRTypeCategory.
     * Uses the same mapping as FOlivePinSerializer::MapPinCategory.
     *
     * Public so callers (e.g., OlivePlanExecutor) can patch manifest entries
     * when graph pins report Wildcard but the authoritative type is known
     * from another source (e.g., UK2Node_EditablePinBase::UserDefinedPins).
     */
    static EOliveIRTypeCategory ConvertPinTypeToIRCategory(const FEdGraphPinType& PinType);

private:
    // ====================================================================
    // Internal Helpers
    // ====================================================================

    /**
     * Compute Levenshtein edit distance between two strings.
     * Used by FindPinSmart for fuzzy matching.
     */
    static int32 LevenshteinDistance(const FString& A, const FString& B);
};
