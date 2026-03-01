// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IR/OliveCompileIR.h"

// Forward declarations
class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class FCompilerResultsLog;

DECLARE_LOG_CATEGORY_EXTERN(LogOliveCompile, Log, All);

/**
 * FOliveCompileManager
 *
 * Singleton manager for Blueprint compilation with structured error reporting.
 * Provides compilation services that parse compiler output into AI-friendly
 * error structures with actionable suggestions.
 *
 * Key Responsibilities:
 * - Compile Blueprints with timing measurement
 * - Parse compile errors and warnings into structured FOliveIRCompileError format
 * - Associate errors with specific nodes/graphs when possible
 * - Generate AI-friendly suggestions for fixing common errors
 * - Pattern-match error messages for categorization
 *
 * Usage:
 *   FOliveCompileManager& Manager = FOliveCompileManager::Get();
 *   FOliveIRCompileResult Result = Manager.Compile(Blueprint);
 *   if (!Result.bSuccess)
 *   {
 *       for (const FOliveIRCompileError& Error : Result.Errors)
 *       {
 *           UE_LOG(LogOliveCompile, Error, TEXT("%s - %s"), *Error.Message, *Error.Suggestion);
 *       }
 *   }
 */
class OLIVEAIEDITOR_API FOliveCompileManager
{
public:
	/**
	 * Get the singleton instance
	 * @return Reference to the compile manager singleton
	 */
	static FOliveCompileManager& Get();

	// ============================================================================
	// Compilation Methods
	// ============================================================================

	/**
	 * Compile a Blueprint and return structured results
	 * @param Blueprint The Blueprint to compile
	 * @return Compile result with errors, warnings, and timing info
	 */
	FOliveIRCompileResult Compile(UBlueprint* Blueprint);

	/**
	 * Compile a Blueprint by asset path
	 * @param AssetPath Path to the Blueprint (e.g., "/Game/Blueprints/BP_Player")
	 * @return Compile result with errors, warnings, and timing info
	 */
	FOliveIRCompileResult Compile(const FString& AssetPath);

	// ============================================================================
	// Error Query Methods
	// ============================================================================

	/**
	 * Check if a Blueprint has any compile errors
	 * @param Blueprint The Blueprint to check
	 * @return True if there are errors (Blueprint->Status == BS_Error)
	 */
	bool HasErrors(const UBlueprint* Blueprint) const;

	/**
	 * Get existing errors from a Blueprint without recompiling
	 * @param Blueprint The Blueprint to query
	 * @return Array of compile errors parsed from the Blueprint
	 */
	TArray<FOliveIRCompileError> GetExistingErrors(const UBlueprint* Blueprint) const;

	// ============================================================================
	// Suggestion Generation
	// ============================================================================

	/**
	 * Generate an AI-friendly suggestion for fixing an error
	 * @param Error The compile error to generate a suggestion for
	 * @return Suggested fix or action in plain text
	 */
	FString GenerateSuggestion(const FOliveIRCompileError& Error) const;

private:
	FOliveCompileManager() = default;
	~FOliveCompileManager() = default;

	// Non-copyable
	FOliveCompileManager(const FOliveCompileManager&) = delete;
	FOliveCompileManager& operator=(const FOliveCompileManager&) = delete;

	// ============================================================================
	// Parsing Methods
	// ============================================================================

	/**
	 * Parse the compile log and node errors from a Blueprint
	 * @param Blueprint The compiled Blueprint
	 * @param OutResult The result to populate with parsed errors
	 */
	void ParseCompileLog(const UBlueprint* Blueprint, FOliveIRCompileResult& OutResult) const;

	/**
	 * Parse a single error message into a structured error
	 * @param Message The error message text
	 * @param Blueprint The Blueprint for context
	 * @return Structured compile error
	 */
	FOliveIRCompileError ParseErrorMessage(const FString& Message, const UBlueprint* Blueprint) const;

	/**
	 * Find the node associated with an error message
	 * @param ErrorMessage The error message to search for
	 * @param Blueprint The Blueprint to search in
	 * @return The associated node if found, nullptr otherwise
	 */
	UEdGraphNode* FindErrorNode(const FString& ErrorMessage, const UBlueprint* Blueprint) const;

	/**
	 * Extract node information from all graphs in a Blueprint
	 * @param Blueprint The Blueprint to scan
	 * @param OutResult The result to populate with node-level errors
	 */
	void ExtractNodeErrors(const UBlueprint* Blueprint, FOliveIRCompileResult& OutResult) const;

	/**
	 * Extract errors from the FCompilerResultsLog that were not already captured
	 * by per-node extraction. This catches graph-level compiler errors (e.g.,
	 * duplicate graph names, interface conflicts) that don't attach to any node.
	 * Deduplicates against existing errors/warnings in OutResult via substring match.
	 * @param CompilerLog The compiler results log from FKismetEditorUtilities::CompileBlueprint
	 * @param OutResult The result to append newly-discovered errors/warnings to
	 */
	void ExtractCompilerLogErrors(const FCompilerResultsLog& CompilerLog, FOliveIRCompileResult& OutResult) const;

	// ============================================================================
	// Error Pattern Matchers
	// ============================================================================

	/**
	 * Match a pin connection error pattern
	 * Pattern: "Cannot connect X to Y: reason"
	 * @param Message The error message to match
	 * @param OutSourcePin Output: source pin name
	 * @param OutTargetPin Output: target pin name
	 * @param OutReason Output: reason for failure
	 * @return True if pattern matched
	 */
	bool MatchPinConnectionError(const FString& Message, FString& OutSourcePin, FString& OutTargetPin, FString& OutReason) const;

	/**
	 * Match a missing variable error pattern
	 * Pattern: "Variable 'X' not found" or "Unknown variable X"
	 * @param Message The error message to match
	 * @param OutVariableName Output: name of the missing variable
	 * @return True if pattern matched
	 */
	bool MatchMissingVariableError(const FString& Message, FString& OutVariableName) const;

	/**
	 * Match a missing function error pattern
	 * Pattern: "Function 'X' not found" or "No function named X in class Y"
	 * @param Message The error message to match
	 * @param OutFunctionName Output: name of the missing function
	 * @param OutClassName Output: class name if specified (may be empty)
	 * @return True if pattern matched
	 */
	bool MatchMissingFunctionError(const FString& Message, FString& OutFunctionName, FString& OutClassName) const;

	/**
	 * Match a type mismatch error pattern
	 * Pattern: "Expected type X but got Y" or similar
	 * @param Message The error message to match
	 * @param OutExpectedType Output: expected type name
	 * @param OutActualType Output: actual type name
	 * @return True if pattern matched
	 */
	bool MatchTypeError(const FString& Message, FString& OutExpectedType, FString& OutActualType) const;

	/**
	 * Match an unconnected pin error pattern
	 * Pattern: "Pin 'X' is not connected" or similar
	 * @param Message The error message to match
	 * @param OutPinName Output: name of the unconnected pin
	 * @return True if pattern matched
	 */
	bool MatchUnconnectedPinError(const FString& Message, FString& OutPinName) const;

	/**
	 * Match a circular dependency error pattern
	 * Pattern: "Circular dependency" or "cycle detected"
	 * @param Message The error message to match
	 * @return True if pattern matched
	 */
	bool MatchCircularDependencyError(const FString& Message) const;

	/**
	 * Match a deprecated node error pattern
	 * Pattern: "deprecated" in message
	 * @param Message The error message to match
	 * @param OutNodeType Output: type of deprecated node if identified
	 * @return True if pattern matched
	 */
	bool MatchDeprecatedNodeError(const FString& Message, FString& OutNodeType) const;

	// ============================================================================
	// Helper Methods
	// ============================================================================

	/**
	 * Load a Blueprint by asset path
	 * @param AssetPath The path to load
	 * @return The loaded Blueprint or nullptr
	 */
	UBlueprint* LoadBlueprint(const FString& AssetPath) const;

	/**
	 * Check if Play-In-Editor is currently active
	 * @return True if PIE is active
	 */
	bool IsPIEActive() const;
};
