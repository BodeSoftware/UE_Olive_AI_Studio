// Copyright Bode Software. All Rights Reserved.

#include "OlivePCGNodeCatalog.h"
#include "PCGSettings.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogOlivePCGCatalog, Log, All);

// FOlivePCGNodeTypeInfo

TSharedPtr<FJsonObject> FOlivePCGNodeTypeInfo::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("class_name"), ClassName);
	Json->SetStringField(TEXT("display_name"), DisplayName);
	Json->SetStringField(TEXT("category"), Category);

	if (!Description.IsEmpty())
	{
		Json->SetStringField(TEXT("description"), Description);
	}

	if (Keywords.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> KeywordsArray;
		for (const FString& Keyword : Keywords)
		{
			KeywordsArray.Add(MakeShared<FJsonValueString>(Keyword));
		}
		Json->SetArrayField(TEXT("keywords"), KeywordsArray);
	}

	if (Properties.Num() > 0)
	{
		TSharedPtr<FJsonObject> PropsJson = MakeShared<FJsonObject>();
		for (const auto& Pair : Properties)
		{
			PropsJson->SetStringField(Pair.Key, Pair.Value);
		}
		Json->SetObjectField(TEXT("properties"), PropsJson);
	}

	return Json;
}

// FOlivePCGNodeCatalog

FOlivePCGNodeCatalog& FOlivePCGNodeCatalog::Get()
{
	static FOlivePCGNodeCatalog Instance;
	return Instance;
}

void FOlivePCGNodeCatalog::Initialize()
{
	if (bInitialized)
	{
		return;
	}

	UE_LOG(LogOlivePCGCatalog, Log, TEXT("Initializing PCG Node Catalog..."));

	// Find all non-abstract subclasses of UPCGSettings
	TArray<UClass*> SettingsClasses;
	GetDerivedClasses(UPCGSettings::StaticClass(), SettingsClasses, true);

	for (UClass* Class : SettingsClasses)
	{
		if (!Class || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
		{
			continue;
		}

		// Skip the base class itself
		if (Class == UPCGSettings::StaticClass())
		{
			continue;
		}

		FOlivePCGNodeTypeInfo Info;
		Info.ClassName = Class->GetName();

		// Extract display name from metadata
#if WITH_EDITOR
		Info.DisplayName = Class->GetMetaData(TEXT("DisplayName"));
		if (Info.DisplayName.IsEmpty())
		{
			// Generate friendly name from class name
			Info.DisplayName = Info.ClassName;
			Info.DisplayName.RemoveFromStart(TEXT("PCG"));
			Info.DisplayName.RemoveFromEnd(TEXT("Settings"));
		}

		Info.Description = Class->GetMetaData(TEXT("ToolTip"));
#else
		Info.DisplayName = Info.ClassName;
#endif

		// Derive category from class hierarchy
		const UClass* ParentClass = Class->GetSuperClass();
		if (ParentClass && ParentClass != UPCGSettings::StaticClass())
		{
			FString ParentName = ParentClass->GetName();
			ParentName.RemoveFromStart(TEXT("PCG"));
			ParentName.RemoveFromEnd(TEXT("Settings"));
			Info.Category = ParentName;
		}
		else
		{
			// Try to derive category from class name
			FString SimpleName = Info.ClassName;
			SimpleName.RemoveFromStart(TEXT("PCG"));
			SimpleName.RemoveFromEnd(TEXT("Settings"));

			if (SimpleName.Contains(TEXT("Sampler"))) Info.Category = TEXT("Sampler");
			else if (SimpleName.Contains(TEXT("Filter"))) Info.Category = TEXT("Filter");
			else if (SimpleName.Contains(TEXT("Spawn"))) Info.Category = TEXT("Spawner");
			else if (SimpleName.Contains(TEXT("Transform"))) Info.Category = TEXT("Transform");
			else if (SimpleName.Contains(TEXT("Debug"))) Info.Category = TEXT("Debug");
			else if (SimpleName.Contains(TEXT("Subgraph"))) Info.Category = TEXT("Subgraph");
			else if (SimpleName.Contains(TEXT("Attribute"))) Info.Category = TEXT("Attribute");
			else if (SimpleName.Contains(TEXT("Density"))) Info.Category = TEXT("Density");
			else Info.Category = TEXT("General");
		}

		// Build keywords from class name parts
		FString SimpleName = Info.ClassName;
		SimpleName.RemoveFromStart(TEXT("PCG"));
		SimpleName.RemoveFromEnd(TEXT("Settings"));
		Info.Keywords.Add(SimpleName);
		Info.Keywords.Add(Info.Category);

		// Extract editable properties
		ExtractPropertiesFromClass(Class, Info.Properties);

		int32 Index = NodeTypes.Num();
		ClassNameIndex.Add(Info.ClassName, Index);
		ClassMap.Add(Info.ClassName, Class);
		NodeTypes.Add(MoveTemp(Info));
	}

	bInitialized = true;
	UE_LOG(LogOlivePCGCatalog, Log, TEXT("PCG Node Catalog initialized: %d node types"), NodeTypes.Num());
}

void FOlivePCGNodeCatalog::Shutdown()
{
	NodeTypes.Empty();
	ClassNameIndex.Empty();
	ClassMap.Empty();
	bInitialized = false;
}

TArray<FOlivePCGNodeTypeInfo> FOlivePCGNodeCatalog::Search(const FString& Query) const
{
	if (Query.IsEmpty())
	{
		return NodeTypes;
	}

	struct FSearchResult
	{
		int32 Index;
		int32 Score;
	};

	TArray<FSearchResult> Results;

	for (int32 i = 0; i < NodeTypes.Num(); ++i)
	{
		int32 Score = ComputeSearchScore(NodeTypes[i], Query);
		if (Score > 0)
		{
			Results.Add({ i, Score });
		}
	}

	// Sort by score descending
	Results.Sort([](const FSearchResult& A, const FSearchResult& B)
	{
		return A.Score > B.Score;
	});

	TArray<FOlivePCGNodeTypeInfo> Output;
	for (const FSearchResult& Result : Results)
	{
		Output.Add(NodeTypes[Result.Index]);
	}

	return Output;
}

TArray<FOlivePCGNodeTypeInfo> FOlivePCGNodeCatalog::GetByCategory(const FString& Category) const
{
	TArray<FOlivePCGNodeTypeInfo> Output;
	for (const FOlivePCGNodeTypeInfo& Info : NodeTypes)
	{
		if (Info.Category.Equals(Category, ESearchCase::IgnoreCase))
		{
			Output.Add(Info);
		}
	}
	return Output;
}

const FOlivePCGNodeTypeInfo* FOlivePCGNodeCatalog::GetByClassName(const FString& ClassName) const
{
	const int32* Index = ClassNameIndex.Find(ClassName);
	if (Index)
	{
		return &NodeTypes[*Index];
	}
	return nullptr;
}

UClass* FOlivePCGNodeCatalog::FindSettingsClass(const FString& Name) const
{
	// Try exact match first
	if (UClass* const* Found = ClassMap.Find(Name))
	{
		return *Found;
	}

	// Try with "Settings" suffix
	FString WithSuffix = Name + TEXT("Settings");
	if (UClass* const* Found = ClassMap.Find(WithSuffix))
	{
		return *Found;
	}

	// Try with "PCG" prefix
	FString WithPrefix = TEXT("PCG") + Name;
	if (UClass* const* Found = ClassMap.Find(WithPrefix))
	{
		return *Found;
	}

	// Try with "PCG" prefix + "Settings" suffix
	FString WithBoth = TEXT("PCG") + Name + TEXT("Settings");
	if (UClass* const* Found = ClassMap.Find(WithBoth))
	{
		return *Found;
	}

	// Try removing "U" prefix if present
	FString WithoutU = Name;
	if (WithoutU.RemoveFromStart(TEXT("U")))
	{
		if (UClass* const* Found = ClassMap.Find(WithoutU))
		{
			return *Found;
		}
	}

	// Case-insensitive search as last resort
	for (const auto& Pair : ClassMap)
	{
		if (Pair.Key.Equals(Name, ESearchCase::IgnoreCase))
		{
			return Pair.Value;
		}
	}

	return nullptr;
}

TSharedPtr<FJsonObject> FOlivePCGNodeCatalog::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> TypesArray;
	for (const FOlivePCGNodeTypeInfo& Info : NodeTypes)
	{
		TypesArray.Add(MakeShared<FJsonValueObject>(Info.ToJson()));
	}
	Json->SetArrayField(TEXT("node_types"), TypesArray);
	Json->SetNumberField(TEXT("count"), NodeTypes.Num());

	return Json;
}

void FOlivePCGNodeCatalog::ExtractPropertiesFromClass(const UClass* Class, TMap<FString, FString>& OutProperties) const
{
	for (TFieldIterator<FProperty> PropIt(Class); PropIt; ++PropIt)
	{
		FProperty* Property = *PropIt;

		// Skip properties from base UPCGSettings and above
		if (Property->GetOwnerClass() == UPCGSettings::StaticClass() ||
			Property->GetOwnerClass() == UObject::StaticClass())
		{
			continue;
		}

		// Only include editable properties
		if (!Property->HasAnyPropertyFlags(CPF_Edit))
		{
			continue;
		}

		OutProperties.Add(Property->GetName(), Property->GetCPPType());
	}
}

int32 FOlivePCGNodeCatalog::ComputeSearchScore(const FOlivePCGNodeTypeInfo& Info, const FString& Query) const
{
	FString LowerQuery = Query.ToLower();
	int32 Score = 0;

	// Exact class name match
	if (Info.ClassName.Equals(Query, ESearchCase::IgnoreCase))
	{
		Score += 100;
	}
	// Class name contains query
	else if (Info.ClassName.Contains(Query, ESearchCase::IgnoreCase))
	{
		Score += 50;
	}

	// Display name match
	if (Info.DisplayName.Contains(Query, ESearchCase::IgnoreCase))
	{
		Score += 40;
	}

	// Category match
	if (Info.Category.Contains(Query, ESearchCase::IgnoreCase))
	{
		Score += 25;
	}

	// Description match
	if (Info.Description.Contains(Query, ESearchCase::IgnoreCase))
	{
		Score += 20;
	}

	// Keyword match
	for (const FString& Keyword : Info.Keywords)
	{
		if (Keyword.Contains(Query, ESearchCase::IgnoreCase))
		{
			Score += 30;
			break;
		}
	}

	return Score;
}
