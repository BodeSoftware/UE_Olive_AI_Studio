// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/OliveValidationEngine.h"

/**
 * Error Builder
 *
 * Standardized JSON error format for all tool responses.
 * Creates consistent error structures that AI models can understand.
 */
class OLIVEAIEDITOR_API FOliveErrorBuilder
{
public:
	/**
	 * Create a success response
	 * @param Data Optional data to include
	 * @return JSON response object
	 */
	static TSharedPtr<FJsonObject> Success(const TSharedPtr<FJsonObject>& Data = nullptr);

	/**
	 * Create an error response
	 * @param Code Error code (e.g., "ASSET_NOT_FOUND")
	 * @param Message Human-readable message
	 * @param Suggestion What the AI should do instead
	 * @param Details Additional key-value details
	 * @return JSON response object
	 */
	static TSharedPtr<FJsonObject> Error(
		const FString& Code,
		const FString& Message,
		const FString& Suggestion = TEXT(""),
		const TMap<FString, FString>& Details = {}
	);

	/**
	 * Create a response from validation result
	 * @param Result Validation result to convert
	 * @return JSON response object
	 */
	static TSharedPtr<FJsonObject> FromValidationResult(const FOliveValidationResult& Result);

	/**
	 * Create a warning response (success with warnings)
	 * @param Data Response data
	 * @param Warnings Warning messages
	 * @return JSON response object
	 */
	static TSharedPtr<FJsonObject> Warning(
		const TSharedPtr<FJsonObject>& Data,
		const TArray<FString>& Warnings
	);

	// ==========================================
	// Common Error Codes
	// ==========================================

	static const FString ERR_NOT_FOUND;           // "ASSET_NOT_FOUND"
	static const FString ERR_TYPE_CONSTRAINT;     // "TYPE_CONSTRAINT_VIOLATION"
	static const FString ERR_INVALID_PARAMS;      // "INVALID_PARAMETERS"
	static const FString ERR_PIE_ACTIVE;          // "PIE_ACTIVE"
	static const FString ERR_COMPILE_FAILED;      // "COMPILATION_FAILED"
	static const FString ERR_INTERNAL;            // "INTERNAL_ERROR"
	static const FString ERR_NOT_IMPLEMENTED;     // "NOT_IMPLEMENTED"
	static const FString ERR_PERMISSION_DENIED;   // "PERMISSION_DENIED"
	static const FString ERR_TIMEOUT;             // "TIMEOUT"
	static const FString ERR_RATE_LIMITED;        // "RATE_LIMITED"
	static const FString ERR_NETWORK;             // "NETWORK_ERROR"
	static const FString ERR_AUTH;                // "AUTHENTICATION_ERROR"
	static const FString ERR_TOOL_NOT_FOUND;      // "TOOL_NOT_FOUND"

	// ==========================================
	// Utility Methods
	// ==========================================

	/**
	 * Serialize response to JSON string
	 */
	static FString ToJsonString(const TSharedPtr<FJsonObject>& Response);

	/**
	 * Create error response for a caught exception
	 */
	static TSharedPtr<FJsonObject> FromException(const FString& ExceptionMessage);

	/**
	 * Check if a response is successful
	 */
	static bool IsSuccess(const TSharedPtr<FJsonObject>& Response);

	/**
	 * Get error code from a response (empty if success)
	 */
	static FString GetErrorCode(const TSharedPtr<FJsonObject>& Response);
};
