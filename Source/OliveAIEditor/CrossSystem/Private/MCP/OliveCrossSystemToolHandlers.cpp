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
// Batch Write Registration
// =============================================================================

void FOliveCrossSystemToolHandlers::RegisterBatchTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	Registry.RegisterTool(
		TEXT("project.batch_write"),
		TEXT("Execute multiple Blueprint graph operations atomically under a single undo transaction"),
		OliveCrossSystemSchemas::ProjectBatchWrite(),
		FOliveToolHandler::CreateRaw(this, &FOliveCrossSystemToolHandlers::HandleBatchWrite),
		{TEXT("crosssystem"), TEXT("write")},
		TEXT("crosssystem")
	);
	RegisteredToolNames.Add(TEXT("project.batch_write"));
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
		return FOliveToolResult::Error(TEXT("MISSING_PATH"), TEXT("'path' parameter is required"));
	}

	// 2. Get ops array
	const TArray<TSharedPtr<FJsonValue>>* OpsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("ops"), OpsArray) || !OpsArray || OpsArray->Num() == 0)
	{
		return FOliveToolResult::Error(TEXT("MISSING_OPS"), TEXT("'ops' array is required and must not be empty"));
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
				OpsArray->Num(), MaxOps));
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
			FString::Printf(TEXT("Could not load Blueprint at '%s'"), *BlueprintPath));
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

// =============================================================================
// Index / Context Handlers
// =============================================================================

void FOliveCrossSystemToolHandlers::RegisterIndexTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	Registry.RegisterTool(
		TEXT("project.index_build"),
		TEXT("Export the project index to a JSON file for external consumption"),
		OliveCrossSystemSchemas::ProjectIndexBuild(),
		FOliveToolHandler::CreateRaw(this, &FOliveCrossSystemToolHandlers::HandleIndexBuild),
		{TEXT("crosssystem"), TEXT("read")},
		TEXT("crosssystem")
	);
	RegisteredToolNames.Add(TEXT("project.index_build"));

	Registry.RegisterTool(
		TEXT("project.index_status"),
		TEXT("Check whether the project index is stale and needs re-export"),
		OliveCrossSystemSchemas::ProjectIndexStatus(),
		FOliveToolHandler::CreateRaw(this, &FOliveCrossSystemToolHandlers::HandleIndexStatus),
		{TEXT("crosssystem"), TEXT("read")},
		TEXT("crosssystem")
	);
	RegisteredToolNames.Add(TEXT("project.index_status"));

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
		return FOliveToolResult::Error(TEXT("MISSING_QUERY"), TEXT("query parameter is required"));
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

#undef LOCTEXT_NAMESPACE
