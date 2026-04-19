// Copyright Bode Software. All Rights Reserved.

#include "OliveWritePipeline.h"
#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "HAL/PlatformTime.h"
#include "Compile/OliveCompileManager.h"
#include "UObject/StrongObjectPtr.h"
#include "Services/OliveAssetResolver.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Knot.h"
#include "K2Node_ExecutionSequence.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphNode_Comment.h"

DEFINE_LOG_CATEGORY(LogOliveWritePipeline);

// ============================================================================
// FOliveWriteResult Factory Methods
// ============================================================================

FOliveToolResult FOliveWriteResult::ToToolResult() const
{
	FOliveToolResult ToolResult;
	ToolResult.bSuccess = bSuccess;
	ToolResult.Data = ResultData;
	ToolResult.Messages = ValidationMessages;
	ToolResult.ExecutionTimeMs = ExecutionTimeMs;
	ToolResult.NextStepGuidance = NextStepGuidance;

	// Safety net: if failure has no messages but has ResultData with error info,
	// synthesize a message so ToJson() produces a structured error block.
	if (!bSuccess && ToolResult.Messages.Num() == 0 && ResultData.IsValid())
	{
		FString Code, Message, Suggestion;
		ResultData->TryGetStringField(TEXT("error_code"), Code);
		ResultData->TryGetStringField(TEXT("error_message"), Message);
		ResultData->TryGetStringField(TEXT("suggestion"), Suggestion);

		if (!Message.IsEmpty())
		{
			FOliveIRMessage SynthMsg;
			SynthMsg.Severity = EOliveIRSeverity::Error;
			SynthMsg.Code = Code.IsEmpty() ? TEXT("PIPELINE_ERROR") : Code;
			SynthMsg.Message = Message;
			SynthMsg.Suggestion = Suggestion;
			ToolResult.Messages.Add(MoveTemp(SynthMsg));
		}
	}

	return ToolResult;
}

FOliveWriteResult FOliveWriteResult::ValidationError(const FOliveValidationResult& Result)
{
	FOliveWriteResult WriteResult;
	WriteResult.bSuccess = false;
	WriteResult.CompletedStage = EOliveWriteStage::Validate;
	WriteResult.ValidationMessages = Result.Messages;

	// Build error data
	WriteResult.ResultData = MakeShareable(new FJsonObject());
	WriteResult.ResultData->SetBoolField(TEXT("success"), false);

	TArray<TSharedPtr<FJsonValue>> MessageArray;
	for (const FOliveIRMessage& Msg : Result.Messages)
	{
		MessageArray.Add(MakeShareable(new FJsonValueObject(Msg.ToJson())));
	}
	WriteResult.ResultData->SetArrayField(TEXT("validation_messages"), MessageArray);

	return WriteResult;
}

FOliveWriteResult FOliveWriteResult::ExecutionError(const FString& Code, const FString& Message, const FString& Suggestion)
{
	FOliveWriteResult WriteResult;
	WriteResult.bSuccess = false;
	WriteResult.CompletedStage = EOliveWriteStage::Execute;

	// Add error message
	FOliveIRMessage ErrorMsg;
	ErrorMsg.Severity = EOliveIRSeverity::Error;
	ErrorMsg.Code = Code;
	ErrorMsg.Message = Message;
	ErrorMsg.Suggestion = Suggestion;
	WriteResult.ValidationMessages.Add(ErrorMsg);

	// Build error data
	WriteResult.ResultData = MakeShareable(new FJsonObject());
	WriteResult.ResultData->SetBoolField(TEXT("success"), false);
	WriteResult.ResultData->SetStringField(TEXT("error_code"), Code);
	WriteResult.ResultData->SetStringField(TEXT("error_message"), Message);
	if (!Suggestion.IsEmpty())
	{
		WriteResult.ResultData->SetStringField(TEXT("suggestion"), Suggestion);
	}

	return WriteResult;
}

FOliveWriteResult FOliveWriteResult::Success(const TSharedPtr<FJsonObject>& Data)
{
	FOliveWriteResult WriteResult;
	WriteResult.bSuccess = true;
	WriteResult.CompletedStage = EOliveWriteStage::Report;
	WriteResult.ResultData = Data ? Data : MakeShareable(new FJsonObject());
	WriteResult.ResultData->SetBoolField(TEXT("success"), true);

	return WriteResult;
}

// ============================================================================
// FOliveWritePipeline Singleton
// ============================================================================

FOliveWritePipeline& FOliveWritePipeline::Get()
{
	static FOliveWritePipeline Instance;
	return Instance;
}

// ============================================================================
// Public Pipeline Execution
// ============================================================================

FOliveWriteResult FOliveWritePipeline::Execute(const FOliveWriteRequest& Request, FOliveWriteExecutor Executor)
{
	const double StartTime = FPlatformTime::Seconds();

	UE_LOG(LogOliveWritePipeline, Log, TEXT("Starting write pipeline for tool '%s' (category: %s, MCP: %d)"),
		*Request.ToolName, *Request.OperationCategory, Request.bFromMCP);

	// Root the target asset for the full pipeline scope (GC safety).
	// Prevents the garbage collector from collecting the asset between stages
	// while UE processes events during long-running pipeline operations.
	TStrongObjectPtr<UObject> RootedAsset(Request.TargetAsset);

	// Stage 1: Validate
	FOliveWriteResult ValidateResult = StageValidate(Request);
	if (!ValidateResult.bSuccess)
	{
		UE_LOG(LogOliveWritePipeline, Warning, TEXT("Validation failed for tool '%s'"), *Request.ToolName);
		ValidateResult.ExecutionTimeMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;
		return ValidateResult;
	}

	// Stage 2: Transact
	TUniquePtr<FOliveTransactionManager::FScopedOliveTransaction> Transaction = StageTransact(Request, Request.TargetAsset);
	if (!Transaction.IsValid())
	{
		UE_LOG(LogOliveWritePipeline, Error, TEXT("Failed to open transaction for tool '%s'"), *Request.ToolName);
		return FOliveWriteResult::ExecutionError(
			TEXT("PIPELINE_TRANSACTION_FAILED"),
			TEXT("Failed to open transaction"),
			TEXT("Ensure the asset is not locked or in use by another editor"));
	}

	// Stage 3: Execute
	UObject* EffectiveTargetAsset = Request.TargetAsset;
	FOliveWriteResult ExecuteResult = StageExecute(Request, Request.TargetAsset, Executor, EffectiveTargetAsset);
	if (!ExecuteResult.bSuccess)
	{
		FString ErrorCode;
		FString ErrorMessage;
		if (ExecuteResult.ResultData.IsValid())
		{
			ExecuteResult.ResultData->TryGetStringField(TEXT("error_code"), ErrorCode);
			ExecuteResult.ResultData->TryGetStringField(TEXT("error_message"), ErrorMessage);
		}
		UE_LOG(LogOliveWritePipeline, Error, TEXT("Execution failed for tool '%s' (%s): %s"),
			*Request.ToolName, *ErrorCode, *ErrorMessage);
		Transaction->Cancel(); // Roll back transaction

		// After rollback, force the Blueprint editor to refresh from the reverted
		// state. This clears any stale caches that may have been updated during
		// execution (e.g., from graph notifications or pin reconstruction).
		// Without this, autosave can encounter stale node references → crash.
		if (UBlueprint* Blueprint = Cast<UBlueprint>(EffectiveTargetAsset))
		{
			Blueprint->BroadcastChanged();
		}

		ExecuteResult.ExecutionTimeMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;
		return ExecuteResult;
	}

	// Stage 4: Verify (optional based on request settings)
	FOliveWriteResult VerifyResult = ExecuteResult;
	if (!Request.bSkipVerification)
	{
		VerifyResult = StageVerify(Request, EffectiveTargetAsset, ExecuteResult);
		// Note: Verification warnings don't cancel the transaction
		// Only execution errors cancel
	}

	// Transaction commits automatically when it goes out of scope here

	// Stage 5: Report
	const double TotalTimeMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;
	FOliveWriteResult FinalResult = StageReport(Request, VerifyResult, TotalTimeMs);

	UE_LOG(LogOliveWritePipeline, Log, TEXT("Pipeline completed for tool '%s' in %.2fms (success: %d)"),
		*Request.ToolName, TotalTimeMs, FinalResult.bSuccess);

	return FinalResult;
}

// ============================================================================
// Pipeline Stages
// ============================================================================

FOliveWriteResult FOliveWritePipeline::StageValidate(const FOliveWriteRequest& Request)
{
	UE_LOG(LogOliveWritePipeline, Verbose, TEXT("Stage 1: Validate - tool '%s'"), *Request.ToolName);

	// Use the validation engine to run registered validation rules
	FOliveValidationEngine& ValidationEngine = FOliveValidationEngine::Get();
	FOliveValidationResult ValidationResult = ValidationEngine.ValidateOperation(
		Request.ToolName,
		Request.Params,
		Request.TargetAsset);

	if (!ValidationResult.bValid)
	{
		return FOliveWriteResult::ValidationError(ValidationResult);
	}

	FOliveWriteResult Result;
	Result.bSuccess = true;
	Result.CompletedStage = EOliveWriteStage::Validate;
	Result.ValidationMessages = ValidationResult.Messages; // Include warnings/info
	return Result;
}

TUniquePtr<FOliveTransactionManager::FScopedOliveTransaction> FOliveWritePipeline::StageTransact(
	const FOliveWriteRequest& Request,
	UObject* TargetAsset)
{
	UE_LOG(LogOliveWritePipeline, Verbose, TEXT("Stage 2: Transact - tool '%s'"), *Request.ToolName);

	// Open transaction
	TUniquePtr<FOliveTransactionManager::FScopedOliveTransaction> Transaction =
		MakeUnique<FOliveTransactionManager::FScopedOliveTransaction>(Request.OperationDescription);

	// Mark target asset as modified (if applicable)
	if (TargetAsset)
	{
		TargetAsset->Modify();
		UE_LOG(LogOliveWritePipeline, Verbose, TEXT("Marked asset '%s' as modified"), *TargetAsset->GetName());
	}

	return Transaction;
}

FOliveWriteResult FOliveWritePipeline::StageExecute(
	const FOliveWriteRequest& Request,
	UObject* TargetAsset,
	FOliveWriteExecutor& Executor,
	UObject*& OutEffectiveTargetAsset)
{
	UE_LOG(LogOliveWritePipeline, Verbose, TEXT("Stage 3: Execute - tool '%s'"), *Request.ToolName);

	// Re-resolve before mutation to guard against hot-reload pointer staleness.
	// After a hot-reload the original TargetAsset pointer may point to a stale
	// (garbage-collected or replaced) class instance. Re-resolving by path gives
	// us the live pointer before we touch the asset.
	// NOTE: Skip re-resolve for create operations (TargetAsset == nullptr) because
	// the asset does not exist yet — resolving by path would always fail.
	UObject* LiveAsset = TargetAsset;
	if (!Request.AssetPath.IsEmpty() && TargetAsset != nullptr)
	{
		FOliveAssetResolveInfo Fresh = FOliveAssetResolver::Get().ResolveByPath(Request.AssetPath);
		if (Fresh.IsSuccess() && Fresh.Asset != TargetAsset)
		{
			UE_LOG(LogOliveWritePipeline, Warning,
				TEXT("StageExecute: TargetAsset pointer changed after hot-reload — using fresh pointer for '%s'"),
				*Request.AssetPath);
			LiveAsset = Fresh.Asset;
		}
		else if (!Fresh.IsSuccess())
		{
			return FOliveWriteResult::ExecutionError(
				TEXT("ASSET_GONE"),
				TEXT("Asset could not be resolved before mutation (possible hot-reload)"),
				TEXT("Retry the operation"));
		}
	}
	OutEffectiveTargetAsset = LiveAsset;

	if (!Executor.IsBound())
	{
		UE_LOG(LogOliveWritePipeline, Error, TEXT("Executor delegate not bound for tool '%s'"), *Request.ToolName);
		return FOliveWriteResult::ExecutionError(
			TEXT("PIPELINE_NO_EXECUTOR"),
			TEXT("Internal error: No executor provided"),
			TEXT("This is a plugin bug - please report it"));
	}

	// Root the effective pointer for this stage in case hot-reload produced a new object.
	TStrongObjectPtr<UObject> RootedEffectiveAsset(LiveAsset);

	// Execute the mutation via the provided delegate, using the freshly resolved asset pointer
	FOliveWriteResult ExecuteResult = Executor.Execute(Request, LiveAsset);
	ExecuteResult.CompletedStage = EOliveWriteStage::Execute;

	return ExecuteResult;
}

FOliveWriteResult FOliveWritePipeline::StageVerify(
	const FOliveWriteRequest& Request,
	UObject* TargetAsset,
	const FOliveWriteResult& ExecuteResult)
{
	UE_LOG(LogOliveWritePipeline, Verbose, TEXT("Stage 4: Verify - tool '%s'"), *Request.ToolName);

	FOliveWriteResult VerifyResult = ExecuteResult;
	VerifyResult.CompletedStage = EOliveWriteStage::Verify;

	// Structural verification for Blueprints
	if (UBlueprint* Blueprint = Cast<UBlueprint>(TargetAsset))
	{
		TArray<FOliveIRMessage> StructuralMessages;
		bool bStructureValid = VerifyBlueprintStructure(Blueprint, StructuralMessages);

		// Post-op orphaned exec-flow detection for graph-editing operations
		if (IsGraphEditOperation(Request.ToolName))
		{
			FString GraphName;
			if (Request.Params.IsValid())
			{
				Request.Params->TryGetStringField(TEXT("graph"), GraphName);
			}

			if (!GraphName.IsEmpty())
			{
				// Search ubergraph pages for the affected graph
				for (UEdGraph* Graph : Blueprint->UbergraphPages)
				{
					if (Graph && Graph->GetName() == GraphName)
					{
						// Collect orphan messages into a separate array first
						TArray<FOliveIRMessage> OrphanMessages;
						int32 AbsoluteOrphanCount = DetectOrphanedExecFlows(Graph, OrphanMessages);

						if (AbsoluteOrphanCount > 0)
						{
							const FString GraphPath = FString::Printf(TEXT("%s::%s"),
								*Blueprint->GetPathName(), *GraphName);

							if (bRunActive)
							{
								// Lazy baseline: first check captures baseline, returns full count
								// Subsequent checks return only new orphans
								SetOrphanBaseline(GraphPath, AbsoluteOrphanCount);
								int32 DeltaOrphanCount = GetOrphanDelta(GraphPath, AbsoluteOrphanCount);

								if (DeltaOrphanCount > 0)
								{
									// Only append the last DeltaOrphanCount messages (newest orphans)
									int32 StartIndex = FMath::Max(0, OrphanMessages.Num() - DeltaOrphanCount);
									for (int32 i = StartIndex; i < OrphanMessages.Num(); i++)
									{
										StructuralMessages.Add(OrphanMessages[i]);
									}
								}

								UE_LOG(LogOliveWritePipeline, Log,
									TEXT("Orphan delta for '%s': %d new (absolute: %d, baseline: %d)"),
									*GraphName, DeltaOrphanCount, AbsoluteOrphanCount,
									AbsoluteOrphanCount - DeltaOrphanCount);
							}
							else
							{
								// No active run -- report everything (compile, one-off tools)
								StructuralMessages.Append(OrphanMessages);
								UE_LOG(LogOliveWritePipeline, Log,
									TEXT("Detected %d orphaned exec flow(s) in graph '%s' of Blueprint '%s'"),
									AbsoluteOrphanCount, *GraphName, *Blueprint->GetName());
							}
						}
						break;
					}
				}
				// Also check function graphs
				for (UEdGraph* Graph : Blueprint->FunctionGraphs)
				{
					if (Graph && Graph->GetName() == GraphName)
					{
						// Collect orphan messages into a separate array first
						TArray<FOliveIRMessage> OrphanMessages;
						int32 AbsoluteOrphanCount = DetectOrphanedExecFlows(Graph, OrphanMessages);

						if (AbsoluteOrphanCount > 0)
						{
							const FString GraphPath = FString::Printf(TEXT("%s::%s"),
								*Blueprint->GetPathName(), *GraphName);

							if (bRunActive)
							{
								SetOrphanBaseline(GraphPath, AbsoluteOrphanCount);
								int32 DeltaOrphanCount = GetOrphanDelta(GraphPath, AbsoluteOrphanCount);

								if (DeltaOrphanCount > 0)
								{
									int32 StartIndex = FMath::Max(0, OrphanMessages.Num() - DeltaOrphanCount);
									for (int32 i = StartIndex; i < OrphanMessages.Num(); i++)
									{
										StructuralMessages.Add(OrphanMessages[i]);
									}
								}

								UE_LOG(LogOliveWritePipeline, Log,
									TEXT("Orphan delta for function '%s': %d new (absolute: %d, baseline: %d)"),
									*GraphName, DeltaOrphanCount, AbsoluteOrphanCount,
									AbsoluteOrphanCount - DeltaOrphanCount);
							}
							else
							{
								StructuralMessages.Append(OrphanMessages);
								UE_LOG(LogOliveWritePipeline, Log,
									TEXT("Detected %d orphaned exec flow(s) in function graph '%s' of Blueprint '%s'"),
									AbsoluteOrphanCount, *GraphName, *Blueprint->GetName());
							}
						}
						break;
					}
				}
			}
		}

		// Post-op unwired required data pin detection for graph-editing operations
		if (IsGraphEditOperation(Request.ToolName) || Request.ToolName == TEXT("blueprint.apply_plan_json"))
		{
			// Check all function graphs and ubergraph pages for unwired required pins
			// Scope to created nodes if available, otherwise check all nodes in affected graphs
			TSet<UEdGraphNode*> NodesToCheck;

			// If we have specific created node IDs, scope the check
			if (ExecuteResult.CreatedNodeIds.Num() > 0)
			{
				for (UEdGraph* Graph : Blueprint->UbergraphPages)
				{
					if (!Graph) continue;
					for (UEdGraphNode* Node : Graph->Nodes)
					{
						if (Node && ExecuteResult.CreatedNodeIds.Contains(Node->NodeGuid.ToString()))
						{
							NodesToCheck.Add(Node);
						}
					}
				}
				for (UEdGraph* Graph : Blueprint->FunctionGraphs)
				{
					if (!Graph) continue;
					for (UEdGraphNode* Node : Graph->Nodes)
					{
						if (Node && ExecuteResult.CreatedNodeIds.Contains(Node->NodeGuid.ToString()))
						{
							NodesToCheck.Add(Node);
						}
					}
				}
			}

			// Check all graphs (with optional node scoping)
			auto CheckGraphForUnwiredPins = [&StructuralMessages, &NodesToCheck](UEdGraph* Graph)
			{
				if (!Graph) return;
				TArray<FString> UnwiredMessages;
				int32 Count = FOliveWritePipeline::DetectUnwiredRequiredDataPins(Graph, NodesToCheck, UnwiredMessages);
				if (Count > 0)
				{
					for (const FString& Msg : UnwiredMessages)
					{
						FOliveIRMessage Warning;
						Warning.Severity = EOliveIRSeverity::Warning;
						Warning.Code = TEXT("UNWIRED_REQUIRED_PIN");
						Warning.Message = Msg;
						Warning.Suggestion = TEXT("Connect the pin or set a default value");
						StructuralMessages.Add(Warning);
					}
				}
			};

			for (UEdGraph* Graph : Blueprint->UbergraphPages)
			{
				CheckGraphForUnwiredPins(Graph);
			}
			for (UEdGraph* Graph : Blueprint->FunctionGraphs)
			{
				CheckGraphForUnwiredPins(Graph);
			}
		}

		// Add structural messages to result
		VerifyResult.ValidationMessages.Append(StructuralMessages);

		if (!bStructureValid)
		{
			UE_LOG(LogOliveWritePipeline, Warning, TEXT("Structural verification failed for Blueprint '%s'"), *Blueprint->GetName());
			// Note: We don't fail the operation on structural warnings
		}

		// Compile if requested
		if (Request.bAutoCompile)
		{
			UE_LOG(LogOliveWritePipeline, Verbose, TEXT("Compiling Blueprint '%s'"), *Blueprint->GetName());
			FOliveIRCompileResult CompileResult = CompileAndGatherErrors(Blueprint);
			VerifyResult.CompileResult = CompileResult;

			if (!CompileResult.bSuccess)
			{
				UE_LOG(LogOliveWritePipeline, Warning, TEXT("Compilation produced %d errors for Blueprint '%s'"),
					CompileResult.Errors.Num(), *Blueprint->GetName());
			}
		}
	}

	return VerifyResult;
}

FOliveWriteResult FOliveWritePipeline::StageReport(
	const FOliveWriteRequest& Request,
	const FOliveWriteResult& VerifyResult,
	double TotalTimeMs)
{
	UE_LOG(LogOliveWritePipeline, Verbose, TEXT("Stage 5: Report - tool '%s'"), *Request.ToolName);

	FOliveWriteResult FinalResult = VerifyResult;
	FinalResult.CompletedStage = EOliveWriteStage::Report;
	FinalResult.ExecutionTimeMs = TotalTimeMs;

	// Add timing information to result data
	if (FinalResult.ResultData.IsValid())
	{
		FinalResult.ResultData->SetNumberField(TEXT("execution_time_ms"), TotalTimeMs);
	}

	// Add compile result to data if present
	if (FinalResult.CompileResult.IsSet())
	{
		const FOliveIRCompileResult& CompileResult = FinalResult.CompileResult.GetValue();
		FinalResult.ResultData->SetObjectField(TEXT("compile_result"), CompileResult.ToJson());

		// Add a concise compile_status string for easy LLM consumption
		const FOliveIRCompileResult& CR = FinalResult.CompileResult.GetValue();
		FString Status;
		if (CR.HasErrors())        Status = TEXT("error");
		else if (CR.HasWarnings()) Status = TEXT("warning");
		else                       Status = TEXT("success");
		FinalResult.ResultData->SetStringField(TEXT("compile_status"), Status);

		// Propagate compile errors to top-level success.
		// Transaction already committed in Stage 3 — nodes persist.
		// Setting bSuccess=false causes FOliveToolResult.bSuccess=false,
		// which triggers self-correction via HasToolFailure().
		if (CR.HasErrors())
		{
			FinalResult.bSuccess = false;

			// Build a concrete top-level error message that includes the first
			// compiler error verbatim. Without this the MCP response body only
			// surfaces a generic "compiled with N errors" string and the AI has
			// to dig into compile_result.errors[] to know what actually broke —
			// which in practice it often fails to do, causing infinite retries
			// on the same broken plan.
			FString FirstErrorText;
			if (CR.Errors.Num() > 0)
			{
				FirstErrorText = CR.Errors[0].Message;
			}

			FString CompileSummary;
			if (!FirstErrorText.IsEmpty())
			{
				CompileSummary = FString::Printf(
					TEXT("Blueprint compile failed (%d error(s)). First error: %s"),
					CR.Errors.Num(), *FirstErrorText);
			}
			else
			{
				CompileSummary = FString::Printf(
					TEXT("Blueprint compiled with %d error(s). Nodes are committed but Blueprint is in error state."),
					CR.Errors.Num());
			}

			FOliveIRMessage CompileErrorMsg;
			CompileErrorMsg.Severity = EOliveIRSeverity::Error;
			CompileErrorMsg.Code = TEXT("COMPILE_FAILED");
			CompileErrorMsg.Message = CompileSummary;
			CompileErrorMsg.Suggestion = TEXT("Read compile_result.errors and fix with connect_pins or set_pin_default.");
			FinalResult.ValidationMessages.Add(CompileErrorMsg);

			// Mirror onto top-level ResultData fields so downstream consumers
			// (MCP response, log extraction at OliveWritePipeline.cpp:190, AI
			// self-correction prompt) see the concrete error without having to
			// traverse compile_result.errors[].
			if (FinalResult.ResultData.IsValid())
			{
				FinalResult.ResultData->SetStringField(TEXT("error_code"), TEXT("COMPILE_FAILED"));
				FinalResult.ResultData->SetStringField(TEXT("error_message"), CompileSummary);
				FinalResult.ResultData->SetStringField(TEXT("suggestion"),
					TEXT("Read compile_result.errors and fix with connect_pins or set_pin_default."));
			}

			UE_LOG(LogOliveWritePipeline, Warning,
				TEXT("StageReport: Compile errors detected — setting bSuccess=false for tool '%s': %s"),
				*Request.ToolName, *CompileSummary);
		}
	}
	else
	{
		FinalResult.ResultData->SetStringField(TEXT("compile_status"), TEXT("dirty"));
	}

	// Add validation messages to data
	if (FinalResult.ValidationMessages.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> MessageArray;
		for (const FOliveIRMessage& Msg : FinalResult.ValidationMessages)
		{
			MessageArray.Add(MakeShareable(new FJsonValueObject(Msg.ToJson())));
		}
		FinalResult.ResultData->SetArrayField(TEXT("messages"), MessageArray);
	}

	// Add navigation hints for UI result cards
	if (FinalResult.ResultData.IsValid())
	{
		if (!Request.AssetPath.IsEmpty())
		{
			FinalResult.ResultData->SetStringField(TEXT("asset_path"), Request.AssetPath);
		}
		if (Request.Params.IsValid() && Request.Params->HasField(TEXT("graph_name")))
		{
			FinalResult.ResultData->SetStringField(TEXT("graph_name"), Request.Params->GetStringField(TEXT("graph_name")));
		}
	}

	return FinalResult;
}

// ============================================================================
// Verification
// ============================================================================

bool FOliveWritePipeline::VerifyBlueprintStructure(UBlueprint* Blueprint, TArray<FOliveIRMessage>& OutMessages) const
{
	if (!Blueprint)
	{
		return false;
	}

	bool bValid = true;

	// Basic structural checks
	if (Blueprint->SimpleConstructionScript == nullptr && Blueprint->BlueprintType == BPTYPE_Normal)
	{
		FOliveIRMessage Warning;
		Warning.Severity = EOliveIRSeverity::Warning;
		Warning.Code = TEXT("BP_MISSING_SCS");
		Warning.Message = TEXT("Blueprint missing SimpleConstructionScript");
		Warning.Suggestion = TEXT("This may be expected for certain Blueprint types");
		OutMessages.Add(Warning);
	}

	// Check for null graphs
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph == nullptr)
		{
			FOliveIRMessage Error;
			Error.Severity = EOliveIRSeverity::Error;
			Error.Code = TEXT("BP_NULL_GRAPH");
			Error.Message = TEXT("Blueprint contains null function graph");
			Error.Suggestion = TEXT("This indicates corruption - consider reverting the change");
			OutMessages.Add(Error);
			bValid = false;
		}
	}

	return bValid;
}

FOliveIRCompileResult FOliveWritePipeline::CompileAndGatherErrors(UBlueprint* Blueprint) const
{
	if (!Blueprint)
	{
		return FOliveIRCompileResult::Failure(TEXT("Null Blueprint"), TEXT(""));
	}

	// Delegate to the CompileManager which provides full structured error extraction
	// including node-level errors, graph names, pattern matching, and suggestions
	return FOliveCompileManager::Get().Compile(Blueprint);
}

// ============================================================================
// Orphaned Exec-Flow Detection
// ============================================================================

bool FOliveWritePipeline::IsGraphEditOperation(const FString& ToolName) const
{
	return ToolName.StartsWith(TEXT("blueprint.add_node"))
		|| ToolName.StartsWith(TEXT("blueprint.remove_node"))
		|| ToolName.StartsWith(TEXT("blueprint.connect_pins"))
		|| ToolName.StartsWith(TEXT("blueprint.disconnect_pins"));
}

int32 FOliveWritePipeline::DetectOrphanedExecFlows(const UEdGraph* Graph, TArray<FOliveIRMessage>& OutMessages) const
{
	if (!Graph)
	{
		return 0;
	}

	int32 OrphanCount = 0;

	for (const UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node)
		{
			continue;
		}

		// Skip comment nodes entirely
		if (Node->IsA<UEdGraphNode_Comment>())
		{
			continue;
		}

		// Skip reroute/knot nodes
		if (Node->IsA<UK2Node_Knot>())
		{
			continue;
		}

		// Skip FunctionResult / return nodes (they are intended terminal nodes)
		if (Node->IsA<UK2Node_FunctionResult>())
		{
			continue;
		}

		// Skip custom events -- they may be called via event dispatchers
		// and their exec output being disconnected is a valid authoring pattern
		if (Node->IsA<UK2Node_CustomEvent>())
		{
			continue;
		}

		// Cast to K2Node to check purity
		const UK2Node* K2Node = Cast<UK2Node>(Node);
		if (K2Node && K2Node->IsNodePure())
		{
			continue;
		}

		// Collect exec input and output pins
		TArray<const UEdGraphPin*> ExecInputs;
		TArray<const UEdGraphPin*> ExecOutputs;

		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->bHidden)
			{
				continue;
			}

			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				if (Pin->Direction == EGPD_Input)
				{
					ExecInputs.Add(Pin);
				}
				else if (Pin->Direction == EGPD_Output)
				{
					ExecOutputs.Add(Pin);
				}
			}
		}

		// No exec outputs means nothing to check
		if (ExecOutputs.Num() == 0)
		{
			continue;
		}

		// Determine if execution can reach this node:
		// - It is an event node (entry point, always reachable), OR
		// - It has at least one connected exec input pin
		bool bIsEventNode = Node->IsA<UK2Node_Event>();
		bool bHasConnectedExecInput = false;
		for (const UEdGraphPin* InputPin : ExecInputs)
		{
			if (InputPin->LinkedTo.Num() > 0)
			{
				bHasConnectedExecInput = true;
				break;
			}
		}

		if (!bIsEventNode && !bHasConnectedExecInput)
		{
			// Node is not reachable via exec flow -- skip it entirely.
			// Unreachable nodes are a different kind of issue, not orphaned exec flow.
			continue;
		}

		// For Sequence nodes: only flag if ALL exec outputs are disconnected.
		// Having some unused Sequence outputs is a normal workflow pattern.
		bool bIsSequenceNode = Node->IsA<UK2Node_ExecutionSequence>();
		if (bIsSequenceNode)
		{
			bool bAnyOutputConnected = false;
			for (const UEdGraphPin* OutputPin : ExecOutputs)
			{
				if (OutputPin->LinkedTo.Num() > 0)
				{
					bAnyOutputConnected = true;
					break;
				}
			}
			if (bAnyOutputConnected)
			{
				// At least one output is connected -- this is fine for Sequence nodes
				continue;
			}
		}

		// Check each exec output pin for orphaned flow
		for (const UEdGraphPin* OutputPin : ExecOutputs)
		{
			if (OutputPin->LinkedTo.Num() == 0)
			{
				// This exec output is disconnected -- orphaned exec flow
				FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
				FString PinName = OutputPin->GetDisplayName().IsEmpty()
					? OutputPin->PinName.ToString()
					: OutputPin->GetDisplayName().ToString();

				FOliveIRMessage Warning;
				Warning.Severity = EOliveIRSeverity::Warning;
				Warning.Code = TEXT("ORPHANED_EXEC_FLOW");
				Warning.Message = FString::Printf(
					TEXT("Node '%s' has disconnected exec output '%s' - execution flow will stop here"),
					*NodeTitle, *PinName);
				Warning.Suggestion = FString::Printf(
					TEXT("Connect the '%s' pin to continue execution, or remove the node if not needed"),
					*PinName);

				// Build structured context
				TSharedPtr<FJsonObject> Context = MakeShareable(new FJsonObject());
				Context->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
				Context->SetStringField(TEXT("node_title"), NodeTitle);
				Context->SetStringField(TEXT("pin_name"), OutputPin->PinName.ToString());

				TSharedPtr<FJsonObject> Position = MakeShareable(new FJsonObject());
				Position->SetNumberField(TEXT("x"), Node->NodePosX);
				Position->SetNumberField(TEXT("y"), Node->NodePosY);
				Context->SetObjectField(TEXT("node_position"), Position);

				Warning.Context = Context;

				OutMessages.Add(Warning);
				OrphanCount++;
			}
		}
	}

	return OrphanCount;
}

// ============================================================================
// Unwired Required Data Pin Detection
// ============================================================================

int32 FOliveWritePipeline::DetectUnwiredRequiredDataPins(
	const UEdGraph* Graph,
	const TSet<UEdGraphNode*>& NodesToCheck,
	TArray<FString>& OutMessages)
{
	if (!Graph)
	{
		return 0;
	}

	// Pin names to skip (self/context pins that are implicitly wired by the engine)
	static const TSet<FName> SkipPinNames = {
		UEdGraphSchema_K2::PN_Self,
		TEXT("WorldContextObject"),
		TEXT("__WorldContext"),
		TEXT("WorldContext")
	};

	int32 UnwiredCount = 0;

	// If NodesToCheck is non-empty, iterate only those. Otherwise iterate all graph nodes.
	TArray<UEdGraphNode*> ScopedNodes;
	if (NodesToCheck.Num() > 0)
	{
		ScopedNodes = NodesToCheck.Array();
	}
	const TArray<UEdGraphNode*>& NodesToIterate = NodesToCheck.Num() > 0 ? ScopedNodes : Graph->Nodes;

	for (UEdGraphNode* Node : NodesToIterate)
	{
		if (!Node)
		{
			continue;
		}

		// Skip comment and reroute nodes
		if (Node->IsA<UEdGraphNode_Comment>() || Node->IsA<UK2Node_Knot>())
		{
			continue;
		}

		const FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();

		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin)
			{
				continue;
			}

			// Only check input pins
			if (Pin->Direction != EGPD_Input)
			{
				continue;
			}

			// Skip exec pins
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				continue;
			}

			// Skip hidden pins
			if (Pin->bHidden)
			{
				continue;
			}

			// Skip non-connectable pins (self pins, etc.)
			if (Pin->bNotConnectable)
			{
				continue;
			}

			// Skip known self/context pin names
			if (SkipPinNames.Contains(Pin->PinName))
			{
				continue;
			}

			// Skip pins with a default value set
			if (!Pin->DefaultValue.IsEmpty())
			{
				continue;
			}

			// Skip pins with autogenerated default values
			if (!Pin->AutogeneratedDefaultValue.IsEmpty())
			{
				continue;
			}

			// Skip pins with a default object set
			if (Pin->DefaultObject != nullptr)
			{
				continue;
			}

			// Skip pins that have connections
			if (Pin->LinkedTo.Num() > 0)
			{
				continue;
			}

			// Skip sub-pins (split struct children) -- they have a parent pin
			if (Pin->ParentPin != nullptr)
			{
				continue;
			}

			// Skip orphaned pins (about to be removed on reconstruct)
			if (Pin->bOrphanedPin)
			{
				continue;
			}

			// This pin is unwired with no default -- report it
			const FString PinName = Pin->GetDisplayName().IsEmpty()
				? Pin->PinName.ToString()
				: Pin->GetDisplayName().ToString();

			const FString PinType = UEdGraphSchema_K2::TypeToText(Pin->PinType).ToString();

			OutMessages.Add(FString::Printf(
				TEXT("Node '%s': pin '%s' (%s) has no connection and no default"),
				*NodeTitle, *PinName, *PinType));
			UnwiredCount++;
		}
	}

	return UnwiredCount;
}

// ============================================================================
// Orphan Detection Baseline
// ============================================================================

void FOliveWritePipeline::SetOrphanBaseline(const FString& GraphPath, int32 CurrentOrphanCount)
{
	if (!OrphanBaselines.Contains(GraphPath))
	{
		OrphanBaselines.Add(GraphPath, CurrentOrphanCount);
		UE_LOG(LogOliveWritePipeline, Verbose,
			TEXT("Orphan baseline set for '%s': %d"), *GraphPath, CurrentOrphanCount);
	}
}

int32 FOliveWritePipeline::GetOrphanDelta(const FString& GraphPath, int32 CurrentOrphanCount) const
{
	if (const int32* Baseline = OrphanBaselines.Find(GraphPath))
	{
		return FMath::Max(0, CurrentOrphanCount - *Baseline);
	}
	// No baseline = first check for this graph = report full count
	return CurrentOrphanCount;
}

void FOliveWritePipeline::ClearOrphanBaselines()
{
	if (OrphanBaselines.Num() > 0)
	{
		UE_LOG(LogOliveWritePipeline, Verbose,
			TEXT("Clearing orphan baselines (%d graphs tracked)"), OrphanBaselines.Num());
	}
	OrphanBaselines.Reset();
	bRunActive = false;
}

