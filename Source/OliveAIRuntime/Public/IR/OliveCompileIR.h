// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "OliveCompileIR.generated.h"

/**
 * Severity of a compile error/warning
 */
UENUM(BlueprintType)
enum class EOliveIRCompileErrorSeverity : uint8
{
	Error,
	Warning,
	Note
};

/**
 * Structured compilation error information.
 * Contains all details needed for AI to understand and potentially fix the error.
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRCompileError
{
	GENERATED_BODY()

	/** Human-readable error message */
	UPROPERTY()
	FString Message;

	/** Error severity level */
	UPROPERTY()
	EOliveIRCompileErrorSeverity Severity = EOliveIRCompileErrorSeverity::Error;

	/** Node ID if error is associated with a specific node */
	UPROPERTY()
	FString NodeId;

	/** Node display name for human reference */
	UPROPERTY()
	FString NodeName;

	/** Name of the graph containing the error */
	UPROPERTY()
	FString GraphName;

	/** Line number (for C++ context errors) */
	UPROPERTY()
	int32 Line = 0;

	/** AI-friendly suggestion for fixing the error */
	UPROPERTY()
	FString Suggestion;

	/**
	 * Convert to JSON representation
	 * @return JSON object containing all error fields
	 */
	TSharedPtr<FJsonObject> ToJson() const;

	/**
	 * Parse from JSON representation
	 * @param JsonObject The JSON to parse
	 * @return Parsed compile error
	 */
	static FOliveIRCompileError FromJson(const TSharedPtr<FJsonObject>& JsonObject);

	/**
	 * Get severity as display string
	 * @return "Error", "Warning", or "Note"
	 */
	FString GetSeverityString() const;

	/**
	 * Create an error with the given message
	 * @param InMessage Error message
	 * @param InSuggestion Optional suggestion for fixing
	 * @return Compile error struct
	 */
	static FOliveIRCompileError MakeError(const FString& InMessage, const FString& InSuggestion = TEXT(""));

	/**
	 * Create a warning with the given message
	 * @param InMessage Warning message
	 * @param InSuggestion Optional suggestion for fixing
	 * @return Compile error struct with Warning severity
	 */
	static FOliveIRCompileError MakeWarning(const FString& InMessage, const FString& InSuggestion = TEXT(""));

	/**
	 * Create a note with the given message
	 * @param InMessage Note message
	 * @return Compile error struct with Note severity
	 */
	static FOliveIRCompileError MakeNote(const FString& InMessage);
};

/**
 * Complete compilation result.
 * Contains success status, all errors and warnings, and timing information.
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRCompileResult
{
	GENERATED_BODY()

	/** Whether compilation succeeded without errors */
	UPROPERTY()
	bool bSuccess = false;

	/** All compilation errors */
	UPROPERTY()
	TArray<FOliveIRCompileError> Errors;

	/** All compilation warnings */
	UPROPERTY()
	TArray<FOliveIRCompileError> Warnings;

	/** Time taken to compile in milliseconds */
	UPROPERTY()
	double CompileTimeMs = 0.0;

	/**
	 * Convert to JSON representation
	 * @return JSON object containing all compile result fields
	 */
	TSharedPtr<FJsonObject> ToJson() const;

	/**
	 * Convert to JSON string
	 * @return Formatted JSON string
	 */
	FString ToJsonString() const;

	/**
	 * Parse from JSON representation
	 * @param JsonObject The JSON to parse
	 * @return Parsed compile result
	 */
	static FOliveIRCompileResult FromJson(const TSharedPtr<FJsonObject>& JsonObject);

	/**
	 * Create a success result
	 * @param InCompileTimeMs Optional compile time
	 * @return Success compile result
	 */
	static FOliveIRCompileResult Success(double InCompileTimeMs = 0.0);

	/**
	 * Create a failure result with an error
	 * @param ErrorMessage Error message
	 * @param Suggestion Optional suggestion
	 * @return Failure compile result
	 */
	static FOliveIRCompileResult Failure(const FString& ErrorMessage, const FString& Suggestion = TEXT(""));

	/**
	 * Add an error to the result
	 * @param Error The error to add
	 */
	void AddError(const FOliveIRCompileError& Error);

	/**
	 * Add a warning to the result
	 * @param Warning The warning to add
	 */
	void AddWarning(const FOliveIRCompileError& Warning);

	/**
	 * Get total error count
	 * @return Number of errors
	 */
	int32 GetErrorCount() const { return Errors.Num(); }

	/**
	 * Get total warning count
	 * @return Number of warnings
	 */
	int32 GetWarningCount() const { return Warnings.Num(); }

	/**
	 * Check if there are any errors
	 * @return True if errors exist
	 */
	bool HasErrors() const { return Errors.Num() > 0; }

	/**
	 * Check if there are any warnings
	 * @return True if warnings exist
	 */
	bool HasWarnings() const { return Warnings.Num() > 0; }
};
