// Copyright Bode Software. All Rights Reserved.

#include "OliveCrossSystemToolHandlers.h"
#include "OliveCrossSystemSchemas.h"
#include "OliveMultiAssetOperations.h"
#include "Index/OliveProjectIndex.h"
#include "Settings/OliveAISettings.h"
#include "Writer/OliveGraphWriter.h"
#include "Compile/OliveCompileManager.h"
#include "Services/OliveBatchExecutionScope.h"
#include "Services/OliveToolParamHelpers.h"
#include "Engine/Blueprint.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Internationalization/Regex.h"

#define LOCTEXT_NAMESPACE "OliveCrossSystemToolHandlers"

DEFINE_LOG_CATEGORY(LogOliveCrossSystemTools);

FOliveCrossSystemToolHandlers& FOliveCrossSystemToolHandlers::Get()
{
	static FOliveCrossSystemToolHandlers Instance;
	return Instance;
}

void FOliveCrossSystemToolHandlers::RegisterAllTools()
{
	UE_LOG(LogOliveCrossSystemTools, Log, TEXT("Registering Cross-System MCP tools..."));

	RegisterBulkTools();
	RegisterIndexTools();

	// Recipe system
	LoadRecipeLibrary();
	RegisterRecipeTools();

	UE_LOG(LogOliveCrossSystemTools, Log, TEXT("Registered %d Cross-System MCP tools"), RegisteredToolNames.Num());
}

void FOliveCrossSystemToolHandlers::UnregisterAllTools()
{
	UE_LOG(LogOliveCrossSystemTools, Log, TEXT("Unregistering Cross-System MCP tools..."));

	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();
	for (const FString& ToolName : RegisteredToolNames)
	{
		Registry.UnregisterTool(ToolName);
	}

	RegisteredToolNames.Empty();
	UE_LOG(LogOliveCrossSystemTools, Log, TEXT("Cross-System MCP tools unregistered"));
}

void FOliveCrossSystemToolHandlers::RegisterBulkTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	Registry.RegisterTool(
		TEXT("project.bulk_read"),
		TEXT("Read up to 20 assets of any type in a single call"),
		OliveCrossSystemSchemas::ProjectBulkRead(),
		FOliveToolHandler::CreateRaw(this, &FOliveCrossSystemToolHandlers::HandleBulkRead),
		{TEXT("crosssystem"), TEXT("read")},
		TEXT("crosssystem")
	);
	RegisteredToolNames.Add(TEXT("project.bulk_read"));

	Registry.RegisterTool(
		TEXT("project.implement_interface"),
		TEXT("Add an interface to multiple Blueprint assets"),
		OliveCrossSystemSchemas::ProjectImplementInterface(),
		FOliveToolHandler::CreateRaw(this, &FOliveCrossSystemToolHandlers::HandleImplementInterface),
		{TEXT("crosssystem"), TEXT("write")},
		TEXT("crosssystem")
	);
	RegisteredToolNames.Add(TEXT("project.implement_interface"));

	Registry.RegisterTool(
		TEXT("project.refactor_rename"),
		TEXT("Rename an asset with dependency-aware reference updates"),
		OliveCrossSystemSchemas::ProjectRefactorRename(),
		FOliveToolHandler::CreateRaw(this, &FOliveCrossSystemToolHandlers::HandleRefactorRename),
		{TEXT("crosssystem"), TEXT("write")},
		TEXT("crosssystem")
	);
	RegisteredToolNames.Add(TEXT("project.refactor_rename"));

	// Removed in AI Freedom Phase 2 — AI uses individual tools (blueprint.create, behaviortree.create, blackboard.create)
	// Registry.RegisterTool(
	// 	TEXT("project.create_ai_character"),
	// 	TEXT("Create a complete AI character setup (Blueprint + BehaviorTree + Blackboard)"),
	// 	OliveCrossSystemSchemas::ProjectCreateAICharacter(),
	// 	FOliveToolHandler::CreateRaw(this, &FOliveCrossSystemToolHandlers::HandleCreateAICharacter),
	// 	{TEXT("crosssystem"), TEXT("write")},
	// 	TEXT("crosssystem")
	// );
	// RegisteredToolNames.Add(TEXT("project.create_ai_character"));

	// Removed in AI Freedom Phase 2 — rarely used, high maintenance cost
	// Registry.RegisterTool(
	// 	TEXT("project.move_to_cpp"),
	// 	TEXT("Analyze Blueprint and scaffold C++ migration artifacts"),
	// 	OliveCrossSystemSchemas::ProjectMoveToCpp(),
	// 	FOliveToolHandler::CreateRaw(this, &FOliveCrossSystemToolHandlers::HandleMoveToCpp),
	// 	{TEXT("crosssystem"), TEXT("write")},
	// 	TEXT("crosssystem")
	// );
	// RegisteredToolNames.Add(TEXT("project.move_to_cpp"));
}

// =============================================================================
// Bulk Operation Handlers
// =============================================================================

FOliveToolResult FOliveCrossSystemToolHandlers::HandleBulkRead(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FString> Paths;
	const TArray<TSharedPtr<FJsonValue>>* PathsArray;
	if (!Params->TryGetArrayField(TEXT("paths"), PathsArray))
	{
		return FOliveToolResult::Error(TEXT("MISSING_PATHS"), TEXT("'paths' array is required"),
			TEXT("Provide an array of /Game/... asset paths. Example: [\"paths\": [\"/Game/BP_Player\", \"/Game/BP_Enemy\"]]"));
	}
	for (const auto& Value : *PathsArray)
	{
		Paths.Add(Value->AsString());
	}

	FString ReadMode = TEXT("summary");
	Params->TryGetStringField(TEXT("read_mode"), ReadMode);

	return FOliveMultiAssetOperations::Get().BulkRead(Paths, ReadMode);
}

FOliveToolResult FOliveCrossSystemToolHandlers::HandleImplementInterface(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FString> Paths;
	const TArray<TSharedPtr<FJsonValue>>* PathsArray;
	if (!Params->TryGetArrayField(TEXT("paths"), PathsArray))
	{
		return FOliveToolResult::Error(TEXT("MISSING_PATHS"), TEXT("'paths' array is required"),
			TEXT("Provide an array of Blueprint asset paths. Example: [\"paths\": [\"/Game/BP_Player\"]]"));
	}
	for (const auto& Value : *PathsArray)
	{
		Paths.Add(Value->AsString());
	}

	FString Interface;
	if (!Params->TryGetStringField(TEXT("interface"), Interface) || Interface.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'interface' is missing"),
			TEXT("Provide the interface name. Example: \"BPI_Interactable\", \"BPI_Damageable\""));
	}
	return FOliveMultiAssetOperations::Get().ImplementInterface(Paths, Interface);
}

FOliveToolResult FOliveCrossSystemToolHandlers::HandleRefactorRename(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'asset_path' is missing"),
			TEXT("Provide the asset path to rename. Example: \"/Game/Blueprints/BP_OldName\""));
	}
	FString NewName;
	if (!Params->TryGetStringField(TEXT("new_name"), NewName) || NewName.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'new_name' is missing"),
			TEXT("Provide the new name for the asset. Example: \"BP_NewName\""));
	}
	bool bUpdateReferences = true;
	Params->TryGetBoolField(TEXT("update_references"), bUpdateReferences);

	return FOliveMultiAssetOperations::Get().RefactorRename(AssetPath, NewName, bUpdateReferences);
}

// =============================================================================
// Index / Context Handlers
// =============================================================================

void FOliveCrossSystemToolHandlers::RegisterIndexTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	// Removed in AI Freedom Phase 2 — project indexing is now automatic (on-demand)
	// Registry.RegisterTool(
	// 	TEXT("project.index_build"),
	// 	TEXT("Export the project index to a JSON file for external consumption"),
	// 	OliveCrossSystemSchemas::ProjectIndexBuild(),
	// 	FOliveToolHandler::CreateRaw(this, &FOliveCrossSystemToolHandlers::HandleIndexBuild),
	// 	{TEXT("crosssystem"), TEXT("read")},
	// 	TEXT("crosssystem")
	// );
	// RegisteredToolNames.Add(TEXT("project.index_build"));

	// Removed in AI Freedom Phase 2 — not needed (was only needed when index_build was manual)
	// Registry.RegisterTool(
	// 	TEXT("project.index_status"),
	// 	TEXT("Check whether the project index is stale and needs re-export"),
	// 	OliveCrossSystemSchemas::ProjectIndexStatus(),
	// 	FOliveToolHandler::CreateRaw(this, &FOliveCrossSystemToolHandlers::HandleIndexStatus),
	// 	{TEXT("crosssystem"), TEXT("read")},
	// 	TEXT("crosssystem")
	// );
	// RegisteredToolNames.Add(TEXT("project.index_status"));

	Registry.RegisterTool(
		TEXT("project.get_relevant_context"),
		TEXT("Search the project index and return the most relevant assets for a query"),
		OliveCrossSystemSchemas::ProjectGetRelevantContext(),
		FOliveToolHandler::CreateRaw(this, &FOliveCrossSystemToolHandlers::HandleGetRelevantContext),
		{TEXT("crosssystem"), TEXT("read")},
		TEXT("crosssystem")
	);
	RegisteredToolNames.Add(TEXT("project.get_relevant_context"));

	// Alias: project.search → same handler (many prompts reference this name)
	Registry.RegisterTool(
		TEXT("project.search"),
		TEXT("Search the project index and return the most relevant assets for a query (alias for project.get_relevant_context)"),
		OliveCrossSystemSchemas::ProjectGetRelevantContext(),
		FOliveToolHandler::CreateRaw(this, &FOliveCrossSystemToolHandlers::HandleGetRelevantContext),
		{TEXT("crosssystem"), TEXT("read")},
		TEXT("crosssystem")
	);
	RegisteredToolNames.Add(TEXT("project.search"));
}

FOliveToolResult FOliveCrossSystemToolHandlers::HandleIndexBuild(const TSharedPtr<FJsonObject>& Params)
{
	bool bForce = false;
	Params->TryGetBoolField(TEXT("force"), bForce);

	FOliveProjectIndex& Index = FOliveProjectIndex::Get();

	if (!Index.IsReady())
	{
		return FOliveToolResult::Error(TEXT("INDEX_NOT_READY"), TEXT("Project index is still building"));
	}

	if (!bForce && !Index.IsProjectMapStale())
	{
		FString Path = FOliveProjectIndex::GetDefaultProjectMapPath();
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("exported"), false);
		Data->SetStringField(TEXT("reason"), TEXT("Index is not stale; use force=true to re-export"));
		Data->SetStringField(TEXT("path"), Path);
		Data->SetNumberField(TEXT("asset_count"), Index.GetAssetCount());
		return FOliveToolResult::Success(Data);
	}

	FString ExportPath = FOliveProjectIndex::GetDefaultProjectMapPath();
	bool bSuccess = Index.ExportProjectMap(ExportPath);

	if (!bSuccess)
	{
		return FOliveToolResult::Error(TEXT("EXPORT_FAILED"), FString::Printf(TEXT("Failed to write project map to %s"), *ExportPath));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("exported"), true);
	Data->SetStringField(TEXT("path"), ExportPath);
	Data->SetNumberField(TEXT("asset_count"), Index.GetAssetCount());
	return FOliveToolResult::Success(Data);
}

FOliveToolResult FOliveCrossSystemToolHandlers::HandleIndexStatus(const TSharedPtr<FJsonObject>& Params)
{
	FOliveProjectIndex& Index = FOliveProjectIndex::Get();
	FString Path = FOliveProjectIndex::GetDefaultProjectMapPath();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("stale"), Index.IsProjectMapStale());
	Data->SetNumberField(TEXT("asset_count"), Index.GetAssetCount());
	Data->SetStringField(TEXT("path"), Path);
	Data->SetBoolField(TEXT("exists"), IFileManager::Get().FileExists(*Path));
	Data->SetBoolField(TEXT("ready"), Index.IsReady());
	return FOliveToolResult::Success(Data);
}

FOliveToolResult FOliveCrossSystemToolHandlers::HandleGetRelevantContext(const TSharedPtr<FJsonObject>& Params)
{
	FString Query;
	if (!Params->TryGetStringField(TEXT("query"), Query) || Query.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("MISSING_QUERY"), TEXT("query parameter is required"),
			TEXT("Provide a search query string. Example: \"player character blueprint\""));
	}

	// Determine max_assets from params or settings
	int32 MaxAssets = 10;
	if (UOliveAISettings* Settings = UOliveAISettings::Get())
	{
		MaxAssets = Settings->RelevantContextMaxAssets;
	}
	double ParamMaxDouble = 0;
	if (Params->TryGetNumberField(TEXT("max_assets"), ParamMaxDouble) && ParamMaxDouble > 0)
	{
		MaxAssets = FMath::Clamp(static_cast<int32>(ParamMaxDouble), 1, 50);
	}

	// Optional kinds filter
	TSet<FString> KindsFilter;
	const TArray<TSharedPtr<FJsonValue>>* KindsArray;
	if (Params->TryGetArrayField(TEXT("kinds"), KindsArray))
	{
		for (const auto& Value : *KindsArray)
		{
			KindsFilter.Add(Value->AsString().ToLower());
		}
	}

	FOliveProjectIndex& Index = FOliveProjectIndex::Get();
	if (!Index.IsReady())
	{
		return FOliveToolResult::Error(TEXT("INDEX_NOT_READY"), TEXT("Project index is still building"));
	}

	// Search with a generous limit so we can filter by kind afterwards
	TArray<FOliveAssetInfo> AllResults = Index.SearchAssets(Query, MaxAssets * 5);

	// Filter by kinds if provided
	TArray<FOliveAssetInfo> Filtered;
	for (const FOliveAssetInfo& Info : AllResults)
	{
		if (KindsFilter.Num() > 0)
		{
			bool bMatch = false;
			FString ClassStr = Info.AssetClass.ToString().ToLower();

			for (const FString& Kind : KindsFilter)
			{
				if (ClassStr.Contains(Kind.ToLower()))
				{
					bMatch = true;
					break;
				}
				// Also check type flags
				if (Kind == TEXT("blueprint") && Info.bIsBlueprint) { bMatch = true; break; }
				if (Kind == TEXT("behaviortree") && Info.bIsBehaviorTree) { bMatch = true; break; }
				if (Kind == TEXT("blackboard") && Info.bIsBlackboard) { bMatch = true; break; }
				if (Kind == TEXT("pcg") && Info.bIsPCG) { bMatch = true; break; }
				if (Kind == TEXT("material") && Info.bIsMaterial) { bMatch = true; break; }
				if (Kind == TEXT("widget") && Info.bIsWidget) { bMatch = true; break; }
				if (Kind == TEXT("cpp") && Info.bIsCppClass) { bMatch = true; break; }
			}

			if (!bMatch)
			{
				continue;
			}
		}

		Filtered.Add(Info);
		if (Filtered.Num() >= MaxAssets)
		{
			break;
		}
	}

	// Build result
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("query"), Query);
	Data->SetNumberField(TEXT("result_count"), Filtered.Num());

	TArray<TSharedPtr<FJsonValue>> AssetsArray;
	TArray<TSharedPtr<FJsonValue>> PathsArray;
	for (const FOliveAssetInfo& Info : Filtered)
	{
		AssetsArray.Add(MakeShared<FJsonValueObject>(Info.ToJson()));
		PathsArray.Add(MakeShared<FJsonValueString>(Info.Path));
	}
	Data->SetArrayField(TEXT("assets"), AssetsArray);
	Data->SetArrayField(TEXT("suggested_bulk_read_paths"), PathsArray);

	return FOliveToolResult::Success(Data);
}

// =============================================================================
// Recipe System
// =============================================================================

void FOliveCrossSystemToolHandlers::LoadRecipeLibrary()
{
	const FString RecipesDir = FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("UE_Olive_AI_Studio/Content/SystemPrompts/Knowledge/recipes"));

	if (!IFileManager::Get().DirectoryExists(*RecipesDir))
	{
		UE_LOG(LogOliveCrossSystemTools, Warning, TEXT("Recipe directory not found: %s"), *RecipesDir);
		return;
	}

	// Load manifest
	const FString ManifestPath = FPaths::Combine(RecipesDir, TEXT("_manifest.json"));
	FString ManifestContent;
	if (!FFileHelper::LoadFileToString(ManifestContent, *ManifestPath))
	{
		UE_LOG(LogOliveCrossSystemTools, Warning, TEXT("Recipe manifest not found: %s"), *ManifestPath);
		return;
	}

	TSharedPtr<FJsonObject> ManifestJson;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ManifestContent);
	if (!FJsonSerializer::Deserialize(Reader, ManifestJson) || !ManifestJson.IsValid())
	{
		UE_LOG(LogOliveCrossSystemTools, Error, TEXT("Failed to parse recipe manifest"));
		return;
	}

	// NIT 3: Check format_version for forward compatibility
	FString FormatVersion;
	ManifestJson->TryGetStringField(TEXT("format_version"), FormatVersion);
	if (FormatVersion.IsEmpty())
	{
		UE_LOG(LogOliveCrossSystemTools, Warning,
			TEXT("Recipe manifest missing format_version — assuming 1.0"));
		FormatVersion = TEXT("1.0");
	}
	else if (!FormatVersion.StartsWith(TEXT("1.")) && !FormatVersion.StartsWith(TEXT("2.")))
	{
		UE_LOG(LogOliveCrossSystemTools, Error,
			TEXT("Recipe manifest format_version '%s' is not supported (expected 1.x or 2.x). Skipping recipe loading."),
			*FormatVersion);
		return;
	}

	const TSharedPtr<FJsonObject>* CategoriesObj;
	if (!ManifestJson->TryGetObjectField(TEXT("categories"), CategoriesObj))
	{
		UE_LOG(LogOliveCrossSystemTools, Warning, TEXT("Recipe manifest has no 'categories' field"));
		return;
	}

	for (const auto& CategoryPair : (*CategoriesObj)->Values)
	{
		const FString& CategoryName = CategoryPair.Key;
		RecipeCategories.Add(CategoryName);

		const TSharedPtr<FJsonObject>* CategoryObj;
		if (!CategoryPair.Value->TryGetObject(CategoryObj))
		{
			continue;
		}

		const TSharedPtr<FJsonObject>* RecipesObj;
		if (!(*CategoryObj)->TryGetObjectField(TEXT("recipes"), RecipesObj))
		{
			continue;
		}

		for (const auto& RecipePair : (*RecipesObj)->Values)
		{
			const FString& RecipeName = RecipePair.Key;
			const FString Key = FString::Printf(TEXT("%s/%s"), *CategoryName, *RecipeName);

			const TSharedPtr<FJsonObject>* RecipeMetaObj;
			if (RecipePair.Value->TryGetObject(RecipeMetaObj))
			{
				FString Description;
				(*RecipeMetaObj)->TryGetStringField(TEXT("description"), Description);
				RecipeDescriptions.Add(Key, Description);
			}

			const FString FilePath = FPaths::Combine(RecipesDir, CategoryName, RecipeName + TEXT(".txt"));
			FString Content;
			if (FFileHelper::LoadFileToString(Content, *FilePath))
			{
				// Parse TAGS header if present (format: "TAGS: keyword1 keyword2\n---\ncontent")
				TArray<FString> FileTags;
				FString ActualContent = Content;

				// Handle both \n and \r\n line endings for the --- separator
				int32 SepIndex = Content.Find(TEXT("---\n"));
				if (SepIndex == INDEX_NONE)
				{
					SepIndex = Content.Find(TEXT("---\r\n"));
				}

				if (SepIndex != INDEX_NONE)
				{
					const FString Header = Content.Left(SepIndex).TrimStartAndEnd();
					if (Header.StartsWith(TEXT("TAGS:"), ESearchCase::IgnoreCase))
					{
						const FString TagLine = Header.Mid(5).TrimStartAndEnd();
						TagLine.ParseIntoArray(FileTags, TEXT(" "), true);
						for (FString& Tag : FileTags)
						{
							Tag = Tag.ToLower();
						}
						// Strip the TAGS header + separator, keep only body content
						int32 ContentStart = SepIndex + 3;
						if (ContentStart < Content.Len() && Content[ContentStart] == TEXT('\r'))
						{
							ContentStart++;
						}
						if (ContentStart < Content.Len() && Content[ContentStart] == TEXT('\n'))
						{
							ContentStart++;
						}
						ActualContent = Content.Mid(ContentStart);
					}
				}

				RecipeLibrary.Add(Key, ActualContent);
				RecipeTags.Add(Key, FileTags);
				UE_LOG(LogOliveCrossSystemTools, Log, TEXT("Loaded recipe: %s (%d chars, %d tags)"),
					*Key, ActualContent.Len(), FileTags.Num());
			}
			else
			{
				UE_LOG(LogOliveCrossSystemTools, Warning, TEXT("Recipe file not found: %s"), *FilePath);
			}
		}
	}

	UE_LOG(LogOliveCrossSystemTools, Log, TEXT("Recipe library loaded: %d recipes in %d categories"),
		RecipeLibrary.Num(), RecipeCategories.Num());

	// Validate tool references in recipe content against the registry.
	// Catches stale tool names after renames/removals.
	const FOliveToolRegistry& Registry = FOliveToolRegistry::Get();
	// Match only real MCP tool namespaces to avoid false positives from
	// recipe pin refs (e.g. @get_hp.auto) and prose (e.g. "e.g.").
	const FRegexPattern ToolRefPattern(
		TEXT("\\b((?:blueprint|project|behaviortree|blackboard|pcg|cpp|olive)\\.[a-z_]+)\\b"));
	for (const auto& Pair : RecipeLibrary)
	{
		FRegexMatcher Matcher(ToolRefPattern, Pair.Value);
		while (Matcher.FindNext())
		{
			const FString ToolRef = Matcher.GetCaptureGroup(1);
			if (!Registry.HasTool(ToolRef))
			{
				UE_LOG(LogOliveCrossSystemTools, Warning,
					TEXT("Recipe '%s' references unregistered tool '%s'"),
					*Pair.Key, *ToolRef);
			}
		}
	}
}

void FOliveCrossSystemToolHandlers::RegisterRecipeTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	{
		FOliveToolDefinition Def;
		Def.Name = TEXT("olive.get_recipe");
		Def.Description = TEXT("Search for patterns, examples, and gotchas for Blueprint workflows. "
			"Query with keywords related to your task or error "
			"(e.g. 'spawn actor transform', 'variable type object', 'function graph entry'). "
			"Returns the most relevant reference entry.");
		Def.InputSchema = OliveCrossSystemSchemas::RecipeGetRecipe();
		Def.Tags = {TEXT("crosssystem"), TEXT("read")};
		// Category is intentionally "crosssystem" — NOT "olive". Focus profiles filter
		// by category, and "crosssystem" ensures visibility in Blueprint/Auto profiles
		// without adding a new category to the profile filter config.
		Def.Category = TEXT("crosssystem");
		Def.WhenToUse = TEXT("Call ONCE at task start to get patterns and gotchas for your workflow. Do NOT call repeatedly mid-build — recipe content is static and does not change between calls.");
		Registry.RegisterTool(Def, FOliveToolHandler::CreateRaw(this, &FOliveCrossSystemToolHandlers::HandleGetRecipe));
	}
	RegisteredToolNames.Add(TEXT("olive.get_recipe"));

	UE_LOG(LogOliveCrossSystemTools, Log, TEXT("Registered recipe tool with %d recipes available"), RecipeLibrary.Num());
}

FOliveToolResult FOliveCrossSystemToolHandlers::HandleGetRecipe(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("RECIPE_INVALID_PARAMS"),
			TEXT("Parameters required"),
			TEXT("Call with {\"query\":\"keywords\"} to search for reference entries."));
	}

	FString Query;
	if (!Params->TryGetStringField(TEXT("query"), Query) || Query.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("RECIPE_MISSING_QUERY"),
			TEXT("'query' parameter is required"),
			TEXT("Call with {\"query\":\"spawn actor transform\"} to search for relevant patterns and examples."));
	}

	// Lowercase the query and split into search terms
	const FString LowerQuery = Query.ToLower();
	TArray<FString> QueryTerms;
	LowerQuery.ParseIntoArray(QueryTerms, TEXT(" "), true);

	if (QueryTerms.Num() == 0)
	{
		return FOliveToolResult::Error(
			TEXT("RECIPE_EMPTY_QUERY"),
			TEXT("Query must contain at least one search term"));
	}

	// Score each recipe entry by keyword matching
	struct FScoredEntry
	{
		FString Key;
		int32 Score;
	};
	TArray<FScoredEntry> ScoredEntries;

	for (const auto& Pair : RecipeLibrary)
	{
		const FString& Key = Pair.Key;
		const FString& Content = Pair.Value;
		const TArray<FString>* Tags = RecipeTags.Find(Key);

		int32 Score = 0;
		const FString LowerContent = Content.ToLower();

		for (const FString& Term : QueryTerms)
		{
			// Tag match: +2 points (exact match in curated tags)
			bool bTagMatch = false;
			if (Tags)
			{
				for (const FString& Tag : *Tags)
				{
					if (Tag == Term)
					{
						bTagMatch = true;
						break;
					}
				}
			}

			if (bTagMatch)
			{
				Score += 2;
			}
			else if (LowerContent.Contains(Term))
			{
				// Content substring match: +1 point (fallback)
				Score += 1;
			}
		}

		if (Score > 0)
		{
			ScoredEntries.Add({ Key, Score });
		}
	}

	// Sort by score descending
	ScoredEntries.Sort([](const FScoredEntry& A, const FScoredEntry& B)
	{
		return A.Score > B.Score;
	});

	// Build result
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("query"), Query);

	// Return top 3 matches
	static constexpr int32 MaxResults = 3;
	TArray<TSharedPtr<FJsonValue>> MatchesArray;

	const int32 ResultCount = FMath::Min(ScoredEntries.Num(), MaxResults);
	for (int32 i = 0; i < ResultCount; ++i)
	{
		const FScoredEntry& Entry = ScoredEntries[i];
		const FString* Content = RecipeLibrary.Find(Entry.Key);
		const TArray<FString>* Tags = RecipeTags.Find(Entry.Key);

		TSharedPtr<FJsonObject> MatchObj = MakeShared<FJsonObject>();
		MatchObj->SetStringField(TEXT("key"), Entry.Key);
		MatchObj->SetNumberField(TEXT("score"), Entry.Score);

		// Include tags array
		TArray<TSharedPtr<FJsonValue>> TagsJsonArray;
		if (Tags)
		{
			for (const FString& Tag : *Tags)
			{
				TagsJsonArray.Add(MakeShared<FJsonValueString>(Tag));
			}
		}
		MatchObj->SetArrayField(TEXT("tags"), TagsJsonArray);

		if (Content)
		{
			MatchObj->SetStringField(TEXT("content"), *Content);
		}

		MatchesArray.Add(MakeShared<FJsonValueObject>(MatchObj));
	}

	Data->SetArrayField(TEXT("matches"), MatchesArray);

	// If no matches, provide a summary of all available entries so the AI can refine its query
	if (ScoredEntries.Num() == 0)
	{
		TArray<TSharedPtr<FJsonValue>> AvailableArray;
		for (const auto& Pair : RecipeLibrary)
		{
			TSharedPtr<FJsonObject> EntryObj = MakeShared<FJsonObject>();
			EntryObj->SetStringField(TEXT("key"), Pair.Key);

			const TArray<FString>* Tags = RecipeTags.Find(Pair.Key);
			TArray<TSharedPtr<FJsonValue>> TagsJsonArray;
			if (Tags)
			{
				for (const FString& Tag : *Tags)
				{
					TagsJsonArray.Add(MakeShared<FJsonValueString>(Tag));
				}
			}
			EntryObj->SetArrayField(TEXT("tags"), TagsJsonArray);

			AvailableArray.Add(MakeShared<FJsonValueObject>(EntryObj));
		}
		Data->SetArrayField(TEXT("available_entries"), AvailableArray);
	}

	return FOliveToolResult::Success(Data);
}

