// Copyright Bode Software. All Rights Reserved.

#include "OliveBTNodeCatalog.h"
#include "OliveBlackboardReader.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FOliveBTNodeCatalog& FOliveBTNodeCatalog::Get()
{
	static FOliveBTNodeCatalog Instance;
	return Instance;
}

void FOliveBTNodeCatalog::Initialize()
{
	if (bInitialized)
	{
		return;
	}

	UE_LOG(LogOliveBTReader, Log, TEXT("Initializing BT Node Catalog..."));

	AllNodes.Empty();
	ClassNameIndex.Empty();

	// Scan all classes
	auto ScanClasses = [this](UClass* BaseClass, const FString& Category)
	{
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* TestClass = *It;

			if (!TestClass->IsChildOf(BaseClass))
			{
				continue;
			}

			if (TestClass->HasAnyClassFlags(CLASS_Abstract))
			{
				continue;
			}

			// Skip the base classes themselves
			if (TestClass == BaseClass)
			{
				continue;
			}

			FOliveBTNodeTypeInfo Info;
			Info.ClassName = TestClass->GetName();
			Info.Category = Category;

			// Get display name from metadata
			Info.DisplayName = TestClass->GetMetaData(TEXT("DisplayName"));
			if (Info.DisplayName.IsEmpty())
			{
				// Generate display name from class name
				Info.DisplayName = Info.ClassName;
				// Strip common prefixes
				Info.DisplayName.RemoveFromStart(TEXT("BTTask_"));
				Info.DisplayName.RemoveFromStart(TEXT("BTDecorator_"));
				Info.DisplayName.RemoveFromStart(TEXT("BTService_"));
			}

			// Get description from tooltip metadata
			Info.Description = TestClass->GetMetaData(TEXT("ToolTip"));
			if (Info.Description.IsEmpty())
			{
				Info.Description = TestClass->GetMetaData(TEXT("ShortTooltip"));
			}

			// Build keywords
			Info.Keywords.Add(Info.ClassName.ToLower());
			Info.Keywords.Add(Info.DisplayName.ToLower());
			Info.Keywords.Add(Category.ToLower());

			// Extract properties
			ExtractPropertiesFromClass(TestClass, Info);

			int32 Index = AllNodes.Num();
			AllNodes.Add(MoveTemp(Info));
			ClassNameIndex.Add(TestClass->GetName(), Index);
		}
	};

	ScanClasses(UBTTaskNode::StaticClass(), TEXT("Task"));
	ScanClasses(UBTDecorator::StaticClass(), TEXT("Decorator"));
	ScanClasses(UBTService::StaticClass(), TEXT("Service"));

	bInitialized = true;

	UE_LOG(LogOliveBTReader, Log, TEXT("BT Node Catalog initialized with %d node types"), AllNodes.Num());
}

void FOliveBTNodeCatalog::Shutdown()
{
	AllNodes.Empty();
	ClassNameIndex.Empty();
	bInitialized = false;

	UE_LOG(LogOliveBTReader, Log, TEXT("BT Node Catalog shutdown"));
}

TArray<FOliveBTNodeTypeInfo> FOliveBTNodeCatalog::Search(const FString& Query) const
{
	TArray<FOliveBTNodeTypeInfo> Results;

	if (Query.IsEmpty())
	{
		return AllNodes;
	}

	FString LowerQuery = Query.ToLower();

	// Score-based fuzzy matching
	struct FScoredResult
	{
		int32 Index;
		int32 Score;
	};

	TArray<FScoredResult> Scored;

	for (int32 i = 0; i < AllNodes.Num(); ++i)
	{
		const FOliveBTNodeTypeInfo& Info = AllNodes[i];
		int32 Score = 0;

		// Exact class name match
		if (Info.ClassName.ToLower() == LowerQuery)
		{
			Score += 100;
		}
		// Class name contains query
		else if (Info.ClassName.ToLower().Contains(LowerQuery))
		{
			Score += 50;
		}

		// Display name match
		if (Info.DisplayName.ToLower().Contains(LowerQuery))
		{
			Score += 40;
		}

		// Description match
		if (Info.Description.ToLower().Contains(LowerQuery))
		{
			Score += 20;
		}

		// Keyword match
		for (const FString& Keyword : Info.Keywords)
		{
			if (Keyword.Contains(LowerQuery))
			{
				Score += 30;
				break;
			}
		}

		if (Score > 0)
		{
			Scored.Add({ i, Score });
		}
	}

	// Sort by score descending
	Scored.Sort([](const FScoredResult& A, const FScoredResult& B)
	{
		return A.Score > B.Score;
	});

	for (const FScoredResult& S : Scored)
	{
		Results.Add(AllNodes[S.Index]);
	}

	return Results;
}

TArray<FOliveBTNodeTypeInfo> FOliveBTNodeCatalog::GetByCategory(const FString& Category) const
{
	TArray<FOliveBTNodeTypeInfo> Results;

	for (const FOliveBTNodeTypeInfo& Info : AllNodes)
	{
		if (Info.Category == Category)
		{
			Results.Add(Info);
		}
	}

	return Results;
}

const FOliveBTNodeTypeInfo* FOliveBTNodeCatalog::GetByClassName(const FString& ClassName) const
{
	const int32* IndexPtr = ClassNameIndex.Find(ClassName);
	if (IndexPtr && AllNodes.IsValidIndex(*IndexPtr))
	{
		return &AllNodes[*IndexPtr];
	}
	return nullptr;
}

void FOliveBTNodeCatalog::ExtractPropertiesFromClass(UClass* NodeClass, FOliveBTNodeTypeInfo& OutInfo)
{
	if (!NodeClass)
	{
		return;
	}

	UObject* CDO = NodeClass->GetDefaultObject();
	if (!CDO)
	{
		return;
	}

	for (TFieldIterator<FProperty> PropIt(NodeClass, EFieldIteratorFlags::IncludeSuper); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;

		// Skip base UBTNode properties
		if (Prop->GetOwnerClass() == UBTNode::StaticClass() ||
			Prop->GetOwnerClass() == UBTCompositeNode::StaticClass())
		{
			continue;
		}

		// Only editable properties
		if (!Prop->HasAnyPropertyFlags(CPF_Edit))
		{
			continue;
		}

		// Check for FBlackboardKeySelector
		if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			if (StructProp->Struct && StructProp->Struct->GetFName() == FName(TEXT("BlackboardKeySelector")))
			{
				OutInfo.BlackboardKeyProperties.Add(Prop->GetName());
				continue;
			}
		}

		// Record property name and type
		FString TypeName = Prop->GetCPPType();
		OutInfo.DefaultProperties.Add(Prop->GetName(), TypeName);
	}
}

TSharedPtr<FJsonObject> FOliveBTNodeCatalog::ToJson() const
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("total_count"), AllNodes.Num());

	TMap<FString, TArray<TSharedPtr<FJsonValue>>> CategorizedNodes;

	for (const FOliveBTNodeTypeInfo& Info : AllNodes)
	{
		CategorizedNodes.FindOrAdd(Info.Category).Add(MakeShared<FJsonValueObject>(Info.ToJson()));
	}

	for (const auto& Pair : CategorizedNodes)
	{
		Root->SetArrayField(Pair.Key.ToLower() + TEXT("s"), Pair.Value);
	}

	return Root;
}

TSharedPtr<FJsonObject> FOliveBTNodeTypeInfo::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("class_name"), ClassName);
	Json->SetStringField(TEXT("display_name"), DisplayName);
	Json->SetStringField(TEXT("category"), Category);

	if (!Description.IsEmpty())
	{
		Json->SetStringField(TEXT("description"), Description);
	}

	if (DefaultProperties.Num() > 0)
	{
		TSharedPtr<FJsonObject> PropsJson = MakeShared<FJsonObject>();
		for (const auto& Pair : DefaultProperties)
		{
			PropsJson->SetStringField(Pair.Key, Pair.Value);
		}
		Json->SetObjectField(TEXT("properties"), PropsJson);
	}

	if (BlackboardKeyProperties.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> BBKeysArray;
		for (const FString& BBKey : BlackboardKeyProperties)
		{
			BBKeysArray.Add(MakeShared<FJsonValueString>(BBKey));
		}
		Json->SetArrayField(TEXT("blackboard_key_properties"), BBKeysArray);
	}

	return Json;
}
