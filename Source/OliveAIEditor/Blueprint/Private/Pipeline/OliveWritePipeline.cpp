// Copyright Bode Software. All Rights Reserved.

#include "OliveWritePipeline.h"
#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/Guid.h"
#include "HAL/PlatformTime.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "JsonObjectConverter.h"
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

FOliveWriteResult FOliveWriteResult::ConfirmationNeeded(EOliveConfirmationRequirement Requirement, const FString& Plan)
{
	FOliveWriteResult WriteResult;
	WriteResult.bSuccess = false; // Not an error, but not completed
	WriteResult.CompletedStage = EOliveWriteStage::Confirm;
	WriteResult.ConfirmationRequired = Requirement;
	WriteResult.PlanDescription = Plan;

	// Build confirmation data
	WriteResult.ResultData = MakeShareable(new FJsonObject());
	WriteResult.ResultData->SetBoolField(TEXT("requires_confirmation"), true);
	WriteResult.ResultData->SetStringField(TEXT("plan"), Plan);

	FString RequirementStr = TEXT("unknown");
	switch (Requirement)
	{
	case EOliveConfirmationRequirement::None:
		RequirementStr = TEXT("none");
		break;
	case EOliveConfirmationRequirement::PlanConfirm:
		RequirementStr = TEXT("plan_confirm");
		break;
	case EOliveConfirmationRequirement::PreviewOnly:
		RequirementStr = TEXT("preview_only");
		break;
	}
	WriteResult.ResultData->SetStringField(TEXT("requirement"), RequirementStr);

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

	// Stage 2: Confirm (skip for MCP)
	if (!Request.bFromMCP)
	{
		TOptional<FOliveWriteResult> ConfirmResult = StageConfirm(Request);
		if (ConfirmResult.IsSet())
		{
			UE_LOG(LogOliveWritePipeline, Log, TEXT("Confirmation required for tool '%s'"), *Request.ToolName);
			ConfirmResult->ExecutionTimeMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;
			return ConfirmResult.GetValue();
		}
	}

	// Stage 3: Transact
	TUniquePtr<FOliveTransactionManager::FScopedOliveTransaction> Transaction = StageTransact(Request, Request.TargetAsset);
	if (!Transaction.IsValid())
	{
		UE_LOG(LogOliveWritePipeline, Error, TEXT("Failed to open transaction for tool '%s'"), *Request.ToolName);
		return FOliveWriteResult::ExecutionError(
			TEXT("PIPELINE_TRANSACTION_FAILED"),
			TEXT("Failed to open transaction"),
			TEXT("Ensure the asset is not locked or in use by another editor"));
	}

	// Stage 4: Execute
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
		ExecuteResult.ExecutionTimeMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;
		return ExecuteResult;
	}

	// Stage 5: Verify (optional based on request settings)
	FOliveWriteResult VerifyResult = ExecuteResult;
	if (!Request.bSkipVerification)
	{
		VerifyResult = StageVerify(Request, EffectiveTargetAsset, ExecuteResult);
		// Note: Verification warnings don't cancel the transaction
		// Only execution errors cancel
	}

	// Transaction commits automatically when it goes out of scope here

	// Stage 6: Report
	const double TotalTimeMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;
	FOliveWriteResult FinalResult = StageReport(Request, VerifyResult, TotalTimeMs);

	UE_LOG(LogOliveWritePipeline, Log, TEXT("Pipeline completed for tool '%s' in %.2fms (success: %d)"),
		*Request.ToolName, TotalTimeMs, FinalResult.bSuccess);

	return FinalResult;
}

FOliveWriteResult FOliveWritePipeline::ExecuteConfirmed(
	const FOliveWriteRequest& Request,
	const FString& ConfirmationToken,
	FOliveWriteExecutor Executor)
{
	FScopeLock Lock(&ConfirmationLock);

	// Validate token
	if (!PendingConfirmations.Contains(ConfirmationToken))
	{
		UE_LOG(LogOliveWritePipeline, Warning, TEXT("Invalid confirmation token: %s"), *ConfirmationToken);
		return FOliveWriteResult::ExecutionError(
			TEXT("PIPELINE_INVALID_TOKEN"),
			TEXT("Invalid or expired confirmation token"),
			TEXT("Request confirmation again with the original operation"));
	}

	// Retrieve and remove the pending request
	FOliveWriteRequest ConfirmedRequest = PendingConfirmations[ConfirmationToken];
	PendingConfirmations.Remove(ConfirmationToken);

	UE_LOG(LogOliveWritePipeline, Log, TEXT("Executing confirmed request for tool '%s'"), *ConfirmedRequest.ToolName);

	// Create a modified request that skips confirmation
	FOliveWriteRequest SkipConfirmRequest = ConfirmedRequest;
	SkipConfirmRequest.bFromMCP = true; // Force skip confirmation stage

	// Execute normally (will skip confirmation stage)
	return Execute(SkipConfirmRequest, Executor);
}

FOliveWriteResult FOliveWritePipeline::GeneratePreview(const FOliveWriteRequest& Request)
{
	UE_LOG(LogOliveWritePipeline, Log, TEXT("Generating Tier 3 preview for tool '%s'"), *Request.ToolName);

	// Stage 1: Validate first (same as normal pipeline)
	FOliveWriteResult ValidateResult = StageValidate(Request);
	if (!ValidateResult.bSuccess)
	{
		return ValidateResult;
	}

	// Build preview components
	TSharedPtr<FJsonObject> PreviewPayload = BuildPreviewPayload(Request);
	TSharedPtr<FJsonObject> ImpactAnalysis = BuildImpactAnalysis(Request);
	TArray<TSharedPtr<FJsonValue>> StructuredChanges = BuildStructuredChanges(Request);

	// Generate confirmation token
	FString ConfirmationToken = GenerateConfirmationToken();

	// Store pending confirmation
	{
		FScopeLock Lock(&ConfirmationLock);
		PendingConfirmations.Add(ConfirmationToken, Request);
	}

	// Build result
	FOliveWriteResult PreviewResult;
	PreviewResult.bSuccess = false; // Not executed yet
	PreviewResult.CompletedStage = EOliveWriteStage::Confirm;
	PreviewResult.ConfirmationRequired = EOliveConfirmationRequirement::PreviewOnly;

	TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
	ResultData->SetBoolField(TEXT("requires_confirmation"), true);
	ResultData->SetStringField(TEXT("requirement"), TEXT("preview_only"));
	ResultData->SetStringField(TEXT("confirmation_token"), ConfirmationToken);

	// Plan: human-readable summary
	FString PlanDescription = GeneratePlanDescription(Request);
	ResultData->SetStringField(TEXT("plan"), PlanDescription);
	PreviewResult.PlanDescription = PlanDescription;

	// Preview: structured preview data
	if (PreviewPayload.IsValid())
	{
		ResultData->SetObjectField(TEXT("preview"), PreviewPayload);
	}

	// Impact: dependency/referencer analysis
	if (ImpactAnalysis.IsValid())
	{
		ResultData->SetObjectField(TEXT("impact"), ImpactAnalysis);
	}

	// Changes: structured change descriptors
	if (StructuredChanges.Num() > 0)
	{
		ResultData->SetArrayField(TEXT("changes"), StructuredChanges);
	}

	PreviewResult.ResultData = ResultData;
	PreviewResult.PreviewData = ResultData;

	UE_LOG(LogOliveWritePipeline, Log, TEXT("Preview generated for tool '%s' with token '%s'"),
		*Request.ToolName, *ConfirmationToken);

	return PreviewResult;
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

TOptional<FOliveWriteResult> FOliveWritePipeline::StageConfirm(const FOliveWriteRequest& Request)
{
	UE_LOG(LogOliveWritePipeline, Verbose, TEXT("Stage 2: Confirm - tool '%s'"), *Request.ToolName);

	// Determine tier for this operation
	EOliveConfirmationTier Tier = GetOperationTier(Request.OperationCategory);
	EOliveConfirmationRequirement Requirement = TierToRequirement(Tier);

	// Tier 1 auto-executes
	if (Requirement == EOliveConfirmationRequirement::None)
	{
		UE_LOG(LogOliveWritePipeline, Verbose, TEXT("Tier 1 operation, auto-executing"));
		return TOptional<FOliveWriteResult>(); // Continue to next stage
	}

	// Tier 2: Plan + Confirm
	if (Requirement == EOliveConfirmationRequirement::PlanConfirm)
	{
		FString PlanDescription = GeneratePlanDescription(Request);
		FString ConfirmationToken = GenerateConfirmationToken();

		// Store pending confirmation
		{
			FScopeLock Lock(&ConfirmationLock);
			PendingConfirmations.Add(ConfirmationToken, Request);
		}

		FOliveWriteResult ConfirmResult = FOliveWriteResult::ConfirmationNeeded(Requirement, PlanDescription);
		ConfirmResult.ResultData->SetStringField(TEXT("confirmation_token"), ConfirmationToken);

		return ConfirmResult;
	}

	// Tier 3: Full preview
	if (Requirement == EOliveConfirmationRequirement::PreviewOnly)
	{
		FOliveWriteResult PreviewResult = GeneratePreview(Request);
		return PreviewResult;
	}

	return TOptional<FOliveWriteResult>(); // Should not reach here
}

TUniquePtr<FOliveTransactionManager::FScopedOliveTransaction> FOliveWritePipeline::StageTransact(
	const FOliveWriteRequest& Request,
	UObject* TargetAsset)
{
	UE_LOG(LogOliveWritePipeline, Verbose, TEXT("Stage 3: Transact - tool '%s'"), *Request.ToolName);

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
	UE_LOG(LogOliveWritePipeline, Verbose, TEXT("Stage 4: Execute - tool '%s'"), *Request.ToolName);

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
	UE_LOG(LogOliveWritePipeline, Verbose, TEXT("Stage 5: Verify - tool '%s'"), *Request.ToolName);

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
						int32 OrphanCount = DetectOrphanedExecFlows(Graph, StructuralMessages);
						if (OrphanCount > 0)
						{
							UE_LOG(LogOliveWritePipeline, Log,
								TEXT("Detected %d orphaned exec flow(s) in graph '%s' of Blueprint '%s'"),
								OrphanCount, *GraphName, *Blueprint->GetName());
						}
						break;
					}
				}
				// Also check function graphs
				for (UEdGraph* Graph : Blueprint->FunctionGraphs)
				{
					if (Graph && Graph->GetName() == GraphName)
					{
						int32 OrphanCount = DetectOrphanedExecFlows(Graph, StructuralMessages);
						if (OrphanCount > 0)
						{
							UE_LOG(LogOliveWritePipeline, Log,
								TEXT("Detected %d orphaned exec flow(s) in function graph '%s' of Blueprint '%s'"),
								OrphanCount, *GraphName, *Blueprint->GetName());
						}
						break;
					}
				}
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
	UE_LOG(LogOliveWritePipeline, Verbose, TEXT("Stage 6: Report - tool '%s'"), *Request.ToolName);

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
// Tier Routing
// ============================================================================

EOliveConfirmationTier FOliveWritePipeline::GetOperationTier(const FString& OperationCategory) const
{
	const UOliveAISettings* Settings = UOliveAISettings::Get();
	if (!Settings)
	{
		UE_LOG(LogOliveWritePipeline, Warning, TEXT("Settings not available, using default Tier 2"));
		return EOliveConfirmationTier::Tier2_PlanConfirm;
	}
	return Settings->GetEffectiveTier(OperationCategory);
}

EOliveConfirmationRequirement FOliveWritePipeline::TierToRequirement(EOliveConfirmationTier Tier) const
{
	switch (Tier)
	{
	case EOliveConfirmationTier::Tier1_AutoExecute:
		return EOliveConfirmationRequirement::None;
	case EOliveConfirmationTier::Tier2_PlanConfirm:
		return EOliveConfirmationRequirement::PlanConfirm;
	case EOliveConfirmationTier::Tier3_Preview:
		return EOliveConfirmationRequirement::PreviewOnly;
	default:
		return EOliveConfirmationRequirement::PlanConfirm;
	}
}

FString FOliveWritePipeline::GeneratePlanDescription(const FOliveWriteRequest& Request) const
{
	// Generate a human-readable description of what will be done
	FString Plan = FString::Printf(TEXT("Operation: %s\n"), *Request.OperationDescription.ToString());
	Plan += FString::Printf(TEXT("Tool: %s\n"), *Request.ToolName);

	if (!Request.AssetPath.IsEmpty())
	{
		Plan += FString::Printf(TEXT("Target: %s\n"), *Request.AssetPath);
	}

	if (Request.Params.IsValid())
	{
		// Add key parameters to plan
		FString ParamsStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ParamsStr);
		FJsonSerializer::Serialize(Request.Params.ToSharedRef(), Writer);

		Plan += FString::Printf(TEXT("\nParameters:\n%s"), *ParamsStr);
	}

	return Plan;
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
// Preview Generation Helpers
// ============================================================================

TSharedPtr<FJsonObject> FOliveWritePipeline::BuildPreviewPayload(const FOliveWriteRequest& Request) const
{
	TSharedPtr<FJsonObject> Preview = MakeShareable(new FJsonObject());

	Preview->SetStringField(TEXT("tool"), Request.ToolName);
	Preview->SetStringField(TEXT("operation"), Request.OperationDescription.ToString());

	if (!Request.AssetPath.IsEmpty())
	{
		Preview->SetStringField(TEXT("target_asset"), Request.AssetPath);

		// For Blueprint targets, add graph info
		if (UBlueprint* Blueprint = Cast<UBlueprint>(Request.TargetAsset))
		{
			Preview->SetStringField(TEXT("asset_type"), TEXT("Blueprint"));
			Preview->SetStringField(TEXT("blueprint_type"),
				Blueprint->BlueprintType == BPTYPE_Normal ? TEXT("Normal") :
				Blueprint->BlueprintType == BPTYPE_FunctionLibrary ? TEXT("FunctionLibrary") :
				Blueprint->BlueprintType == BPTYPE_Interface ? TEXT("Interface") :
				Blueprint->BlueprintType == BPTYPE_MacroLibrary ? TEXT("MacroLibrary") : TEXT("Other"));

			if (Blueprint->ParentClass)
			{
				Preview->SetStringField(TEXT("parent_class"), Blueprint->ParentClass->GetName());
			}

			// List graphs
			TArray<TSharedPtr<FJsonValue>> GraphArray;
			for (UEdGraph* Graph : Blueprint->FunctionGraphs)
			{
				if (Graph)
				{
					TSharedPtr<FJsonObject> GraphObj = MakeShareable(new FJsonObject());
					GraphObj->SetStringField(TEXT("name"), Graph->GetName());
					GraphObj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
					GraphArray.Add(MakeShareable(new FJsonValueObject(GraphObj)));
				}
			}
			if (GraphArray.Num() > 0)
			{
				Preview->SetArrayField(TEXT("existing_graphs"), GraphArray);
			}

			// Count existing variables
			Preview->SetNumberField(TEXT("existing_variable_count"), Blueprint->NewVariables.Num());
		}
	}

	// Add key parameters summary
	if (Request.Params.IsValid())
	{
		TSharedPtr<FJsonObject> ParamSummary = MakeShareable(new FJsonObject());
		TArray<FString> Keys;
		Request.Params->Values.GetKeys(Keys);

		for (const FString& Key : Keys)
		{
			// Include string and boolean params in summary, skip large objects
			const TSharedPtr<FJsonValue>& Value = Request.Params->Values[Key];
			if (Value->Type == EJson::String || Value->Type == EJson::Boolean || Value->Type == EJson::Number)
			{
				ParamSummary->SetField(Key, Value);
			}
		}
		Preview->SetObjectField(TEXT("parameters"), ParamSummary);
	}

	return Preview;
}

TSharedPtr<FJsonObject> FOliveWritePipeline::BuildImpactAnalysis(const FOliveWriteRequest& Request) const
{
	TSharedPtr<FJsonObject> Impact = MakeShareable(new FJsonObject());

	if (Request.AssetPath.IsEmpty())
	{
		Impact->SetNumberField(TEXT("affected_count"), 0);
		return Impact;
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	// Get dependencies
	TArray<FAssetIdentifier> Dependencies;
	AssetRegistry.GetDependencies(FAssetIdentifier(FName(*Request.AssetPath)), Dependencies);

	TArray<TSharedPtr<FJsonValue>> DepsArray;
	for (const FAssetIdentifier& Dep : Dependencies)
	{
		DepsArray.Add(MakeShareable(new FJsonValueString(Dep.PackageName.ToString())));
	}
	Impact->SetArrayField(TEXT("dependencies"), DepsArray);

	// Get referencers
	TArray<FAssetIdentifier> Referencers;
	AssetRegistry.GetReferencers(FAssetIdentifier(FName(*Request.AssetPath)), Referencers);

	TArray<TSharedPtr<FJsonValue>> RefsArray;
	for (const FAssetIdentifier& Ref : Referencers)
	{
		RefsArray.Add(MakeShareable(new FJsonValueString(Ref.PackageName.ToString())));
	}
	Impact->SetArrayField(TEXT("referencers"), RefsArray);

	Impact->SetNumberField(TEXT("dependency_count"), DepsArray.Num());
	Impact->SetNumberField(TEXT("referencer_count"), RefsArray.Num());
	Impact->SetNumberField(TEXT("affected_count"), DepsArray.Num() + RefsArray.Num());

	return Impact;
}

TArray<TSharedPtr<FJsonValue>> FOliveWritePipeline::BuildStructuredChanges(const FOliveWriteRequest& Request) const
{
	TArray<TSharedPtr<FJsonValue>> Changes;

	if (!Request.Params.IsValid())
	{
		return Changes;
	}

	// Parse changes based on tool name category
	const FString& ToolName = Request.ToolName;

	if (ToolName.Contains(TEXT("add_variable")))
	{
		TSharedPtr<FJsonObject> Change = MakeShareable(new FJsonObject());
		Change->SetStringField(TEXT("type"), TEXT("add_variable"));
		Change->SetStringField(TEXT("name"), Request.Params->GetStringField(TEXT("name")));
		FString VarType;
		Request.Params->TryGetStringField(TEXT("type"), VarType);
		Change->SetStringField(TEXT("variable_type"), VarType);
		Changes.Add(MakeShareable(new FJsonValueObject(Change)));
	}
	else if (ToolName.Contains(TEXT("add_component")))
	{
		TSharedPtr<FJsonObject> Change = MakeShareable(new FJsonObject());
		Change->SetStringField(TEXT("type"), TEXT("add_component"));
		Change->SetStringField(TEXT("component_class"), Request.Params->GetStringField(TEXT("component_class")));
		FString Name;
		Request.Params->TryGetStringField(TEXT("name"), Name);
		Change->SetStringField(TEXT("name"), Name);
		Changes.Add(MakeShareable(new FJsonValueObject(Change)));
	}
	else if (ToolName.Contains(TEXT("add_function")) || ToolName.Contains(TEXT("create_function")))
	{
		TSharedPtr<FJsonObject> Change = MakeShareable(new FJsonObject());
		Change->SetStringField(TEXT("type"), TEXT("add_function"));
		Change->SetStringField(TEXT("name"), Request.Params->GetStringField(TEXT("name")));
		Changes.Add(MakeShareable(new FJsonValueObject(Change)));
	}
	else if (ToolName.Contains(TEXT("add_node")) || ToolName.Contains(TEXT("create_node")))
	{
		TSharedPtr<FJsonObject> Change = MakeShareable(new FJsonObject());
		Change->SetStringField(TEXT("type"), TEXT("add_node"));
		FString NodeClass;
		Request.Params->TryGetStringField(TEXT("node_class"), NodeClass);
		Change->SetStringField(TEXT("node_class"), NodeClass);
		FString Graph;
		Request.Params->TryGetStringField(TEXT("graph"), Graph);
		Change->SetStringField(TEXT("graph"), Graph);
		Changes.Add(MakeShareable(new FJsonValueObject(Change)));
	}
	else if (ToolName.Contains(TEXT("remove_")))
	{
		TSharedPtr<FJsonObject> Change = MakeShareable(new FJsonObject());
		Change->SetStringField(TEXT("type"), TEXT("remove"));
		FString TargetName;
		Request.Params->TryGetStringField(TEXT("name"), TargetName);
		if (TargetName.IsEmpty())
		{
			Request.Params->TryGetStringField(TEXT("node_id"), TargetName);
		}
		Change->SetStringField(TEXT("target"), TargetName);
		Changes.Add(MakeShareable(new FJsonValueObject(Change)));
	}
	else if (ToolName.Contains(TEXT("set_parent")) || ToolName.Contains(TEXT("reparent")))
	{
		TSharedPtr<FJsonObject> Change = MakeShareable(new FJsonObject());
		Change->SetStringField(TEXT("type"), TEXT("reparent"));
		FString NewParent;
		Request.Params->TryGetStringField(TEXT("new_parent"), NewParent);
		if (NewParent.IsEmpty())
		{
			Request.Params->TryGetStringField(TEXT("parent_class"), NewParent);
		}
		Change->SetStringField(TEXT("new_parent"), NewParent);
		Changes.Add(MakeShareable(new FJsonValueObject(Change)));
	}
	else if (ToolName.Contains(TEXT("connect_")) || ToolName.Contains(TEXT("wire")))
	{
		TSharedPtr<FJsonObject> Change = MakeShareable(new FJsonObject());
		Change->SetStringField(TEXT("type"), TEXT("connect_pins"));
		FString SourceNode, SourcePin, TargetNode, TargetPin;
		Request.Params->TryGetStringField(TEXT("source_node"), SourceNode);
		Request.Params->TryGetStringField(TEXT("source_pin"), SourcePin);
		Request.Params->TryGetStringField(TEXT("target_node"), TargetNode);
		Request.Params->TryGetStringField(TEXT("target_pin"), TargetPin);
		Change->SetStringField(TEXT("source"), SourceNode + TEXT(":") + SourcePin);
		Change->SetStringField(TEXT("target"), TargetNode + TEXT(":") + TargetPin);
		Changes.Add(MakeShareable(new FJsonValueObject(Change)));
	}
	else if (ToolName.Contains(TEXT("modify_")) || ToolName.Contains(TEXT("set_")))
	{
		TSharedPtr<FJsonObject> Change = MakeShareable(new FJsonObject());
		Change->SetStringField(TEXT("type"), TEXT("modify"));
		Change->SetStringField(TEXT("tool"), ToolName);
		// Include all string params as context
		TArray<FString> Keys;
		Request.Params->Values.GetKeys(Keys);
		for (const FString& Key : Keys)
		{
			FString Val;
			if (Request.Params->TryGetStringField(Key, Val) && Key != TEXT("path") && Key != TEXT("blueprint") && Key != TEXT("asset"))
			{
				Change->SetStringField(Key, Val);
			}
		}
		Changes.Add(MakeShareable(new FJsonValueObject(Change)));
	}
	else
	{
		// Generic change descriptor
		TSharedPtr<FJsonObject> Change = MakeShareable(new FJsonObject());
		Change->SetStringField(TEXT("type"), TEXT("operation"));
		Change->SetStringField(TEXT("tool"), ToolName);
		Changes.Add(MakeShareable(new FJsonValueObject(Change)));
	}

	return Changes;
}

// ============================================================================
// State Management
// ============================================================================

FString FOliveWritePipeline::GenerateConfirmationToken()
{
	FGuid Guid = FGuid::NewGuid();
	return Guid.ToString(EGuidFormats::DigitsWithHyphensLower);
}
