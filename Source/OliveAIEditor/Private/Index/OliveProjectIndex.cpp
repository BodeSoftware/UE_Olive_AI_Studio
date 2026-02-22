// Copyright Bode Software. All Rights Reserved.

#include "Index/OliveProjectIndex.h"
#include "OliveAIEditorModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FOliveProjectIndex& FOliveProjectIndex::Get()
{
	static FOliveProjectIndex Instance;
	return Instance;
}

FOliveProjectIndex::FOliveProjectIndex()
{
}

FOliveProjectIndex::~FOliveProjectIndex()
{
	Shutdown();
}

void FOliveProjectIndex::Initialize()
{
	UE_LOG(LogOliveAI, Log, TEXT("Initializing project index..."));

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	// Check if asset registry is ready
	if (AssetRegistry.IsLoadingAssets())
	{
		// Defer initialization
		FilesLoadedHandle = AssetRegistry.OnFilesLoaded().AddRaw(this, &FOliveProjectIndex::OnFilesLoaded);
		UE_LOG(LogOliveAI, Log, TEXT("Asset registry still loading, deferring index build..."));
		return;
	}

	// Asset registry is ready, build now
	RebuildIndex();
}

void FOliveProjectIndex::Shutdown()
{
	IAssetRegistry* AssetRegistry = FModuleManager::GetModulePtr<FAssetRegistryModule>("AssetRegistry")
		? &FModuleManager::GetModuleChecked<FAssetRegistryModule>("AssetRegistry").Get()
		: nullptr;

	if (AssetRegistry)
	{
		if (AssetAddedHandle.IsValid())
		{
			AssetRegistry->OnAssetAdded().Remove(AssetAddedHandle);
		}
		if (AssetRemovedHandle.IsValid())
		{
			AssetRegistry->OnAssetRemoved().Remove(AssetRemovedHandle);
		}
		if (AssetRenamedHandle.IsValid())
		{
			AssetRegistry->OnAssetRenamed().Remove(AssetRenamedHandle);
		}
		if (AssetUpdatedHandle.IsValid())
		{
			AssetRegistry->OnAssetUpdated().Remove(AssetUpdatedHandle);
		}
		if (FilesLoadedHandle.IsValid())
		{
			AssetRegistry->OnFilesLoaded().Remove(FilesLoadedHandle);
		}
	}

	FScopeLock Lock(&IndexLock);
	AssetIndex.Empty();
	ClassHierarchy.Empty();
	bIsReady = false;

	UE_LOG(LogOliveAI, Log, TEXT("Project index shutdown"));
}

void FOliveProjectIndex::OnFilesLoaded()
{
	UE_LOG(LogOliveAI, Log, TEXT("Asset registry files loaded, building index..."));
	RebuildIndex();
}

void FOliveProjectIndex::RebuildIndex()
{
	if (bIsBuilding)
	{
		return;
	}

	bIsBuilding = true;
	UE_LOG(LogOliveAI, Log, TEXT("Building project index..."));

	// Build project config
	BuildProjectConfig();

	// Get all assets
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	TArray<FAssetData> AllAssets;
	AssetRegistry.GetAllAssets(AllAssets, true);

	UE_LOG(LogOliveAI, Log, TEXT("Found %d assets to index"), AllAssets.Num());

	{
		FScopeLock Lock(&IndexLock);
		AssetIndex.Empty(AllAssets.Num());

		for (const FAssetData& AssetData : AllAssets)
		{
			// Skip engine content by default
			if (AssetData.PackageName.ToString().StartsWith(TEXT("/Engine/")))
			{
				continue;
			}

			FOliveAssetInfo Info = AssetDataToInfo(AssetData);
			AssetIndex.Add(Info.Path, Info);
		}
	}

	// Build class hierarchy
	BuildClassHierarchy();

	// Register for updates
	AssetAddedHandle = AssetRegistry.OnAssetAdded().AddRaw(this, &FOliveProjectIndex::OnAssetAdded);
	AssetRemovedHandle = AssetRegistry.OnAssetRemoved().AddRaw(this, &FOliveProjectIndex::OnAssetRemoved);
	AssetRenamedHandle = AssetRegistry.OnAssetRenamed().AddRaw(this, &FOliveProjectIndex::OnAssetRenamed);
	AssetUpdatedHandle = AssetRegistry.OnAssetUpdated().AddRaw(this, &FOliveProjectIndex::OnAssetUpdated);

	bIsBuilding = false;
	bIsReady = true;

	UE_LOG(LogOliveAI, Log, TEXT("Project index ready: %d assets indexed"), AssetIndex.Num());
}

void FOliveProjectIndex::BuildProjectConfig()
{
	ProjectConfig.ProjectName = FApp::GetProjectName();
	ProjectConfig.EngineVersion = FEngineVersion::Current().ToString();

	// Get enabled plugins
	IPluginManager& PluginManager = IPluginManager::Get();
	TArray<TSharedRef<IPlugin>> EnabledPlugins = PluginManager.GetEnabledPlugins();
	for (const TSharedRef<IPlugin>& Plugin : EnabledPlugins)
	{
		ProjectConfig.EnabledPlugins.Add(Plugin->GetName());
	}
}

void FOliveProjectIndex::BuildClassHierarchy()
{
	// Build from registered UClasses
	// This is simplified - full implementation would traverse all native and BP classes
	FScopeLock Lock(&IndexLock);
	ClassHierarchy.Empty();

	// Add common base classes
	TArray<FName> CommonClasses = {
		FName(TEXT("AActor")),
		FName(TEXT("APawn")),
		FName(TEXT("ACharacter")),
		FName(TEXT("UActorComponent")),
		FName(TEXT("USceneComponent")),
		FName(TEXT("UObject"))
	};

	for (const FName& ClassName : CommonClasses)
	{
		FOliveClassHierarchyNode Node;
		Node.ClassName = ClassName;
		ClassHierarchy.Add(ClassName, Node);
	}
}

FOliveAssetInfo FOliveProjectIndex::AssetDataToInfo(const FAssetData& AssetData) const
{
	FOliveAssetInfo Info;
	Info.Name = AssetData.AssetName.ToString();
	Info.Path = AssetData.GetObjectPathString();
	Info.AssetClass = AssetData.AssetClassPath.GetAssetName();

	// Check asset type
	FString ClassStr = Info.AssetClass.ToString();
	Info.bIsBlueprint = ClassStr.Contains(TEXT("Blueprint"));
	Info.bIsBehaviorTree = ClassStr == TEXT("BehaviorTree");
	Info.bIsBlackboard = ClassStr == TEXT("BlackboardData");
	Info.bIsPCG = ClassStr.Contains(TEXT("PCG"));
	Info.bIsMaterial = ClassStr.Contains(TEXT("Material"));
	Info.bIsWidget = ClassStr.Contains(TEXT("Widget"));

	// Track BT -> Blackboard association when available in asset tags.
	if (Info.bIsBehaviorTree)
	{
		static const TArray<FName> BlackboardTagCandidates = {
			FName(TEXT("BlackboardAsset")),
			FName(TEXT("Blackboard")),
			FName(TEXT("BlackboardData"))
		};

		for (const FName& TagName : BlackboardTagCandidates)
		{
			FAssetTagValueRef TagValue = AssetData.TagsAndValues.FindTag(TagName);
			if (TagValue.IsSet())
			{
				Info.AssociatedBlackboardPath = TagValue.GetValue();
				break;
			}
		}
	}

	// Get parent class for Blueprints
	if (Info.bIsBlueprint)
	{
		FAssetTagValueRef ParentTag = AssetData.TagsAndValues.FindTag(FName(TEXT("ParentClass")));
		if (ParentTag.IsSet())
		{
			Info.ParentClass = FName(*ParentTag.GetValue());
		}
	}

	return Info;
}

TArray<FOliveAssetInfo> FOliveProjectIndex::SearchAssets(const FString& Query, int32 MaxResults) const
{
	TArray<TPair<int32, FOliveAssetInfo>> ScoredResults;

	FScopeLock Lock(&IndexLock);

	// For @mention UX we want a sensible default list even when the query is empty.
	// Return a small sample (unordered) rather than nothing.
	if (Query.IsEmpty())
	{
		TArray<FOliveAssetInfo> Results;
		for (const auto& Pair : AssetIndex)
		{
			Results.Add(Pair.Value);
			if (Results.Num() >= MaxResults)
			{
				break;
			}
		}
		return Results;
	}

	for (const auto& Pair : AssetIndex)
	{
		int32 Score = CalculateFuzzyScore(Query, Pair.Value.Name);
		if (Score > 50)  // Minimum threshold
		{
			ScoredResults.Emplace(Score, Pair.Value);
		}
	}

	// Sort by score descending
	ScoredResults.Sort([](const auto& A, const auto& B) { return A.Key > B.Key; });

	// Return top results
	TArray<FOliveAssetInfo> Results;
	for (int32 i = 0; i < FMath::Min(MaxResults, ScoredResults.Num()); ++i)
	{
		Results.Add(ScoredResults[i].Value);
	}

	return Results;
}

int32 FOliveProjectIndex::CalculateFuzzyScore(const FString& Query, const FString& Target) const
{
	if (Query.IsEmpty() || Target.IsEmpty())
	{
		return 0;
	}

	FString LowerQuery = Query.ToLower();
	FString LowerTarget = Target.ToLower();

	// Exact match
	if (LowerTarget == LowerQuery)
	{
		return 1000;
	}

	// Starts with
	if (LowerTarget.StartsWith(LowerQuery))
	{
		return 500 + (100 * LowerQuery.Len() / LowerTarget.Len());
	}

	// Contains
	if (LowerTarget.Contains(LowerQuery))
	{
		int32 Index = LowerTarget.Find(LowerQuery);
		return 200 - Index;  // Earlier match = higher score
	}

	// Acronym match (e.g., "BP_PlayerCharacter" matches "bppc")
	FString Acronym;
	for (int32 i = 0; i < Target.Len(); ++i)
	{
		if (i == 0 || Target[i] == '_' || FChar::IsUpper(Target[i]))
		{
			if (Target[i] != '_')
			{
				Acronym.AppendChar(FChar::ToLower(Target[i]));
			}
		}
	}

	if (Acronym.Contains(LowerQuery) || LowerQuery.Contains(Acronym))
	{
		return 300;
	}

	// Partial character match
	int32 MatchingChars = 0;
	for (TCHAR C : LowerQuery)
	{
		if (LowerTarget.Contains(FString::Chr(C)))
		{
			MatchingChars++;
		}
	}

	return (100 * MatchingChars / LowerQuery.Len());
}

TArray<FOliveAssetInfo> FOliveProjectIndex::GetAssetsByClass(FName ClassName) const
{
	TArray<FOliveAssetInfo> Results;

	FScopeLock Lock(&IndexLock);
	for (const auto& Pair : AssetIndex)
	{
		if (Pair.Value.AssetClass == ClassName)
		{
			Results.Add(Pair.Value);
		}
	}

	return Results;
}

TOptional<FOliveAssetInfo> FOliveProjectIndex::GetAssetByPath(const FString& Path) const
{
	FScopeLock Lock(&IndexLock);

	const FOliveAssetInfo* Found = AssetIndex.Find(Path);
	if (Found)
	{
		return *Found;
	}

	return TOptional<FOliveAssetInfo>();
}

TArray<FOliveAssetInfo> FOliveProjectIndex::GetAllBlueprints() const
{
	TArray<FOliveAssetInfo> Results;

	FScopeLock Lock(&IndexLock);
	for (const auto& Pair : AssetIndex)
	{
		if (Pair.Value.bIsBlueprint)
		{
			Results.Add(Pair.Value);
		}
	}

	return Results;
}

TArray<FOliveAssetInfo> FOliveProjectIndex::GetAllBehaviorTrees() const
{
	TArray<FOliveAssetInfo> Results;

	FScopeLock Lock(&IndexLock);
	for (const auto& Pair : AssetIndex)
	{
		if (Pair.Value.bIsBehaviorTree)
		{
			Results.Add(Pair.Value);
		}
	}

	return Results;
}

TArray<FOliveAssetInfo> FOliveProjectIndex::GetAllCppClasses() const
{
	FScopeLock Lock(&IndexLock);
	TArray<FOliveAssetInfo> Result;
	for (const auto& Pair : AssetIndex)
	{
		if (Pair.Value.bIsCppClass)
		{
			Result.Add(Pair.Value);
		}
	}
	return Result;
}

FString FOliveProjectIndex::FindHeaderForClass(const FString& ClassName) const
{
	FScopeLock Lock(&IndexLock);
	for (const auto& Pair : AssetIndex)
	{
		if (Pair.Value.bIsCppClass && Pair.Value.Name == ClassName)
		{
			return Pair.Value.SourceHeaderPath;
		}
	}
	return FString();
}

TArray<FName> FOliveProjectIndex::GetChildClasses(FName ParentClass) const
{
	FScopeLock Lock(&IndexLock);

	const FOliveClassHierarchyNode* Node = ClassHierarchy.Find(ParentClass);
	if (Node)
	{
		return Node->ChildClasses;
	}

	return {};
}

TArray<FName> FOliveProjectIndex::GetParentChain(FName ChildClass) const
{
	TArray<FName> Chain;

	FScopeLock Lock(&IndexLock);

	FName CurrentClass = ChildClass;
	int32 MaxDepth = 50;

	while (CurrentClass != NAME_None && MaxDepth-- > 0)
	{
		const FOliveClassHierarchyNode* Node = ClassHierarchy.Find(CurrentClass);
		if (!Node)
		{
			break;
		}

		Chain.Add(CurrentClass);
		CurrentClass = Node->ParentClassName;
	}

	return Chain;
}

bool FOliveProjectIndex::IsChildOf(FName ChildClass, FName ParentClass) const
{
	TArray<FName> Chain = GetParentChain(ChildClass);
	return Chain.Contains(ParentClass);
}

TArray<FString> FOliveProjectIndex::GetDependencies(const FString& AssetPath) const
{
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	TArray<FName> Dependencies;
	AssetRegistry.GetDependencies(FName(*AssetPath), Dependencies);

	TArray<FString> Results;
	for (const FName& Dep : Dependencies)
	{
		Results.Add(Dep.ToString());
	}

	return Results;
}

TArray<FString> FOliveProjectIndex::GetReferencers(const FString& AssetPath) const
{
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	TArray<FName> Referencers;
	AssetRegistry.GetReferencers(FName(*AssetPath), Referencers);

	TArray<FString> Results;
	for (const FName& Ref : Referencers)
	{
		Results.Add(Ref.ToString());
	}

	return Results;
}

int32 FOliveProjectIndex::GetAssetCount() const
{
	FScopeLock Lock(&IndexLock);
	return AssetIndex.Num();
}

// Asset registry callbacks

void FOliveProjectIndex::OnAssetAdded(const FAssetData& AssetData)
{
	if (AssetData.PackageName.ToString().StartsWith(TEXT("/Engine/")))
	{
		return;
	}

	FOliveAssetInfo Info = AssetDataToInfo(AssetData);

	FScopeLock Lock(&IndexLock);
	AssetIndex.Add(Info.Path, Info);
	bDirty = true;

	UE_LOG(LogOliveAI, Verbose, TEXT("Asset added to index: %s"), *Info.Path);
}

void FOliveProjectIndex::OnAssetRemoved(const FAssetData& AssetData)
{
	FString Path = AssetData.GetObjectPathString();

	FScopeLock Lock(&IndexLock);
	AssetIndex.Remove(Path);
	bDirty = true;

	UE_LOG(LogOliveAI, Verbose, TEXT("Asset removed from index: %s"), *Path);
}

void FOliveProjectIndex::OnAssetRenamed(const FAssetData& AssetData, const FString& OldPath)
{
	FScopeLock Lock(&IndexLock);

	AssetIndex.Remove(OldPath);

	FOliveAssetInfo Info = AssetDataToInfo(AssetData);
	AssetIndex.Add(Info.Path, Info);
	bDirty = true;

	UE_LOG(LogOliveAI, Verbose, TEXT("Asset renamed in index: %s -> %s"), *OldPath, *Info.Path);
}

void FOliveProjectIndex::OnAssetUpdated(const FAssetData& AssetData)
{
	FOliveAssetInfo Info = AssetDataToInfo(AssetData);

	FScopeLock Lock(&IndexLock);
	AssetIndex.Add(Info.Path, Info);  // Overwrite existing
	bDirty = true;

	UE_LOG(LogOliveAI, Verbose, TEXT("Asset updated in index: %s"), *Info.Path);
}

// Tick

void FOliveProjectIndex::Tick(float DeltaTime)
{
	TimeSinceLastProcess += DeltaTime;

	if (TimeSinceLastProcess >= ProcessInterval)
	{
		ProcessPendingUpdates();
		TimeSinceLastProcess = 0.0f;
	}
}

TStatId FOliveProjectIndex::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(FOliveProjectIndex, STATGROUP_Tickables);
}

void FOliveProjectIndex::ProcessPendingUpdates()
{
	TPair<FString, bool> Update;
	while (PendingUpdates.Dequeue(Update))
	{
		// Process queued updates
		// This is for batching rapid changes
	}
}

// JSON export

TSharedPtr<FJsonObject> FOliveAssetInfo::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Name);
	Json->SetStringField(TEXT("path"), Path);
	Json->SetStringField(TEXT("class"), AssetClass.ToString());

	if (ParentClass != NAME_None)
	{
		Json->SetStringField(TEXT("parent_class"), ParentClass.ToString());
	}

	if (Interfaces.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> InterfacesArray;
		for (const FString& Interface : Interfaces)
		{
			InterfacesArray.Add(MakeShared<FJsonValueString>(Interface));
		}
		Json->SetArrayField(TEXT("interfaces"), InterfacesArray);
	}

	// Type flags
	Json->SetBoolField(TEXT("is_blueprint"), bIsBlueprint);
	Json->SetBoolField(TEXT("is_behavior_tree"), bIsBehaviorTree);
	Json->SetBoolField(TEXT("is_blackboard"), bIsBlackboard);
	Json->SetBoolField(TEXT("is_pcg"), bIsPCG);
	if (!AssociatedBlackboardPath.IsEmpty())
	{
		Json->SetStringField(TEXT("associated_blackboard"), AssociatedBlackboardPath);
	}
	if (bIsCppClass)
	{
		Json->SetBoolField(TEXT("is_cpp_class"), true);
	}
	if (!SourceHeaderPath.IsEmpty())
	{
		Json->SetStringField(TEXT("source_header"), SourceHeaderPath);
	}

	return Json;
}

TSharedPtr<FJsonObject> FOliveProjectConfig::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("project_name"), ProjectName);
	Json->SetStringField(TEXT("engine_version"), EngineVersion);

	TArray<TSharedPtr<FJsonValue>> PluginsArray;
	for (const FString& Plugin : EnabledPlugins)
	{
		PluginsArray.Add(MakeShared<FJsonValueString>(Plugin));
	}
	Json->SetArrayField(TEXT("enabled_plugins"), PluginsArray);

	return Json;
}

FString FOliveProjectIndex::GetSearchResultsJson(const FString& Query, int32 MaxResults) const
{
	TArray<FOliveAssetInfo> Results = SearchAssets(Query, MaxResults);

	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("query"), Query);
	Json->SetNumberField(TEXT("total_results"), Results.Num());

	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	for (const FOliveAssetInfo& Info : Results)
	{
		ResultsArray.Add(MakeShared<FJsonValueObject>(Info.ToJson()));
	}
	Json->SetArrayField(TEXT("results"), ResultsArray);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);
	return OutputString;
}

FString FOliveProjectIndex::GetAssetInfoJson(const FString& Path) const
{
	TOptional<FOliveAssetInfo> Info = GetAssetByPath(Path);

	if (Info.IsSet())
	{
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(Info.GetValue().ToJson().ToSharedRef(), Writer);
		return OutputString;
	}

	return TEXT("{\"error\": \"Asset not found\"}");
}

FString FOliveProjectIndex::GetProjectConfigJson() const
{
	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(ProjectConfig.ToJson().ToSharedRef(), Writer);
	return OutputString;
}

FString FOliveProjectIndex::GetClassHierarchyJson(FName RootClass) const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

	FScopeLock Lock(&IndexLock);

	TArray<TSharedPtr<FJsonValue>> NodesArray;
	for (const auto& Pair : ClassHierarchy)
	{
		NodesArray.Add(MakeShared<FJsonValueObject>(Pair.Value.ToJson()));
	}
	Json->SetArrayField(TEXT("classes"), NodesArray);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);
	return OutputString;
}

// Project Map Export

FString FOliveProjectIndex::GetDefaultProjectMapPath()
{
	return FPaths::ProjectSavedDir() / TEXT("OliveAI/ProjectMap.json");
}

bool FOliveProjectIndex::IsProjectMapStale() const
{
	return bDirty;
}

bool FOliveProjectIndex::ExportProjectMap(const FString& FilePath) const
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	// Metadata
	{
		TSharedPtr<FJsonObject> Metadata = MakeShared<FJsonObject>();
		Metadata->SetStringField(TEXT("timestamp"), FDateTime::UtcNow().ToIso8601());
		Metadata->SetNumberField(TEXT("asset_count"), GetAssetCount());

		FScopeLock Lock(&IndexLock);
		Metadata->SetNumberField(TEXT("class_count"), ClassHierarchy.Num());
		Root->SetObjectField(TEXT("metadata"), Metadata);
	}

	// Assets
	{
		TArray<TSharedPtr<FJsonValue>> AssetsArray;
		FScopeLock Lock(&IndexLock);
		for (const auto& Pair : AssetIndex)
		{
			AssetsArray.Add(MakeShared<FJsonValueObject>(Pair.Value.ToJson()));
		}
		Root->SetArrayField(TEXT("assets"), AssetsArray);
	}

	// Class hierarchy
	{
		TSharedPtr<FJsonObject> HierarchyJson = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> NodesArray;
		FScopeLock Lock(&IndexLock);
		for (const auto& Pair : ClassHierarchy)
		{
			NodesArray.Add(MakeShared<FJsonValueObject>(Pair.Value.ToJson()));
		}
		HierarchyJson->SetArrayField(TEXT("classes"), NodesArray);
		Root->SetObjectField(TEXT("class_hierarchy"), HierarchyJson);
	}

	// Project config
	Root->SetObjectField(TEXT("project_config"), ProjectConfig.ToJson());

	// Serialize to string
	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	// Ensure directory exists and save
	FString Directory = FPaths::GetPath(FilePath);
	IFileManager::Get().MakeDirectory(*Directory, true);

	if (FFileHelper::SaveStringToFile(OutputString, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		bDirty = false;
		UE_LOG(LogOliveAI, Log, TEXT("Project map exported to: %s"), *FilePath);
		return true;
	}

	UE_LOG(LogOliveAI, Error, TEXT("Failed to export project map to: %s"), *FilePath);
	return false;
}

TSharedPtr<FJsonObject> FOliveClassHierarchyNode::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), ClassName.ToString());

	if (ParentClassName != NAME_None)
	{
		Json->SetStringField(TEXT("parent"), ParentClassName.ToString());
	}

	if (ChildClasses.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ChildrenArray;
		for (const FName& Child : ChildClasses)
		{
			ChildrenArray.Add(MakeShared<FJsonValueString>(Child.ToString()));
		}
		Json->SetArrayField(TEXT("children"), ChildrenArray);
	}

	if (bIsBlueprintClass)
	{
		Json->SetBoolField(TEXT("is_blueprint"), true);
		Json->SetStringField(TEXT("blueprint_path"), BlueprintPath);
	}

	return Json;
}
