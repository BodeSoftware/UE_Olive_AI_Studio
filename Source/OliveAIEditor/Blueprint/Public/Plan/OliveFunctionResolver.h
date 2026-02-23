// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// Forward declarations
class UFunction;
class UClass;
class UBlueprint;

DECLARE_LOG_CATEGORY_EXTERN(LogOliveFunctionResolver, Log, All);

/**
 * Result of function resolution with match quality information.
 */
struct OLIVEAIEDITOR_API FOliveFunctionMatch
{
    /** The resolved UFunction pointer (nullptr if not found) */
    UFunction* Function = nullptr;

    /** The class that owns this function */
    UClass* OwningClass = nullptr;

    /** How the function was found */
    enum class EMatchMethod : uint8
    {
        ExactName,           // Exact FindFunctionByName match
        K2Prefix,            // Added/removed K2_ prefix
        Alias,               // Common alias map (e.g., "Print" -> "PrintString")
        CatalogExact,        // Exact match in node catalog
        CatalogFuzzy,        // Fuzzy match in node catalog (score >= threshold)
        BroadClassSearch,    // Found by iterating all Blueprint Function Libraries
        ParentClassSearch,   // Found on the Blueprint's parent class hierarchy
    };

    EMatchMethod MatchMethod = EMatchMethod::ExactName;

    /** Match confidence score (0-100). 100 = exact, lower = fuzzier */
    int32 Confidence = 0;

    /** Display name for error messages */
    FString DisplayName;

    /** Whether this match resolved successfully */
    bool IsValid() const { return Function != nullptr; }
};

/**
 * FOliveFunctionResolver
 *
 * Smart function resolution that goes far beyond exact name matching.
 * Implements a prioritized fallback chain to find the UFunction* that
 * best matches an AI-provided function name.
 *
 * Resolution order:
 *   1. Exact FindFunctionByName on TargetClass (if provided)
 *   2. Exact match on core library classes + Blueprint parent hierarchy
 *   3. K2_ prefix manipulation (add K2_ prefix, remove K2_ prefix)
 *   4. Common alias map (static table of known aliases)
 *   5. Node catalog exact match on function name or display name
 *   6. Node catalog fuzzy match (score >= MinFuzzyScore)
 *   7. Broad search: iterate all UBlueprintFunctionLibrary subclasses
 *
 * If multiple candidates are found at the same priority level, the
 * resolver picks the one whose class is closest to the Blueprint's
 * parent class in the inheritance hierarchy. If still ambiguous,
 * it returns an error with all candidates listed.
 *
 * Thread Safety: Stateless, all methods are static. Thread-safe for reads.
 *                Does NOT hold persistent state. Safe to call from any thread
 *                that can access UObjects (game thread recommended).
 */
class OLIVEAIEDITOR_API FOliveFunctionResolver
{
public:
    /**
     * Resolve a function name to a UFunction*.
     *
     * @param FunctionName   Name as provided by the AI (may be approximate)
     * @param TargetClass    Optional class name for disambiguation (may be empty)
     * @param Blueprint      The target Blueprint (for parent class hierarchy search, may be nullptr)
     * @return Match result with function pointer and confidence
     */
    static FOliveFunctionMatch Resolve(
        const FString& FunctionName,
        const FString& TargetClass,
        UBlueprint* Blueprint);

    /**
     * Get all candidates for a function name (for "did you mean?" suggestions).
     *
     * @param FunctionName   Name to search for
     * @param MaxResults     Maximum candidates to return
     * @return Array of matches sorted by confidence descending
     */
    static TArray<FOliveFunctionMatch> GetCandidates(
        const FString& FunctionName,
        int32 MaxResults = 5);

    /**
     * Convert a match method enum to a human-readable string.
     * @param Method The match method to convert
     * @return String representation (e.g., "exact", "k2_prefix", "alias", etc.)
     */
    static FString MatchMethodToString(FOliveFunctionMatch::EMatchMethod Method);

private:
    // ====================================================================
    // Resolution Strategies (tried in order)
    // ====================================================================

    /** Strategy 1: Exact name match on a specific class */
    static UFunction* TryExactMatch(
        const FString& FunctionName,
        UClass* Class);

    /** Strategy 2: Try adding or removing the K2_ prefix on a specific class */
    static UFunction* TryK2PrefixMatch(
        const FString& FunctionName,
        UClass* Class,
        FString& OutResolvedName);

    /** Strategy 3: Look up in the common alias map, then resolve the alias */
    static UFunction* TryAliasMatch(
        const FString& FunctionName,
        FString& OutResolvedName);

    /** Strategy 4: Search the node catalog for exact function name or display name match */
    static FOliveFunctionMatch TryCatalogMatch(
        const FString& FunctionName);

    /** Strategy 5: Broad search across all loaded UBlueprintFunctionLibrary subclasses */
    static TArray<FOliveFunctionMatch> BroadSearch(
        const FString& FunctionName,
        int32 MaxResults);

    // ====================================================================
    // Alias Map
    // ====================================================================

    /**
     * Get the static alias map.
     * Maps common short names / AI-typical names to actual UE function names.
     * Key = AI-provided name (case-insensitive lookup).
     * Value = exact UE function name.
     */
    static const TMap<FString, FString>& GetAliasMap();

    /**
     * Get the class search order for a given Blueprint.
     * Returns: TargetClass (if any), parent class hierarchy, then common library classes.
     *
     * @param TargetClass Optional class name to search first
     * @param Blueprint The Blueprint (for parent class hierarchy), may be nullptr
     * @return Ordered array of classes to search
     */
    static TArray<UClass*> GetSearchOrder(
        const FString& TargetClass,
        UBlueprint* Blueprint);

    /**
     * Find a UClass by name. Wraps FOliveNodeFactory::Get().FindClass().
     * Falls back to FindFirstObject if the factory doesn't find it.
     */
    static UClass* FindClassByName(const FString& ClassName);

    /** Minimum fuzzy match score from the node catalog to accept */
    static constexpr int32 MinFuzzyScore = 60;
};
