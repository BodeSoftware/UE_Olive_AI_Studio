// Copyright Bode Software. All Rights Reserved.

#include "MCP/OliveJsonRpc.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace OliveJsonRpc
{
	// ==========================================
	// Request Parsing
	// ==========================================

	TSharedPtr<FJsonObject> ParseRequest(const FString& JsonString, FString& OutError)
	{
		if (JsonString.IsEmpty())
		{
			OutError = TEXT("Empty JSON string");
			return nullptr;
		}

		TSharedPtr<FJsonObject> JsonObject;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);

		if (!FJsonSerializer::Deserialize(Reader, JsonObject))
		{
			OutError = TEXT("Failed to parse JSON");
			return nullptr;
		}

		if (!JsonObject.IsValid())
		{
			OutError = TEXT("Parsed JSON is not an object");
			return nullptr;
		}

		return JsonObject;
	}

	bool ValidateRequest(const TSharedPtr<FJsonObject>& Request, FString& OutError)
	{
		if (!Request.IsValid())
		{
			OutError = TEXT("Request is null");
			return false;
		}

		// Check jsonrpc version
		if (!Request->HasField(TEXT("jsonrpc")))
		{
			OutError = TEXT("Missing 'jsonrpc' field");
			return false;
		}

		FString Version = Request->GetStringField(TEXT("jsonrpc"));
		if (Version != TEXT("2.0"))
		{
			OutError = FString::Printf(TEXT("Invalid JSON-RPC version: %s (expected 2.0)"), *Version);
			return false;
		}

		// Check method
		if (!Request->HasField(TEXT("method")))
		{
			OutError = TEXT("Missing 'method' field");
			return false;
		}

		const TSharedPtr<FJsonValue>& MethodField = Request->TryGetField(TEXT("method"));
		if (!MethodField.IsValid() || MethodField->Type != EJson::String)
		{
			OutError = TEXT("'method' must be a string");
			return false;
		}

		// ID is optional (notification if missing)
		// Params is optional

		// If params is present, must be object or array
		if (Request->HasField(TEXT("params")))
		{
			const TSharedPtr<FJsonValue>& ParamsField = Request->TryGetField(TEXT("params"));
			if (ParamsField.IsValid())
			{
				if (ParamsField->Type != EJson::Object && ParamsField->Type != EJson::Array)
				{
					OutError = TEXT("'params' must be an object or array");
					return false;
				}
			}
		}

		return true;
	}

	TSharedPtr<FJsonValue> GetRequestId(const TSharedPtr<FJsonObject>& Request)
	{
		if (!Request.IsValid())
		{
			return nullptr;
		}

		return Request->TryGetField(TEXT("id"));
	}

	FString GetMethod(const TSharedPtr<FJsonObject>& Request)
	{
		if (!Request.IsValid())
		{
			return TEXT("");
		}

		return Request->GetStringField(TEXT("method"));
	}

	TSharedPtr<FJsonObject> GetParams(const TSharedPtr<FJsonObject>& Request)
	{
		if (!Request.IsValid())
		{
			return nullptr;
		}

		const TSharedPtr<FJsonObject>* Params;
		if (Request->TryGetObjectField(TEXT("params"), Params))
		{
			return *Params;
		}

		return nullptr;
	}

	// ==========================================
	// Response Building
	// ==========================================

	TSharedPtr<FJsonObject> CreateResponse(
		const TSharedPtr<FJsonValue>& Id,
		const TSharedPtr<FJsonObject>& Result)
	{
		TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));

		if (Result.IsValid())
		{
			Response->SetObjectField(TEXT("result"), Result);
		}
		else
		{
			// Empty result object
			Response->SetObjectField(TEXT("result"), MakeShared<FJsonObject>());
		}

		if (Id.IsValid())
		{
			Response->SetField(TEXT("id"), Id);
		}
		else
		{
			Response->SetField(TEXT("id"), MakeShared<FJsonValueNull>());
		}

		return Response;
	}

	TSharedPtr<FJsonObject> CreateArrayResponse(
		const TSharedPtr<FJsonValue>& Id,
		const TArray<TSharedPtr<FJsonValue>>& Result)
	{
		TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		Response->SetArrayField(TEXT("result"), Result);

		if (Id.IsValid())
		{
			Response->SetField(TEXT("id"), Id);
		}
		else
		{
			Response->SetField(TEXT("id"), MakeShared<FJsonValueNull>());
		}

		return Response;
	}

	TSharedPtr<FJsonObject> CreateErrorResponse(
		const TSharedPtr<FJsonValue>& Id,
		int32 Code,
		const FString& Message,
		const TSharedPtr<FJsonObject>& Data)
	{
		TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));

		TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
		ErrorObj->SetNumberField(TEXT("code"), Code);
		ErrorObj->SetStringField(TEXT("message"), Message);

		if (Data.IsValid())
		{
			ErrorObj->SetObjectField(TEXT("data"), Data);
		}

		Response->SetObjectField(TEXT("error"), ErrorObj);

		if (Id.IsValid())
		{
			Response->SetField(TEXT("id"), Id);
		}
		else
		{
			Response->SetField(TEXT("id"), MakeShared<FJsonValueNull>());
		}

		return Response;
	}

	TSharedPtr<FJsonObject> CreateNotification(
		const FString& Method,
		const TSharedPtr<FJsonObject>& Params)
	{
		TSharedPtr<FJsonObject> Notification = MakeShared<FJsonObject>();
		Notification->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		Notification->SetStringField(TEXT("method"), Method);

		if (Params.IsValid())
		{
			Notification->SetObjectField(TEXT("params"), Params);
		}

		// No ID for notifications

		return Notification;
	}

	// ==========================================
	// Serialization
	// ==========================================

	FString Serialize(const TSharedPtr<FJsonObject>& Json)
	{
		if (!Json.IsValid())
		{
			return TEXT("{}");
		}

		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);
		return OutputString;
	}

	bool IsNotification(const TSharedPtr<FJsonObject>& Request)
	{
		if (!Request.IsValid())
		{
			return false;
		}

		return !Request->HasField(TEXT("id"));
	}

	// ==========================================
	// Error Message Helpers
	// ==========================================

	FString GetErrorMessage(int32 Code)
	{
		switch (Code)
		{
		case PARSE_ERROR:
			return TEXT("Parse error");
		case INVALID_REQUEST:
			return TEXT("Invalid request");
		case METHOD_NOT_FOUND:
			return TEXT("Method not found");
		case INVALID_PARAMS:
			return TEXT("Invalid params");
		case INTERNAL_ERROR:
			return TEXT("Internal error");
		case TOOL_NOT_FOUND:
			return TEXT("Tool not found");
		case TOOL_EXECUTION_ERROR:
			return TEXT("Tool execution error");
		case RESOURCE_NOT_FOUND:
			return TEXT("Resource not found");
		case NOT_INITIALIZED:
			return TEXT("Client not initialized");
		case SERVER_BUSY:
			return TEXT("Server busy");
		case TIMEOUT:
			return TEXT("Operation timed out");
		default:
			if (Code >= -32099 && Code <= -32000)
			{
				return TEXT("Server error");
			}
			return TEXT("Unknown error");
		}
	}
}
