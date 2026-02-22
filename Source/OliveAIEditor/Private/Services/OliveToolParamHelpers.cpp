// Copyright Olive AI Studio. All Rights Reserved.

#include "Services/OliveToolParamHelpers.h"
#include "MCP/OliveToolRegistry.h"

FString OliveToolParamHelpers::GetRequiredString(const TSharedPtr<FJsonObject>& Params, const FString& Key, FString& OutError)
{
	if (!Params.IsValid() || !Params->HasField(Key))
	{
		OutError = FString::Printf(TEXT("Missing required parameter: %s"), *Key);
		return FString();
	}

	FString Value;
	if (!Params->TryGetStringField(Key, Value) || Value.IsEmpty())
	{
		OutError = FString::Printf(TEXT("Missing required parameter: %s"), *Key);
		return FString();
	}

	return Value;
}

FString OliveToolParamHelpers::GetOptionalString(const TSharedPtr<FJsonObject>& Params, const FString& Key, const FString& Default)
{
	if (!Params.IsValid() || !Params->HasField(Key))
	{
		return Default;
	}

	FString Value;
	if (Params->TryGetStringField(Key, Value))
	{
		return Value;
	}

	return Default;
}

int32 OliveToolParamHelpers::GetOptionalInt(const TSharedPtr<FJsonObject>& Params, const FString& Key, int32 Default)
{
	if (!Params.IsValid() || !Params->HasField(Key))
	{
		return Default;
	}

	double Value;
	if (Params->TryGetNumberField(Key, Value))
	{
		return static_cast<int32>(Value);
	}

	return Default;
}

bool OliveToolParamHelpers::GetOptionalBool(const TSharedPtr<FJsonObject>& Params, const FString& Key, bool Default)
{
	if (!Params.IsValid() || !Params->HasField(Key))
	{
		return Default;
	}

	bool Value;
	if (Params->TryGetBoolField(Key, Value))
	{
		return Value;
	}

	return Default;
}

TMap<FString, FString> OliveToolParamHelpers::ParseNodeProperties(const TSharedPtr<FJsonObject>& Params)
{
	TMap<FString, FString> Result;

	if (!Params.IsValid())
	{
		return Result;
	}

	const TSharedPtr<FJsonObject>* PropertiesObj = nullptr;
	if (Params->TryGetObjectField(TEXT("properties"), PropertiesObj) && PropertiesObj && PropertiesObj->IsValid())
	{
		for (const auto& Pair : (*PropertiesObj)->Values)
		{
			FString StringValue;
			if (Pair.Value.IsValid() && Pair.Value->TryGetString(StringValue))
			{
				Result.Add(Pair.Key, StringValue);
			}
		}
	}

	return Result;
}

FOliveToolResult OliveToolParamHelpers::MissingParamError(const FString& ParamName, const FString& Hint)
{
	FString Suggestion = Hint.IsEmpty()
		? FString::Printf(TEXT("Provide the '%s' parameter"), *ParamName)
		: Hint;

	return FOliveToolResult::Error(
		TEXT("INVALID_PARAMS"),
		FString::Printf(TEXT("Missing required parameter: %s"), *ParamName),
		Suggestion
	);
}
