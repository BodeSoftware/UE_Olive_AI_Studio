// Copyright Olive AI Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

struct FOliveToolResult;

/**
 * Shared parameter extraction helpers for tool handlers.
 * Eliminates repetitive TryGetStringField + error construction boilerplate.
 */
namespace OliveToolParamHelpers
{
	/** Returns the string value for Key, or empty string + sets OutError if missing/empty */
	FString GetRequiredString(const TSharedPtr<FJsonObject>& Params, const FString& Key, FString& OutError);

	/** Returns string value or Default if missing */
	FString GetOptionalString(const TSharedPtr<FJsonObject>& Params, const FString& Key, const FString& Default = TEXT(""));

	/** Returns int value or Default if missing */
	int32 GetOptionalInt(const TSharedPtr<FJsonObject>& Params, const FString& Key, int32 Default = 0);

	/** Returns bool value or Default if missing */
	bool GetOptionalBool(const TSharedPtr<FJsonObject>& Params, const FString& Key, bool Default = false);

	/** Parse a "properties" sub-object into a key-value string map */
	TMap<FString, FString> ParseNodeProperties(const TSharedPtr<FJsonObject>& Params);

	/** Build a standard missing-parameter error result */
	FOliveToolResult MissingParamError(const FString& ParamName, const FString& Hint = TEXT(""));
}
