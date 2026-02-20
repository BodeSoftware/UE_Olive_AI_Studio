// Copyright Bode Software. All Rights Reserved.

#include "OliveNodeCatalog.h"
#include "OliveAIEditorModule.h"
#include "Profiles/OliveFocusProfileManager.h"

// UE includes
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_ForEachElementInEnum.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/KismetArrayLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/UObjectIterator.h"
#include "EdGraph/EdGraphPin.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY(LogOliveNodeCatalog);

// ============================================================================
// FOliveNodeTypeInfo Implementation
// ============================================================================

TSharedPtr<FJsonObject> FOliveNodeTypeInfo::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

	// Identity
	Json->SetStringField(TEXT("type_id"), TypeId);
	Json->SetStringField(TEXT("display_name"), DisplayName);
	Json->SetStringField(TEXT("category"), Category);

	if (!Subcategory.IsEmpty())
	{
		Json->SetStringField(TEXT("subcategory"), Subcategory);
	}

	if (!Description.IsEmpty())
	{
		Json->SetStringField(TEXT("description"), Description);
	}

	if (!Usage.IsEmpty())
	{
		Json->SetStringField(TEXT("usage"), Usage);
	}

	// Examples
	if (Examples.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ExamplesArray;
		for (const FString& Example : Examples)
		{
			ExamplesArray.Add(MakeShared<FJsonValueString>(Example));
		}
		Json->SetArrayField(TEXT("examples"), ExamplesArray);
	}

	// Keywords
	if (Keywords.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> KeywordsArray;
		for (const FString& Keyword : Keywords)
		{
			KeywordsArray.Add(MakeShared<FJsonValueString>(Keyword));
		}
		Json->SetArrayField(TEXT("keywords"), KeywordsArray);
	}

	// Pins
	if (InputPins.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> InputsArray;
		for (const FOliveIRPin& Pin : InputPins)
		{
			InputsArray.Add(MakeShared<FJsonValueObject>(Pin.ToJson()));
		}
		Json->SetArrayField(TEXT("input_pins"), InputsArray);
	}

	if (OutputPins.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> OutputsArray;
		for (const FOliveIRPin& Pin : OutputPins)
		{
			OutputsArray.Add(MakeShared<FJsonValueObject>(Pin.ToJson()));
		}
		Json->SetArrayField(TEXT("output_pins"), OutputsArray);
	}

	// Flags
	Json->SetBoolField(TEXT("is_pure"), bIsPure);
	Json->SetBoolField(TEXT("is_latent"), bIsLatent);
	Json->SetBoolField(TEXT("requires_target"), bRequiresTarget);

	if (bIsCompact)
	{
		Json->SetBoolField(TEXT("is_compact"), bIsCompact);
	}
	if (bIsDeprecated)
	{
		Json->SetBoolField(TEXT("is_deprecated"), bIsDeprecated);
	}
	if (bIsEvent)
	{
		Json->SetBoolField(TEXT("is_event"), bIsEvent);
	}

	// Function info
	if (!FunctionName.IsEmpty())
	{
		Json->SetStringField(TEXT("function_name"), FunctionName);
	}
	if (!FunctionClass.IsEmpty())
	{
		Json->SetStringField(TEXT("function_class"), FunctionClass);
	}
	if (!RequiredClass.IsEmpty())
	{
		Json->SetStringField(TEXT("required_class"), RequiredClass);
	}

	// Tags
	if (Tags.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> TagsArray;
		for (const FString& Tag : Tags)
		{
			TagsArray.Add(MakeShared<FJsonValueString>(Tag));
		}
		Json->SetArrayField(TEXT("tags"), TagsArray);
	}

	if (!Source.IsEmpty())
	{
		Json->SetStringField(TEXT("source"), Source);
	}

	return Json;
}

FOliveNodeTypeInfo FOliveNodeTypeInfo::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveNodeTypeInfo Info;

	if (!JsonObject.IsValid())
	{
		return Info;
	}

	Info.TypeId = JsonObject->GetStringField(TEXT("type_id"));
	Info.DisplayName = JsonObject->GetStringField(TEXT("display_name"));
	Info.Category = JsonObject->GetStringField(TEXT("category"));
	Info.Subcategory = JsonObject->GetStringField(TEXT("subcategory"));
	Info.Description = JsonObject->GetStringField(TEXT("description"));
	Info.Usage = JsonObject->GetStringField(TEXT("usage"));

	// Parse arrays
	const TArray<TSharedPtr<FJsonValue>>* ExamplesArray;
	if (JsonObject->TryGetArrayField(TEXT("examples"), ExamplesArray))
	{
		for (const TSharedPtr<FJsonValue>& Value : *ExamplesArray)
		{
			Info.Examples.Add(Value->AsString());
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* KeywordsArray;
	if (JsonObject->TryGetArrayField(TEXT("keywords"), KeywordsArray))
	{
		for (const TSharedPtr<FJsonValue>& Value : *KeywordsArray)
		{
			Info.Keywords.Add(Value->AsString());
		}
	}

	// Flags
	Info.bIsPure = JsonObject->GetBoolField(TEXT("is_pure"));
	Info.bIsLatent = JsonObject->GetBoolField(TEXT("is_latent"));
	Info.bRequiresTarget = JsonObject->GetBoolField(TEXT("requires_target"));
	JsonObject->TryGetBoolField(TEXT("is_compact"), Info.bIsCompact);
	JsonObject->TryGetBoolField(TEXT("is_deprecated"), Info.bIsDeprecated);
	JsonObject->TryGetBoolField(TEXT("is_event"), Info.bIsEvent);

	// Function info
	Info.FunctionName = JsonObject->GetStringField(TEXT("function_name"));
	Info.FunctionClass = JsonObject->GetStringField(TEXT("function_class"));
	Info.RequiredClass = JsonObject->GetStringField(TEXT("required_class"));
	Info.Source = JsonObject->GetStringField(TEXT("source"));

	return Info;
}

int32 FOliveNodeTypeInfo::MatchScore(const FString& Query) const
{
	if (Query.IsEmpty())
	{
		return 0;
	}

	FString LowerQuery = Query.ToLower();
	FString LowerDisplayName = DisplayName.ToLower();
	FString LowerCategory = Category.ToLower();
	FString LowerDescription = Description.ToLower();

	int32 Score = 0;

	// Exact match on display name = highest score
	if (LowerDisplayName == LowerQuery)
	{
		Score = 1000;
	}
	else if (LowerDisplayName.StartsWith(LowerQuery))
	{
		Score = 500;
	}
	else if (LowerDisplayName.Contains(LowerQuery))
	{
		Score = 200;
	}
	else if (LowerCategory.Contains(LowerQuery))
	{
		Score = 100;
	}
	else if (LowerDescription.Contains(LowerQuery))
	{
		Score = 50;
	}

	// Keyword matches
	for (const FString& Keyword : Keywords)
	{
		if (Keyword.ToLower().Contains(LowerQuery))
		{
			Score = FMath::Max(Score, 75);
			break;
		}
	}

	// Function name match
	if (!FunctionName.IsEmpty() && FunctionName.ToLower().Contains(LowerQuery))
	{
		Score = FMath::Max(Score, 150);
	}

	return Score;
}

// ============================================================================
// FOliveNodeCatalog Singleton
// ============================================================================

FOliveNodeCatalog& FOliveNodeCatalog::Get()
{
	static FOliveNodeCatalog Instance;
	return Instance;
}

// ============================================================================
// Lifecycle
// ============================================================================

void FOliveNodeCatalog::Initialize()
{
	if (bInitialized)
	{
		UE_LOG(LogOliveNodeCatalog, Warning, TEXT("Node catalog already initialized"));
		return;
	}

	UE_LOG(LogOliveNodeCatalog, Log, TEXT("Initializing node catalog..."));

	Rebuild();
}

void FOliveNodeCatalog::Rebuild()
{
	if (bIsBuilding)
	{
		UE_LOG(LogOliveNodeCatalog, Warning, TEXT("Catalog rebuild already in progress"));
		return;
	}

	bIsBuilding = true;
	UE_LOG(LogOliveNodeCatalog, Log, TEXT("Building node catalog..."));

	{
		FScopeLock Lock(&CatalogLock);
		NodeTypes.Empty();
		CategoryIndex.Empty();
		ClassIndex.Empty();
		TagIndex.Empty();
	}

	// Build from various sources
	BuildFromK2NodeClasses();
	BuildFromFunctionLibraries();

	// Add manual entries for commonly used nodes
	AddBuiltInFlowControlNodes();
	AddMathNodes();
	AddStringNodes();
	AddArrayNodes();
	AddUtilityNodes();

	// Build lookup indexes
	BuildIndexes();

	bIsBuilding = false;
	bInitialized = true;

	UE_LOG(LogOliveNodeCatalog, Log, TEXT("Node catalog ready: %d node types indexed"), GetNodeCount());
}

void FOliveNodeCatalog::Shutdown()
{
	FScopeLock Lock(&CatalogLock);
	NodeTypes.Empty();
	CategoryIndex.Empty();
	ClassIndex.Empty();
	TagIndex.Empty();
	bInitialized = false;

	UE_LOG(LogOliveNodeCatalog, Log, TEXT("Node catalog shutdown"));
}

// ============================================================================
// Search & Queries
// ============================================================================

TArray<FOliveNodeTypeInfo> FOliveNodeCatalog::Search(const FString& Query, int32 MaxResults) const
{
	FScopeLock Lock(&CatalogLock);

	TArray<TPair<int32, FOliveNodeTypeInfo>> ScoredResults;
	FString LowerQuery = Query.ToLower();

	for (const auto& Pair : NodeTypes)
	{
		const FOliveNodeTypeInfo& Info = Pair.Value;
		int32 Score = Info.MatchScore(Query);

		if (Score > 0)
		{
			ScoredResults.Add(TPair<int32, FOliveNodeTypeInfo>(Score, Info));
		}
	}

	// Sort by score (descending)
	ScoredResults.Sort([](const auto& A, const auto& B) { return A.Key > B.Key; });

	// Extract results
	TArray<FOliveNodeTypeInfo> Results;
	Results.Reserve(FMath::Min(ScoredResults.Num(), MaxResults));

	for (int32 i = 0; i < FMath::Min(ScoredResults.Num(), MaxResults); i++)
	{
		Results.Add(ScoredResults[i].Value);
	}

	return Results;
}

TArray<FOliveNodeTypeInfo> FOliveNodeCatalog::GetByCategory(const FString& Category) const
{
	FScopeLock Lock(&CatalogLock);

	TArray<FOliveNodeTypeInfo> Results;

	const TArray<FString>* TypeIds = CategoryIndex.Find(Category);
	if (TypeIds)
	{
		Results.Reserve(TypeIds->Num());
		for (const FString& TypeId : *TypeIds)
		{
			const FOliveNodeTypeInfo* Info = NodeTypes.Find(TypeId);
			if (Info)
			{
				Results.Add(*Info);
			}
		}
	}

	return Results;
}

TArray<FOliveNodeTypeInfo> FOliveNodeCatalog::GetForClass(UClass* ContextClass) const
{
	FScopeLock Lock(&CatalogLock);

	TArray<FOliveNodeTypeInfo> Results;

	if (!ContextClass)
	{
		return Results;
	}

	// Collect all class names in the hierarchy
	TArray<FString> ClassNames;
	UClass* CurrentClass = ContextClass;
	while (CurrentClass)
	{
		ClassNames.Add(CurrentClass->GetName());
		CurrentClass = CurrentClass->GetSuperClass();
	}

	// Find all nodes available for these classes
	TSet<FString> AddedTypeIds;
	for (const FString& ClassName : ClassNames)
	{
		const TArray<FString>* TypeIds = ClassIndex.Find(ClassName);
		if (TypeIds)
		{
			for (const FString& TypeId : *TypeIds)
			{
				if (!AddedTypeIds.Contains(TypeId))
				{
					const FOliveNodeTypeInfo* Info = NodeTypes.Find(TypeId);
					if (Info)
					{
						Results.Add(*Info);
						AddedTypeIds.Add(TypeId);
					}
				}
			}
		}
	}

	return Results;
}

TOptional<FOliveNodeTypeInfo> FOliveNodeCatalog::GetNodeType(const FString& TypeId) const
{
	FScopeLock Lock(&CatalogLock);

	const FOliveNodeTypeInfo* Info = NodeTypes.Find(TypeId);
	if (Info)
	{
		return *Info;
	}

	return TOptional<FOliveNodeTypeInfo>();
}

bool FOliveNodeCatalog::HasNodeType(const FString& TypeId) const
{
	FScopeLock Lock(&CatalogLock);
	return NodeTypes.Contains(TypeId);
}

TArray<FString> FOliveNodeCatalog::GetCategories() const
{
	FScopeLock Lock(&CatalogLock);

	TArray<FString> Categories;
	CategoryIndex.GetKeys(Categories);
	Categories.Sort();

	return Categories;
}

int32 FOliveNodeCatalog::GetNodeCount() const
{
	FScopeLock Lock(&CatalogLock);
	return NodeTypes.Num();
}

// ============================================================================
// JSON Export
// ============================================================================

FString FOliveNodeCatalog::ToJson() const
{
	FScopeLock Lock(&CatalogLock);

	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetNumberField(TEXT("total_nodes"), NodeTypes.Num());

	// Add all nodes
	TArray<TSharedPtr<FJsonValue>> NodesArray;
	for (const auto& Pair : NodeTypes)
	{
		NodesArray.Add(MakeShared<FJsonValueObject>(Pair.Value.ToJson()));
	}
	Json->SetArrayField(TEXT("nodes"), NodesArray);

	// Add categories
	TArray<TSharedPtr<FJsonValue>> CategoriesArray;
	for (const auto& Pair : CategoryIndex)
	{
		TSharedPtr<FJsonObject> CatObj = MakeShared<FJsonObject>();
		CatObj->SetStringField(TEXT("name"), Pair.Key);
		CatObj->SetNumberField(TEXT("count"), Pair.Value.Num());
		CategoriesArray.Add(MakeShared<FJsonValueObject>(CatObj));
	}
	Json->SetArrayField(TEXT("categories"), CategoriesArray);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);

	return OutputString;
}

FString FOliveNodeCatalog::ToJsonForProfile(const FString& ProfileName) const
{
	FScopeLock Lock(&CatalogLock);

	// Get profile filtering
	TArray<FString> AllowedCategories = FOliveFocusProfileManager::Get().GetToolCategoriesForProfile(ProfileName);
	TArray<FString> ExcludedTools = FOliveFocusProfileManager::Get().GetExcludedToolsForProfile(ProfileName);

	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("profile"), ProfileName);

	TArray<TSharedPtr<FJsonValue>> NodesArray;
	int32 IncludedCount = 0;

	for (const auto& Pair : NodeTypes)
	{
		const FOliveNodeTypeInfo& Info = Pair.Value;

		// Check if excluded
		if (ExcludedTools.Contains(Info.TypeId))
		{
			continue;
		}

		// Check category filter (empty = all allowed)
		bool bCategoryAllowed = AllowedCategories.Num() == 0;
		if (!bCategoryAllowed)
		{
			for (const FString& AllowedCat : AllowedCategories)
			{
				if (Info.Category.Contains(AllowedCat) || Info.Tags.Contains(AllowedCat))
				{
					bCategoryAllowed = true;
					break;
				}
			}
		}

		if (bCategoryAllowed)
		{
			NodesArray.Add(MakeShared<FJsonValueObject>(Info.ToJson()));
			IncludedCount++;
		}
	}

	Json->SetNumberField(TEXT("total_nodes"), IncludedCount);
	Json->SetArrayField(TEXT("nodes"), NodesArray);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);

	return OutputString;
}

FString FOliveNodeCatalog::SearchToJson(const FString& Query, int32 MaxResults) const
{
	TArray<FOliveNodeTypeInfo> Results = Search(Query, MaxResults);

	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("query"), Query);
	Json->SetNumberField(TEXT("total_results"), Results.Num());

	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	for (const FOliveNodeTypeInfo& Info : Results)
	{
		ResultsArray.Add(MakeShared<FJsonValueObject>(Info.ToJson()));
	}
	Json->SetArrayField(TEXT("results"), ResultsArray);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);

	return OutputString;
}

FString FOliveNodeCatalog::GetCategoriesJson() const
{
	FScopeLock Lock(&CatalogLock);

	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> CategoriesArray;
	for (const auto& Pair : CategoryIndex)
	{
		TSharedPtr<FJsonObject> CatObj = MakeShared<FJsonObject>();
		CatObj->SetStringField(TEXT("name"), Pair.Key);
		CatObj->SetNumberField(TEXT("count"), Pair.Value.Num());
		CategoriesArray.Add(MakeShared<FJsonValueObject>(CatObj));
	}
	Json->SetArrayField(TEXT("categories"), CategoriesArray);
	Json->SetNumberField(TEXT("total_categories"), CategoryIndex.Num());

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);

	return OutputString;
}

// ============================================================================
// Build Methods
// ============================================================================

void FOliveNodeCatalog::BuildFromK2NodeClasses()
{
	UE_LOG(LogOliveNodeCatalog, Verbose, TEXT("Building catalog from K2Node classes..."));

	int32 NodesAdded = 0;

	// Iterate all UK2Node subclasses
	for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
	{
		UClass* Class = *ClassIt;
		if (!Class->IsChildOf(UK2Node::StaticClass()))
		{
			continue;
		}

		// Skip abstract classes
		if (Class->HasAnyClassFlags(CLASS_Abstract))
		{
			continue;
		}

		// Skip internal/deprecated classes
		if (Class->HasAnyClassFlags(CLASS_Deprecated | CLASS_Hidden))
		{
			continue;
		}

		// Create default object to query metadata
		UK2Node* DefaultNode = Class->GetDefaultObject<UK2Node>();
		if (!DefaultNode)
		{
			continue;
		}

		// Skip nodes that explicitly don't want to be in palette
		if (!DefaultNode->ShouldShowNodeProperties())
		{
			continue;
		}

		FOliveNodeTypeInfo Info;
		Info.TypeId = Class->GetName();
		Info.DisplayName = DefaultNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
		Info.Category = DefaultNode->GetMenuCategory().ToString();
		Info.Description = DefaultNode->GetTooltipText().ToString();
		Info.bIsPure = DefaultNode->IsNodePure();
		Info.Source = TEXT("K2Node");

		// Check for latent functions
		if (Class->IsChildOf(UK2Node_CallFunction::StaticClass()))
		{
			// Latent functions have a specific flag
			// This is checked more accurately when we have the actual function
		}

		// Skip if we already have this (prefer more specific entries)
		if (NodeTypes.Contains(Info.TypeId))
		{
			continue;
		}

		// Add tags based on node category
		if (Info.Category.Contains(TEXT("Flow Control")))
		{
			Info.Tags.Add(TEXT("flow"));
			Info.Tags.Add(TEXT("control"));
		}
		else if (Info.Category.Contains(TEXT("Math")))
		{
			Info.Tags.Add(TEXT("math"));
		}
		else if (Info.Category.Contains(TEXT("Variable")))
		{
			Info.Tags.Add(TEXT("variable"));
		}

		AddNodeType(Info);
		NodesAdded++;
	}

	UE_LOG(LogOliveNodeCatalog, Log, TEXT("Added %d nodes from K2Node classes"), NodesAdded);
}

void FOliveNodeCatalog::BuildFromFunctionLibraries()
{
	UE_LOG(LogOliveNodeCatalog, Verbose, TEXT("Building catalog from function libraries..."));

	int32 FunctionsAdded = 0;

	// Find all UBlueprintFunctionLibrary subclasses
	for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
	{
		UClass* Class = *ClassIt;
		if (!Class->IsChildOf(UBlueprintFunctionLibrary::StaticClass()))
		{
			continue;
		}

		// Skip abstract classes
		if (Class->HasAnyClassFlags(CLASS_Abstract))
		{
			continue;
		}

		// Iterate BlueprintCallable functions
		for (TFieldIterator<UFunction> FuncIt(Class, EFieldIteratorFlags::ExcludeSuper); FuncIt; ++FuncIt)
		{
			UFunction* Function = *FuncIt;
			if (!ShouldIncludeFunction(Function))
			{
				continue;
			}

			FOliveNodeTypeInfo Info;
			Info.TypeId = FString::Printf(TEXT("Func_%s_%s"), *Class->GetName(), *Function->GetName());
			Info.FunctionName = Function->GetName();
			Info.FunctionClass = Class->GetName();
			Info.bIsPure = Function->HasAnyFunctionFlags(FUNC_BlueprintPure);
			Info.bIsLatent = Function->HasMetaData(TEXT("Latent"));
			Info.Source = TEXT("FunctionLibrary");

			// Get display name
			Info.DisplayName = Function->GetDisplayNameText().ToString();
			if (Info.DisplayName.IsEmpty())
			{
				Info.DisplayName = Function->GetName();
			}

			// Get category from metadata
			Info.Category = GetFunctionCategory(Function);

			// Get description/tooltip
			Info.Description = Function->GetToolTipText().ToString();

			// Get keywords
			FString Keywords = Function->GetMetaData(TEXT("Keywords"));
			if (!Keywords.IsEmpty())
			{
				TArray<FString> KeywordArray;
				Keywords.ParseIntoArray(KeywordArray, TEXT(","), true);
				for (FString& Keyword : KeywordArray)
				{
					Info.Keywords.Add(Keyword.TrimStartAndEnd());
				}
			}

			// Check if compact node
			Info.bIsCompact = Function->HasMetaData(TEXT("CompactNodeTitle"));

			// Check if deprecated
			Info.bIsDeprecated = Function->HasMetaData(TEXT("DeprecatedFunction"));

			// Build pins from function signature
			BuildPinsFromFunction(Function, Info);

			// Add category-based tags
			if (Info.Category.Contains(TEXT("Math")))
			{
				Info.Tags.Add(TEXT("math"));
			}
			if (Info.Category.Contains(TEXT("String")))
			{
				Info.Tags.Add(TEXT("string"));
			}
			if (Info.Category.Contains(TEXT("Array")))
			{
				Info.Tags.Add(TEXT("array"));
			}
			if (Info.Category.Contains(TEXT("Utilities")))
			{
				Info.Tags.Add(TEXT("utility"));
			}

			AddNodeType(Info);
			FunctionsAdded++;
		}
	}

	UE_LOG(LogOliveNodeCatalog, Log, TEXT("Added %d functions from function libraries"), FunctionsAdded);
}

void FOliveNodeCatalog::AddBuiltInFlowControlNodes()
{
	// Branch
	{
		FOliveNodeTypeInfo Branch;
		Branch.TypeId = TEXT("BuiltIn_Branch");
		Branch.DisplayName = TEXT("Branch");
		Branch.Category = TEXT("Flow Control");
		Branch.Description = TEXT("Execute True or False path based on boolean condition");
		Branch.Usage = TEXT("Use for if/else logic based on a condition. Connect your condition to the Condition pin, then wire your True and False execution paths.");
		Branch.bIsPure = false;
		Branch.Source = TEXT("BuiltIn");
		Branch.Tags.Add(TEXT("flow"));
		Branch.Tags.Add(TEXT("control"));
		Branch.Keywords.Add(TEXT("if"));
		Branch.Keywords.Add(TEXT("else"));
		Branch.Keywords.Add(TEXT("condition"));
		Branch.Examples.Add(TEXT("Check if player health is below 20%, then show low health warning"));

		// Input pins
		FOliveIRPin ExecIn;
		ExecIn.Name = TEXT("execute");
		ExecIn.DisplayName = TEXT("");
		ExecIn.bIsInput = true;
		ExecIn.bIsExec = true;
		Branch.InputPins.Add(ExecIn);

		FOliveIRPin ConditionPin;
		ConditionPin.Name = TEXT("Condition");
		ConditionPin.DisplayName = TEXT("Condition");
		ConditionPin.bIsInput = true;
		ConditionPin.bIsExec = false;
		ConditionPin.Type.Category = EOliveIRTypeCategory::Bool;
		Branch.InputPins.Add(ConditionPin);

		// Output pins
		FOliveIRPin TruePin;
		TruePin.Name = TEXT("True");
		TruePin.DisplayName = TEXT("True");
		TruePin.bIsInput = false;
		TruePin.bIsExec = true;
		Branch.OutputPins.Add(TruePin);

		FOliveIRPin FalsePin;
		FalsePin.Name = TEXT("False");
		FalsePin.DisplayName = TEXT("False");
		FalsePin.bIsInput = false;
		FalsePin.bIsExec = true;
		Branch.OutputPins.Add(FalsePin);

		AddNodeType(Branch);
	}

	// Sequence
	{
		FOliveNodeTypeInfo Sequence;
		Sequence.TypeId = TEXT("BuiltIn_Sequence");
		Sequence.DisplayName = TEXT("Sequence");
		Sequence.Category = TEXT("Flow Control");
		Sequence.Description = TEXT("Execute multiple outputs in order, one after another");
		Sequence.Usage = TEXT("Use when you need to execute multiple actions in sequence from a single event. Add more output pins with the 'Add pin' button.");
		Sequence.bIsPure = false;
		Sequence.Source = TEXT("BuiltIn");
		Sequence.Tags.Add(TEXT("flow"));
		Sequence.Tags.Add(TEXT("control"));
		Sequence.Keywords.Add(TEXT("order"));
		Sequence.Keywords.Add(TEXT("multiple"));
		Sequence.Examples.Add(TEXT("On BeginPlay: First set up variables, then spawn enemies, then start music"));

		FOliveIRPin ExecIn;
		ExecIn.Name = TEXT("execute");
		ExecIn.bIsInput = true;
		ExecIn.bIsExec = true;
		Sequence.InputPins.Add(ExecIn);

		FOliveIRPin Then0;
		Then0.Name = TEXT("Then 0");
		Then0.DisplayName = TEXT("Then 0");
		Then0.bIsInput = false;
		Then0.bIsExec = true;
		Sequence.OutputPins.Add(Then0);

		FOliveIRPin Then1;
		Then1.Name = TEXT("Then 1");
		Then1.DisplayName = TEXT("Then 1");
		Then1.bIsInput = false;
		Then1.bIsExec = true;
		Sequence.OutputPins.Add(Then1);

		AddNodeType(Sequence);
	}

	// Delay
	{
		FOliveNodeTypeInfo Delay;
		Delay.TypeId = TEXT("BuiltIn_Delay");
		Delay.DisplayName = TEXT("Delay");
		Delay.Category = TEXT("Flow Control");
		Delay.Subcategory = TEXT("Timing");
		Delay.Description = TEXT("Wait for a specified amount of time before continuing execution");
		Delay.Usage = TEXT("Use to pause execution flow for a duration. This is a latent node that pauses Blueprint execution (not the game). Use for timing effects, sequences, etc.");
		Delay.bIsPure = false;
		Delay.bIsLatent = true;
		Delay.Source = TEXT("BuiltIn");
		Delay.Tags.Add(TEXT("flow"));
		Delay.Tags.Add(TEXT("timing"));
		Delay.Keywords.Add(TEXT("wait"));
		Delay.Keywords.Add(TEXT("pause"));
		Delay.Keywords.Add(TEXT("timer"));
		Delay.Examples.Add(TEXT("Show message, wait 2 seconds, then hide message"));

		FOliveIRPin ExecIn;
		ExecIn.Name = TEXT("execute");
		ExecIn.bIsInput = true;
		ExecIn.bIsExec = true;
		Delay.InputPins.Add(ExecIn);

		FOliveIRPin Duration;
		Duration.Name = TEXT("Duration");
		Duration.DisplayName = TEXT("Duration");
		Duration.bIsInput = true;
		Duration.bIsExec = false;
		Duration.Type.Category = EOliveIRTypeCategory::Float;
		Duration.DefaultValue = TEXT("0.2");
		Delay.InputPins.Add(Duration);

		FOliveIRPin Completed;
		Completed.Name = TEXT("Completed");
		Completed.DisplayName = TEXT("Completed");
		Completed.bIsInput = false;
		Completed.bIsExec = true;
		Delay.OutputPins.Add(Completed);

		AddNodeType(Delay);
	}

	// For Loop
	{
		FOliveNodeTypeInfo ForLoop;
		ForLoop.TypeId = TEXT("BuiltIn_ForLoop");
		ForLoop.DisplayName = TEXT("For Loop");
		ForLoop.Category = TEXT("Flow Control");
		ForLoop.Subcategory = TEXT("Loops");
		ForLoop.Description = TEXT("Loop from First Index to Last Index, executing Loop Body for each iteration");
		ForLoop.Usage = TEXT("Use to iterate a specific number of times. The Index output gives you the current iteration number (starting from First Index).");
		ForLoop.bIsPure = false;
		ForLoop.Source = TEXT("BuiltIn");
		ForLoop.Tags.Add(TEXT("flow"));
		ForLoop.Tags.Add(TEXT("loop"));
		ForLoop.Keywords.Add(TEXT("iterate"));
		ForLoop.Keywords.Add(TEXT("repeat"));
		ForLoop.Keywords.Add(TEXT("for"));
		ForLoop.Examples.Add(TEXT("Spawn 10 enemies at random positions"));

		FOliveIRPin ExecIn;
		ExecIn.Name = TEXT("execute");
		ExecIn.bIsInput = true;
		ExecIn.bIsExec = true;
		ForLoop.InputPins.Add(ExecIn);

		FOliveIRPin FirstIndex;
		FirstIndex.Name = TEXT("FirstIndex");
		FirstIndex.DisplayName = TEXT("First Index");
		FirstIndex.bIsInput = true;
		FirstIndex.Type.Category = EOliveIRTypeCategory::Int;
		FirstIndex.DefaultValue = TEXT("0");
		ForLoop.InputPins.Add(FirstIndex);

		FOliveIRPin LastIndex;
		LastIndex.Name = TEXT("LastIndex");
		LastIndex.DisplayName = TEXT("Last Index");
		LastIndex.bIsInput = true;
		LastIndex.Type.Category = EOliveIRTypeCategory::Int;
		ForLoop.InputPins.Add(LastIndex);

		FOliveIRPin LoopBody;
		LoopBody.Name = TEXT("LoopBody");
		LoopBody.DisplayName = TEXT("Loop Body");
		LoopBody.bIsInput = false;
		LoopBody.bIsExec = true;
		ForLoop.OutputPins.Add(LoopBody);

		FOliveIRPin Index;
		Index.Name = TEXT("Index");
		Index.DisplayName = TEXT("Index");
		Index.bIsInput = false;
		Index.Type.Category = EOliveIRTypeCategory::Int;
		ForLoop.OutputPins.Add(Index);

		FOliveIRPin Completed;
		Completed.Name = TEXT("Completed");
		Completed.DisplayName = TEXT("Completed");
		Completed.bIsInput = false;
		Completed.bIsExec = true;
		ForLoop.OutputPins.Add(Completed);

		AddNodeType(ForLoop);
	}

	// For Each Loop
	{
		FOliveNodeTypeInfo ForEachLoop;
		ForEachLoop.TypeId = TEXT("BuiltIn_ForEachLoop");
		ForEachLoop.DisplayName = TEXT("For Each Loop");
		ForEachLoop.Category = TEXT("Flow Control");
		ForEachLoop.Subcategory = TEXT("Loops");
		ForEachLoop.Description = TEXT("Loop through each element in an array");
		ForEachLoop.Usage = TEXT("Use to iterate over all elements in an array. The Array Element output gives you the current element, and Array Index gives you its position.");
		ForEachLoop.bIsPure = false;
		ForEachLoop.Source = TEXT("BuiltIn");
		ForEachLoop.Tags.Add(TEXT("flow"));
		ForEachLoop.Tags.Add(TEXT("loop"));
		ForEachLoop.Tags.Add(TEXT("array"));
		ForEachLoop.Keywords.Add(TEXT("iterate"));
		ForEachLoop.Keywords.Add(TEXT("array"));
		ForEachLoop.Keywords.Add(TEXT("foreach"));
		ForEachLoop.Examples.Add(TEXT("Apply damage to all enemies in the Enemies array"));

		AddNodeType(ForEachLoop);
	}

	// While Loop
	{
		FOliveNodeTypeInfo WhileLoop;
		WhileLoop.TypeId = TEXT("BuiltIn_WhileLoop");
		WhileLoop.DisplayName = TEXT("While Loop");
		WhileLoop.Category = TEXT("Flow Control");
		WhileLoop.Subcategory = TEXT("Loops");
		WhileLoop.Description = TEXT("Loop while a condition is true");
		WhileLoop.Usage = TEXT("Use when you need to loop until a condition becomes false. Be careful to ensure the condition eventually becomes false to avoid infinite loops.");
		WhileLoop.bIsPure = false;
		WhileLoop.Source = TEXT("BuiltIn");
		WhileLoop.Tags.Add(TEXT("flow"));
		WhileLoop.Tags.Add(TEXT("loop"));
		WhileLoop.Keywords.Add(TEXT("iterate"));
		WhileLoop.Keywords.Add(TEXT("condition"));
		WhileLoop.Keywords.Add(TEXT("while"));
		WhileLoop.Examples.Add(TEXT("Keep spawning enemies while enemy count is less than max"));

		AddNodeType(WhileLoop);
	}

	// Do Once
	{
		FOliveNodeTypeInfo DoOnce;
		DoOnce.TypeId = TEXT("BuiltIn_DoOnce");
		DoOnce.DisplayName = TEXT("Do Once");
		DoOnce.Category = TEXT("Flow Control");
		DoOnce.Description = TEXT("Execute the output only once, blocking subsequent calls");
		DoOnce.Usage = TEXT("Use when you want code to run only the first time it's triggered. Can be reset with the Reset input.");
		DoOnce.bIsPure = false;
		DoOnce.Source = TEXT("BuiltIn");
		DoOnce.Tags.Add(TEXT("flow"));
		DoOnce.Tags.Add(TEXT("control"));
		DoOnce.Keywords.Add(TEXT("once"));
		DoOnce.Keywords.Add(TEXT("single"));
		DoOnce.Keywords.Add(TEXT("first"));
		DoOnce.Examples.Add(TEXT("Show tutorial message only the first time player enters an area"));

		AddNodeType(DoOnce);
	}

	// Flip Flop
	{
		FOliveNodeTypeInfo FlipFlop;
		FlipFlop.TypeId = TEXT("BuiltIn_FlipFlop");
		FlipFlop.DisplayName = TEXT("Flip Flop");
		FlipFlop.Category = TEXT("Flow Control");
		FlipFlop.Description = TEXT("Alternate between two outputs with each call");
		FlipFlop.Usage = TEXT("Use to toggle between two actions. First call goes to A, second to B, third to A, etc.");
		FlipFlop.bIsPure = false;
		FlipFlop.Source = TEXT("BuiltIn");
		FlipFlop.Tags.Add(TEXT("flow"));
		FlipFlop.Tags.Add(TEXT("control"));
		FlipFlop.Keywords.Add(TEXT("toggle"));
		FlipFlop.Keywords.Add(TEXT("alternate"));
		FlipFlop.Keywords.Add(TEXT("switch"));
		FlipFlop.Examples.Add(TEXT("Toggle a door between open and closed states"));

		AddNodeType(FlipFlop);
	}

	// Gate
	{
		FOliveNodeTypeInfo Gate;
		Gate.TypeId = TEXT("BuiltIn_Gate");
		Gate.DisplayName = TEXT("Gate");
		Gate.Category = TEXT("Flow Control");
		Gate.Description = TEXT("A gate that can be opened and closed to control execution flow");
		Gate.Usage = TEXT("Use to control whether execution can pass through. Open/Close inputs control the gate state, Toggle switches it.");
		Gate.bIsPure = false;
		Gate.Source = TEXT("BuiltIn");
		Gate.Tags.Add(TEXT("flow"));
		Gate.Tags.Add(TEXT("control"));
		Gate.Keywords.Add(TEXT("block"));
		Gate.Keywords.Add(TEXT("allow"));
		Gate.Keywords.Add(TEXT("pass"));
		Gate.Examples.Add(TEXT("Only allow damage while player is not invincible"));

		AddNodeType(Gate);
	}

	// Switch on Int
	{
		FOliveNodeTypeInfo SwitchInt;
		SwitchInt.TypeId = TEXT("BuiltIn_SwitchOnInt");
		SwitchInt.DisplayName = TEXT("Switch on Int");
		SwitchInt.Category = TEXT("Flow Control");
		SwitchInt.Subcategory = TEXT("Switch");
		SwitchInt.Description = TEXT("Branch execution based on an integer value");
		SwitchInt.Usage = TEXT("Use instead of multiple Branch nodes when you have many integer cases. Add case pins for each value you want to handle.");
		SwitchInt.bIsPure = false;
		SwitchInt.Source = TEXT("BuiltIn");
		SwitchInt.Tags.Add(TEXT("flow"));
		SwitchInt.Tags.Add(TEXT("control"));
		SwitchInt.Tags.Add(TEXT("switch"));
		SwitchInt.Keywords.Add(TEXT("case"));
		SwitchInt.Keywords.Add(TEXT("switch"));
		SwitchInt.Keywords.Add(TEXT("select"));

		AddNodeType(SwitchInt);
	}

	// Switch on String
	{
		FOliveNodeTypeInfo SwitchString;
		SwitchString.TypeId = TEXT("BuiltIn_SwitchOnString");
		SwitchString.DisplayName = TEXT("Switch on String");
		SwitchString.Category = TEXT("Flow Control");
		SwitchString.Subcategory = TEXT("Switch");
		SwitchString.Description = TEXT("Branch execution based on a string value");
		SwitchString.Usage = TEXT("Use for string-based branching like handling different command strings or state names.");
		SwitchString.bIsPure = false;
		SwitchString.Source = TEXT("BuiltIn");
		SwitchString.Tags.Add(TEXT("flow"));
		SwitchString.Tags.Add(TEXT("control"));
		SwitchString.Tags.Add(TEXT("switch"));
		SwitchString.Keywords.Add(TEXT("case"));
		SwitchString.Keywords.Add(TEXT("switch"));
		SwitchString.Keywords.Add(TEXT("text"));

		AddNodeType(SwitchString);
	}

	// Switch on Enum (generic entry)
	{
		FOliveNodeTypeInfo SwitchEnum;
		SwitchEnum.TypeId = TEXT("BuiltIn_SwitchOnEnum");
		SwitchEnum.DisplayName = TEXT("Switch on Enum");
		SwitchEnum.Category = TEXT("Flow Control");
		SwitchEnum.Subcategory = TEXT("Switch");
		SwitchEnum.Description = TEXT("Branch execution based on an enum value");
		SwitchEnum.Usage = TEXT("Use for enum-based state machines or handling different enum cases. The node automatically shows all enum values as output pins.");
		SwitchEnum.bIsPure = false;
		SwitchEnum.Source = TEXT("BuiltIn");
		SwitchEnum.Tags.Add(TEXT("flow"));
		SwitchEnum.Tags.Add(TEXT("control"));
		SwitchEnum.Tags.Add(TEXT("switch"));
		SwitchEnum.Keywords.Add(TEXT("enum"));
		SwitchEnum.Keywords.Add(TEXT("case"));
		SwitchEnum.Keywords.Add(TEXT("state"));

		AddNodeType(SwitchEnum);
	}

	UE_LOG(LogOliveNodeCatalog, Log, TEXT("Added built-in flow control nodes"));
}

void FOliveNodeCatalog::AddMathNodes()
{
	// Add (Float)
	{
		FOliveNodeTypeInfo Add;
		Add.TypeId = TEXT("BuiltIn_Add_Float");
		Add.DisplayName = TEXT("Add (float)");
		Add.Category = TEXT("Math");
		Add.Subcategory = TEXT("Float");
		Add.Description = TEXT("Add two floating point numbers");
		Add.Usage = TEXT("Returns A + B. Use for any addition of decimal numbers.");
		Add.bIsPure = true;
		Add.bIsCompact = true;
		Add.Source = TEXT("BuiltIn");
		Add.Tags.Add(TEXT("math"));
		Add.Keywords.Add(TEXT("+"));
		Add.Keywords.Add(TEXT("plus"));
		Add.Keywords.Add(TEXT("sum"));

		AddNodeType(Add);
	}

	// Subtract (Float)
	{
		FOliveNodeTypeInfo Subtract;
		Subtract.TypeId = TEXT("BuiltIn_Subtract_Float");
		Subtract.DisplayName = TEXT("Subtract (float)");
		Subtract.Category = TEXT("Math");
		Subtract.Subcategory = TEXT("Float");
		Subtract.Description = TEXT("Subtract two floating point numbers");
		Subtract.Usage = TEXT("Returns A - B. Use for any subtraction of decimal numbers.");
		Subtract.bIsPure = true;
		Subtract.bIsCompact = true;
		Subtract.Source = TEXT("BuiltIn");
		Subtract.Tags.Add(TEXT("math"));
		Subtract.Keywords.Add(TEXT("-"));
		Subtract.Keywords.Add(TEXT("minus"));
		Subtract.Keywords.Add(TEXT("difference"));

		AddNodeType(Subtract);
	}

	// Multiply (Float)
	{
		FOliveNodeTypeInfo Multiply;
		Multiply.TypeId = TEXT("BuiltIn_Multiply_Float");
		Multiply.DisplayName = TEXT("Multiply (float)");
		Multiply.Category = TEXT("Math");
		Multiply.Subcategory = TEXT("Float");
		Multiply.Description = TEXT("Multiply two floating point numbers");
		Multiply.Usage = TEXT("Returns A * B. Use for any multiplication of decimal numbers.");
		Multiply.bIsPure = true;
		Multiply.bIsCompact = true;
		Multiply.Source = TEXT("BuiltIn");
		Multiply.Tags.Add(TEXT("math"));
		Multiply.Keywords.Add(TEXT("*"));
		Multiply.Keywords.Add(TEXT("times"));
		Multiply.Keywords.Add(TEXT("product"));

		AddNodeType(Multiply);
	}

	// Divide (Float)
	{
		FOliveNodeTypeInfo Divide;
		Divide.TypeId = TEXT("BuiltIn_Divide_Float");
		Divide.DisplayName = TEXT("Divide (float)");
		Divide.Category = TEXT("Math");
		Divide.Subcategory = TEXT("Float");
		Divide.Description = TEXT("Divide two floating point numbers");
		Divide.Usage = TEXT("Returns A / B. Be careful of division by zero.");
		Divide.bIsPure = true;
		Divide.bIsCompact = true;
		Divide.Source = TEXT("BuiltIn");
		Divide.Tags.Add(TEXT("math"));
		Divide.Keywords.Add(TEXT("/"));
		Divide.Keywords.Add(TEXT("quotient"));

		AddNodeType(Divide);
	}

	// Sin
	{
		FOliveNodeTypeInfo Sin;
		Sin.TypeId = TEXT("BuiltIn_Sin");
		Sin.DisplayName = TEXT("Sin");
		Sin.Category = TEXT("Math");
		Sin.Subcategory = TEXT("Trig");
		Sin.Description = TEXT("Returns the sine of an angle in radians");
		Sin.Usage = TEXT("Input is in radians. For degrees, multiply by PI/180 first or use Sin (Degrees).");
		Sin.bIsPure = true;
		Sin.Source = TEXT("BuiltIn");
		Sin.Tags.Add(TEXT("math"));
		Sin.Tags.Add(TEXT("trig"));
		Sin.Keywords.Add(TEXT("sine"));
		Sin.Keywords.Add(TEXT("trigonometry"));

		AddNodeType(Sin);
	}

	// Cos
	{
		FOliveNodeTypeInfo Cos;
		Cos.TypeId = TEXT("BuiltIn_Cos");
		Cos.DisplayName = TEXT("Cos");
		Cos.Category = TEXT("Math");
		Cos.Subcategory = TEXT("Trig");
		Cos.Description = TEXT("Returns the cosine of an angle in radians");
		Cos.Usage = TEXT("Input is in radians. For degrees, multiply by PI/180 first or use Cos (Degrees).");
		Cos.bIsPure = true;
		Cos.Source = TEXT("BuiltIn");
		Cos.Tags.Add(TEXT("math"));
		Cos.Tags.Add(TEXT("trig"));
		Cos.Keywords.Add(TEXT("cosine"));
		Cos.Keywords.Add(TEXT("trigonometry"));

		AddNodeType(Cos);
	}

	// Clamp
	{
		FOliveNodeTypeInfo Clamp;
		Clamp.TypeId = TEXT("BuiltIn_Clamp");
		Clamp.DisplayName = TEXT("Clamp");
		Clamp.Category = TEXT("Math");
		Clamp.Description = TEXT("Clamp a value between a minimum and maximum");
		Clamp.Usage = TEXT("Returns Value constrained to [Min, Max]. Use to ensure a value stays within bounds.");
		Clamp.bIsPure = true;
		Clamp.Source = TEXT("BuiltIn");
		Clamp.Tags.Add(TEXT("math"));
		Clamp.Keywords.Add(TEXT("limit"));
		Clamp.Keywords.Add(TEXT("bound"));
		Clamp.Keywords.Add(TEXT("constrain"));
		Clamp.Examples.Add(TEXT("Clamp health between 0 and MaxHealth"));

		AddNodeType(Clamp);
	}

	// Lerp
	{
		FOliveNodeTypeInfo Lerp;
		Lerp.TypeId = TEXT("BuiltIn_Lerp");
		Lerp.DisplayName = TEXT("Lerp");
		Lerp.Category = TEXT("Math");
		Lerp.Subcategory = TEXT("Interpolation");
		Lerp.Description = TEXT("Linear interpolation between two values");
		Lerp.Usage = TEXT("Returns A + Alpha*(B-A). Alpha of 0 returns A, Alpha of 1 returns B. Great for smooth transitions.");
		Lerp.bIsPure = true;
		Lerp.Source = TEXT("BuiltIn");
		Lerp.Tags.Add(TEXT("math"));
		Lerp.Tags.Add(TEXT("interpolation"));
		Lerp.Keywords.Add(TEXT("interpolate"));
		Lerp.Keywords.Add(TEXT("blend"));
		Lerp.Keywords.Add(TEXT("smooth"));
		Lerp.Examples.Add(TEXT("Smoothly move between two positions over time"));

		AddNodeType(Lerp);
	}

	// Random Float in Range
	{
		FOliveNodeTypeInfo RandomFloat;
		RandomFloat.TypeId = TEXT("BuiltIn_RandomFloatInRange");
		RandomFloat.DisplayName = TEXT("Random Float in Range");
		RandomFloat.Category = TEXT("Math");
		RandomFloat.Subcategory = TEXT("Random");
		RandomFloat.Description = TEXT("Returns a random float between Min and Max (inclusive)");
		RandomFloat.Usage = TEXT("Use for random variation in values like damage, spawn positions, etc.");
		RandomFloat.bIsPure = true;
		RandomFloat.Source = TEXT("BuiltIn");
		RandomFloat.Tags.Add(TEXT("math"));
		RandomFloat.Tags.Add(TEXT("random"));
		RandomFloat.Keywords.Add(TEXT("random"));
		RandomFloat.Keywords.Add(TEXT("rand"));

		AddNodeType(RandomFloat);
	}

	// Random Integer in Range
	{
		FOliveNodeTypeInfo RandomInt;
		RandomInt.TypeId = TEXT("BuiltIn_RandomIntegerInRange");
		RandomInt.DisplayName = TEXT("Random Integer in Range");
		RandomInt.Category = TEXT("Math");
		RandomInt.Subcategory = TEXT("Random");
		RandomInt.Description = TEXT("Returns a random integer between Min and Max (inclusive)");
		RandomInt.Usage = TEXT("Use for random selection from a range, like picking a random array index.");
		RandomInt.bIsPure = true;
		RandomInt.Source = TEXT("BuiltIn");
		RandomInt.Tags.Add(TEXT("math"));
		RandomInt.Tags.Add(TEXT("random"));
		RandomInt.Keywords.Add(TEXT("random"));
		RandomInt.Keywords.Add(TEXT("rand"));
		RandomInt.Keywords.Add(TEXT("int"));

		AddNodeType(RandomInt);
	}

	// Abs
	{
		FOliveNodeTypeInfo Abs;
		Abs.TypeId = TEXT("BuiltIn_Abs");
		Abs.DisplayName = TEXT("Abs");
		Abs.Category = TEXT("Math");
		Abs.Description = TEXT("Returns the absolute value (removes sign)");
		Abs.Usage = TEXT("Returns |A|. Negative numbers become positive, positive numbers stay positive.");
		Abs.bIsPure = true;
		Abs.Source = TEXT("BuiltIn");
		Abs.Tags.Add(TEXT("math"));
		Abs.Keywords.Add(TEXT("absolute"));
		Abs.Keywords.Add(TEXT("magnitude"));

		AddNodeType(Abs);
	}

	// Min/Max
	{
		FOliveNodeTypeInfo Min;
		Min.TypeId = TEXT("BuiltIn_Min");
		Min.DisplayName = TEXT("Min");
		Min.Category = TEXT("Math");
		Min.Description = TEXT("Returns the smaller of two values");
		Min.Usage = TEXT("Returns the minimum of A and B.");
		Min.bIsPure = true;
		Min.Source = TEXT("BuiltIn");
		Min.Tags.Add(TEXT("math"));
		Min.Keywords.Add(TEXT("minimum"));
		Min.Keywords.Add(TEXT("smaller"));
		Min.Keywords.Add(TEXT("least"));

		AddNodeType(Min);

		FOliveNodeTypeInfo Max;
		Max.TypeId = TEXT("BuiltIn_Max");
		Max.DisplayName = TEXT("Max");
		Max.Category = TEXT("Math");
		Max.Description = TEXT("Returns the larger of two values");
		Max.Usage = TEXT("Returns the maximum of A and B.");
		Max.bIsPure = true;
		Max.Source = TEXT("BuiltIn");
		Max.Tags.Add(TEXT("math"));
		Max.Keywords.Add(TEXT("maximum"));
		Max.Keywords.Add(TEXT("larger"));
		Max.Keywords.Add(TEXT("greatest"));

		AddNodeType(Max);
	}

	// Floor/Ceil/Round
	{
		FOliveNodeTypeInfo Floor;
		Floor.TypeId = TEXT("BuiltIn_Floor");
		Floor.DisplayName = TEXT("Floor");
		Floor.Category = TEXT("Math");
		Floor.Description = TEXT("Rounds down to the nearest integer");
		Floor.bIsPure = true;
		Floor.Source = TEXT("BuiltIn");
		Floor.Tags.Add(TEXT("math"));
		Floor.Keywords.Add(TEXT("round"));
		Floor.Keywords.Add(TEXT("down"));
		Floor.Keywords.Add(TEXT("truncate"));

		AddNodeType(Floor);

		FOliveNodeTypeInfo Ceil;
		Ceil.TypeId = TEXT("BuiltIn_Ceil");
		Ceil.DisplayName = TEXT("Ceil");
		Ceil.Category = TEXT("Math");
		Ceil.Description = TEXT("Rounds up to the nearest integer");
		Ceil.bIsPure = true;
		Ceil.Source = TEXT("BuiltIn");
		Ceil.Tags.Add(TEXT("math"));
		Ceil.Keywords.Add(TEXT("round"));
		Ceil.Keywords.Add(TEXT("up"));
		Ceil.Keywords.Add(TEXT("ceiling"));

		AddNodeType(Ceil);

		FOliveNodeTypeInfo Round;
		Round.TypeId = TEXT("BuiltIn_Round");
		Round.DisplayName = TEXT("Round");
		Round.Category = TEXT("Math");
		Round.Description = TEXT("Rounds to the nearest integer");
		Round.bIsPure = true;
		Round.Source = TEXT("BuiltIn");
		Round.Tags.Add(TEXT("math"));
		Round.Keywords.Add(TEXT("round"));
		Round.Keywords.Add(TEXT("nearest"));

		AddNodeType(Round);
	}

	UE_LOG(LogOliveNodeCatalog, Log, TEXT("Added built-in math nodes"));
}

void FOliveNodeCatalog::AddStringNodes()
{
	// Append
	{
		FOliveNodeTypeInfo Append;
		Append.TypeId = TEXT("BuiltIn_Append");
		Append.DisplayName = TEXT("Append");
		Append.Category = TEXT("String");
		Append.Description = TEXT("Concatenate two strings together");
		Append.Usage = TEXT("Returns A + B as a single string. Use for building messages, paths, etc.");
		Append.bIsPure = true;
		Append.Source = TEXT("BuiltIn");
		Append.Tags.Add(TEXT("string"));
		Append.Keywords.Add(TEXT("concatenate"));
		Append.Keywords.Add(TEXT("join"));
		Append.Keywords.Add(TEXT("+"));
		Append.Examples.Add(TEXT("Build greeting: 'Hello, ' + PlayerName"));

		AddNodeType(Append);
	}

	// Contains
	{
		FOliveNodeTypeInfo Contains;
		Contains.TypeId = TEXT("BuiltIn_Contains");
		Contains.DisplayName = TEXT("Contains");
		Contains.Category = TEXT("String");
		Contains.Description = TEXT("Check if a string contains a substring");
		Contains.Usage = TEXT("Returns true if SearchIn contains Substring. Case sensitivity can be configured.");
		Contains.bIsPure = true;
		Contains.Source = TEXT("BuiltIn");
		Contains.Tags.Add(TEXT("string"));
		Contains.Keywords.Add(TEXT("find"));
		Contains.Keywords.Add(TEXT("search"));
		Contains.Keywords.Add(TEXT("includes"));

		AddNodeType(Contains);
	}

	// Len
	{
		FOliveNodeTypeInfo Len;
		Len.TypeId = TEXT("BuiltIn_Len");
		Len.DisplayName = TEXT("Len");
		Len.Category = TEXT("String");
		Len.Description = TEXT("Get the length of a string");
		Len.Usage = TEXT("Returns the number of characters in the string.");
		Len.bIsPure = true;
		Len.Source = TEXT("BuiltIn");
		Len.Tags.Add(TEXT("string"));
		Len.Keywords.Add(TEXT("length"));
		Len.Keywords.Add(TEXT("size"));
		Len.Keywords.Add(TEXT("count"));

		AddNodeType(Len);
	}

	// Format Text
	{
		FOliveNodeTypeInfo FormatText;
		FormatText.TypeId = TEXT("BuiltIn_FormatText");
		FormatText.DisplayName = TEXT("Format Text");
		FormatText.Category = TEXT("String");
		FormatText.Description = TEXT("Format a text string with variable substitution");
		FormatText.Usage = TEXT("Use {0}, {1}, etc. as placeholders in the format string. Arguments are substituted in order.");
		FormatText.bIsPure = true;
		FormatText.Source = TEXT("BuiltIn");
		FormatText.Tags.Add(TEXT("string"));
		FormatText.Tags.Add(TEXT("text"));
		FormatText.Keywords.Add(TEXT("format"));
		FormatText.Keywords.Add(TEXT("template"));
		FormatText.Keywords.Add(TEXT("substitute"));
		FormatText.Examples.Add(TEXT("'Player {0} scored {1} points' with args PlayerName, Score"));

		AddNodeType(FormatText);
	}

	// To Upper/Lower
	{
		FOliveNodeTypeInfo ToUpper;
		ToUpper.TypeId = TEXT("BuiltIn_ToUpper");
		ToUpper.DisplayName = TEXT("To Upper");
		ToUpper.Category = TEXT("String");
		ToUpper.Description = TEXT("Convert string to uppercase");
		ToUpper.bIsPure = true;
		ToUpper.Source = TEXT("BuiltIn");
		ToUpper.Tags.Add(TEXT("string"));
		ToUpper.Keywords.Add(TEXT("uppercase"));
		ToUpper.Keywords.Add(TEXT("capitalize"));

		AddNodeType(ToUpper);

		FOliveNodeTypeInfo ToLower;
		ToLower.TypeId = TEXT("BuiltIn_ToLower");
		ToLower.DisplayName = TEXT("To Lower");
		ToLower.Category = TEXT("String");
		ToLower.Description = TEXT("Convert string to lowercase");
		ToLower.bIsPure = true;
		ToLower.Source = TEXT("BuiltIn");
		ToLower.Tags.Add(TEXT("string"));
		ToLower.Keywords.Add(TEXT("lowercase"));

		AddNodeType(ToLower);
	}

	// Split String
	{
		FOliveNodeTypeInfo Split;
		Split.TypeId = TEXT("BuiltIn_ParseIntoArray");
		Split.DisplayName = TEXT("Parse Into Array");
		Split.Category = TEXT("String");
		Split.Description = TEXT("Split a string into an array by a delimiter");
		Split.Usage = TEXT("Splits the source string at each occurrence of the delimiter and returns an array of substrings.");
		Split.bIsPure = true;
		Split.Source = TEXT("BuiltIn");
		Split.Tags.Add(TEXT("string"));
		Split.Tags.Add(TEXT("array"));
		Split.Keywords.Add(TEXT("split"));
		Split.Keywords.Add(TEXT("explode"));
		Split.Keywords.Add(TEXT("tokenize"));

		AddNodeType(Split);
	}

	// Join String Array
	{
		FOliveNodeTypeInfo Join;
		Join.TypeId = TEXT("BuiltIn_JoinStringArray");
		Join.DisplayName = TEXT("Join String Array");
		Join.Category = TEXT("String");
		Join.Description = TEXT("Join an array of strings with a separator");
		Join.Usage = TEXT("Combines all strings in the array into a single string, with the separator between each.");
		Join.bIsPure = true;
		Join.Source = TEXT("BuiltIn");
		Join.Tags.Add(TEXT("string"));
		Join.Tags.Add(TEXT("array"));
		Join.Keywords.Add(TEXT("join"));
		Join.Keywords.Add(TEXT("implode"));
		Join.Keywords.Add(TEXT("combine"));

		AddNodeType(Join);
	}

	// Int/Float to String
	{
		FOliveNodeTypeInfo IntToString;
		IntToString.TypeId = TEXT("BuiltIn_IntToString");
		IntToString.DisplayName = TEXT("Int to String");
		IntToString.Category = TEXT("String");
		IntToString.Subcategory = TEXT("Conversions");
		IntToString.Description = TEXT("Convert an integer to a string");
		IntToString.bIsPure = true;
		IntToString.Source = TEXT("BuiltIn");
		IntToString.Tags.Add(TEXT("string"));
		IntToString.Tags.Add(TEXT("conversion"));
		IntToString.Keywords.Add(TEXT("convert"));
		IntToString.Keywords.Add(TEXT("toString"));

		AddNodeType(IntToString);

		FOliveNodeTypeInfo FloatToString;
		FloatToString.TypeId = TEXT("BuiltIn_FloatToString");
		FloatToString.DisplayName = TEXT("Float to String");
		FloatToString.Category = TEXT("String");
		FloatToString.Subcategory = TEXT("Conversions");
		FloatToString.Description = TEXT("Convert a float to a string");
		FloatToString.bIsPure = true;
		FloatToString.Source = TEXT("BuiltIn");
		FloatToString.Tags.Add(TEXT("string"));
		FloatToString.Tags.Add(TEXT("conversion"));
		FloatToString.Keywords.Add(TEXT("convert"));
		FloatToString.Keywords.Add(TEXT("toString"));

		AddNodeType(FloatToString);
	}

	UE_LOG(LogOliveNodeCatalog, Log, TEXT("Added built-in string nodes"));
}

void FOliveNodeCatalog::AddArrayNodes()
{
	// Add
	{
		FOliveNodeTypeInfo Add;
		Add.TypeId = TEXT("BuiltIn_Array_Add");
		Add.DisplayName = TEXT("Add");
		Add.Category = TEXT("Array");
		Add.Description = TEXT("Add an element to the end of an array");
		Add.Usage = TEXT("Adds the element to the array and returns the index where it was added.");
		Add.bIsPure = false;
		Add.Source = TEXT("BuiltIn");
		Add.Tags.Add(TEXT("array"));
		Add.Keywords.Add(TEXT("push"));
		Add.Keywords.Add(TEXT("append"));
		Add.Keywords.Add(TEXT("insert"));

		AddNodeType(Add);
	}

	// Remove
	{
		FOliveNodeTypeInfo Remove;
		Remove.TypeId = TEXT("BuiltIn_Array_Remove");
		Remove.DisplayName = TEXT("Remove");
		Remove.Category = TEXT("Array");
		Remove.Description = TEXT("Remove an element from an array by value");
		Remove.Usage = TEXT("Removes the first occurrence of the element. Returns true if found and removed.");
		Remove.bIsPure = false;
		Remove.Source = TEXT("BuiltIn");
		Remove.Tags.Add(TEXT("array"));
		Remove.Keywords.Add(TEXT("delete"));
		Remove.Keywords.Add(TEXT("erase"));

		AddNodeType(Remove);
	}

	// Remove Index
	{
		FOliveNodeTypeInfo RemoveIndex;
		RemoveIndex.TypeId = TEXT("BuiltIn_Array_RemoveIndex");
		RemoveIndex.DisplayName = TEXT("Remove Index");
		RemoveIndex.Category = TEXT("Array");
		RemoveIndex.Description = TEXT("Remove an element at a specific index");
		RemoveIndex.Usage = TEXT("Removes the element at the given index. Subsequent elements shift down.");
		RemoveIndex.bIsPure = false;
		RemoveIndex.Source = TEXT("BuiltIn");
		RemoveIndex.Tags.Add(TEXT("array"));
		RemoveIndex.Keywords.Add(TEXT("delete"));
		RemoveIndex.Keywords.Add(TEXT("erase"));
		RemoveIndex.Keywords.Add(TEXT("index"));

		AddNodeType(RemoveIndex);
	}

	// Find
	{
		FOliveNodeTypeInfo Find;
		Find.TypeId = TEXT("BuiltIn_Array_Find");
		Find.DisplayName = TEXT("Find");
		Find.Category = TEXT("Array");
		Find.Description = TEXT("Find the index of an element in an array");
		Find.Usage = TEXT("Returns the index of the first occurrence, or -1 if not found.");
		Find.bIsPure = true;
		Find.Source = TEXT("BuiltIn");
		Find.Tags.Add(TEXT("array"));
		Find.Keywords.Add(TEXT("search"));
		Find.Keywords.Add(TEXT("index"));
		Find.Keywords.Add(TEXT("locate"));

		AddNodeType(Find);
	}

	// Contains
	{
		FOliveNodeTypeInfo Contains;
		Contains.TypeId = TEXT("BuiltIn_Array_Contains");
		Contains.DisplayName = TEXT("Contains");
		Contains.Category = TEXT("Array");
		Contains.Description = TEXT("Check if an array contains an element");
		Contains.Usage = TEXT("Returns true if the element exists in the array.");
		Contains.bIsPure = true;
		Contains.Source = TEXT("BuiltIn");
		Contains.Tags.Add(TEXT("array"));
		Contains.Keywords.Add(TEXT("includes"));
		Contains.Keywords.Add(TEXT("has"));

		AddNodeType(Contains);
	}

	// Length
	{
		FOliveNodeTypeInfo Length;
		Length.TypeId = TEXT("BuiltIn_Array_Length");
		Length.DisplayName = TEXT("Length");
		Length.Category = TEXT("Array");
		Length.Description = TEXT("Get the number of elements in an array");
		Length.Usage = TEXT("Returns the count of elements currently in the array.");
		Length.bIsPure = true;
		Length.Source = TEXT("BuiltIn");
		Length.Tags.Add(TEXT("array"));
		Length.Keywords.Add(TEXT("size"));
		Length.Keywords.Add(TEXT("count"));
		Length.Keywords.Add(TEXT("num"));

		AddNodeType(Length);
	}

	// Get
	{
		FOliveNodeTypeInfo Get;
		Get.TypeId = TEXT("BuiltIn_Array_Get");
		Get.DisplayName = TEXT("Get");
		Get.Category = TEXT("Array");
		Get.Description = TEXT("Get an element from an array by index");
		Get.Usage = TEXT("Returns the element at the specified index. Index must be valid (0 to Length-1).");
		Get.bIsPure = true;
		Get.Source = TEXT("BuiltIn");
		Get.Tags.Add(TEXT("array"));
		Get.Keywords.Add(TEXT("index"));
		Get.Keywords.Add(TEXT("element"));
		Get.Keywords.Add(TEXT("access"));

		AddNodeType(Get);
	}

	// Set
	{
		FOliveNodeTypeInfo Set;
		Set.TypeId = TEXT("BuiltIn_Array_Set");
		Set.DisplayName = TEXT("Set Array Elem");
		Set.Category = TEXT("Array");
		Set.Description = TEXT("Set an element in an array at a specific index");
		Set.Usage = TEXT("Replaces the element at the given index with the new value.");
		Set.bIsPure = false;
		Set.Source = TEXT("BuiltIn");
		Set.Tags.Add(TEXT("array"));
		Set.Keywords.Add(TEXT("assign"));
		Set.Keywords.Add(TEXT("replace"));
		Set.Keywords.Add(TEXT("update"));

		AddNodeType(Set);
	}

	// Clear
	{
		FOliveNodeTypeInfo Clear;
		Clear.TypeId = TEXT("BuiltIn_Array_Clear");
		Clear.DisplayName = TEXT("Clear");
		Clear.Category = TEXT("Array");
		Clear.Description = TEXT("Remove all elements from an array");
		Clear.Usage = TEXT("Empties the array. Length becomes 0.");
		Clear.bIsPure = false;
		Clear.Source = TEXT("BuiltIn");
		Clear.Tags.Add(TEXT("array"));
		Clear.Keywords.Add(TEXT("empty"));
		Clear.Keywords.Add(TEXT("reset"));

		AddNodeType(Clear);
	}

	// Sort
	{
		FOliveNodeTypeInfo Sort;
		Sort.TypeId = TEXT("BuiltIn_Array_Sort");
		Sort.DisplayName = TEXT("Sort");
		Sort.Category = TEXT("Array");
		Sort.Description = TEXT("Sort an array in ascending order");
		Sort.Usage = TEXT("Sorts the array elements in place. For custom sorting, use Sort with Predicate.");
		Sort.bIsPure = false;
		Sort.Source = TEXT("BuiltIn");
		Sort.Tags.Add(TEXT("array"));
		Sort.Keywords.Add(TEXT("order"));
		Sort.Keywords.Add(TEXT("arrange"));

		AddNodeType(Sort);
	}

	// Shuffle
	{
		FOliveNodeTypeInfo Shuffle;
		Shuffle.TypeId = TEXT("BuiltIn_Array_Shuffle");
		Shuffle.DisplayName = TEXT("Shuffle");
		Shuffle.Category = TEXT("Array");
		Shuffle.Description = TEXT("Randomly shuffle the elements of an array");
		Shuffle.Usage = TEXT("Rearranges elements in random order. Great for randomizing spawn order, card decks, etc.");
		Shuffle.bIsPure = false;
		Shuffle.Source = TEXT("BuiltIn");
		Shuffle.Tags.Add(TEXT("array"));
		Shuffle.Tags.Add(TEXT("random"));
		Shuffle.Keywords.Add(TEXT("randomize"));
		Shuffle.Keywords.Add(TEXT("mix"));

		AddNodeType(Shuffle);
	}

	// Filter
	{
		FOliveNodeTypeInfo Filter;
		Filter.TypeId = TEXT("BuiltIn_Array_Filter");
		Filter.DisplayName = TEXT("Filter Array");
		Filter.Category = TEXT("Array");
		Filter.Description = TEXT("Filter an array based on a predicate");
		Filter.Usage = TEXT("Returns a new array containing only elements that match the filter criteria.");
		Filter.bIsPure = true;
		Filter.Source = TEXT("BuiltIn");
		Filter.Tags.Add(TEXT("array"));
		Filter.Keywords.Add(TEXT("where"));
		Filter.Keywords.Add(TEXT("select"));
		Filter.Keywords.Add(TEXT("predicate"));

		AddNodeType(Filter);
	}

	UE_LOG(LogOliveNodeCatalog, Log, TEXT("Added built-in array nodes"));
}

void FOliveNodeCatalog::AddUtilityNodes()
{
	// Print String
	{
		FOliveNodeTypeInfo PrintString;
		PrintString.TypeId = TEXT("BuiltIn_PrintString");
		PrintString.DisplayName = TEXT("Print String");
		PrintString.Category = TEXT("Development");
		PrintString.Description = TEXT("Print a message to the screen and/or log");
		PrintString.Usage = TEXT("Use for debugging. Configure whether to print to screen, log, or both. Set duration for on-screen messages.");
		PrintString.bIsPure = false;
		PrintString.Source = TEXT("BuiltIn");
		PrintString.Tags.Add(TEXT("debug"));
		PrintString.Tags.Add(TEXT("utility"));
		PrintString.Keywords.Add(TEXT("debug"));
		PrintString.Keywords.Add(TEXT("log"));
		PrintString.Keywords.Add(TEXT("print"));
		PrintString.Keywords.Add(TEXT("console"));
		PrintString.Examples.Add(TEXT("Print player health to screen for debugging"));

		AddNodeType(PrintString);
	}

	// Set Timer by Event/Function
	{
		FOliveNodeTypeInfo SetTimerByEvent;
		SetTimerByEvent.TypeId = TEXT("BuiltIn_SetTimerByEvent");
		SetTimerByEvent.DisplayName = TEXT("Set Timer by Event");
		SetTimerByEvent.Category = TEXT("Utilities");
		SetTimerByEvent.Subcategory = TEXT("Timer");
		SetTimerByEvent.Description = TEXT("Set a timer that calls an event after a delay");
		SetTimerByEvent.Usage = TEXT("The event will be called after the specified time. Can optionally loop.");
		SetTimerByEvent.bIsPure = false;
		SetTimerByEvent.bIsLatent = true;
		SetTimerByEvent.Source = TEXT("BuiltIn");
		SetTimerByEvent.Tags.Add(TEXT("timer"));
		SetTimerByEvent.Tags.Add(TEXT("utility"));
		SetTimerByEvent.Keywords.Add(TEXT("timer"));
		SetTimerByEvent.Keywords.Add(TEXT("delay"));
		SetTimerByEvent.Keywords.Add(TEXT("schedule"));
		SetTimerByEvent.Examples.Add(TEXT("Call RespawnPlayer event 5 seconds after death"));

		AddNodeType(SetTimerByEvent);

		FOliveNodeTypeInfo SetTimerByFunction;
		SetTimerByFunction.TypeId = TEXT("BuiltIn_SetTimerByFunctionName");
		SetTimerByFunction.DisplayName = TEXT("Set Timer by Function Name");
		SetTimerByFunction.Category = TEXT("Utilities");
		SetTimerByFunction.Subcategory = TEXT("Timer");
		SetTimerByFunction.Description = TEXT("Set a timer that calls a function by name after a delay");
		SetTimerByFunction.Usage = TEXT("Specify the function name as a string. The function will be called after the delay.");
		SetTimerByFunction.bIsPure = false;
		SetTimerByFunction.bIsLatent = true;
		SetTimerByFunction.Source = TEXT("BuiltIn");
		SetTimerByFunction.Tags.Add(TEXT("timer"));
		SetTimerByFunction.Tags.Add(TEXT("utility"));
		SetTimerByFunction.Keywords.Add(TEXT("timer"));
		SetTimerByFunction.Keywords.Add(TEXT("delay"));
		SetTimerByFunction.Keywords.Add(TEXT("function"));

		AddNodeType(SetTimerByFunction);
	}

	// Clear Timer
	{
		FOliveNodeTypeInfo ClearTimer;
		ClearTimer.TypeId = TEXT("BuiltIn_ClearTimer");
		ClearTimer.DisplayName = TEXT("Clear Timer by Handle");
		ClearTimer.Category = TEXT("Utilities");
		ClearTimer.Subcategory = TEXT("Timer");
		ClearTimer.Description = TEXT("Stop a timer that was previously set");
		ClearTimer.Usage = TEXT("Pass the timer handle returned from Set Timer to stop it.");
		ClearTimer.bIsPure = false;
		ClearTimer.Source = TEXT("BuiltIn");
		ClearTimer.Tags.Add(TEXT("timer"));
		ClearTimer.Tags.Add(TEXT("utility"));
		ClearTimer.Keywords.Add(TEXT("stop"));
		ClearTimer.Keywords.Add(TEXT("cancel"));

		AddNodeType(ClearTimer);
	}

	// Spawn Actor
	{
		FOliveNodeTypeInfo SpawnActor;
		SpawnActor.TypeId = TEXT("BuiltIn_SpawnActor");
		SpawnActor.DisplayName = TEXT("Spawn Actor from Class");
		SpawnActor.Category = TEXT("Actor");
		SpawnActor.Description = TEXT("Spawn a new actor instance in the world");
		SpawnActor.Usage = TEXT("Specify the class to spawn and the transform (location, rotation, scale). Returns the spawned actor.");
		SpawnActor.bIsPure = false;
		SpawnActor.Source = TEXT("BuiltIn");
		SpawnActor.Tags.Add(TEXT("actor"));
		SpawnActor.Tags.Add(TEXT("spawn"));
		SpawnActor.Keywords.Add(TEXT("create"));
		SpawnActor.Keywords.Add(TEXT("instantiate"));
		SpawnActor.Keywords.Add(TEXT("new"));
		SpawnActor.Examples.Add(TEXT("Spawn an enemy at a spawn point location"));

		AddNodeType(SpawnActor);
	}

	// Destroy Actor
	{
		FOliveNodeTypeInfo DestroyActor;
		DestroyActor.TypeId = TEXT("BuiltIn_DestroyActor");
		DestroyActor.DisplayName = TEXT("Destroy Actor");
		DestroyActor.Category = TEXT("Actor");
		DestroyActor.Description = TEXT("Destroy an actor, removing it from the world");
		DestroyActor.Usage = TEXT("The actor will be destroyed at the end of the frame. Any references will become invalid.");
		DestroyActor.bIsPure = false;
		DestroyActor.Source = TEXT("BuiltIn");
		DestroyActor.Tags.Add(TEXT("actor"));
		DestroyActor.Keywords.Add(TEXT("delete"));
		DestroyActor.Keywords.Add(TEXT("remove"));
		DestroyActor.Keywords.Add(TEXT("kill"));

		AddNodeType(DestroyActor);
	}

	// Get Player Pawn/Controller
	{
		FOliveNodeTypeInfo GetPlayerPawn;
		GetPlayerPawn.TypeId = TEXT("BuiltIn_GetPlayerPawn");
		GetPlayerPawn.DisplayName = TEXT("Get Player Pawn");
		GetPlayerPawn.Category = TEXT("Game");
		GetPlayerPawn.Description = TEXT("Get the pawn for a player by index");
		GetPlayerPawn.Usage = TEXT("Returns the pawn controlled by the specified player. Index 0 is the first player.");
		GetPlayerPawn.bIsPure = true;
		GetPlayerPawn.Source = TEXT("BuiltIn");
		GetPlayerPawn.Tags.Add(TEXT("player"));
		GetPlayerPawn.Tags.Add(TEXT("game"));
		GetPlayerPawn.Keywords.Add(TEXT("player"));
		GetPlayerPawn.Keywords.Add(TEXT("character"));

		AddNodeType(GetPlayerPawn);

		FOliveNodeTypeInfo GetPlayerController;
		GetPlayerController.TypeId = TEXT("BuiltIn_GetPlayerController");
		GetPlayerController.DisplayName = TEXT("Get Player Controller");
		GetPlayerController.Category = TEXT("Game");
		GetPlayerController.Description = TEXT("Get the controller for a player by index");
		GetPlayerController.Usage = TEXT("Returns the controller for the specified player. Useful for input and camera control.");
		GetPlayerController.bIsPure = true;
		GetPlayerController.Source = TEXT("BuiltIn");
		GetPlayerController.Tags.Add(TEXT("player"));
		GetPlayerController.Tags.Add(TEXT("game"));
		GetPlayerController.Keywords.Add(TEXT("player"));
		GetPlayerController.Keywords.Add(TEXT("controller"));
		GetPlayerController.Keywords.Add(TEXT("input"));

		AddNodeType(GetPlayerController);
	}

	// Cast
	{
		FOliveNodeTypeInfo Cast;
		Cast.TypeId = TEXT("BuiltIn_Cast");
		Cast.DisplayName = TEXT("Cast To");
		Cast.Category = TEXT("Utilities");
		Cast.Subcategory = TEXT("Casting");
		Cast.Description = TEXT("Cast an object to a specific type");
		Cast.Usage = TEXT("Use when you have a base type but need access to functionality of a derived type. Returns null if cast fails.");
		Cast.bIsPure = false;
		Cast.Source = TEXT("BuiltIn");
		Cast.Tags.Add(TEXT("cast"));
		Cast.Tags.Add(TEXT("utility"));
		Cast.Keywords.Add(TEXT("convert"));
		Cast.Keywords.Add(TEXT("type"));
		Cast.Keywords.Add(TEXT("as"));
		Cast.Examples.Add(TEXT("Cast GetPlayerPawn result to BP_PlayerCharacter to access custom functions"));

		AddNodeType(Cast);
	}

	// IsValid
	{
		FOliveNodeTypeInfo IsValid;
		IsValid.TypeId = TEXT("BuiltIn_IsValid");
		IsValid.DisplayName = TEXT("Is Valid");
		IsValid.Category = TEXT("Utilities");
		IsValid.Description = TEXT("Check if an object reference is valid (not null and not pending kill)");
		IsValid.Usage = TEXT("Use before accessing object references that might be invalid. Returns true if safe to use.");
		IsValid.bIsPure = true;
		IsValid.Source = TEXT("BuiltIn");
		IsValid.Tags.Add(TEXT("utility"));
		IsValid.Keywords.Add(TEXT("null"));
		IsValid.Keywords.Add(TEXT("check"));
		IsValid.Keywords.Add(TEXT("valid"));
		IsValid.Examples.Add(TEXT("Check if TargetEnemy is valid before applying damage"));

		AddNodeType(IsValid);
	}

	// Get/Set Actor Location
	{
		FOliveNodeTypeInfo GetActorLocation;
		GetActorLocation.TypeId = TEXT("BuiltIn_GetActorLocation");
		GetActorLocation.DisplayName = TEXT("Get Actor Location");
		GetActorLocation.Category = TEXT("Transformation");
		GetActorLocation.Description = TEXT("Get the world location of an actor");
		GetActorLocation.Usage = TEXT("Returns the actor's position as a Vector (X, Y, Z).");
		GetActorLocation.bIsPure = true;
		GetActorLocation.Source = TEXT("BuiltIn");
		GetActorLocation.Tags.Add(TEXT("transform"));
		GetActorLocation.Tags.Add(TEXT("actor"));
		GetActorLocation.Keywords.Add(TEXT("position"));
		GetActorLocation.Keywords.Add(TEXT("location"));
		GetActorLocation.Keywords.Add(TEXT("coordinates"));

		AddNodeType(GetActorLocation);

		FOliveNodeTypeInfo SetActorLocation;
		SetActorLocation.TypeId = TEXT("BuiltIn_SetActorLocation");
		SetActorLocation.DisplayName = TEXT("Set Actor Location");
		SetActorLocation.Category = TEXT("Transformation");
		SetActorLocation.Description = TEXT("Set the world location of an actor");
		SetActorLocation.Usage = TEXT("Teleports the actor to the new position. Set Sweep to true for collision detection during movement.");
		SetActorLocation.bIsPure = false;
		SetActorLocation.Source = TEXT("BuiltIn");
		SetActorLocation.Tags.Add(TEXT("transform"));
		SetActorLocation.Tags.Add(TEXT("actor"));
		SetActorLocation.Keywords.Add(TEXT("position"));
		SetActorLocation.Keywords.Add(TEXT("move"));
		SetActorLocation.Keywords.Add(TEXT("teleport"));

		AddNodeType(SetActorLocation);
	}

	// Get/Set Actor Rotation
	{
		FOliveNodeTypeInfo GetActorRotation;
		GetActorRotation.TypeId = TEXT("BuiltIn_GetActorRotation");
		GetActorRotation.DisplayName = TEXT("Get Actor Rotation");
		GetActorRotation.Category = TEXT("Transformation");
		GetActorRotation.Description = TEXT("Get the world rotation of an actor");
		GetActorRotation.Usage = TEXT("Returns the actor's rotation as a Rotator (Pitch, Yaw, Roll).");
		GetActorRotation.bIsPure = true;
		GetActorRotation.Source = TEXT("BuiltIn");
		GetActorRotation.Tags.Add(TEXT("transform"));
		GetActorRotation.Tags.Add(TEXT("actor"));
		GetActorRotation.Keywords.Add(TEXT("orientation"));
		GetActorRotation.Keywords.Add(TEXT("angle"));

		AddNodeType(GetActorRotation);

		FOliveNodeTypeInfo SetActorRotation;
		SetActorRotation.TypeId = TEXT("BuiltIn_SetActorRotation");
		SetActorRotation.DisplayName = TEXT("Set Actor Rotation");
		SetActorRotation.Category = TEXT("Transformation");
		SetActorRotation.Description = TEXT("Set the world rotation of an actor");
		SetActorRotation.Usage = TEXT("Rotates the actor to the specified orientation.");
		SetActorRotation.bIsPure = false;
		SetActorRotation.Source = TEXT("BuiltIn");
		SetActorRotation.Tags.Add(TEXT("transform"));
		SetActorRotation.Tags.Add(TEXT("actor"));
		SetActorRotation.Keywords.Add(TEXT("rotate"));
		SetActorRotation.Keywords.Add(TEXT("orientation"));

		AddNodeType(SetActorRotation);
	}

	// Make Vector/Rotator/Transform
	{
		FOliveNodeTypeInfo MakeVector;
		MakeVector.TypeId = TEXT("BuiltIn_MakeVector");
		MakeVector.DisplayName = TEXT("Make Vector");
		MakeVector.Category = TEXT("Utilities");
		MakeVector.Subcategory = TEXT("Struct");
		MakeVector.Description = TEXT("Construct a Vector from X, Y, Z components");
		MakeVector.Usage = TEXT("Creates a Vector from individual float values. Use for positions, directions, etc.");
		MakeVector.bIsPure = true;
		MakeVector.Source = TEXT("BuiltIn");
		MakeVector.Tags.Add(TEXT("vector"));
		MakeVector.Tags.Add(TEXT("struct"));
		MakeVector.Keywords.Add(TEXT("construct"));
		MakeVector.Keywords.Add(TEXT("create"));

		AddNodeType(MakeVector);

		FOliveNodeTypeInfo MakeRotator;
		MakeRotator.TypeId = TEXT("BuiltIn_MakeRotator");
		MakeRotator.DisplayName = TEXT("Make Rotator");
		MakeRotator.Category = TEXT("Utilities");
		MakeRotator.Subcategory = TEXT("Struct");
		MakeRotator.Description = TEXT("Construct a Rotator from Pitch, Yaw, Roll");
		MakeRotator.Usage = TEXT("Creates a Rotator from angles in degrees.");
		MakeRotator.bIsPure = true;
		MakeRotator.Source = TEXT("BuiltIn");
		MakeRotator.Tags.Add(TEXT("rotator"));
		MakeRotator.Tags.Add(TEXT("struct"));
		MakeRotator.Keywords.Add(TEXT("construct"));
		MakeRotator.Keywords.Add(TEXT("rotation"));

		AddNodeType(MakeRotator);

		FOliveNodeTypeInfo MakeTransform;
		MakeTransform.TypeId = TEXT("BuiltIn_MakeTransform");
		MakeTransform.DisplayName = TEXT("Make Transform");
		MakeTransform.Category = TEXT("Utilities");
		MakeTransform.Subcategory = TEXT("Struct");
		MakeTransform.Description = TEXT("Construct a Transform from location, rotation, and scale");
		MakeTransform.Usage = TEXT("Creates a Transform combining position, rotation, and scale.");
		MakeTransform.bIsPure = true;
		MakeTransform.Source = TEXT("BuiltIn");
		MakeTransform.Tags.Add(TEXT("transform"));
		MakeTransform.Tags.Add(TEXT("struct"));
		MakeTransform.Keywords.Add(TEXT("construct"));

		AddNodeType(MakeTransform);
	}

	// Break Vector/Rotator
	{
		FOliveNodeTypeInfo BreakVector;
		BreakVector.TypeId = TEXT("BuiltIn_BreakVector");
		BreakVector.DisplayName = TEXT("Break Vector");
		BreakVector.Category = TEXT("Utilities");
		BreakVector.Subcategory = TEXT("Struct");
		BreakVector.Description = TEXT("Extract X, Y, Z components from a Vector");
		BreakVector.Usage = TEXT("Decomposes a Vector into its individual float components.");
		BreakVector.bIsPure = true;
		BreakVector.Source = TEXT("BuiltIn");
		BreakVector.Tags.Add(TEXT("vector"));
		BreakVector.Tags.Add(TEXT("struct"));
		BreakVector.Keywords.Add(TEXT("decompose"));
		BreakVector.Keywords.Add(TEXT("split"));

		AddNodeType(BreakVector);

		FOliveNodeTypeInfo BreakRotator;
		BreakRotator.TypeId = TEXT("BuiltIn_BreakRotator");
		BreakRotator.DisplayName = TEXT("Break Rotator");
		BreakRotator.Category = TEXT("Utilities");
		BreakRotator.Subcategory = TEXT("Struct");
		BreakRotator.Description = TEXT("Extract Pitch, Yaw, Roll from a Rotator");
		BreakRotator.Usage = TEXT("Decomposes a Rotator into its angle components.");
		BreakRotator.bIsPure = true;
		BreakRotator.Source = TEXT("BuiltIn");
		BreakRotator.Tags.Add(TEXT("rotator"));
		BreakRotator.Tags.Add(TEXT("struct"));
		BreakRotator.Keywords.Add(TEXT("decompose"));
		BreakRotator.Keywords.Add(TEXT("split"));

		AddNodeType(BreakRotator);
	}

	// Event nodes (common events)
	{
		FOliveNodeTypeInfo BeginPlay;
		BeginPlay.TypeId = TEXT("BuiltIn_Event_BeginPlay");
		BeginPlay.DisplayName = TEXT("Event BeginPlay");
		BeginPlay.Category = TEXT("Events");
		BeginPlay.Description = TEXT("Called when the game starts or the actor is spawned");
		BeginPlay.Usage = TEXT("Use for initialization logic. Called once when the actor becomes active.");
		BeginPlay.bIsPure = false;
		BeginPlay.bIsEvent = true;
		BeginPlay.Source = TEXT("BuiltIn");
		BeginPlay.Tags.Add(TEXT("event"));
		BeginPlay.Keywords.Add(TEXT("start"));
		BeginPlay.Keywords.Add(TEXT("initialize"));
		BeginPlay.Keywords.Add(TEXT("begin"));

		AddNodeType(BeginPlay);

		FOliveNodeTypeInfo Tick;
		Tick.TypeId = TEXT("BuiltIn_Event_Tick");
		Tick.DisplayName = TEXT("Event Tick");
		Tick.Category = TEXT("Events");
		Tick.Description = TEXT("Called every frame");
		Tick.Usage = TEXT("Use sparingly - runs every frame so can impact performance. Delta Seconds gives time since last frame.");
		Tick.bIsPure = false;
		Tick.bIsEvent = true;
		Tick.Source = TEXT("BuiltIn");
		Tick.Tags.Add(TEXT("event"));
		Tick.Keywords.Add(TEXT("update"));
		Tick.Keywords.Add(TEXT("frame"));
		Tick.Keywords.Add(TEXT("loop"));

		AddNodeType(Tick);

		FOliveNodeTypeInfo EndPlay;
		EndPlay.TypeId = TEXT("BuiltIn_Event_EndPlay");
		EndPlay.DisplayName = TEXT("Event EndPlay");
		EndPlay.Category = TEXT("Events");
		EndPlay.Description = TEXT("Called when the actor is destroyed or the game ends");
		EndPlay.Usage = TEXT("Use for cleanup logic. The End Play Reason tells you why (destroyed, level change, etc.).");
		EndPlay.bIsPure = false;
		EndPlay.bIsEvent = true;
		EndPlay.Source = TEXT("BuiltIn");
		EndPlay.Tags.Add(TEXT("event"));
		EndPlay.Keywords.Add(TEXT("destroy"));
		EndPlay.Keywords.Add(TEXT("cleanup"));
		EndPlay.Keywords.Add(TEXT("end"));

		AddNodeType(EndPlay);
	}

	UE_LOG(LogOliveNodeCatalog, Log, TEXT("Added built-in utility nodes"));
}

void FOliveNodeCatalog::BuildPinsFromFunction(const UFunction* Function, FOliveNodeTypeInfo& OutInfo)
{
	if (!Function)
	{
		return;
	}

	// Add exec pins for non-pure functions
	if (!OutInfo.bIsPure)
	{
		FOliveIRPin ExecIn;
		ExecIn.Name = TEXT("execute");
		ExecIn.bIsInput = true;
		ExecIn.bIsExec = true;
		OutInfo.InputPins.Add(ExecIn);

		FOliveIRPin ExecOut;
		ExecOut.Name = TEXT("then");
		ExecOut.bIsInput = false;
		ExecOut.bIsExec = true;
		OutInfo.OutputPins.Add(ExecOut);
	}

	// Iterate function parameters
	for (TFieldIterator<FProperty> PropIt(Function); PropIt; ++PropIt)
	{
		FProperty* Property = *PropIt;

		FOliveIRPin Pin;
		Pin.Name = Property->GetName();
		Pin.DisplayName = Property->GetDisplayNameText().ToString();
		if (Pin.DisplayName.IsEmpty())
		{
			Pin.DisplayName = Pin.Name;
		}

		// Determine direction
		bool bIsOutput = Property->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm);
		Pin.bIsInput = !bIsOutput;
		Pin.bIsExec = false;

		// Determine type (simplified)
		if (CastField<FBoolProperty>(Property))
		{
			Pin.Type.Category = EOliveIRTypeCategory::Bool;
		}
		else if (CastField<FIntProperty>(Property))
		{
			Pin.Type.Category = EOliveIRTypeCategory::Int;
		}
		else if (CastField<FFloatProperty>(Property) || CastField<FDoubleProperty>(Property))
		{
			Pin.Type.Category = EOliveIRTypeCategory::Float;
		}
		else if (CastField<FStrProperty>(Property))
		{
			Pin.Type.Category = EOliveIRTypeCategory::String;
		}
		else if (CastField<FNameProperty>(Property))
		{
			Pin.Type.Category = EOliveIRTypeCategory::Name;
		}
		else if (CastField<FTextProperty>(Property))
		{
			Pin.Type.Category = EOliveIRTypeCategory::Text;
		}
		else if (FStructProperty* StructProp = CastField<FStructProperty>(Property))
		{
			Pin.Type.Category = EOliveIRTypeCategory::Struct;
			Pin.Type.StructName = StructProp->Struct->GetName();
		}
		else if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Property))
		{
			Pin.Type.Category = EOliveIRTypeCategory::Object;
			if (ObjProp->PropertyClass)
			{
				Pin.Type.ClassName = ObjProp->PropertyClass->GetName();
			}
		}
		else if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
		{
			Pin.Type.Category = EOliveIRTypeCategory::Enum;
			if (EnumProp->GetEnum())
			{
				Pin.Type.EnumName = EnumProp->GetEnum()->GetName();
			}
		}
		else if (CastField<FArrayProperty>(Property))
		{
			Pin.Type.Category = EOliveIRTypeCategory::Array;
		}
		else
		{
			Pin.Type.Category = EOliveIRTypeCategory::Unknown;
		}

		// Get default value
		FString DefaultValue;
		if (Property->HasMetaData(TEXT("CPP_Default_")))
		{
			// Simplification - full implementation would parse default values properly
		}

		if (bIsOutput)
		{
			OutInfo.OutputPins.Add(Pin);
		}
		else
		{
			OutInfo.InputPins.Add(Pin);
		}
	}
}

void FOliveNodeCatalog::BuildIndexes()
{
	FScopeLock Lock(&CatalogLock);

	CategoryIndex.Empty();
	ClassIndex.Empty();
	TagIndex.Empty();

	for (const auto& Pair : NodeTypes)
	{
		const FOliveNodeTypeInfo& Info = Pair.Value;

		// Category index
		if (!CategoryIndex.Contains(Info.Category))
		{
			CategoryIndex.Add(Info.Category, TArray<FString>());
		}
		CategoryIndex[Info.Category].Add(Info.TypeId);

		// Class index (for member functions)
		if (!Info.FunctionClass.IsEmpty())
		{
			if (!ClassIndex.Contains(Info.FunctionClass))
			{
				ClassIndex.Add(Info.FunctionClass, TArray<FString>());
			}
			ClassIndex[Info.FunctionClass].Add(Info.TypeId);
		}

		// Required class index
		if (!Info.RequiredClass.IsEmpty())
		{
			if (!ClassIndex.Contains(Info.RequiredClass))
			{
				ClassIndex.Add(Info.RequiredClass, TArray<FString>());
			}
			ClassIndex[Info.RequiredClass].Add(Info.TypeId);
		}

		// Tag index
		for (const FString& Tag : Info.Tags)
		{
			if (!TagIndex.Contains(Tag))
			{
				TagIndex.Add(Tag, TArray<FString>());
			}
			TagIndex[Tag].Add(Info.TypeId);
		}
	}

	UE_LOG(LogOliveNodeCatalog, Verbose, TEXT("Built indexes: %d categories, %d classes, %d tags"),
		CategoryIndex.Num(), ClassIndex.Num(), TagIndex.Num());
}

void FOliveNodeCatalog::AddNodeType(const FOliveNodeTypeInfo& Info)
{
	FScopeLock Lock(&CatalogLock);

	// Skip if already exists (prefer existing, more specific entries)
	if (NodeTypes.Contains(Info.TypeId))
	{
		return;
	}

	NodeTypes.Add(Info.TypeId, Info);
}

// ============================================================================
// Helper Methods
// ============================================================================

FString FOliveNodeCatalog::GetFunctionCategory(const UFunction* Function) const
{
	if (!Function)
	{
		return TEXT("Uncategorized");
	}

	// Try to get category from metadata
	FString Category = Function->GetMetaData(TEXT("Category"));
	if (!Category.IsEmpty())
	{
		return Category;
	}

	// Fall back to owning class name
	if (UClass* OwnerClass = Function->GetOwnerClass())
	{
		return OwnerClass->GetName();
	}

	return TEXT("Uncategorized");
}

bool FOliveNodeCatalog::ShouldIncludeFunction(const UFunction* Function) const
{
	if (!Function)
	{
		return false;
	}

	// Must be Blueprint callable
	if (!Function->HasAnyFunctionFlags(FUNC_BlueprintCallable))
	{
		return false;
	}

	// Skip editor-only functions in most cases
	if (Function->HasMetaData(TEXT("DevelopmentOnly")))
	{
		// Still include for editor plugin
	}

	// Skip deprecated
	if (Function->HasMetaData(TEXT("DeprecatedFunction")))
	{
		// Include but mark as deprecated
	}

	// Skip internal functions
	if (Function->HasMetaData(TEXT("BlueprintInternalUseOnly")))
	{
		return false;
	}

	return true;
}
