// Copyright Bode Software. All Rights Reserved.

#include "OliveWritePipeline.h"
#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "HAL/PlatformTime.h"
#include "Compile/OliveCompileManager.h"
#include "Services/OliveAssetResolver.h"
#include "ScopedTransaction.h"

DEFINE_LOG_CATEGORY(LogOliveWritePipeline);

// ============================================================================
// FOliveWriteResult factory methods
// ============================================================================

FOliveToolResult FOliveWriteResult::ToToolResult() const
{
	FOliveToolResult ToolResult;
	ToolResult.bSuccess = bSuccess;
	ToolResult.Data = ResultData;
	ToolResult.Messages = ValidationMessages;
	ToolResult.ExecutionTimeMs = ExecutionTimeMs;
	ToolResult.NextStepGuidance = NextStepGuidance;

	if (!bSuccess && ToolResult.Messages.Num() == 0 && ResultData.IsValid())
	{
		FString Code, Message, Suggestion;
		ResultData->TryGetStringField(TEXT("error_code"), Code);
		ResultData->TryGetStringField(TEXT("error_message"), Message);
		ResultData->TryGetStringField(TEXT("suggestion"), Suggestion);
		if (!Message.IsEmpty())
		{
			FOliveIRMessage Synth;
			Synth.Severity = EOliveIRSeverity::Error;
			Synth.Code = Code.IsEmpty() ? TEXT("PIPELINE_ERROR") : Code;
			Synth.Message = Message;
			Synth.Suggestion = Suggestion;
			ToolResult.Messages.Add(MoveTemp(Synth));
		}
	}

	return ToolResult;
}

FOliveWriteResult FOliveWriteResult::ValidationError(const FOliveValidationResult& Result)
{
	FOliveWriteResult Out;
	Out.bSuccess = false;
	Out.CompletedStage = EOliveWriteStage::Validate;
	Out.ValidationMessages = Result.Messages;

	Out.ResultData = MakeShared<FJsonObject>();
	Out.ResultData->SetBoolField(TEXT("success"), false);

	TArray<TSharedPtr<FJsonValue>> MessageArray;
	for (const FOliveIRMessage& Msg : Result.Messages)
	{
		MessageArray.Add(MakeShared<FJsonValueObject>(Msg.ToJson()));
	}
	Out.ResultData->SetArrayField(TEXT("validation_messages"), MessageArray);

	return Out;
}

FOliveWriteResult FOliveWriteResult::ExecutionError(const FString& Code, const FString& Message, const FString& Suggestion)
{
	FOliveWriteResult Out;
	Out.bSuccess = false;
	Out.CompletedStage = EOliveWriteStage::Execute;

	FOliveIRMessage Msg;
	Msg.Severity = EOliveIRSeverity::Error;
	Msg.Code = Code;
	Msg.Message = Message;
	Msg.Suggestion = Suggestion;
	Out.ValidationMessages.Add(Msg);

	Out.ResultData = MakeShared<FJsonObject>();
	Out.ResultData->SetBoolField(TEXT("success"), false);
	Out.ResultData->SetStringField(TEXT("error_code"), Code);
	Out.ResultData->SetStringField(TEXT("error_message"), Message);
	if (!Suggestion.IsEmpty())
	{
		Out.ResultData->SetStringField(TEXT("suggestion"), Suggestion);
	}

	return Out;
}

FOliveWriteResult FOliveWriteResult::Success(const TSharedPtr<FJsonObject>& Data)
{
	FOliveWriteResult Out;
	Out.bSuccess = true;
	Out.CompletedStage = EOliveWriteStage::Report;
	Out.ResultData = Data.IsValid() ? Data : MakeShared<FJsonObject>();
	Out.ResultData->SetBoolField(TEXT("success"), true);
	return Out;
}

// ============================================================================
// FOliveWritePipeline
// ============================================================================

FOliveWritePipeline& FOliveWritePipeline::Get()
{
	static FOliveWritePipeline Instance;
	return Instance;
}

FOliveWriteResult FOliveWritePipeline::Execute(const FOliveWriteRequest& Request, FOliveWriteExecutor Executor)
{
	const double StartSeconds = FPlatformTime::Seconds();

	// Resolve target asset if caller didn't pre-load it.
	UObject* Target = Request.TargetAsset;
	if (!Target && !Request.AssetPath.IsEmpty())
	{
		FOliveAssetResolveInfo ResolveInfo = FOliveAssetResolver::Get().ResolveByPath(Request.AssetPath);
		if (ResolveInfo.IsSuccess())
		{
			Target = ResolveInfo.Asset;
		}
	}

	// Open a scoped transaction so UE's undo/redo system captures the write.
	const FText TxDesc = Request.OperationDescription.IsEmpty()
		? FText::FromString(Request.ToolName)
		: Request.OperationDescription;
	FScopedTransaction Tx(TxDesc);

	if (Target)
	{
		Target->Modify();
	}

	// Run the handler-provided executor lambda.
	FOliveWriteResult Result = Executor.IsBound()
		? Executor.Execute(Request, Target)
		: FOliveWriteResult::ExecutionError(
			TEXT("PIPELINE_NO_EXECUTOR"),
			TEXT("Write request had no executor bound"),
			TEXT("This is an internal handler bug — bind an FOliveWriteExecutor before calling Execute()."));

	// Auto-compile if requested and we have a Blueprint target.
	if (Result.bSuccess && Request.bAutoCompile && !Request.bSkipVerification)
	{
		if (UBlueprint* BP = Cast<UBlueprint>(Target))
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
			FOliveIRCompileResult CR = FOliveCompileManager::Get().Compile(BP);
			Result.CompileResult = CR;
			if (CR.HasErrors())
			{
				Result.bSuccess = false;
				if (Result.ResultData.IsValid())
				{
					Result.ResultData->SetBoolField(TEXT("success"), false);
				}
			}
		}
	}

	Result.ExecutionTimeMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	return Result;
}
