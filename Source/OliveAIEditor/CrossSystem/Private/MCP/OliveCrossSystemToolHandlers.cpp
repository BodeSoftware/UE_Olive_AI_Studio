// Copyright Bode Software. All Rights Reserved.

#include "OliveCrossSystemToolHandlers.h"
#include "OliveCrossSystemSchemas.h"
#include "OliveSnapshotManager.h"
#include "OliveMultiAssetOperations.h"

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
	RegisterSnapshotTools();

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

	Registry.RegisterTool(
		TEXT("project.create_ai_character"),
		TEXT("Create a complete AI character setup (Blueprint + BehaviorTree + Blackboard)"),
		OliveCrossSystemSchemas::ProjectCreateAICharacter(),
		FOliveToolHandler::CreateRaw(this, &FOliveCrossSystemToolHandlers::HandleCreateAICharacter),
		{TEXT("crosssystem"), TEXT("write")},
		TEXT("crosssystem")
	);
	RegisteredToolNames.Add(TEXT("project.create_ai_character"));

	Registry.RegisterTool(
		TEXT("project.move_to_cpp"),
		TEXT("Analyze Blueprint and scaffold C++ migration artifacts"),
		OliveCrossSystemSchemas::ProjectMoveToCpp(),
		FOliveToolHandler::CreateRaw(this, &FOliveCrossSystemToolHandlers::HandleMoveToCpp),
		{TEXT("crosssystem"), TEXT("write")},
		TEXT("crosssystem")
	);
	RegisteredToolNames.Add(TEXT("project.move_to_cpp"));
}

void FOliveCrossSystemToolHandlers::RegisterSnapshotTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	Registry.RegisterTool(
		TEXT("project.snapshot"),
		TEXT("Save IR state of assets for later comparison or rollback"),
		OliveCrossSystemSchemas::ProjectSnapshot(),
		FOliveToolHandler::CreateRaw(this, &FOliveCrossSystemToolHandlers::HandleSnapshot),
		{TEXT("crosssystem"), TEXT("read")},
		TEXT("crosssystem")
	);
	RegisteredToolNames.Add(TEXT("project.snapshot"));

	Registry.RegisterTool(
		TEXT("project.list_snapshots"),
		TEXT("List available snapshots with filtering"),
		OliveCrossSystemSchemas::ProjectListSnapshots(),
		FOliveToolHandler::CreateRaw(this, &FOliveCrossSystemToolHandlers::HandleListSnapshots),
		{TEXT("crosssystem"), TEXT("read")},
		TEXT("crosssystem")
	);
	RegisteredToolNames.Add(TEXT("project.list_snapshots"));

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
// Bulk Operation Handlers
// =============================================================================

FOliveToolResult FOliveCrossSystemToolHandlers::HandleBulkRead(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FString> Paths;
	const TArray<TSharedPtr<FJsonValue>>* PathsArray;
	if (!Params->TryGetArrayField(TEXT("paths"), PathsArray))
	{
		return FOliveToolResult::Error(TEXT("MISSING_PATHS"), TEXT("paths array is required"));
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
		return FOliveToolResult::Error(TEXT("MISSING_PATHS"), TEXT("paths array is required"));
	}
	for (const auto& Value : *PathsArray)
	{
		Paths.Add(Value->AsString());
	}

	FString Interface = Params->GetStringField(TEXT("interface"));
	return FOliveMultiAssetOperations::Get().ImplementInterface(Paths, Interface);
}

FOliveToolResult FOliveCrossSystemToolHandlers::HandleRefactorRename(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString NewName = Params->GetStringField(TEXT("new_name"));
	bool bUpdateReferences = true;
	Params->TryGetBoolField(TEXT("update_references"), bUpdateReferences);

	return FOliveMultiAssetOperations::Get().RefactorRename(AssetPath, NewName, bUpdateReferences);
}

FOliveToolResult FOliveCrossSystemToolHandlers::HandleCreateAICharacter(const TSharedPtr<FJsonObject>& Params)
{
	FString Name = Params->GetStringField(TEXT("name"));
	FString Path = Params->GetStringField(TEXT("path"));

	FString ParentClass = TEXT("ACharacter");
	Params->TryGetStringField(TEXT("parent_class"), ParentClass);

	FString BehaviorTreeRoot = TEXT("Selector");
	Params->TryGetStringField(TEXT("behavior_tree_root"), BehaviorTreeRoot);

	TArray<TPair<FString, FString>> BlackboardKeys;
	const TArray<TSharedPtr<FJsonValue>>* KeysArray;
	if (Params->TryGetArrayField(TEXT("blackboard_keys"), KeysArray))
	{
		for (const auto& Value : *KeysArray)
		{
			TSharedPtr<FJsonObject> KeyObj = Value->AsObject();
			if (KeyObj.IsValid())
			{
				FString KeyName = KeyObj->GetStringField(TEXT("name"));
				FString KeyType = KeyObj->GetStringField(TEXT("key_type"));
				BlackboardKeys.Add(TPair<FString, FString>(KeyName, KeyType));
			}
		}
	}

	return FOliveMultiAssetOperations::Get().CreateAICharacter(Name, Path, ParentClass, BlackboardKeys, BehaviorTreeRoot);
}

FOliveToolResult FOliveCrossSystemToolHandlers::HandleMoveToCpp(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString ModuleName = Params->GetStringField(TEXT("module_name"));
	FString TargetClassName = Params->GetStringField(TEXT("target_class_name"));

	FString ParentClass;
	Params->TryGetStringField(TEXT("parent_class"), ParentClass);

	bool bCreateWrapperBlueprint = true;
	Params->TryGetBoolField(TEXT("create_wrapper_blueprint"), bCreateWrapperBlueprint);

	bool bCompileAfter = false;
	Params->TryGetBoolField(TEXT("compile_after"), bCompileAfter);

	return FOliveMultiAssetOperations::Get().MoveToCpp(
		AssetPath,
		ModuleName,
		TargetClassName,
		ParentClass,
		bCreateWrapperBlueprint,
		bCompileAfter);
}

// =============================================================================
// Snapshot Handlers
// =============================================================================

FOliveToolResult FOliveCrossSystemToolHandlers::HandleSnapshot(const TSharedPtr<FJsonObject>& Params)
{
	FString Name = Params->GetStringField(TEXT("name"));

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
	FString SnapshotId = Params->GetStringField(TEXT("snapshot_id"));

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
	FString SnapshotId = Params->GetStringField(TEXT("snapshot_id"));

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
