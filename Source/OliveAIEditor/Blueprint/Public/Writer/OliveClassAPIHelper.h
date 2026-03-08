// Copyright Bode Software. All Rights Reserved.

/**
 * OliveClassAPIHelper.h
 *
 * Lightweight static utility for enumerating the Blueprint-visible API surface
 * of a UClass. Returns callable functions and visible properties filtered to
 * what is accessible from Blueprint graphs.
 *
 * Used by:
 * - OliveBlueprintPlanResolver (class-scoped FUNCTION_NOT_FOUND suggestions)
 * - OliveAgentPipeline (component API map injection into Builder prompt)
 *
 * All methods are static. No state, no singleton.
 */

#pragma once

#include "CoreMinimal.h"

class UClass;

class OLIVEAIEDITOR_API FOliveClassAPIHelper
{
public:
	// No instances -- static utility only
	FOliveClassAPIHelper() = delete;

	/**
	 * Enumerate Blueprint-callable functions on a class (including inherited).
	 * Filters out internal, deprecated, and editor-only functions.
	 *
	 * @param Class      The UClass to scan (may be nullptr -- returns empty)
	 * @param MaxResults Maximum entries to return (default 15, 0 = unlimited)
	 * @return Function names sorted alphabetically, capped at MaxResults
	 */
	static TArray<FString> GetCallableFunctions(UClass* Class, int32 MaxResults = 15);

	/**
	 * Enumerate Blueprint-visible properties on a class (including inherited).
	 * Filters out deprecated and replication-internal properties.
	 *
	 * @param Class      The UClass to scan (may be nullptr -- returns empty)
	 * @param MaxResults Maximum entries to return (default 10, 0 = unlimited)
	 * @return Pairs of (PropertyName, TypeString) sorted alphabetically by name.
	 *         TypeString is human-readable (e.g., "float", "FVector", "bool", "UStaticMesh*")
	 */
	static TArray<TPair<FString, FString>> GetVisibleProperties(UClass* Class, int32 MaxResults = 10);

	/**
	 * Format a compact API summary for a class.
	 * Output format:
	 *   ### ClassName
	 *   Functions: Func1, Func2, Func3, ...
	 *   Properties: PropName (type), PropName2 (type2), ...
	 *
	 * Returns empty string if Class is null or has no callable functions/visible properties.
	 *
	 * @param Class          The UClass to summarize
	 * @param MaxFunctions   Maximum function entries (default 15)
	 * @param MaxProperties  Maximum property entries (default 10)
	 * @return Formatted markdown block
	 */
	static FString FormatCompactAPISummary(UClass* Class, int32 MaxFunctions = 15, int32 MaxProperties = 10);

	/**
	 * Build class-scoped suggestions for a FUNCTION_NOT_FOUND error.
	 * Scores functions and properties by similarity to FailedFunctionName,
	 * detects property-name cross-matches (e.g., "SetSpeed" -> "MaxSpeed"),
	 * and formats actionable guidance.
	 *
	 * @param TargetClass        The UClass where the function was expected
	 * @param FailedFunctionName The function name the AI tried (e.g., "SetSpeed")
	 * @param MaxFunctions       Maximum function suggestions (default 5)
	 * @param MaxProperties      Maximum property suggestions (default 3)
	 * @return Formatted suggestion string, empty if TargetClass is nullptr
	 */
	static FString BuildScopedSuggestions(
		UClass* TargetClass,
		const FString& FailedFunctionName,
		int32 MaxFunctions = 5,
		int32 MaxProperties = 3);

	/**
	 * Score a candidate name against a search query for similarity.
	 * Uses three cumulative criteria:
	 * 1. Substring containment (either direction) -- strong signal
	 * 2. Common prefix length -- medium signal
	 * 3. CamelCase word overlap -- catches reordering
	 *
	 * Extracted from the fuzzy matching algorithm in OliveNodeFactory::FindFunction().
	 *
	 * @param CandidateName  The function/property name to score
	 * @param SearchName     The AI's attempted name
	 * @return Score (higher is better, 0 means no match)
	 */
	static int32 ScoreSimilarity(const FString& CandidateName, const FString& SearchName);

private:
	/**
	 * Check if a function name should be filtered out of API listings.
	 * Matches internal UE functions that are not useful for Blueprint authors.
	 *
	 * @param FuncName The function name to check
	 * @return True if the function should be excluded
	 */
	static bool ShouldFilterFunction(const FString& FuncName);

	/**
	 * Check if a property name should be filtered out of API listings.
	 * Matches replication callbacks and deprecated markers.
	 *
	 * @param PropName The property name to check
	 * @return True if the property should be excluded
	 */
	static bool ShouldFilterProperty(const FString& PropName);

	/**
	 * Get a human-readable type string for a property.
	 * For object properties returns "UClassName*", for structs returns "FStructName",
	 * for basic types returns "float", "int32", "bool", etc.
	 *
	 * @param Property The property to get the type string for
	 * @return Human-readable type name
	 */
	static FString GetPropertyTypeString(const FProperty* Property);

	/**
	 * Split a CamelCase string into lowercase words.
	 * e.g., "GetActorLocation" -> ["get", "actor", "location"]
	 *
	 * @param Str The CamelCase string to split
	 * @return Array of lowercase words
	 */
	static TArray<FString> SplitCamelCase(const FString& Str);
};
