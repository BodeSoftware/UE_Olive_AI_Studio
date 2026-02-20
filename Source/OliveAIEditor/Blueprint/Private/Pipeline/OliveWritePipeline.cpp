// Copyright Bode Software. All Rights Reserved.

#include "OliveWritePipeline.h"
#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/Guid.h"
#include "HAL/PlatformTime.h"
#include "JsonObjectConverter.h"

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
	FOliveWriteResult ExecuteResult = StageExecute(Request, Request.TargetAsset, Executor);
	if (!ExecuteResult.bSuccess)
	{
		UE_LOG(LogOliveWritePipeline, Error, TEXT("Execution failed for tool '%s'"), *Request.ToolName);
		Transaction->Cancel(); // Roll back transaction
		ExecuteResult.ExecutionTimeMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;
		return ExecuteResult;
	}

	// Stage 5: Verify (optional based on request settings)
	FOliveWriteResult VerifyResult = ExecuteResult;
	if (!Request.bSkipVerification)
	{
		VerifyResult = StageVerify(Request, Request.TargetAsset, ExecuteResult);
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
	// TODO: Implement preview generation for Tier 3 operations
	// This requires dry-run execution or simulation
	UE_LOG(LogOliveWritePipeline, Warning, TEXT("Preview generation not yet implemented for tool '%s'"), *Request.ToolName);

	FOliveWriteResult PreviewResult;
	PreviewResult.bSuccess = true;
	PreviewResult.CompletedStage = EOliveWriteStage::Confirm;
	PreviewResult.PreviewData = MakeShareable(new FJsonObject());
	PreviewResult.PreviewData->SetStringField(TEXT("status"), TEXT("preview_not_implemented"));
	PreviewResult.PreviewData->SetStringField(TEXT("message"), TEXT("Preview generation will be implemented in a future update"));

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

	// Tier 2/3 require confirmation
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
	FOliveWriteExecutor& Executor)
{
	UE_LOG(LogOliveWritePipeline, Verbose, TEXT("Stage 4: Execute - tool '%s'"), *Request.ToolName);

	if (!Executor.IsBound())
	{
		UE_LOG(LogOliveWritePipeline, Error, TEXT("Executor delegate not bound for tool '%s'"), *Request.ToolName);
		return FOliveWriteResult::ExecutionError(
			TEXT("PIPELINE_NO_EXECUTOR"),
			TEXT("Internal error: No executor provided"),
			TEXT("This is a plugin bug - please report it"));
	}

	// Execute the mutation via the provided delegate
	FOliveWriteResult ExecuteResult = Executor.Execute(Request, TargetAsset);
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

	return FinalResult;
}

// ============================================================================
// Tier Routing
// ============================================================================

EOliveConfirmationTier FOliveWritePipeline::GetOperationTier(const FString& OperationCategory) const
{
	UOliveAISettings* Settings = UOliveAISettings::Get();
	if (!Settings)
	{
		UE_LOG(LogOliveWritePipeline, Warning, TEXT("Settings not available, using default Tier 2"));
		return EOliveConfirmationTier::Tier2_PlanConfirm; // Safe default
	}

	if (OperationCategory == TEXT("variable"))
	{
		return Settings->VariableOperationsTier;
	}
	else if (OperationCategory == TEXT("component"))
	{
		return Settings->ComponentOperationsTier;
	}
	else if (OperationCategory == TEXT("function_creation"))
	{
		return Settings->FunctionCreationTier;
	}
	else if (OperationCategory == TEXT("graph_editing"))
	{
		return Settings->GraphEditingTier;
	}
	else if (OperationCategory == TEXT("refactoring"))
	{
		return Settings->RefactoringTier;
	}
	else if (OperationCategory == TEXT("delete"))
	{
		return Settings->DeleteOperationsTier;
	}

	UE_LOG(LogOliveWritePipeline, Verbose, TEXT("Unknown operation category '%s', using default Tier 2"), *OperationCategory);
	return EOliveConfirmationTier::Tier2_PlanConfirm;
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

	const double StartTime = FPlatformTime::Seconds();

	// Mark Blueprint as modified to ensure compilation happens
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	// Compile the Blueprint
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None);

	const double CompileTime = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	// Gather compilation results
	FOliveIRCompileResult Result;
	Result.CompileTimeMs = CompileTime;

	// Check compile status
	if (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings)
	{
		Result.bSuccess = true;
		UE_LOG(LogOliveWritePipeline, Verbose, TEXT("Blueprint compiled successfully in %.2fms"), CompileTime);
	}
	else
	{
		Result.bSuccess = false;
		UE_LOG(LogOliveWritePipeline, Warning, TEXT("Blueprint compilation failed"));

		// Add generic error if status indicates failure
		FOliveIRCompileError GenericError;
		GenericError.Message = TEXT("Blueprint compilation failed");
		GenericError.Severity = EOliveIRCompileErrorSeverity::Error;
		GenericError.Suggestion = TEXT("Check the Blueprint for errors in the graph");
		Result.Errors.Add(GenericError);
	}

	// TODO: Extract specific errors from Blueprint's error log
	// This requires parsing Blueprint->CompileLog or similar

	return Result;
}

// ============================================================================
// State Management
// ============================================================================

FString FOliveWritePipeline::GenerateConfirmationToken()
{
	FGuid Guid = FGuid::NewGuid();
	return Guid.ToString(EGuidFormats::DigitsWithHyphensLower);
}
