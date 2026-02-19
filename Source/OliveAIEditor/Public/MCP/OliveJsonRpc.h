// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * JSON-RPC 2.0 Utilities
 *
 * Helper functions for parsing and creating JSON-RPC 2.0 messages
 * used by the MCP protocol.
 */
namespace OliveJsonRpc
{
	// ==========================================
	// Standard JSON-RPC 2.0 Error Codes
	// ==========================================

	/** Parse error - Invalid JSON was received */
	constexpr int32 PARSE_ERROR = -32700;

	/** Invalid Request - The JSON sent is not a valid Request object */
	constexpr int32 INVALID_REQUEST = -32600;

	/** Method not found - The method does not exist */
	constexpr int32 METHOD_NOT_FOUND = -32601;

	/** Invalid params - Invalid method parameters */
	constexpr int32 INVALID_PARAMS = -32602;

	/** Internal error - Internal JSON-RPC error */
	constexpr int32 INTERNAL_ERROR = -32603;

	// ==========================================
	// MCP-Specific Error Codes (-32000 to -32099)
	// ==========================================

	/** Tool not found */
	constexpr int32 TOOL_NOT_FOUND = -32000;

	/** Tool execution error */
	constexpr int32 TOOL_EXECUTION_ERROR = -32001;

	/** Resource not found */
	constexpr int32 RESOURCE_NOT_FOUND = -32002;

	/** Client not initialized */
	constexpr int32 NOT_INITIALIZED = -32003;

	/** Server busy */
	constexpr int32 SERVER_BUSY = -32004;

	/** Timeout */
	constexpr int32 TIMEOUT = -32005;

	// ==========================================
	// Request Parsing
	// ==========================================

	/**
	 * Parse a JSON-RPC request from string
	 * @param JsonString Raw JSON string
	 * @param OutError Error message if parsing fails
	 * @return Parsed JSON object, nullptr on failure
	 */
	OLIVEAIEDITOR_API TSharedPtr<FJsonObject> ParseRequest(const FString& JsonString, FString& OutError);

	/**
	 * Validate that a JSON object is a valid JSON-RPC 2.0 request
	 * @param Request JSON object to validate
	 * @param OutError Error message if invalid
	 * @return true if valid
	 */
	OLIVEAIEDITOR_API bool ValidateRequest(const TSharedPtr<FJsonObject>& Request, FString& OutError);

	/**
	 * Extract request ID (can be string, number, or null)
	 * @param Request JSON-RPC request
	 * @return ID as JSON value, or nullptr
	 */
	OLIVEAIEDITOR_API TSharedPtr<FJsonValue> GetRequestId(const TSharedPtr<FJsonObject>& Request);

	/**
	 * Get the method name from a request
	 * @param Request JSON-RPC request
	 * @return Method name, empty if not found
	 */
	OLIVEAIEDITOR_API FString GetMethod(const TSharedPtr<FJsonObject>& Request);

	/**
	 * Get the params from a request
	 * @param Request JSON-RPC request
	 * @return Params as JSON object, or nullptr if not present
	 */
	OLIVEAIEDITOR_API TSharedPtr<FJsonObject> GetParams(const TSharedPtr<FJsonObject>& Request);

	// ==========================================
	// Response Building
	// ==========================================

	/**
	 * Create a success response
	 * @param Id Request ID
	 * @param Result Result object
	 * @return Complete JSON-RPC response
	 */
	OLIVEAIEDITOR_API TSharedPtr<FJsonObject> CreateResponse(
		const TSharedPtr<FJsonValue>& Id,
		const TSharedPtr<FJsonObject>& Result
	);

	/**
	 * Create a success response with array result
	 * @param Id Request ID
	 * @param Result Result array
	 * @return Complete JSON-RPC response
	 */
	OLIVEAIEDITOR_API TSharedPtr<FJsonObject> CreateArrayResponse(
		const TSharedPtr<FJsonValue>& Id,
		const TArray<TSharedPtr<FJsonValue>>& Result
	);

	/**
	 * Create an error response
	 * @param Id Request ID (can be nullptr)
	 * @param Code Error code
	 * @param Message Error message
	 * @param Data Additional error data (optional)
	 * @return Complete JSON-RPC error response
	 */
	OLIVEAIEDITOR_API TSharedPtr<FJsonObject> CreateErrorResponse(
		const TSharedPtr<FJsonValue>& Id,
		int32 Code,
		const FString& Message,
		const TSharedPtr<FJsonObject>& Data = nullptr
	);

	/**
	 * Create a notification (no ID, no response expected)
	 * @param Method Notification method name
	 * @param Params Notification parameters
	 * @return JSON-RPC notification
	 */
	OLIVEAIEDITOR_API TSharedPtr<FJsonObject> CreateNotification(
		const FString& Method,
		const TSharedPtr<FJsonObject>& Params = nullptr
	);

	// ==========================================
	// Serialization
	// ==========================================

	/**
	 * Serialize JSON object to string
	 * @param Json JSON object to serialize
	 * @return JSON string
	 */
	OLIVEAIEDITOR_API FString Serialize(const TSharedPtr<FJsonObject>& Json);

	/**
	 * Check if a request is a notification (no ID)
	 * @param Request JSON-RPC request
	 * @return true if notification
	 */
	OLIVEAIEDITOR_API bool IsNotification(const TSharedPtr<FJsonObject>& Request);

	// ==========================================
	// Error Message Helpers
	// ==========================================

	/**
	 * Get human-readable message for error code
	 * @param Code JSON-RPC error code
	 * @return Human-readable message
	 */
	OLIVEAIEDITOR_API FString GetErrorMessage(int32 Code);
}
