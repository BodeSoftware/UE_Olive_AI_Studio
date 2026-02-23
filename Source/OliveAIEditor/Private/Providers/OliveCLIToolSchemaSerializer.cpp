// Copyright Bode Software. All Rights Reserved.

/**
 * OliveCLIToolSchemaSerializer.cpp
 *
 * Implementation of the CLI tool schema serializer. Converts FOliveToolDefinition
 * arrays into compact, readable text for CLI provider system prompts.
 */

#include "Providers/OliveCLIToolSchemaSerializer.h"
#include "MCP/OliveToolRegistry.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"

// ==========================================
// Constants
// ==========================================

namespace OliveCLISchemaConstants
{
	/** Category name used for tools that have no Category set */
	static const FString DefaultCategory = TEXT("general");
}

// ==========================================
// Anonymous namespace helpers
// ==========================================

namespace
{
	/**
	 * Format a default-value annotation string from a JSON value.
	 * Returns strings like: [default: "foo"], [default: 42], [default: true]
	 *
	 * @param DefaultVal The JSON value representing the default
	 * @return Annotation string including brackets, or empty if value type is unsupported
	 */
	FString FormatDefaultAnnotation(const TSharedPtr<FJsonValue>& DefaultVal)
	{
		if (!DefaultVal.IsValid())
		{
			return FString();
		}

		switch (DefaultVal->Type)
		{
		case EJson::String:
		{
			FString StrVal;
			DefaultVal->TryGetString(StrVal);
			return FString::Printf(TEXT(" [default: \"%s\"]"), *StrVal);
		}

		case EJson::Number:
		{
			double NumVal = 0.0;
			DefaultVal->TryGetNumber(NumVal);
			// Use integer format if the value is whole
			if (FMath::IsNearlyEqual(NumVal, FMath::RoundToDouble(NumVal)))
			{
				return FString::Printf(TEXT(" [default: %d]"), static_cast<int32>(NumVal));
			}
			return FString::Printf(TEXT(" [default: %.2f]"), NumVal);
		}

		case EJson::Boolean:
		{
			bool BoolVal = false;
			DefaultVal->TryGetBool(BoolVal);
			return FString::Printf(TEXT(" [default: %s]"), BoolVal ? TEXT("true") : TEXT("false"));
		}

		default:
			// Complex defaults (arrays, objects) — no annotation, just omit
			return FString();
		}
	}
}

// ==========================================
// Public API
// ==========================================

FString FOliveCLIToolSchemaSerializer::Serialize(const TArray<FOliveToolDefinition>& Tools, bool bCompact)
{
	if (Tools.Num() == 0)
	{
		return FString();
	}

	// Group tools by category
	TMap<FString, TArray<const FOliveToolDefinition*>> CategoryGroups;

	for (const FOliveToolDefinition& Tool : Tools)
	{
		const FString& Category = Tool.Category.IsEmpty()
			? OliveCLISchemaConstants::DefaultCategory
			: Tool.Category;

		CategoryGroups.FindOrAdd(Category).Add(&Tool);
	}

	// Sort category names for deterministic output
	TArray<FString> SortedCategories;
	CategoryGroups.GetKeys(SortedCategories);
	SortedCategories.Sort();

	// Build output
	FString Output;
	Output += TEXT("## Available Tools\n\n");

	bool bFirstCategory = true;
	for (const FString& Category : SortedCategories)
	{
		if (!bFirstCategory)
		{
			Output += TEXT("\n");
		}
		bFirstCategory = false;

		Output += FString::Printf(TEXT("### %s\n"), *Category);

		const TArray<const FOliveToolDefinition*>& GroupTools = CategoryGroups[Category];
		for (const FOliveToolDefinition* Tool : GroupTools)
		{
			Output += SerializeSingleTool(*Tool, bCompact);
		}
	}

	return Output;
}

int32 FOliveCLIToolSchemaSerializer::EstimateTokens(const TArray<FOliveToolDefinition>& Tools)
{
	// Rough heuristic: 1 token per 4 characters
	return Serialize(Tools, false).Len() / 4;
}

// ==========================================
// Private Helpers
// ==========================================

FString FOliveCLIToolSchemaSerializer::SerializeSingleTool(const FOliveToolDefinition& Tool, bool bCompact)
{
	FString ParamList = SerializeParams(Tool.InputSchema);
	FString Result = FString::Printf(TEXT("- %s(%s)\n"), *Tool.Name, *ParamList);

	if (!bCompact && !Tool.Description.IsEmpty())
	{
		Result += FString::Printf(TEXT("  %s\n"), *Tool.Description);
	}

	return Result;
}

FString FOliveCLIToolSchemaSerializer::SerializeParams(const TSharedPtr<FJsonObject>& Schema)
{
	if (!Schema.IsValid())
	{
		return FString();
	}

	// Extract "properties" object
	const TSharedPtr<FJsonObject>* PropertiesPtr = nullptr;
	if (!Schema->TryGetObjectField(TEXT("properties"), PropertiesPtr) || !PropertiesPtr || !PropertiesPtr->IsValid())
	{
		return FString();
	}

	const TSharedPtr<FJsonObject>& Properties = *PropertiesPtr;

	// Extract "required" array into a set for fast lookup
	TSet<FString> RequiredParams;
	const TArray<TSharedPtr<FJsonValue>>* RequiredArray = nullptr;
	if (Schema->TryGetArrayField(TEXT("required"), RequiredArray) && RequiredArray)
	{
		for (const TSharedPtr<FJsonValue>& Val : *RequiredArray)
		{
			FString ParamName;
			if (Val.IsValid() && Val->TryGetString(ParamName))
			{
				RequiredParams.Add(ParamName);
			}
		}
	}

	// Build parameter list — one entry per property
	TArray<FString> ParamStrings;

	for (const auto& Pair : Properties->Values)
	{
		const FString& ParamName = Pair.Key;
		const TSharedPtr<FJsonValue>& ParamValue = Pair.Value;

		FString ParamType = TEXT("string");
		FString Annotation;

		// Extract type and annotation from the property schema object
		if (ParamValue.IsValid() && ParamValue->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject>& ParamObj = ParamValue->AsObject();
			if (ParamObj.IsValid())
			{
				FString TypeStr;
				if (ParamObj->TryGetStringField(TEXT("type"), TypeStr))
				{
					ParamType = TypeStr;
				}

				// Check for default value — takes priority over [required] annotation
				if (ParamObj->HasField(TEXT("default")))
				{
					Annotation = FormatDefaultAnnotation(ParamObj->Values[TEXT("default")]);
				}
				else if (RequiredParams.Contains(ParamName))
				{
					Annotation = TEXT(" [required]");
				}
			}
		}
		else if (RequiredParams.Contains(ParamName))
		{
			// Non-object property value but listed as required
			Annotation = TEXT(" [required]");
		}

		ParamStrings.Add(FString::Printf(TEXT("%s: %s%s"), *ParamName, *ParamType, *Annotation));
	}

	return FString::Join(ParamStrings, TEXT(", "));
}
