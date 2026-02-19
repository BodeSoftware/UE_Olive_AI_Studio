// Copyright Bode Software. All Rights Reserved.

#include "Services/OliveErrorBuilder.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// Define error codes
const FString FOliveErrorBuilder::ERR_NOT_FOUND = TEXT("ASSET_NOT_FOUND");
const FString FOliveErrorBuilder::ERR_TYPE_CONSTRAINT = TEXT("TYPE_CONSTRAINT_VIOLATION");
const FString FOliveErrorBuilder::ERR_INVALID_PARAMS = TEXT("INVALID_PARAMETERS");
const FString FOliveErrorBuilder::ERR_PIE_ACTIVE = TEXT("PIE_ACTIVE");
const FString FOliveErrorBuilder::ERR_COMPILE_FAILED = TEXT("COMPILATION_FAILED");
const FString FOliveErrorBuilder::ERR_INTERNAL = TEXT("INTERNAL_ERROR");
const FString FOliveErrorBuilder::ERR_NOT_IMPLEMENTED = TEXT("NOT_IMPLEMENTED");
const FString FOliveErrorBuilder::ERR_PERMISSION_DENIED = TEXT("PERMISSION_DENIED");
const FString FOliveErrorBuilder::ERR_TIMEOUT = TEXT("TIMEOUT");
const FString FOliveErrorBuilder::ERR_RATE_LIMITED = TEXT("RATE_LIMITED");
const FString FOliveErrorBuilder::ERR_NETWORK = TEXT("NETWORK_ERROR");
const FString FOliveErrorBuilder::ERR_AUTH = TEXT("AUTHENTICATION_ERROR");
const FString FOliveErrorBuilder::ERR_TOOL_NOT_FOUND = TEXT("TOOL_NOT_FOUND");

TSharedPtr<FJsonObject> FOliveErrorBuilder::Success(const TSharedPtr<FJsonObject>& Data)
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("success"), true);

	if (Data.IsValid())
	{
		Response->SetObjectField(TEXT("data"), Data);
	}

	return Response;
}

TSharedPtr<FJsonObject> FOliveErrorBuilder::Error(
	const FString& Code,
	const FString& Message,
	const FString& Suggestion,
	const TMap<FString, FString>& Details)
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("success"), false);

	TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
	ErrorObj->SetStringField(TEXT("code"), Code);
	ErrorObj->SetStringField(TEXT("message"), Message);

	if (!Suggestion.IsEmpty())
	{
		ErrorObj->SetStringField(TEXT("suggestion"), Suggestion);
	}

	if (Details.Num() > 0)
	{
		TSharedPtr<FJsonObject> DetailsObj = MakeShared<FJsonObject>();
		for (const auto& Pair : Details)
		{
			DetailsObj->SetStringField(Pair.Key, Pair.Value);
		}
		ErrorObj->SetObjectField(TEXT("details"), DetailsObj);
	}

	ErrorObj->SetStringField(TEXT("severity"), TEXT("error"));

	Response->SetObjectField(TEXT("error"), ErrorObj);

	return Response;
}

TSharedPtr<FJsonObject> FOliveErrorBuilder::FromValidationResult(const FOliveValidationResult& Result)
{
	if (Result.bValid && !Result.HasErrors())
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

		// Include warnings if any
		if (Result.HasWarnings())
		{
			TArray<FString> WarningMessages;
			for (const FOliveIRMessage& Msg : Result.GetWarnings())
			{
				WarningMessages.Add(Msg.Message);
			}
			return Warning(Data, WarningMessages);
		}

		return Success(Data);
	}

	// Build error response from first error
	TArray<FOliveIRMessage> Errors = Result.GetErrors();
	if (Errors.Num() > 0)
	{
		const FOliveIRMessage& FirstError = Errors[0];

		TMap<FString, FString> Details;
		for (const auto& Pair : FirstError.Details)
		{
			Details.Add(Pair.Key, Pair.Value);
		}

		// Include additional error count if more than one
		if (Errors.Num() > 1)
		{
			Details.Add(TEXT("additional_errors"), FString::FromInt(Errors.Num() - 1));
		}

		return Error(FirstError.Code, FirstError.Message, FirstError.Suggestion, Details);
	}

	// Fallback
	return Error(ERR_INTERNAL, TEXT("Validation failed for unknown reason"));
}

TSharedPtr<FJsonObject> FOliveErrorBuilder::Warning(
	const TSharedPtr<FJsonObject>& Data,
	const TArray<FString>& Warnings)
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("success"), true);

	if (Data.IsValid())
	{
		Response->SetObjectField(TEXT("data"), Data);
	}

	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarningsArray;
		for (const FString& Warning : Warnings)
		{
			WarningsArray.Add(MakeShared<FJsonValueString>(Warning));
		}
		Response->SetArrayField(TEXT("warnings"), WarningsArray);
	}

	return Response;
}

FString FOliveErrorBuilder::ToJsonString(const TSharedPtr<FJsonObject>& Response)
{
	if (!Response.IsValid())
	{
		return TEXT("{}");
	}

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Response.ToSharedRef(), Writer);
	return OutputString;
}

TSharedPtr<FJsonObject> FOliveErrorBuilder::FromException(const FString& ExceptionMessage)
{
	return Error(
		ERR_INTERNAL,
		FString::Printf(TEXT("An exception occurred: %s"), *ExceptionMessage),
		TEXT("This is likely a bug. Please report it.")
	);
}

bool FOliveErrorBuilder::IsSuccess(const TSharedPtr<FJsonObject>& Response)
{
	if (!Response.IsValid())
	{
		return false;
	}

	return Response->GetBoolField(TEXT("success"));
}

FString FOliveErrorBuilder::GetErrorCode(const TSharedPtr<FJsonObject>& Response)
{
	if (!Response.IsValid())
	{
		return TEXT("");
	}

	const TSharedPtr<FJsonObject>* ErrorObj;
	if (Response->TryGetObjectField(TEXT("error"), ErrorObj))
	{
		return (*ErrorObj)->GetStringField(TEXT("code"));
	}

	return TEXT("");
}
