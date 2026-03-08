// Copyright Bode Software. All Rights Reserved.

/**
 * OliveClassAPIHelper.cpp
 *
 * Implementation of the static class API enumeration helper.
 * Provides Blueprint-visible function and property listings for a UClass,
 * used for FUNCTION_NOT_FOUND error suggestions and component API map injection.
 */

#include "Writer/OliveClassAPIHelper.h"
#include "UObject/UnrealType.h"
#include "UObject/Class.h"

// ============================================================================
// Function Name Filter
// ============================================================================

bool FOliveClassAPIHelper::ShouldFilterFunction(const FString& FuncName)
{
	// Prefix-based filter list: internal UE functions not useful for Blueprint authors
	static const TArray<FString> FilteredPrefixes = {
		TEXT("DEPRECATED_"),
		TEXT("Internal_"),
		TEXT("PostEditChange"),
		TEXT("Serialize"),
		TEXT("BeginDestroy"),
		TEXT("FinishDestroy"),
		TEXT("PostInitProperties"),
		TEXT("PostLoad"),
		TEXT("AddReferencedObjects"),
		TEXT("GetLifetimeReplicatedProps"),
		TEXT("OnRep_"),
		TEXT("ReceiveTick"),
		TEXT("ReceiveBeginPlay"),
		TEXT("ReceiveEndPlay"),
		TEXT("ReceiveAnyDamage"),
		TEXT("ReceivePointDamage"),
		TEXT("ReceiveRadialDamage"),
		TEXT("ReceiveActorBeginOverlap"),
		TEXT("ReceiveActorEndOverlap"),
		TEXT("ReceiveHit"),
		TEXT("ReceiveDestroyed"),
		TEXT("UserConstructionScript"),
	};

	for (const FString& Prefix : FilteredPrefixes)
	{
		if (FuncName.StartsWith(Prefix))
		{
			return true;
		}
	}

	return false;
}

// ============================================================================
// Property Name Filter
// ============================================================================

bool FOliveClassAPIHelper::ShouldFilterProperty(const FString& PropName)
{
	return PropName.StartsWith(TEXT("bRep_"))
		|| PropName.StartsWith(TEXT("Rep_"))
		|| PropName.StartsWith(TEXT("DEPRECATED_"))
		|| PropName.StartsWith(TEXT("bNet"));
}

// ============================================================================
// Property Type String
// ============================================================================

FString FOliveClassAPIHelper::GetPropertyTypeString(const FProperty* Property)
{
	if (!Property)
	{
		return TEXT("unknown");
	}

	// Basic types
	if (Property->IsA<FBoolProperty>())
	{
		return TEXT("bool");
	}
	if (Property->IsA<FByteProperty>())
	{
		const FByteProperty* ByteProp = CastField<FByteProperty>(Property);
		if (ByteProp->Enum)
		{
			return ByteProp->Enum->GetName();
		}
		return TEXT("uint8");
	}
	if (Property->IsA<FIntProperty>())
	{
		return TEXT("int32");
	}
	if (Property->IsA<FInt64Property>())
	{
		return TEXT("int64");
	}
	if (Property->IsA<FFloatProperty>())
	{
		return TEXT("float");
	}
	if (Property->IsA<FDoubleProperty>())
	{
		return TEXT("double");
	}
	if (Property->IsA<FStrProperty>())
	{
		return TEXT("FString");
	}
	if (Property->IsA<FNameProperty>())
	{
		return TEXT("FName");
	}
	if (Property->IsA<FTextProperty>())
	{
		return TEXT("FText");
	}

	// Enum property
	if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
	{
		if (EnumProp->GetEnum())
		{
			return EnumProp->GetEnum()->GetName();
		}
		return TEXT("enum");
	}

	// Struct property
	if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
	{
		if (StructProp->Struct)
		{
			return FString::Printf(TEXT("F%s"), *StructProp->Struct->GetName());
		}
		return TEXT("struct");
	}

	// Object property
	if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Property))
	{
		if (ObjProp->PropertyClass)
		{
			return FString::Printf(TEXT("U%s*"), *ObjProp->PropertyClass->GetName());
		}
		return TEXT("UObject*");
	}

	// Array property
	if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
	{
		const FString InnerType = GetPropertyTypeString(ArrayProp->Inner);
		return FString::Printf(TEXT("TArray<%s>"), *InnerType);
	}

	// Set property
	if (const FSetProperty* SetProp = CastField<FSetProperty>(Property))
	{
		const FString ElemType = GetPropertyTypeString(SetProp->ElementProp);
		return FString::Printf(TEXT("TSet<%s>"), *ElemType);
	}

	// Map property
	if (const FMapProperty* MapProp = CastField<FMapProperty>(Property))
	{
		const FString KeyType = GetPropertyTypeString(MapProp->KeyProp);
		const FString ValueType = GetPropertyTypeString(MapProp->ValueProp);
		return FString::Printf(TEXT("TMap<%s, %s>"), *KeyType, *ValueType);
	}

	// Fallback: use the CPPType metadata
	return Property->GetCPPType();
}

// ============================================================================
// CamelCase Splitter
// ============================================================================

TArray<FString> FOliveClassAPIHelper::SplitCamelCase(const FString& Str)
{
	TArray<FString> Words;
	FString Current;

	for (int32 i = 0; i < Str.Len(); i++)
	{
		const TCHAR Ch = Str[i];
		if (FChar::IsUpper(Ch) && Current.Len() > 0)
		{
			Words.Add(Current.ToLower());
			Current = FString::Chr(Ch);
		}
		else
		{
			Current += Ch;
		}
	}

	if (Current.Len() > 0)
	{
		Words.Add(Current.ToLower());
	}

	return Words;
}

// ============================================================================
// ScoreSimilarity
// ============================================================================

int32 FOliveClassAPIHelper::ScoreSimilarity(const FString& CandidateName, const FString& SearchName)
{
	const FString LowerCandidate = CandidateName.ToLower();
	const FString LowerSearch = SearchName.ToLower();
	const int32 SearchLen = LowerSearch.Len();

	if (SearchLen == 0 || LowerCandidate.Len() == 0)
	{
		return 0;
	}

	// Skip exact matches (would have been found by the primary search)
	if (LowerCandidate == LowerSearch)
	{
		return 0;
	}

	int32 Score = 0;

	// 1. Substring containment (either direction) -- strong signal
	if (LowerCandidate.Contains(LowerSearch))
	{
		Score += 80;
	}
	else if (LowerSearch.Contains(LowerCandidate))
	{
		Score += 70;
	}

	// 2. Common prefix length -- medium signal
	int32 PrefixLen = 0;
	const int32 MinLen = FMath::Min(LowerCandidate.Len(), SearchLen);
	for (int32 i = 0; i < MinLen; i++)
	{
		if (LowerCandidate[i] == LowerSearch[i])
		{
			PrefixLen++;
		}
		else
		{
			break;
		}
	}
	if (PrefixLen >= 3)
	{
		Score += (PrefixLen * 40) / SearchLen;
	}

	// 3. CamelCase word overlap -- handles reordering
	{
		const TArray<FString> SearchWords = SplitCamelCase(SearchName);
		const TArray<FString> CandidateWords = SplitCamelCase(CandidateName);

		int32 CommonWords = 0;
		for (const FString& SW : SearchWords)
		{
			if (SW.Len() < 2)
			{
				continue; // Skip trivial single-char words
			}
			for (const FString& CW : CandidateWords)
			{
				if (SW == CW)
				{
					CommonWords++;
					break;
				}
			}
		}

		if (CommonWords > 0 && SearchWords.Num() > 0)
		{
			Score += (CommonWords * 30) / SearchWords.Num();
		}
	}

	return Score;
}

// ============================================================================
// GetCallableFunctions
// ============================================================================

TArray<FString> FOliveClassAPIHelper::GetCallableFunctions(UClass* Class, int32 MaxResults)
{
	TArray<FString> Result;

	if (!Class)
	{
		return Result;
	}

	TSet<FString> Seen;

	for (TFieldIterator<UFunction> FuncIt(Class, EFieldIteratorFlags::IncludeSuper); FuncIt; ++FuncIt)
	{
		const UFunction* Func = *FuncIt;
		if (!Func)
		{
			continue;
		}

		// Must be Blueprint-callable
		if (!Func->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure))
		{
			continue;
		}

		const FString FuncName = Func->GetName();

		// Dedup
		if (Seen.Contains(FuncName))
		{
			continue;
		}

		// Filter internal/deprecated functions
		if (ShouldFilterFunction(FuncName))
		{
			continue;
		}

		Seen.Add(FuncName);
		Result.Add(FuncName);
	}

	// Sort alphabetically for stable output
	Result.Sort();

	// Cap at MaxResults
	if (MaxResults > 0 && Result.Num() > MaxResults)
	{
		Result.SetNum(MaxResults);
	}

	return Result;
}

// ============================================================================
// GetVisibleProperties
// ============================================================================

TArray<TPair<FString, FString>> FOliveClassAPIHelper::GetVisibleProperties(UClass* Class, int32 MaxResults)
{
	TArray<TPair<FString, FString>> Result;

	if (!Class)
	{
		return Result;
	}

	TSet<FString> Seen;

	for (TFieldIterator<FProperty> PropIt(Class, EFieldIteratorFlags::IncludeSuper); PropIt; ++PropIt)
	{
		const FProperty* Prop = *PropIt;
		if (!Prop)
		{
			continue;
		}

		// Must be Blueprint-visible
		if (!Prop->HasAnyPropertyFlags(CPF_BlueprintVisible | CPF_BlueprintReadOnly))
		{
			continue;
		}

		// Skip deprecated
		if (Prop->HasAnyPropertyFlags(CPF_Deprecated))
		{
			continue;
		}

		const FString PropName = Prop->GetName();

		// Dedup
		if (Seen.Contains(PropName))
		{
			continue;
		}

		// Filter replication internals
		if (ShouldFilterProperty(PropName))
		{
			continue;
		}

		Seen.Add(PropName);
		Result.Add(TPair<FString, FString>(PropName, GetPropertyTypeString(Prop)));
	}

	// Sort alphabetically by name
	Result.Sort([](const TPair<FString, FString>& A, const TPair<FString, FString>& B)
	{
		return A.Key < B.Key;
	});

	// Cap at MaxResults
	if (MaxResults > 0 && Result.Num() > MaxResults)
	{
		Result.SetNum(MaxResults);
	}

	return Result;
}

// ============================================================================
// FormatCompactAPISummary
// ============================================================================

FString FOliveClassAPIHelper::FormatCompactAPISummary(UClass* Class, int32 MaxFunctions, int32 MaxProperties)
{
	if (!Class)
	{
		return FString();
	}

	const TArray<FString> Functions = GetCallableFunctions(Class, MaxFunctions);
	const TArray<TPair<FString, FString>> Properties = GetVisibleProperties(Class, MaxProperties);

	if (Functions.Num() == 0 && Properties.Num() == 0)
	{
		return FString();
	}

	FString Output;

	// Class header
	Output += FString::Printf(TEXT("### %s\n"), *Class->GetName());

	// Functions line
	if (Functions.Num() > 0)
	{
		Output += TEXT("Functions: ");
		Output += FString::Join(Functions, TEXT(", "));
		Output += TEXT("\n");
	}

	// Properties line
	if (Properties.Num() > 0)
	{
		Output += TEXT("Properties: ");
		for (int32 i = 0; i < Properties.Num(); i++)
		{
			if (i > 0)
			{
				Output += TEXT(", ");
			}
			Output += FString::Printf(TEXT("%s (%s)"), *Properties[i].Key, *Properties[i].Value);
		}
		Output += TEXT("\n");
	}

	return Output;
}

// ============================================================================
// BuildScopedSuggestions
// ============================================================================

FString FOliveClassAPIHelper::BuildScopedSuggestions(
	UClass* TargetClass,
	const FString& FailedFunctionName,
	int32 MaxFunctions,
	int32 MaxProperties)
{
	if (!TargetClass || FailedFunctionName.IsEmpty())
	{
		return FString();
	}

	const FString ClassName = TargetClass->GetName();
	const FString LowerSearch = FailedFunctionName.ToLower();

	// --- Score and rank functions ---
	struct FScoredEntry
	{
		FString Name;
		int32 Score;
	};

	TArray<FScoredEntry> ScoredFunctions;
	TSet<FString> SeenFunctions;

	for (TFieldIterator<UFunction> FuncIt(TargetClass, EFieldIteratorFlags::IncludeSuper); FuncIt; ++FuncIt)
	{
		const UFunction* Func = *FuncIt;
		if (!Func || !Func->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure))
		{
			continue;
		}

		const FString FuncName = Func->GetName();
		if (SeenFunctions.Contains(FuncName) || ShouldFilterFunction(FuncName))
		{
			continue;
		}
		SeenFunctions.Add(FuncName);

		const int32 Score = ScoreSimilarity(FuncName, FailedFunctionName);
		if (Score >= 15)
		{
			ScoredFunctions.Add({FuncName, Score});
		}
	}

	// Sort by score descending
	ScoredFunctions.Sort([](const FScoredEntry& A, const FScoredEntry& B)
	{
		return A.Score > B.Score;
	});

	if (ScoredFunctions.Num() > MaxFunctions)
	{
		ScoredFunctions.SetNum(MaxFunctions);
	}

	// --- Score and rank properties ---
	TArray<FScoredEntry> ScoredProperties;
	TArray<TPair<FString, FString>> PropertyTypes; // parallel array for type info
	TSet<FString> SeenProperties;

	for (TFieldIterator<FProperty> PropIt(TargetClass, EFieldIteratorFlags::IncludeSuper); PropIt; ++PropIt)
	{
		const FProperty* Prop = *PropIt;
		if (!Prop)
		{
			continue;
		}

		if (!Prop->HasAnyPropertyFlags(CPF_BlueprintVisible | CPF_BlueprintReadOnly))
		{
			continue;
		}
		if (Prop->HasAnyPropertyFlags(CPF_Deprecated))
		{
			continue;
		}

		const FString PropName = Prop->GetName();
		if (SeenProperties.Contains(PropName) || ShouldFilterProperty(PropName))
		{
			continue;
		}
		SeenProperties.Add(PropName);

		const int32 Score = ScoreSimilarity(PropName, FailedFunctionName);
		if (Score >= 15)
		{
			ScoredProperties.Add({PropName, Score});
			PropertyTypes.Add(TPair<FString, FString>(PropName, GetPropertyTypeString(Prop)));
		}
	}

	// Sort properties by score descending
	// We need to keep PropertyTypes in sync, so sort indices
	TArray<int32> PropIndices;
	for (int32 i = 0; i < ScoredProperties.Num(); i++)
	{
		PropIndices.Add(i);
	}
	PropIndices.Sort([&ScoredProperties](int32 A, int32 B)
	{
		return ScoredProperties[A].Score > ScoredProperties[B].Score;
	});

	// Build sorted property list
	TArray<FScoredEntry> SortedProperties;
	TArray<TPair<FString, FString>> SortedPropertyTypes;
	for (int32 Idx : PropIndices)
	{
		SortedProperties.Add(ScoredProperties[Idx]);
		SortedPropertyTypes.Add(PropertyTypes[Idx]);
	}

	if (SortedProperties.Num() > MaxProperties)
	{
		SortedProperties.SetNum(MaxProperties);
		SortedPropertyTypes.SetNum(MaxProperties);
	}

	// If nothing matched at all, return empty
	if (ScoredFunctions.Num() == 0 && SortedProperties.Num() == 0)
	{
		return FString();
	}

	// --- Build output ---
	FString Output;
	Output += FString::Printf(TEXT("Available on %s:\n"), *ClassName);

	// Function suggestions
	if (ScoredFunctions.Num() > 0)
	{
		Output += TEXT("Functions: ");
		for (int32 i = 0; i < ScoredFunctions.Num(); i++)
		{
			if (i > 0)
			{
				Output += TEXT(", ");
			}
			Output += ScoredFunctions[i].Name;
		}
		Output += TEXT("\n");
	}

	// Property suggestions
	if (SortedProperties.Num() > 0)
	{
		Output += TEXT("Properties: ");
		for (int32 i = 0; i < SortedProperties.Num(); i++)
		{
			if (i > 0)
			{
				Output += TEXT(", ");
			}
			Output += FString::Printf(TEXT("%s (%s)"),
				*SortedProperties[i].Name, *SortedPropertyTypes[i].Value);
		}
		Output += TEXT("\n");
	}

	// --- Cross-match detection ---
	// Check if any property name is contained in the search name or vice versa
	// This catches patterns like "SetSpeed" -> "MaxSpeed" (both contain "Speed")
	for (int32 i = 0; i < SortedProperties.Num(); i++)
	{
		const FString LowerProp = SortedProperties[i].Name.ToLower();

		// Check if the search name contains the property name or vice versa
		if (LowerSearch.Contains(LowerProp) || LowerProp.Contains(LowerSearch))
		{
			Output += FString::Printf(
				TEXT("Likely fix: '%s' is a property (%s), not a function. ")
				TEXT("Use op:set_var target:%s instead of op:call target:%s.\n"),
				*SortedProperties[i].Name,
				*SortedPropertyTypes[i].Value,
				*SortedProperties[i].Name,
				*FailedFunctionName);
			break; // Only show the first (highest-scored) cross-match
		}

		// Also check CamelCase word overlap for less obvious matches
		// e.g., "SetMaxSpeed" -> "MaxSpeed" (word "speed" overlap, word "max" overlap)
		const TArray<FString> SearchWords = SplitCamelCase(FailedFunctionName);
		const TArray<FString> PropWords = SplitCamelCase(SortedProperties[i].Name);

		int32 CommonWords = 0;
		int32 NonTrivialSearchWords = 0;
		for (const FString& SW : SearchWords)
		{
			if (SW.Len() < 2)
			{
				continue;
			}
			NonTrivialSearchWords++;
			for (const FString& PW : PropWords)
			{
				if (SW == PW)
				{
					CommonWords++;
					break;
				}
			}
		}

		// If most non-trivial words match, suggest the property
		if (NonTrivialSearchWords > 0 && CommonWords >= NonTrivialSearchWords / 2 + 1)
		{
			Output += FString::Printf(
				TEXT("Likely fix: '%s' is a property (%s), not a function. ")
				TEXT("Use op:set_var target:%s instead of op:call target:%s.\n"),
				*SortedProperties[i].Name,
				*SortedPropertyTypes[i].Value,
				*SortedProperties[i].Name,
				*FailedFunctionName);
			break;
		}
	}

	return Output;
}
