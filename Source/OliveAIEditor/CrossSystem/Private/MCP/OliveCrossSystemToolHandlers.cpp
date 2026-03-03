// Copyright Bode Software. All Rights Reserved.

#include "OliveCrossSystemToolHandlers.h"
#include "OliveCrossSystemSchemas.h"
#include "OliveSnapshotManager.h"
#include "OliveMultiAssetOperations.h"
#include "Index/OliveProjectIndex.h"
#include "Settings/OliveAISettings.h"
#include "Writer/OliveGraphWriter.h"
#include "Compile/OliveCompileManager.h"
#include "Services/OliveBatchExecutionScope.h"
#include "Services/OliveToolParamHelpers.h"
#include "Services/OliveGraphBatchExecutor.h"
#include "Engine/Blueprint.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Internationalization/Regex.h"
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

	RegisterBulkTools();
	RegisterBatchTools();
	RegisterSnapshotTools();
	RegisterIndexTools();

	// Recipe system
	LoadRecipeLibrary();
	RegisterRecipeTools();

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

FOliveToolResult FOliveCrossSystemToolHandlers::HandleCreateAICharacter(const TSharedPtr<FJsonObject>& Params)
{
	FString Name;
	if (!Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'name' is missing"),
			TEXT("Provide the base name for the AI character. Example: \"EnemyGuard\""));
	}
	FString Path;
	if (!Params->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing"),
			TEXT("Provide the /Game/... directory path. Example: \"/Game/AI/Characters\""));
	}

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
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'asset_path' is missing"),
			TEXT("Provide the Blueprint asset path to migrate. Example: \"/Game/Blueprints/BP_Player\""));
	}
	FString ModuleName;
	if (!Params->TryGetStringField(TEXT("module_name"), ModuleName) || ModuleName.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'module_name' is missing"),
			TEXT("Provide the target C++ module name. Use cpp.list_project_classes to see available modules."));
	}
	FString TargetClassName;
	if (!Params->TryGetStringField(TEXT("target_class_name"), TargetClassName) || TargetClassName.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'target_class_name' is missing"),
			TEXT("Provide the target C++ class name. Example: \"AMyPlayerCharacter\""));
	}

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
// Batch Write Registration
// =============================================================================

void FOliveCrossSystemToolHandlers::RegisterBatchTools()
{
	// Removed in AI Freedom Phase 2 — plan-JSON handles Blueprint batching; individual BT/PCG/C++ ops are fast enough
	// FOliveToolRegistry& Registry = FOliveToolRegistry::Get();
	//
	// Registry.RegisterTool(
	// 	TEXT("project.batch_write"),
	// 	TEXT("Execute multiple Blueprint graph operations atomically under a single undo transaction"),
	// 	OliveCrossSystemSchemas::ProjectBatchWrite(),
	// 	FOliveToolHandler::CreateRaw(this, &FOliveCrossSystemToolHandlers::HandleBatchWrite),
	// 	{TEXT("crosssystem"), TEXT("write")},
	// 	TEXT("crosssystem")
	// );
	// RegisteredToolNames.Add(TEXT("project.batch_write"));
}

// =============================================================================
// Batch Write — Thin wrappers delegating to FOliveGraphBatchExecutor
// =============================================================================

static bool ResolveTemplateReferences(
	TSharedPtr<FJsonObject>& OpParams,
	const TMap<FString, TSharedPtr<FJsonObject>>& OpResults,
	FString& OutError)
{
	return FOliveGraphBatchExecutor::ResolveTemplateReferences(OpParams, OpResults, OutError);
}

static const TSet<FString>& GetBatchWriteAllowlist()
{
	return FOliveGraphBatchExecutor::GetBatchWriteAllowlist();
}

static FOliveBlueprintWriteResult DispatchWriterOp(
	const FString& ToolName,
	const FString& BlueprintPath,
	const TSharedPtr<FJsonObject>& OpParams)
{
	return FOliveGraphBatchExecutor::DispatchWriterOp(ToolName, BlueprintPath, OpParams);
}

// =============================================================================
// Batch Write Handler
// =============================================================================

FOliveToolResult FOliveCrossSystemToolHandlers::HandleBatchWrite(const TSharedPtr<FJsonObject>& Params)
{
	// -------------------------------------------------------------------------
	// Phase 1 — Validate (no mutation)
	// -------------------------------------------------------------------------

	// 1. Parse and resolve asset path
	FString BlueprintPath;
	if (!Params->TryGetStringField(TEXT("path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("MISSING_PATH"), TEXT("'path' parameter is required"),
			TEXT("Provide the Blueprint asset path. Example: \"/Game/Blueprints/BP_Player\""));
	}

	// 2. Get ops array
	const TArray<TSharedPtr<FJsonValue>>* OpsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("ops"), OpsArray) || !OpsArray || OpsArray->Num() == 0)
	{
		return FOliveToolResult::Error(TEXT("MISSING_OPS"), TEXT("'ops' array is required and must not be empty"),
			TEXT("Provide an 'ops' array of {tool, params} objects. Example: [{\"tool\":\"blueprint.add_node\",\"params\":{\"type\":\"PrintString\"}}]"));
	}

	// 3. Check ops count against settings limit
	int32 MaxOps = 200;
	if (UOliveAISettings* Settings = UOliveAISettings::Get())
	{
		MaxOps = Settings->BatchWriteMaxOps;
	}
	if (OpsArray->Num() > MaxOps)
	{
		return FOliveToolResult::Error(TEXT("TOO_MANY_OPS"),
			FString::Printf(TEXT("Batch contains %d ops but maximum is %d (configurable via BatchWriteMaxOps)"),
				OpsArray->Num(), MaxOps),
			TEXT("Split the batch into smaller chunks, or increase BatchWriteMaxOps in settings."));
	}

	// Parse options
	bool bDryRun = false;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

	bool bAutoCompile = true;
	Params->TryGetBoolField(TEXT("auto_compile"), bAutoCompile);

	bool bStopOnError = true;
	Params->TryGetBoolField(TEXT("stop_on_error"), bStopOnError);

	FString TopLevelGraph;
	Params->TryGetStringField(TEXT("graph"), TopLevelGraph);

	const TSet<FString>& Allowlist = GetBatchWriteAllowlist();

	// 4-7. Parse ops, validate tool names, inject defaults, validate template refs
	struct FBatchOp
	{
		FString Id;
		FString ToolName;
		TSharedPtr<FJsonObject> OpParams;
	};
	TArray<FBatchOp> Ops;
	TSet<FString> DeclaredIds;

	for (int32 i = 0; i < OpsArray->Num(); ++i)
	{
		TSharedPtr<FJsonObject> OpObj = (*OpsArray)[i]->AsObject();
		if (!OpObj.IsValid())
		{
			return FOliveToolResult::Error(TEXT("INVALID_OP"),
				FString::Printf(TEXT("Op at index %d is not a valid JSON object"), i + 1));
		}

		FBatchOp Op;
		OpObj->TryGetStringField(TEXT("id"), Op.Id);
		Op.ToolName = OpObj->GetStringField(TEXT("tool"));
		const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
		if (OpObj->TryGetObjectField(TEXT("params"), ParamsObj) && ParamsObj && ParamsObj->IsValid())
		{
			Op.OpParams = *ParamsObj;
		}

		if (Op.ToolName.IsEmpty())
		{
			return FOliveToolResult::Error(TEXT("MISSING_TOOL"),
				FString::Printf(TEXT("Op at index %d is missing 'tool' field"), i + 1));
		}

		// Allowlist check
		if (!Allowlist.Contains(Op.ToolName))
		{
			return FOliveToolResult::Error(TEXT("TOOL_NOT_ALLOWED"),
				FString::Printf(TEXT("Op %d: tool '%s' is not allowed in batch_write. Allowed: blueprint.add_node, blueprint.connect_pins, blueprint.disconnect_pins, blueprint.set_pin_default, blueprint.set_node_property, blueprint.remove_node"),
					i + 1, *Op.ToolName));
		}

		if (!Op.OpParams.IsValid())
		{
			Op.OpParams = MakeShared<FJsonObject>();
		}

		// Inject top-level path if missing from op params
		if (!Op.OpParams->HasField(TEXT("path")))
		{
			Op.OpParams->SetStringField(TEXT("path"), BlueprintPath);
		}

		// Inject top-level graph if provided and missing from op params
		if (!TopLevelGraph.IsEmpty() && !Op.OpParams->HasField(TEXT("graph")))
		{
			Op.OpParams->SetStringField(TEXT("graph"), TopLevelGraph);
		}

		// Track declared ids for forward-reference validation
		if (!Op.Id.IsEmpty())
		{
			if (DeclaredIds.Contains(Op.Id))
			{
				return FOliveToolResult::Error(TEXT("DUPLICATE_OP_ID"),
					FString::Printf(TEXT("Op %d: duplicate id '%s'"), i + 1, *Op.Id));
			}
			DeclaredIds.Add(Op.Id);
		}

		// Validate template references point to earlier ops
		for (const auto& ParamPair : Op.OpParams->Values)
		{
			if (!ParamPair.Value.IsValid())
			{
				return FOliveToolResult::Error(
					TEXT("INVALID_PARAM_VALUE"),
					FString::Printf(TEXT("Op %d: param '%s' has an invalid null JSON value"), i + 1, *ParamPair.Key));
			}

			if (ParamPair.Value->Type != EJson::String)
			{
				continue;
			}

			FString ParamValue = ParamPair.Value->AsString();
			int32 SearchIdx = 0;
			while (SearchIdx < ParamValue.Len())
			{
				int32 TplStart = ParamValue.Find(TEXT("${"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchIdx);
				if (TplStart == INDEX_NONE)
				{
					break;
				}
				int32 TplEnd = ParamValue.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, TplStart + 2);
				if (TplEnd == INDEX_NONE)
				{
					return FOliveToolResult::Error(TEXT("INVALID_TEMPLATE"),
						FString::Printf(TEXT("Op %d: unclosed template in param '%s'"), i + 1, *ParamPair.Key));
				}
				FString Content = ParamValue.Mid(TplStart + 2, TplEnd - TplStart - 2);
				FString RefId;
				FString RefField;
				if (!Content.Split(TEXT("."), &RefId, &RefField))
				{
					return FOliveToolResult::Error(TEXT("INVALID_TEMPLATE"),
						FString::Printf(TEXT("Op %d: invalid template '${%s}' — expected ${opId.field}"), i + 1, *Content));
				}

				// Check that referenced id was declared in an earlier op
				bool bFoundEarlier = false;
				for (int32 j = 0; j < Ops.Num(); ++j)
				{
					if (Ops[j].Id == RefId)
					{
						bFoundEarlier = true;
						break;
					}
				}
				if (!bFoundEarlier)
				{
					return FOliveToolResult::Error(TEXT("FORWARD_REFERENCE"),
						FString::Printf(TEXT("Op %d: template '${%s}' references id '%s' which is not declared in an earlier op"),
							i + 1, *Content, *RefId));
				}

				SearchIdx = TplEnd + 1;
			}
		}

		Ops.Add(MoveTemp(Op));
	}

	// -------------------------------------------------------------------------
	// Phase 2 — Dry run: return normalized ops without executing
	// -------------------------------------------------------------------------
	if (bDryRun)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("asset_path"), BlueprintPath);
		Data->SetNumberField(TEXT("total_ops"), Ops.Num());
		Data->SetBoolField(TEXT("dry_run"), true);
		Data->SetStringField(TEXT("validation"), TEXT("passed"));

		TArray<TSharedPtr<FJsonValue>> NormalizedOps;
		for (int32 i = 0; i < Ops.Num(); ++i)
		{
			TSharedPtr<FJsonObject> OpJson = MakeShared<FJsonObject>();
			OpJson->SetNumberField(TEXT("index_1based"), i + 1);
			OpJson->SetStringField(TEXT("tool"), Ops[i].ToolName);
			if (!Ops[i].Id.IsEmpty())
			{
				OpJson->SetStringField(TEXT("id"), Ops[i].Id);
			}
			OpJson->SetObjectField(TEXT("params"), Ops[i].OpParams);
			NormalizedOps.Add(MakeShared<FJsonValueObject>(OpJson));
		}
		Data->SetArrayField(TEXT("ops"), NormalizedOps);

		return FOliveToolResult::Success(Data);
	}

	// -------------------------------------------------------------------------
	// Phase 3 — Execute
	// -------------------------------------------------------------------------

	// Load the Blueprint once
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	if (!Blueprint)
	{
		return FOliveToolResult::Error(TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Could not load Blueprint at '%s'"), *BlueprintPath),
			TEXT("Verify the asset exists and is a Blueprint. Use project.search to find the correct path."));
	}

	// Results storage
	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	TMap<FString, TSharedPtr<FJsonObject>> OpResultsById;
	TSharedPtr<FJsonObject> FailedOp;
	int32 CompletedCount = 0;

	{
		// One outer transaction for the entire batch
		FScopedTransaction Transaction(LOCTEXT("BatchWrite", "Batch Write"));

		// Enter batch scope to suppress inner transactions in writer methods
		FOliveBatchExecutionScope BatchScope;

		Blueprint->Modify();

		for (int32 i = 0; i < Ops.Num(); ++i)
		{
			FBatchOp& Op = Ops[i];

			// Resolve template references from prior op results
			FString TemplateError;
			if (!ResolveTemplateReferences(Op.OpParams, OpResultsById, TemplateError))
			{
				// Template resolution failed
				if (!FailedOp.IsValid())
				{
					FailedOp = MakeShared<FJsonObject>();
					FailedOp->SetNumberField(TEXT("index_1based"), i + 1);
					FailedOp->SetStringField(TEXT("tool"), Op.ToolName);
					FailedOp->SetStringField(TEXT("error_code"), TEXT("TEMPLATE_RESOLUTION_FAILED"));
					FailedOp->SetStringField(TEXT("error_message"), TemplateError);
				}

				TSharedPtr<FJsonObject> OpResult = MakeShared<FJsonObject>();
				OpResult->SetNumberField(TEXT("index_1based"), i + 1);
				OpResult->SetStringField(TEXT("tool"), Op.ToolName);
				OpResult->SetBoolField(TEXT("success"), false);
				OpResult->SetStringField(TEXT("error"), TemplateError);
				ResultsArray.Add(MakeShared<FJsonValueObject>(OpResult));

				if (bStopOnError)
				{
					break;
				}
				continue;
			}

			// Dispatch to writer
			FOliveBlueprintWriteResult WriteResult = DispatchWriterOp(Op.ToolName, BlueprintPath, Op.OpParams);

			// Build result entry
			TSharedPtr<FJsonObject> OpResult = MakeShared<FJsonObject>();
			OpResult->SetNumberField(TEXT("index_1based"), i + 1);
			OpResult->SetStringField(TEXT("tool"), Op.ToolName);
			OpResult->SetBoolField(TEXT("success"), WriteResult.bSuccess);

			if (WriteResult.bSuccess)
			{
				CompletedCount++;

				// Build data from the write result
				TSharedPtr<FJsonObject> OpData = MakeShared<FJsonObject>();
				if (!WriteResult.CreatedNodeId.IsEmpty())
				{
					OpData->SetStringField(TEXT("node_id"), WriteResult.CreatedNodeId);
				}
				if (!WriteResult.CreatedItemName.IsEmpty())
				{
					OpData->SetStringField(TEXT("item_name"), WriteResult.CreatedItemName);
				}
				if (WriteResult.Warnings.Num() > 0)
				{
					TArray<TSharedPtr<FJsonValue>> WarningsArr;
					for (const FString& W : WriteResult.Warnings)
					{
						WarningsArr.Add(MakeShared<FJsonValueString>(W));
					}
					OpData->SetArrayField(TEXT("warnings"), WarningsArr);
				}

				OpResult->SetObjectField(TEXT("data"), OpData);

				// Store for template resolution by later ops
				if (!Op.Id.IsEmpty())
				{
					OpResultsById.Add(Op.Id, OpData);
				}
			}
			else
			{
				FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");
				OpResult->SetStringField(TEXT("error"), ErrorMsg);

				if (!FailedOp.IsValid())
				{
					FailedOp = MakeShared<FJsonObject>();
					FailedOp->SetNumberField(TEXT("index_1based"), i + 1);
					FailedOp->SetStringField(TEXT("tool"), Op.ToolName);
					FailedOp->SetStringField(TEXT("error_code"), TEXT("OP_FAILED"));
					FailedOp->SetStringField(TEXT("error_message"), ErrorMsg);
				}

				ResultsArray.Add(MakeShared<FJsonValueObject>(OpResult));

				if (bStopOnError)
				{
					break;
				}
				continue;
			}

			ResultsArray.Add(MakeShared<FJsonValueObject>(OpResult));
		}

		// If any op failed, cancel the transaction so no partial mutations remain.
		// This is required for the tool's "atomic batch" contract.
		if (FailedOp.IsValid())
		{
			Transaction.Cancel();
		}
	} // FScopedTransaction destroyed here — commits if we reach this point

	// -------------------------------------------------------------------------
	// Compile (if all succeeded and auto_compile is true)
	// -------------------------------------------------------------------------
	TSharedPtr<FJsonObject> CompileResult;
	bool bAllSucceeded = !FailedOp.IsValid();

	if (bAllSucceeded && bAutoCompile)
	{
		FOliveIRCompileResult CompileIR = FOliveCompileManager::Get().Compile(Blueprint);

		CompileResult = MakeShared<FJsonObject>();
		CompileResult->SetBoolField(TEXT("success"), CompileIR.bSuccess);

		TArray<TSharedPtr<FJsonValue>> CompileErrors;
		for (const FOliveIRCompileError& Err : CompileIR.Errors)
		{
			CompileErrors.Add(MakeShared<FJsonValueString>(Err.Message));
		}
		CompileResult->SetArrayField(TEXT("errors"), CompileErrors);

		TArray<TSharedPtr<FJsonValue>> CompileWarnings;
		for (const FOliveIRCompileError& Warn : CompileIR.Warnings)
		{
			CompileWarnings.Add(MakeShared<FJsonValueString>(Warn.Message));
		}
		CompileResult->SetArrayField(TEXT("warnings"), CompileWarnings);
	}

	// -------------------------------------------------------------------------
	// Phase 4 — Build output
	// -------------------------------------------------------------------------
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), BlueprintPath);
	Data->SetNumberField(TEXT("total_ops"), Ops.Num());
	Data->SetNumberField(TEXT("completed_ops"), CompletedCount);
	Data->SetBoolField(TEXT("rolled_back"), FailedOp.IsValid());

	if (FailedOp.IsValid())
	{
		Data->SetObjectField(TEXT("failed_op"), FailedOp);
	}
	else
	{
		Data->SetField(TEXT("failed_op"), MakeShared<FJsonValueNull>());
	}

	Data->SetArrayField(TEXT("results"), ResultsArray);

	// Build id_map: declared op ids -> created node ids
	TSharedPtr<FJsonObject> IdMap = MakeShared<FJsonObject>();
	for (const auto& Pair : OpResultsById)
	{
		FString NodeId;
		if (Pair.Value->TryGetStringField(TEXT("node_id"), NodeId))
		{
			IdMap->SetStringField(Pair.Key, NodeId);
		}
	}
	Data->SetObjectField(TEXT("id_map"), IdMap);

	if (CompileResult.IsValid())
	{
		Data->SetObjectField(TEXT("compile_result"), CompileResult);
	}

	if (bAllSucceeded)
	{
		return FOliveToolResult::Success(Data);
	}
	else
	{
		FOliveToolResult Result = FOliveToolResult::Error(
			TEXT("BATCH_FAILED"),
			TEXT("project.batch_write failed"),
			TEXT("Inspect data.failed_op and data.results for details; fix the first failing op and retry.")
		);
		Result.Data = Data;
		return Result;
	}
}

// =============================================================================
// Snapshot Handlers
// =============================================================================

FOliveToolResult FOliveCrossSystemToolHandlers::HandleSnapshot(const TSharedPtr<FJsonObject>& Params)
{
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

	Registry.RegisterTool(
		TEXT("olive.get_recipe"),
		TEXT("Search for patterns, examples, and gotchas for Blueprint workflows. "
			"Query with keywords related to your task or error "
			"(e.g. 'spawn actor transform', 'variable type object', 'function graph entry'). "
			"Returns the most relevant reference entry."),
		OliveCrossSystemSchemas::RecipeGetRecipe(),
		FOliveToolHandler::CreateRaw(this, &FOliveCrossSystemToolHandlers::HandleGetRecipe),
		{TEXT("crosssystem"), TEXT("read")},
		// Category is intentionally "crosssystem" — NOT "olive". Focus profiles filter
		// by category, and "crosssystem" ensures visibility in Blueprint/Auto profiles
		// without adding a new category to the profile filter config.
		TEXT("crosssystem")
	);
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
		TEXT("Search ~150K real-world community Blueprints for gameplay patterns like weapons, pickups, "
			"inventory, interaction, AI, movement, doors, health systems, and more. "
			"Returns compact graph summaries showing node flow, data wiring, and pin defaults "
			"so you can see how other developers structured similar systems. "
			"Useful when building gameplay systems to discover common variable layouts, "
			"component choices, and wiring patterns you might not think of. "
			"Results prefer UE 5.0+ examples. Quality varies — adapt rather than copy."),
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
	// --- Parameter extraction ---
	FString Query;
	if (!Params->TryGetStringField(TEXT("query"), Query) || Query.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("MISSING_PARAM"),
			TEXT("'query' is required"),
			TEXT("Provide search terms like 'gun fire reload'"));
	}

	FString TypeFilter;
	Params->TryGetStringField(TEXT("type"), TypeFilter);

	int32 MaxResults = 5;
	if (Params->HasField(TEXT("max_results")))
	{
		MaxResults = FMath::Clamp(static_cast<int32>(Params->GetNumberField(TEXT("max_results"))), 1, 10);
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
		// Database not available — return empty result, NOT an error
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> EmptyArray;
		Result->SetArrayField(TEXT("results"), EmptyArray);
		Result->SetNumberField(TEXT("count"), 0);
		Result->SetStringField(TEXT("note"),
			TEXT("Community blueprint index not found. This is an optional feature — place community_blueprints.db in Content/CommunityBlueprints/."));
		return FOliveToolResult::Success(Result);
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
		// 0=slug (skip), 1=title, 2=type, 3=ue_version, 4=node_count,
		// 5=functions, 6=variables, 7=components, 8=compact, 9=url
		FString Title, Type, UeVersion, Functions, Variables, Components, Compact, Url;
		int32 NodeCount = 0;

		Row.GetColumnValueByIndex(1, Title);
		Row.GetColumnValueByIndex(2, Type);
		Row.GetColumnValueByIndex(3, UeVersion);
		Row.GetColumnValueByIndex(4, NodeCount);
		Row.GetColumnValueByIndex(5, Functions);
		Row.GetColumnValueByIndex(6, Variables);
		Row.GetColumnValueByIndex(7, Components);
		Row.GetColumnValueByIndex(8, Compact);
		Row.GetColumnValueByIndex(9, Url);

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
			FString::Printf(TEXT("Showing %d of %lld matches. Quality varies — browse 2-3 pages before committing to a pattern. Use offset=%d for next page."),
				Results.Num(), TotalMatches, Offset + Results.Num()));
	}
	else
	{
		Response->SetStringField(TEXT("note"),
			TEXT("Community examples from blueprintue.com. Quality varies — use your judgment on which patterns to follow."));
	}

	return FOliveToolResult::Success(Response);
}

#undef LOCTEXT_NAMESPACE
