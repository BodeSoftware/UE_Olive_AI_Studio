// Copyright Bode Software. All Rights Reserved.

#include "OliveCrossSystemToolHandlers.h"
#include "OliveCrossSystemSchemas.h"
#include "OliveSnapshotManager.h"
#include "OliveMultiAssetOperations.h"
#include "Index/OliveProjectIndex.h"
#include "Settings/OliveAISettings.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SQLiteDatabase.h"

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

	RegisterReadTools();
	RegisterSnapshotTools();
	RegisterIndexTools();

	// Community blueprint search
	RegisterCommunityTools();

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

	CloseCommunityDatabase();

	RegisteredToolNames.Empty();
	UE_LOG(LogOliveCrossSystemTools, Log, TEXT("Cross-System MCP tools unregistered"));
}

void FOliveCrossSystemToolHandlers::RegisterReadTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	// project.read — consolidated read dispatcher. Accepts an `include` array
	// that selects which read-family sub-responses to merge into the output.
	// Legacy tools (project.get_asset_info, project.get_class_hierarchy,
	// project.get_config, project.get_dependencies, project.get_info,
	// project.get_referencers, project.get_relevant_context, project.bulk_read)
	// are silent aliases registered in OliveToolRegistry::GetToolAliases().
	{
		TSharedPtr<FJsonObject> Schema = MakeShareable(new FJsonObject());
		Schema->SetStringField(TEXT("type"), TEXT("object"));

		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject());

		TSharedPtr<FJsonObject> PathProp = MakeShareable(new FJsonObject());
		PathProp->SetStringField(TEXT("type"), TEXT("string"));
		PathProp->SetStringField(TEXT("description"),
			TEXT("Asset path (required for asset_info/dependencies/referencers; optional for class_hierarchy/config/info). Use /Game/... form."));
		Props->SetObjectField(TEXT("path"), PathProp);

		TSharedPtr<FJsonObject> IncludeItemSchema = MakeShareable(new FJsonObject());
		IncludeItemSchema->SetStringField(TEXT("type"), TEXT("string"));
		TArray<TSharedPtr<FJsonValue>> EnumVals;
		for (const TCHAR* V : { TEXT("asset_info"), TEXT("class_hierarchy"), TEXT("config"),
			TEXT("dependencies"), TEXT("info"), TEXT("referencers"), TEXT("relevant_context"), TEXT("bulk") })
		{
			EnumVals.Add(MakeShareable(new FJsonValueString(V)));
		}
		IncludeItemSchema->SetArrayField(TEXT("enum"), EnumVals);

		TSharedPtr<FJsonObject> IncludeProp = MakeShareable(new FJsonObject());
		IncludeProp->SetStringField(TEXT("type"), TEXT("array"));
		IncludeProp->SetStringField(TEXT("description"),
			TEXT("Which read-family sections to include. Each key maps to a named sub-object in the result. Default: [\"asset_info\"]."));
		IncludeProp->SetObjectField(TEXT("items"), IncludeItemSchema);
		Props->SetObjectField(TEXT("include"), IncludeProp);

		// Relevant-context pass-through params
		TSharedPtr<FJsonObject> QueryProp = MakeShareable(new FJsonObject());
		QueryProp->SetStringField(TEXT("type"), TEXT("string"));
		QueryProp->SetStringField(TEXT("description"), TEXT("Query string for include=[\"relevant_context\"]"));
		Props->SetObjectField(TEXT("query"), QueryProp);

		TSharedPtr<FJsonObject> MaxAssetsProp = MakeShareable(new FJsonObject());
		MaxAssetsProp->SetStringField(TEXT("type"), TEXT("integer"));
		MaxAssetsProp->SetStringField(TEXT("description"), TEXT("max_assets for include=[\"relevant_context\"]"));
		Props->SetObjectField(TEXT("max_assets"), MaxAssetsProp);

		TSharedPtr<FJsonObject> KindsProp = MakeShareable(new FJsonObject());
		KindsProp->SetStringField(TEXT("type"), TEXT("array"));
		TSharedPtr<FJsonObject> KindsItem = MakeShareable(new FJsonObject());
		KindsItem->SetStringField(TEXT("type"), TEXT("string"));
		KindsProp->SetObjectField(TEXT("items"), KindsItem);
		KindsProp->SetStringField(TEXT("description"), TEXT("kinds filter for include=[\"relevant_context\"]"));
		Props->SetObjectField(TEXT("kinds"), KindsProp);

		// bulk_read pass-through params
		TSharedPtr<FJsonObject> PathsProp = MakeShareable(new FJsonObject());
		PathsProp->SetStringField(TEXT("type"), TEXT("array"));
		TSharedPtr<FJsonObject> PathsItem = MakeShareable(new FJsonObject());
		PathsItem->SetStringField(TEXT("type"), TEXT("string"));
		PathsProp->SetObjectField(TEXT("items"), PathsItem);
		PathsProp->SetStringField(TEXT("description"), TEXT("Paths array for include=[\"bulk\"] (up to 20)"));
		Props->SetObjectField(TEXT("paths"), PathsProp);

		TSharedPtr<FJsonObject> ReadModeProp = MakeShareable(new FJsonObject());
		ReadModeProp->SetStringField(TEXT("type"), TEXT("string"));
		ReadModeProp->SetStringField(TEXT("description"), TEXT("read_mode for include=[\"bulk\"]: summary|full"));
		Props->SetObjectField(TEXT("read_mode"), ReadModeProp);

		// class_hierarchy pass-through
		TSharedPtr<FJsonObject> RootClassProp = MakeShareable(new FJsonObject());
		RootClassProp->SetStringField(TEXT("type"), TEXT("string"));
		RootClassProp->SetStringField(TEXT("description"), TEXT("root_class for include=[\"class_hierarchy\"] (defaults to Actor)"));
		Props->SetObjectField(TEXT("root_class"), RootClassProp);

		Schema->SetObjectField(TEXT("properties"), Props);

		Registry.RegisterTool(
			TEXT("project.read"),
			TEXT("Read project data. Use `include` to pick sections: asset_info, class_hierarchy, config, dependencies, info, referencers, relevant_context, bulk. Multiple keys are merged into a single response."),
			Schema,
			FOliveToolHandler::CreateRaw(this, &FOliveCrossSystemToolHandlers::HandleProjectRead),
			{TEXT("crosssystem"), TEXT("read")},
			TEXT("crosssystem")
		);
		RegisteredToolNames.Add(TEXT("project.read"));
	}

	// project.refactor_rename — unchanged (survives as-is).
	Registry.RegisterTool(
		TEXT("project.refactor_rename"),
		TEXT("Rename an asset with dependency-aware reference updates"),
		OliveCrossSystemSchemas::ProjectRefactorRename(),
		FOliveToolHandler::CreateRaw(this, &FOliveCrossSystemToolHandlers::HandleRefactorRename),
		{TEXT("crosssystem"), TEXT("write")},
		TEXT("crosssystem")
	);
	RegisteredToolNames.Add(TEXT("project.refactor_rename"));

	// Deleted (no alias) in P5 consolidation:
	//   - project.bulk_read         -> alias to project.read (include=["bulk"])
	//   - project.implement_interface -> hard delete (duplicate of blueprint.add entity=interface)
	//   - project.create_ai_character -> hard delete (too specialized)
	//   - project.move_to_cpp         -> hard delete (low usage)
}

void FOliveCrossSystemToolHandlers::RegisterSnapshotTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	// project.snapshot — consolidated: default action "create" takes a snapshot,
	// action="list" lists existing snapshots (subsumes project.list_snapshots).
	// The "list" action is reached via the project.list_snapshots silent alias
	// in OliveToolRegistry::GetToolAliases() or by passing action explicitly.
	{
		TSharedPtr<FJsonObject> Schema = MakeShareable(new FJsonObject());
		Schema->SetStringField(TEXT("type"), TEXT("object"));

		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject());

		TSharedPtr<FJsonObject> ActionProp = MakeShareable(new FJsonObject());
		ActionProp->SetStringField(TEXT("type"), TEXT("string"));
		TArray<TSharedPtr<FJsonValue>> ActionEnum;
		ActionEnum.Add(MakeShareable(new FJsonValueString(TEXT("create"))));
		ActionEnum.Add(MakeShareable(new FJsonValueString(TEXT("list"))));
		ActionProp->SetArrayField(TEXT("enum"), ActionEnum);
		ActionProp->SetStringField(TEXT("description"),
			TEXT("'create' (default) saves IR state of assets; 'list' returns available snapshots with optional asset_filter"));
		Props->SetObjectField(TEXT("action"), ActionProp);

		TSharedPtr<FJsonObject> NameProp = MakeShareable(new FJsonObject());
		NameProp->SetStringField(TEXT("type"), TEXT("string"));
		NameProp->SetStringField(TEXT("description"), TEXT("Name for this snapshot (required for action='create')"));
		Props->SetObjectField(TEXT("name"), NameProp);

		TSharedPtr<FJsonObject> PathsProp = MakeShareable(new FJsonObject());
		PathsProp->SetStringField(TEXT("type"), TEXT("array"));
		TSharedPtr<FJsonObject> PathsItem = MakeShareable(new FJsonObject());
		PathsItem->SetStringField(TEXT("type"), TEXT("string"));
		PathsProp->SetObjectField(TEXT("items"), PathsItem);
		PathsProp->SetStringField(TEXT("description"), TEXT("Asset paths to snapshot (required for action='create')"));
		Props->SetObjectField(TEXT("paths"), PathsProp);

		TSharedPtr<FJsonObject> DescProp = MakeShareable(new FJsonObject());
		DescProp->SetStringField(TEXT("type"), TEXT("string"));
		DescProp->SetStringField(TEXT("description"), TEXT("Optional description of why this snapshot was taken (action='create')"));
		Props->SetObjectField(TEXT("description"), DescProp);

		TSharedPtr<FJsonObject> AssetFilterProp = MakeShareable(new FJsonObject());
		AssetFilterProp->SetStringField(TEXT("type"), TEXT("string"));
		AssetFilterProp->SetStringField(TEXT("description"), TEXT("Filter snapshots containing this asset path (action='list' only)"));
		Props->SetObjectField(TEXT("asset_filter"), AssetFilterProp);

		Schema->SetObjectField(TEXT("properties"), Props);

		Registry.RegisterTool(
			TEXT("project.snapshot"),
			TEXT("Snapshot operations. action='create' (default) saves IR state of assets for later comparison/rollback. action='list' returns existing snapshots filtered by asset_filter."),
			Schema,
			FOliveToolHandler::CreateRaw(this, &FOliveCrossSystemToolHandlers::HandleSnapshot),
			{TEXT("crosssystem"), TEXT("read")},
			TEXT("crosssystem")
		);
		RegisteredToolNames.Add(TEXT("project.snapshot"));
	}

	Registry.RegisterTool(
		TEXT("project.rollback"),
		TEXT("Restore assets to a previous snapshot state"),
		OliveCrossSystemSchemas::ProjectRollback(),
		FOliveToolHandler::CreateRaw(this, &FOliveCrossSystemToolHandlers::HandleRollback),
		{TEXT("crosssystem"), TEXT("write")},
		TEXT("crosssystem")
	);
	RegisteredToolNames.Add(TEXT("project.rollback"));

	Registry.RegisterTool(
		TEXT("project.diff"),
		TEXT("Compare current asset state against a snapshot"),
		OliveCrossSystemSchemas::ProjectDiff(),
		FOliveToolHandler::CreateRaw(this, &FOliveCrossSystemToolHandlers::HandleDiff),
		{TEXT("crosssystem"), TEXT("read")},
		TEXT("crosssystem")
	);
	RegisteredToolNames.Add(TEXT("project.diff"));
}

// =============================================================================
// Read / Refactor Handlers (post-P5)
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
// Deleted in P5 (no alias):
//   - project.batch_write (callers can loop)
//   - project.create_ai_character (too specialized)
//   - project.implement_interface (duplicate of blueprint.add entity=interface)
//   - project.move_to_cpp (low usage)
// Their handler method bodies have been removed. If needed as direct API,
// call FOliveMultiAssetOperations::Get() helpers instead.
// =============================================================================


// =============================================================================
// Snapshot Handlers
// =============================================================================

FOliveToolResult FOliveCrossSystemToolHandlers::HandleSnapshot(const TSharedPtr<FJsonObject>& Params)
{
	// P5: action-dispatch. Default "create" mirrors the legacy create-a-snapshot
	// behavior. action="list" folds in project.list_snapshots.
	FString Action = TEXT("create");
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("action"), Action);
	}
	Action = Action.ToLower();

	if (Action == TEXT("list"))
	{
		return HandleListSnapshots(Params);
	}

	if (Action != TEXT("create"))
	{
		return FOliveToolResult::Error(TEXT("INVALID_ACTION"),
			FString::Printf(TEXT("Unknown action '%s' for project.snapshot"), *Action),
			TEXT("Use action='create' (default) to snapshot assets, or action='list' to list snapshots."));
	}

	FString Name;
	if (!Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'name' is missing"),
			TEXT("Provide a name for the snapshot. Example: \"before_refactor\""));
	}

	TArray<FString> Paths;
	const TArray<TSharedPtr<FJsonValue>>* PathsArray;
	if (Params->TryGetArrayField(TEXT("paths"), PathsArray))
	{
		for (const auto& Value : *PathsArray)
		{
			Paths.Add(Value->AsString());
		}
	}

	FString Description;
	Params->TryGetStringField(TEXT("description"), Description);

	return FOliveSnapshotManager::Get().CreateSnapshot(Name, Paths, Description);
}

FOliveToolResult FOliveCrossSystemToolHandlers::HandleListSnapshots(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetFilter;
	Params->TryGetStringField(TEXT("asset_filter"), AssetFilter);
	return FOliveSnapshotManager::Get().ListSnapshots(AssetFilter);
}

FOliveToolResult FOliveCrossSystemToolHandlers::HandleRollback(const TSharedPtr<FJsonObject>& Params)
{
	FString SnapshotId;
	if (!Params->TryGetStringField(TEXT("snapshot_id"), SnapshotId) || SnapshotId.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'snapshot_id' is missing"),
			TEXT("Provide the snapshot ID. Use project.list_snapshots to see available snapshots."));
	}

	TArray<FString> Paths;
	const TArray<TSharedPtr<FJsonValue>>* PathsArray;
	if (Params->TryGetArrayField(TEXT("paths"), PathsArray))
	{
		for (const auto& Value : *PathsArray)
		{
			Paths.Add(Value->AsString());
		}
	}

	bool bPreviewOnly = true;
	Params->TryGetBoolField(TEXT("preview_only"), bPreviewOnly);

	FString ConfirmationToken;
	Params->TryGetStringField(TEXT("confirmation_token"), ConfirmationToken);

	return FOliveSnapshotManager::Get().RollbackSnapshot(SnapshotId, Paths, bPreviewOnly, ConfirmationToken);
}

FOliveToolResult FOliveCrossSystemToolHandlers::HandleDiff(const TSharedPtr<FJsonObject>& Params)
{
	FString SnapshotId;
	if (!Params->TryGetStringField(TEXT("snapshot_id"), SnapshotId) || SnapshotId.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'snapshot_id' is missing"),
			TEXT("Provide the snapshot ID to compare against. Use project.list_snapshots to see available snapshots."));
	}

	TArray<FString> Paths;
	const TArray<TSharedPtr<FJsonValue>>* PathsArray;
	if (Params->TryGetArrayField(TEXT("paths"), PathsArray))
	{
		for (const auto& Value : *PathsArray)
		{
			Paths.Add(Value->AsString());
		}
	}

	return FOliveSnapshotManager::Get().DiffSnapshot(SnapshotId, Paths);
}

// =============================================================================
// Index / Context Handlers
// =============================================================================

void FOliveCrossSystemToolHandlers::RegisterIndexTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	// project.index — action-dispatch: "status" (default) checks staleness/readiness,
	// "build" exports the project map JSON for external consumption.
	// Legacy tools project.index_build / project.index_status are silent aliases
	// registered in OliveToolRegistry::GetToolAliases().
	{
		TSharedPtr<FJsonObject> Schema = MakeShareable(new FJsonObject());
		Schema->SetStringField(TEXT("type"), TEXT("object"));

		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject());

		TSharedPtr<FJsonObject> ActionProp = MakeShareable(new FJsonObject());
		ActionProp->SetStringField(TEXT("type"), TEXT("string"));
		TArray<TSharedPtr<FJsonValue>> ActionEnum;
		ActionEnum.Add(MakeShareable(new FJsonValueString(TEXT("status"))));
		ActionEnum.Add(MakeShareable(new FJsonValueString(TEXT("build"))));
		ActionProp->SetArrayField(TEXT("enum"), ActionEnum);
		ActionProp->SetStringField(TEXT("description"),
			TEXT("'status' (default) checks whether the project index is stale. 'build' exports the index JSON."));
		Props->SetObjectField(TEXT("action"), ActionProp);

		TSharedPtr<FJsonObject> ForceProp = MakeShareable(new FJsonObject());
		ForceProp->SetStringField(TEXT("type"), TEXT("boolean"));
		ForceProp->SetStringField(TEXT("description"), TEXT("Force re-export even if the index is not stale (action='build' only)"));
		Props->SetObjectField(TEXT("force"), ForceProp);

		Schema->SetObjectField(TEXT("properties"), Props);

		Registry.RegisterTool(
			TEXT("project.index"),
			TEXT("Project index management. action='status' (default) reports staleness/readiness; action='build' exports the project map JSON."),
			Schema,
			FOliveToolHandler::CreateRaw(this, &FOliveCrossSystemToolHandlers::HandleProjectIndex),
			{TEXT("crosssystem"), TEXT("read")},
			TEXT("crosssystem")
		);
		RegisteredToolNames.Add(TEXT("project.index"));
	}

	// project.search — unchanged (relevant-context search; many prompts reference it).
	Registry.RegisterTool(
		TEXT("project.search"),
		TEXT("Search the project index and return the most relevant assets for a query."),
		OliveCrossSystemSchemas::ProjectGetRelevantContext(),
		FOliveToolHandler::CreateRaw(this, &FOliveCrossSystemToolHandlers::HandleGetRelevantContext),
		{TEXT("crosssystem"), TEXT("read")},
		TEXT("crosssystem")
	);
	RegisteredToolNames.Add(TEXT("project.search"));
}

// -----------------------------------------------------------------------------
// P5 Dispatchers
// -----------------------------------------------------------------------------

FOliveToolResult FOliveCrossSystemToolHandlers::HandleProjectRead(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Parameters required"),
			TEXT("Provide at least 'include': [\"asset_info\"] and a 'path'."));
	}

	// Parse include list. Default: ["asset_info"] (backwards-compat, requires 'path').
	TArray<FString> Include;
	const TArray<TSharedPtr<FJsonValue>>* IncludeArray = nullptr;
	if (Params->TryGetArrayField(TEXT("include"), IncludeArray) && IncludeArray)
	{
		for (const TSharedPtr<FJsonValue>& V : *IncludeArray)
		{
			if (V.IsValid())
			{
				const FString Key = V->AsString().ToLower();
				if (!Key.IsEmpty())
				{
					Include.AddUnique(Key);
				}
			}
		}
	}
	if (Include.Num() == 0)
	{
		Include.Add(TEXT("asset_info"));
	}

	FString Path;
	Params->TryGetStringField(TEXT("path"), Path);

	FOliveProjectIndex& Index = FOliveProjectIndex::Get();

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Missing;
	TArray<TSharedPtr<FJsonValue>> Errors;

	// Helper: emit a per-include error entry (non-fatal; we keep going).
	auto PushErr = [&Errors](const FString& Key, const FString& Code, const FString& Message)
	{
		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("include"), Key);
		E->SetStringField(TEXT("code"), Code);
		E->SetStringField(TEXT("message"), Message);
		Errors.Add(MakeShared<FJsonValueObject>(E));
	};

	for (const FString& Key : Include)
	{
		if (Key == TEXT("asset_info"))
		{
			if (Path.IsEmpty())
			{
				PushErr(Key, TEXT("MISSING_PATH"), TEXT("'path' is required for include=asset_info"));
				continue;
			}
			TOptional<FOliveAssetInfo> AssetInfo = Index.GetAssetByPath(Path);
			if (AssetInfo.IsSet())
			{
				Out->SetObjectField(Key, AssetInfo->ToJson());
			}
			else
			{
				PushErr(Key, TEXT("ASSET_NOT_FOUND"), FString::Printf(TEXT("Asset not found: %s"), *Path));
			}
		}
		else if (Key == TEXT("dependencies"))
		{
			if (Path.IsEmpty())
			{
				PushErr(Key, TEXT("MISSING_PATH"), TEXT("'path' is required for include=dependencies"));
				continue;
			}
			TArray<FString> Deps = Index.GetDependencies(Path);
			TSharedPtr<FJsonObject> Sub = MakeShared<FJsonObject>();
			Sub->SetStringField(TEXT("asset"), Path);
			Sub->SetNumberField(TEXT("count"), Deps.Num());
			TArray<TSharedPtr<FJsonValue>> DepsArr;
			for (const FString& D : Deps) { DepsArr.Add(MakeShared<FJsonValueString>(D)); }
			Sub->SetArrayField(TEXT("dependencies"), DepsArr);
			Out->SetObjectField(Key, Sub);
		}
		else if (Key == TEXT("referencers"))
		{
			if (Path.IsEmpty())
			{
				PushErr(Key, TEXT("MISSING_PATH"), TEXT("'path' is required for include=referencers"));
				continue;
			}
			TArray<FString> Refs = Index.GetReferencers(Path);
			TSharedPtr<FJsonObject> Sub = MakeShared<FJsonObject>();
			Sub->SetStringField(TEXT("asset"), Path);
			Sub->SetNumberField(TEXT("count"), Refs.Num());
			TArray<TSharedPtr<FJsonValue>> RefsArr;
			for (const FString& R : Refs) { RefsArr.Add(MakeShared<FJsonValueString>(R)); }
			Sub->SetArrayField(TEXT("referencers"), RefsArr);
			Out->SetObjectField(Key, Sub);
		}
		else if (Key == TEXT("class_hierarchy"))
		{
			FName RootClass = NAME_None;
			FString RootClassStr;
			if (Params->TryGetStringField(TEXT("root_class"), RootClassStr) && !RootClassStr.IsEmpty())
			{
				RootClass = FName(*RootClassStr);
			}
			const FString Json = Index.GetClassHierarchyJson(RootClass);
			TSharedPtr<FJsonObject> Sub;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
			if (FJsonSerializer::Deserialize(Reader, Sub) && Sub.IsValid())
			{
				Out->SetObjectField(Key, Sub);
			}
			else
			{
				TSharedPtr<FJsonObject> Empty = MakeShared<FJsonObject>();
				Empty->SetArrayField(TEXT("hierarchy"), TArray<TSharedPtr<FJsonValue>>());
				Out->SetObjectField(Key, Empty);
			}
		}
		else if (Key == TEXT("config") || Key == TEXT("info"))
		{
			// "info" is an alias of "config" here — both return project configuration.
			const FString Json = Index.GetProjectConfigJson();
			TSharedPtr<FJsonObject> Sub;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
			if (FJsonSerializer::Deserialize(Reader, Sub) && Sub.IsValid())
			{
				Out->SetObjectField(Key, Sub);
			}
			else
			{
				Out->SetObjectField(Key, MakeShared<FJsonObject>());
			}
		}
		else if (Key == TEXT("relevant_context"))
		{
			FOliveToolResult R = HandleGetRelevantContext(Params);
			if (R.bSuccess && R.Data.IsValid())
			{
				Out->SetObjectField(Key, R.Data);
			}
			else
			{
				const FString Code = (R.Messages.Num() > 0) ? R.Messages[0].Code : TEXT("RELEVANT_CONTEXT_FAILED");
				const FString Msg = (R.Messages.Num() > 0) ? R.Messages[0].Message : TEXT("Unknown error");
				PushErr(Key, Code, Msg);
			}
		}
		else if (Key == TEXT("bulk"))
		{
			FOliveToolResult R = HandleBulkRead(Params);
			if (R.bSuccess && R.Data.IsValid())
			{
				Out->SetObjectField(Key, R.Data);
			}
			else
			{
				const FString Code = (R.Messages.Num() > 0) ? R.Messages[0].Code : TEXT("BULK_READ_FAILED");
				const FString Msg = (R.Messages.Num() > 0) ? R.Messages[0].Message : TEXT("Unknown error");
				PushErr(Key, Code, Msg);
			}
		}
		else
		{
			Missing.Add(MakeShared<FJsonValueString>(Key));
		}
	}

	// Emit metadata so callers can diagnose partial responses.
	TArray<TSharedPtr<FJsonValue>> IncludeOut;
	for (const FString& K : Include) { IncludeOut.Add(MakeShared<FJsonValueString>(K)); }
	Out->SetArrayField(TEXT("include"), IncludeOut);

	if (Missing.Num() > 0)
	{
		Out->SetArrayField(TEXT("unknown_include_keys"), Missing);
	}
	if (Errors.Num() > 0)
	{
		Out->SetArrayField(TEXT("errors"), Errors);
	}

	return FOliveToolResult::Success(Out);
}

FOliveToolResult FOliveCrossSystemToolHandlers::HandleProjectIndex(const TSharedPtr<FJsonObject>& Params)
{
	FString Action = TEXT("status");
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("action"), Action);
	}
	Action = Action.ToLower();

	if (Action == TEXT("build"))
	{
		return HandleIndexBuild(Params);
	}
	if (Action == TEXT("status") || Action.IsEmpty())
	{
		return HandleIndexStatus(Params);
	}

	return FOliveToolResult::Error(TEXT("INVALID_ACTION"),
		FString::Printf(TEXT("Unknown action '%s' for project.index"), *Action),
		TEXT("Use action='status' (default) or action='build'."));
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
// Community Blueprint Search
// =============================================================================

void FOliveCrossSystemToolHandlers::OpenCommunityDatabase()
{
	if (bCommunityDbInitAttempted)
	{
		return;
	}
	bCommunityDbInitAttempted = true;

	const FString DbPath = FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("UE_Olive_AI_Studio/Content/CommunityBlueprints/community_blueprints.db"));

	if (!FPaths::FileExists(DbPath))
	{
		UE_LOG(LogOliveCrossSystemTools, Log, TEXT("Community blueprint database not found at %s (optional feature)"), *DbPath);
		return;
	}

	CommunityDb = MakeShared<FSQLiteDatabase>();

	// ReadOnly is sufficient for SELECT + FTS5 MATCH queries.
	// IMPORTANT: The .db file MUST use rollback journal mode (not WAL).
	// UE's custom SQLite VFS (SQLITE_OS_OTHER) doesn't implement shared memory,
	// which WAL mode requires. If queries fail with "unable to open database file",
	// convert the .db: sqlite3 community_blueprints.db "PRAGMA journal_mode=DELETE;"
	if (!CommunityDb->Open(*DbPath, ESQLiteDatabaseOpenMode::ReadOnly))
	{
		UE_LOG(LogOliveCrossSystemTools, Warning, TEXT("Failed to open community blueprint database: %s"), *CommunityDb->GetLastError());
		CommunityDb.Reset();
		return;
	}

	UE_LOG(LogOliveCrossSystemTools, Log, TEXT("Opened community blueprint database: %s"), *DbPath);
}

void FOliveCrossSystemToolHandlers::CloseCommunityDatabase()
{
	if (CommunityDb.IsValid())
	{
		CommunityDb->Close();
		CommunityDb.Reset();
	}
	bCommunityDbInitAttempted = false;
}

void FOliveCrossSystemToolHandlers::RegisterCommunityTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	Registry.RegisterTool(
		TEXT("olive.search_community_blueprints"),
		TEXT("Search ~150K real-world community Blueprints for gameplay patterns. "
			"Two modes: 'browse' returns compact summaries (title, type, node_count, description snippet) "
			"for scanning many results quickly. 'detail' (default) returns full graph data. "
			"Recommended workflow: browse first with max_results:15-20, then fetch full detail "
			"on 3-5 promising entries using the 'ids' parameter. "
			"Results prefer UE 5.0+ examples. Quality varies -- adapt rather than copy."),
		OliveCrossSystemSchemas::CommunitySearch(),
		FOliveToolHandler::CreateRaw(this, &FOliveCrossSystemToolHandlers::HandleSearchCommunityBlueprints),
		{TEXT("crosssystem"), TEXT("read")},
		TEXT("crosssystem")
	);
	RegisteredToolNames.Add(TEXT("olive.search_community_blueprints"));

	UE_LOG(LogOliveCrossSystemTools, Log, TEXT("Registered community blueprint search tool"));
}

FOliveToolResult FOliveCrossSystemToolHandlers::HandleSearchCommunityBlueprints(const TSharedPtr<FJsonObject>& Params)
{
	// --- Mode extraction ---
	FString Mode = TEXT("detail");
	Params->TryGetStringField(TEXT("mode"), Mode);
	Mode = Mode.ToLower();
	const bool bIsBrowseMode = (Mode == TEXT("browse"));

	// --- IDs extraction (for targeted detail fetch) ---
	TArray<FString> RequestedIds;
	const TArray<TSharedPtr<FJsonValue>>* IdsArray = nullptr;
	if (Params->TryGetArrayField(TEXT("ids"), IdsArray) && IdsArray)
	{
		for (const TSharedPtr<FJsonValue>& Val : *IdsArray)
		{
			FString Id;
			if (Val->TryGetString(Id) && !Id.IsEmpty())
			{
				RequestedIds.Add(Id);
			}
		}
	}
	const bool bHasIds = RequestedIds.Num() > 0;

	// --- Query extraction ---
	FString Query;
	Params->TryGetStringField(TEXT("query"), Query);

	// query is required unless ids are provided
	if (Query.IsEmpty() && !bHasIds)
	{
		return FOliveToolResult::Error(TEXT("MISSING_PARAM"),
			TEXT("'query' is required (unless 'ids' is provided)"),
			TEXT("Provide search terms like 'gun fire reload' or use 'ids' to fetch specific entries"));
	}

	FString TypeFilter;
	Params->TryGetStringField(TEXT("type"), TypeFilter);

	// Browse mode allows more results (compact, low token cost)
	const int32 MaxResultsCap = bIsBrowseMode ? 20 : 10;
	int32 MaxResults = bIsBrowseMode ? 10 : 5; // default differs by mode
	if (Params->HasField(TEXT("max_results")))
	{
		MaxResults = FMath::Clamp(
			static_cast<int32>(Params->GetNumberField(TEXT("max_results"))),
			1, MaxResultsCap);
	}

	int32 Offset = 0;
	if (Params->HasField(TEXT("offset")))
	{
		Offset = FMath::Max(0, static_cast<int32>(Params->GetNumberField(TEXT("offset"))));
	}

	// --- Lazy-init database ---
	OpenCommunityDatabase();
	if (!CommunityDb.IsValid())
	{
		// Database not available -- return empty result, NOT an error
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> EmptyArray;
		Result->SetArrayField(TEXT("results"), EmptyArray);
		Result->SetNumberField(TEXT("count"), 0);
		Result->SetStringField(TEXT("note"),
			TEXT("Community blueprint index not found. This is an optional feature -- place community_blueprints.db in Content/CommunityBlueprints/."));
		return FOliveToolResult::Success(Result);
	}

	// --- IDs fetch path (direct slug lookup, no FTS) ---
	if (bHasIds)
	{
		// Clamp to max 10 IDs per request
		if (RequestedIds.Num() > 10)
		{
			RequestedIds.SetNum(10);
		}

		// Build SQL with IN clause using placeholders
		FString Placeholders;
		for (int32 i = 0; i < RequestedIds.Num(); ++i)
		{
			if (i > 0) Placeholders += TEXT(",");
			Placeholders += TEXT("?");
		}

		const FString IdSql = FString::Printf(TEXT(
			"SELECT b.slug, b.title, b.type, b.ue_version, b.node_count, "
			"b.functions, b.variables, b.components, b.compact, b.url "
			"FROM blueprints b "
			"WHERE b.slug IN (%s)"), *Placeholders);

		FSQLitePreparedStatement IdStmt = CommunityDb->PrepareStatement(
			*IdSql, ESQLitePreparedStatementFlags::None);

		if (!IdStmt.IsValid())
		{
			return FOliveToolResult::Error(TEXT("DB_ERROR"),
				FString::Printf(TEXT("Failed to prepare ID lookup: %s"),
					*CommunityDb->GetLastError()),
				TEXT("Check database integrity"));
		}

		for (int32 i = 0; i < RequestedIds.Num(); ++i)
		{
			IdStmt.SetBindingValueByIndex(i + 1, RequestedIds[i]);
		}

		TArray<TSharedPtr<FJsonValue>> Results;
		IdStmt.Execute([&Results](const FSQLitePreparedStatement& Row)
			-> ESQLitePreparedStatementExecuteRowResult
		{
			// Same column extraction as the full detail path
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			FString Slug, Title, Type, UeVersion, Functions, Variables, Components, Compact, Url;
			int32 NodeCount = 0;

			Row.GetColumnValueByIndex(0, Slug);
			Row.GetColumnValueByIndex(1, Title);
			Row.GetColumnValueByIndex(2, Type);
			Row.GetColumnValueByIndex(3, UeVersion);
			Row.GetColumnValueByIndex(4, NodeCount);
			Row.GetColumnValueByIndex(5, Functions);
			Row.GetColumnValueByIndex(6, Variables);
			Row.GetColumnValueByIndex(7, Components);
			Row.GetColumnValueByIndex(8, Compact);
			Row.GetColumnValueByIndex(9, Url);

			Entry->SetStringField(TEXT("id"), Slug);
			Entry->SetStringField(TEXT("title"), Title);
			Entry->SetStringField(TEXT("type"), Type);
			Entry->SetStringField(TEXT("ue_version"), UeVersion);
			Entry->SetNumberField(TEXT("node_count"), NodeCount);
			Entry->SetStringField(TEXT("functions"), Functions);
			Entry->SetStringField(TEXT("variables"), Variables);
			if (!Components.IsEmpty())
			{
				Entry->SetStringField(TEXT("components"), Components);
			}
			Entry->SetStringField(TEXT("compact"), Compact);
			Entry->SetStringField(TEXT("url"), Url);

			Results.Add(MakeShared<FJsonValueObject>(Entry));
			return ESQLitePreparedStatementExecuteRowResult::Continue;
		});

		TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetArrayField(TEXT("results"), Results);
		Response->SetNumberField(TEXT("count"), Results.Num());
		Response->SetStringField(TEXT("mode"), TEXT("detail"));
		return FOliveToolResult::Success(Response);
	}

	// --- Build FTS5 query string ---
	// Multi-word queries use OR logic: "gun fire reload" -> "gun OR fire OR reload"
	FString FtsQuery;
	TArray<FString> Terms;
	Query.ParseIntoArrayWS(Terms);
	if (Terms.Num() > 1)
	{
		FtsQuery = FString::Join(Terms, TEXT(" OR "));
	}
	else
	{
		FtsQuery = Query;
	}

	// --- Build and execute SQL ---
	const bool bHasTypeFilter = !TypeFilter.IsEmpty();

	// --- Count total matches (before applying LIMIT/OFFSET) ---
	int64 TotalMatches = 0;
	{
		const TCHAR* CountSqlWithoutType = TEXT(
			"SELECT COUNT(*) "
			"FROM blueprints_fts fts "
			"JOIN blueprints b ON b.slug = fts.slug "
			"WHERE blueprints_fts MATCH ?");

		const TCHAR* CountSqlWithType = TEXT(
			"SELECT COUNT(*) "
			"FROM blueprints_fts fts "
			"JOIN blueprints b ON b.slug = fts.slug "
			"WHERE blueprints_fts MATCH ? AND b.type = ?");

		FSQLitePreparedStatement CountStmt = CommunityDb->PrepareStatement(
			bHasTypeFilter ? CountSqlWithType : CountSqlWithoutType,
			ESQLitePreparedStatementFlags::None);

		if (CountStmt.IsValid())
		{
			int32 CountBindIdx = 1;
			CountStmt.SetBindingValueByIndex(CountBindIdx++, FtsQuery);
			if (bHasTypeFilter)
			{
				CountStmt.SetBindingValueByIndex(CountBindIdx++, TypeFilter);
			}

			CountStmt.Execute([&TotalMatches](const FSQLitePreparedStatement& Row) -> ESQLitePreparedStatementExecuteRowResult
			{
				Row.GetColumnValueByIndex(0, TotalMatches);
				return ESQLitePreparedStatementExecuteRowResult::Stop;
			});
		}
		// If count fails, TotalMatches stays 0 -- we'll omit it from the response
	}

	// --- Browse mode (compact summaries) ---
	if (bIsBrowseMode)
	{
		const TCHAR* BrowseSqlWithoutType = TEXT(
			"SELECT b.slug, b.title, b.type, b.ue_version, b.node_count, "
			"SUBSTR(b.compact, 1, 150) "
			"FROM blueprints_fts fts "
			"JOIN blueprints b ON b.slug = fts.slug "
			"WHERE blueprints_fts MATCH ? "
			"ORDER BY "
			"CASE WHEN CAST(b.ue_version AS REAL) >= 5.0 THEN 0 ELSE 1 END, "
			"rank "
			"LIMIT ? OFFSET ?");

		const TCHAR* BrowseSqlWithType = TEXT(
			"SELECT b.slug, b.title, b.type, b.ue_version, b.node_count, "
			"SUBSTR(b.compact, 1, 150) "
			"FROM blueprints_fts fts "
			"JOIN blueprints b ON b.slug = fts.slug "
			"WHERE blueprints_fts MATCH ? AND b.type = ? "
			"ORDER BY "
			"CASE WHEN CAST(b.ue_version AS REAL) >= 5.0 THEN 0 ELSE 1 END, "
			"rank "
			"LIMIT ? OFFSET ?");

		FSQLitePreparedStatement BrowseStmt = CommunityDb->PrepareStatement(
			bHasTypeFilter ? BrowseSqlWithType : BrowseSqlWithoutType,
			ESQLitePreparedStatementFlags::None);

		if (!BrowseStmt.IsValid())
		{
			return FOliveToolResult::Error(TEXT("DB_ERROR"),
				FString::Printf(TEXT("Failed to prepare browse query: %s"),
					*CommunityDb->GetLastError()),
				TEXT("Check database integrity"));
		}

		int32 BrowseBindIdx = 1;
		BrowseStmt.SetBindingValueByIndex(BrowseBindIdx++, FtsQuery);
		if (bHasTypeFilter)
		{
			BrowseStmt.SetBindingValueByIndex(BrowseBindIdx++, TypeFilter);
		}
		BrowseStmt.SetBindingValueByIndex(BrowseBindIdx++, MaxResults);
		BrowseStmt.SetBindingValueByIndex(BrowseBindIdx++, Offset);

		TArray<TSharedPtr<FJsonValue>> Results;

		BrowseStmt.Execute([&Results](const FSQLitePreparedStatement& Row)
			-> ESQLitePreparedStatementExecuteRowResult
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			FString Slug, Title, Type, UeVersion, DescSnippet;
			int32 NodeCount = 0;

			Row.GetColumnValueByIndex(0, Slug);
			Row.GetColumnValueByIndex(1, Title);
			Row.GetColumnValueByIndex(2, Type);
			Row.GetColumnValueByIndex(3, UeVersion);
			Row.GetColumnValueByIndex(4, NodeCount);
			Row.GetColumnValueByIndex(5, DescSnippet);

			Entry->SetStringField(TEXT("id"), Slug);
			Entry->SetStringField(TEXT("title"), Title);
			Entry->SetStringField(TEXT("type"), Type);
			Entry->SetStringField(TEXT("ue_version"), UeVersion);
			Entry->SetNumberField(TEXT("node_count"), NodeCount);
			if (!DescSnippet.IsEmpty())
			{
				Entry->SetStringField(TEXT("description"), DescSnippet);
			}

			Results.Add(MakeShared<FJsonValueObject>(Entry));
			return ESQLitePreparedStatementExecuteRowResult::Continue;
		});

		TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetArrayField(TEXT("results"), Results);
		Response->SetNumberField(TEXT("count"), Results.Num());
		Response->SetStringField(TEXT("mode"), TEXT("browse"));

		if (TotalMatches > 0)
		{
			Response->SetNumberField(TEXT("total_matches"), static_cast<double>(TotalMatches));
			Response->SetBoolField(TEXT("has_more"), (Offset + Results.Num()) < TotalMatches);
		}

		if (TotalMatches > 0 && (Offset + Results.Num()) < TotalMatches)
		{
			Response->SetStringField(TEXT("note"),
				FString::Printf(TEXT("Showing %d of %lld matches (browse mode). "
					"Use 'ids' with mode:'detail' to fetch full data for specific entries. "
					"Use offset=%d for more results."),
					Results.Num(), TotalMatches, Offset + Results.Num()));
		}

		return FOliveToolResult::Success(Response);
	}

	// --- Detail mode with query (existing path) ---
	const TCHAR* SqlWithoutType = TEXT(
		"SELECT b.slug, b.title, b.type, b.ue_version, b.node_count, "
		"b.functions, b.variables, b.components, b.compact, b.url "
		"FROM blueprints_fts fts "
		"JOIN blueprints b ON b.slug = fts.slug "
		"WHERE blueprints_fts MATCH ? "
		"ORDER BY "
		"CASE WHEN CAST(b.ue_version AS REAL) >= 5.0 THEN 0 ELSE 1 END, "
		"rank "
		"LIMIT ? OFFSET ?");

	const TCHAR* SqlWithType = TEXT(
		"SELECT b.slug, b.title, b.type, b.ue_version, b.node_count, "
		"b.functions, b.variables, b.components, b.compact, b.url "
		"FROM blueprints_fts fts "
		"JOIN blueprints b ON b.slug = fts.slug "
		"WHERE blueprints_fts MATCH ? AND b.type = ? "
		"ORDER BY "
		"CASE WHEN CAST(b.ue_version AS REAL) >= 5.0 THEN 0 ELSE 1 END, "
		"rank "
		"LIMIT ? OFFSET ?");

	FSQLitePreparedStatement Statement = CommunityDb->PrepareStatement(
		bHasTypeFilter ? SqlWithType : SqlWithoutType,
		ESQLitePreparedStatementFlags::None);

	if (!Statement.IsValid())
	{
		return FOliveToolResult::Error(TEXT("DB_ERROR"),
			FString::Printf(TEXT("Failed to prepare SQL statement: %s"), *CommunityDb->GetLastError()),
			TEXT("The community blueprint database may be corrupted or in WAL journal mode. "
				"Convert with: sqlite3 community_blueprints.db \"PRAGMA journal_mode=DELETE;\""));
	}

	// Bind parameters (1-indexed in SQLite)
	int32 BindIdx = 1;
	Statement.SetBindingValueByIndex(BindIdx++, FtsQuery);
	if (bHasTypeFilter)
	{
		Statement.SetBindingValueByIndex(BindIdx++, TypeFilter);
	}
	Statement.SetBindingValueByIndex(BindIdx++, MaxResults);
	Statement.SetBindingValueByIndex(BindIdx++, Offset);

	// Execute and collect results
	TArray<TSharedPtr<FJsonValue>> Results;

	int64 RowCount = Statement.Execute([&Results](const FSQLitePreparedStatement& Row) -> ESQLitePreparedStatementExecuteRowResult
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();

		// Column indices match SELECT order:
		// 0=slug, 1=title, 2=type, 3=ue_version, 4=node_count,
		// 5=functions, 6=variables, 7=components, 8=compact, 9=url
		FString Slug, Title, Type, UeVersion, Functions, Variables, Components, Compact, Url;
		int32 NodeCount = 0;

		Row.GetColumnValueByIndex(0, Slug);
		Row.GetColumnValueByIndex(1, Title);
		Row.GetColumnValueByIndex(2, Type);
		Row.GetColumnValueByIndex(3, UeVersion);
		Row.GetColumnValueByIndex(4, NodeCount);
		Row.GetColumnValueByIndex(5, Functions);
		Row.GetColumnValueByIndex(6, Variables);
		Row.GetColumnValueByIndex(7, Components);
		Row.GetColumnValueByIndex(8, Compact);
		Row.GetColumnValueByIndex(9, Url);

		Entry->SetStringField(TEXT("id"), Slug);
		Entry->SetStringField(TEXT("title"), Title);
		Entry->SetStringField(TEXT("type"), Type);
		Entry->SetStringField(TEXT("ue_version"), UeVersion);
		Entry->SetNumberField(TEXT("node_count"), NodeCount);
		Entry->SetStringField(TEXT("functions"), Functions);
		Entry->SetStringField(TEXT("variables"), Variables);
		if (!Components.IsEmpty())
		{
			Entry->SetStringField(TEXT("components"), Components);
		}
		Entry->SetStringField(TEXT("compact"), Compact);
		Entry->SetStringField(TEXT("url"), Url);

		Results.Add(MakeShared<FJsonValueObject>(Entry));
		return ESQLitePreparedStatementExecuteRowResult::Continue;
	});

	if (RowCount == INDEX_NONE)
	{
		return FOliveToolResult::Error(TEXT("QUERY_ERROR"),
			FString::Printf(TEXT("Query failed: %s"), *CommunityDb->GetLastError()),
			TEXT("Try simpler search terms. FTS5 special characters may cause issues."));
	}

	// --- Build response ---
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetArrayField(TEXT("results"), Results);
	Response->SetNumberField(TEXT("count"), Results.Num());
	Response->SetStringField(TEXT("mode"), TEXT("detail"));

	// Total matches and pagination hint
	if (TotalMatches > 0)
	{
		Response->SetNumberField(TEXT("total_matches"), static_cast<double>(TotalMatches));
		Response->SetBoolField(TEXT("has_more"), (Offset + Results.Num()) < TotalMatches);
	}

	// Dynamic note with pagination hint when more results exist
	const bool bHasMore = TotalMatches > 0 && (Offset + Results.Num()) < TotalMatches;
	if (bHasMore)
	{
		Response->SetStringField(TEXT("note"),
			FString::Printf(TEXT("Showing %d of %lld matches. Quality varies -- browse 2-3 pages before committing to a pattern. Use offset=%d for next page."),
				Results.Num(), TotalMatches, Offset + Results.Num()));
	}
	else
	{
		Response->SetStringField(TEXT("note"),
			TEXT("Community examples from blueprintue.com. Quality varies -- use your judgment on which patterns to follow."));
	}

	return FOliveToolResult::Success(Response);
}

#undef LOCTEXT_NAMESPACE
