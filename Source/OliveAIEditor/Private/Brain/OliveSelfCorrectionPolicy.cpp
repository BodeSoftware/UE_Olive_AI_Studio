// Copyright Bode Software. All Rights Reserved.

#include "Brain/OliveSelfCorrectionPolicy.h"
#include "Brain/OliveRetryPolicy.h"
#include "OliveAIEditorModule.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"

FOliveSelfCorrectionPolicy::FOliveSelfCorrectionPolicy()
{
}

FOliveCorrectionDecision FOliveSelfCorrectionPolicy::Evaluate(
	const FString& ToolName,
	const FString& ResultJson,
	FOliveLoopDetector& LoopDetector,
	const FOliveRetryPolicy& Policy)
{
	FOliveCorrectionDecision Decision;

	// Check for compile failure
	FString CompileErrors, AssetPath;
	if (HasCompileFailure(ResultJson, CompileErrors, AssetPath))
	{
		CurrentAttemptCount++;
		Decision.AttemptNumber = CurrentAttemptCount;
		Decision.MaxAttempts = Policy.MaxRetriesPerError;

		// Build error signature and record
		const FString Signature = FOliveLoopDetector::BuildCompileErrorSignature(AssetPath, CompileErrors);
		LoopDetector.RecordAttempt(Signature, FString::Printf(TEXT("compile_retry_%d"), CurrentAttemptCount));

		// Check for loops
		if (LoopDetector.IsLooping(Signature, Policy) || LoopDetector.IsOscillating() || LoopDetector.IsBudgetExhausted(Policy))
		{
			Decision.Action = EOliveCorrectionAction::StopWorker;
			Decision.LoopReport = LoopDetector.BuildLoopReport();
			UE_LOG(LogOliveAI, Warning, TEXT("SelfCorrection: Loop detected for compile failure on '%s'. Stopping."), *AssetPath);
			return Decision;
		}

		// Feed back errors for retry
		Decision.Action = EOliveCorrectionAction::FeedBackErrors;
		Decision.EnrichedMessage = BuildCompileErrorMessage(ToolName, CompileErrors, CurrentAttemptCount, Policy.MaxRetriesPerError);
		UE_LOG(LogOliveAI, Log, TEXT("SelfCorrection: Compile failure on '%s', attempt %d/%d"),
			*AssetPath, CurrentAttemptCount, Policy.MaxRetriesPerError);
		return Decision;
	}

	// Check for tool failure
	FString ErrorCode, ErrorMessage;
	if (HasToolFailure(ResultJson, ErrorCode, ErrorMessage))
	{
		CurrentAttemptCount++;
		Decision.AttemptNumber = CurrentAttemptCount;
		Decision.MaxAttempts = Policy.MaxRetriesPerError;

		// Build error signature and record
		const FString Signature = FOliveLoopDetector::BuildToolErrorSignature(ToolName, ErrorCode, TEXT(""));
		LoopDetector.RecordAttempt(Signature, FString::Printf(TEXT("tool_retry_%d"), CurrentAttemptCount));

		// Check for loops
		if (LoopDetector.IsLooping(Signature, Policy) || LoopDetector.IsOscillating() || LoopDetector.IsBudgetExhausted(Policy))
		{
			Decision.Action = EOliveCorrectionAction::StopWorker;
			Decision.LoopReport = LoopDetector.BuildLoopReport();
			UE_LOG(LogOliveAI, Warning, TEXT("SelfCorrection: Loop detected for tool '%s' error '%s'. Stopping."), *ToolName, *ErrorCode);
			return Decision;
		}

		// Feed back errors for retry
		Decision.Action = EOliveCorrectionAction::FeedBackErrors;
		Decision.EnrichedMessage = BuildToolErrorMessage(ToolName, ErrorCode, ErrorMessage, CurrentAttemptCount, Policy.MaxRetriesPerError);
		UE_LOG(LogOliveAI, Log, TEXT("SelfCorrection: Tool failure '%s' error '%s', attempt %d/%d"),
			*ToolName, *ErrorCode, CurrentAttemptCount, Policy.MaxRetriesPerError);
		return Decision;
	}

	// Success — tool worked fine
	Decision.Action = EOliveCorrectionAction::Continue;
	return Decision;
}

void FOliveSelfCorrectionPolicy::Reset()
{
	CurrentAttemptCount = 0;
}

bool FOliveSelfCorrectionPolicy::HasCompileFailure(const FString& ResultJson, FString& OutErrors, FString& OutAssetPath) const
{
	TSharedPtr<FJsonObject> JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResultJson);
	if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* CompileResult;
	if (!JsonObj->TryGetObjectField(TEXT("compile_result"), CompileResult))
	{
		return false;
	}

	bool bSuccess = true;
	(*CompileResult)->TryGetBoolField(TEXT("success"), bSuccess);
	if (bSuccess)
	{
		return false;
	}

	// Extract errors
	OutErrors.Empty();
	const TArray<TSharedPtr<FJsonValue>>* ErrorsArray;
	if ((*CompileResult)->TryGetArrayField(TEXT("errors"), ErrorsArray))
	{
		for (const auto& ErrVal : *ErrorsArray)
		{
			if (ErrVal->Type == EJson::String)
			{
				OutErrors += ErrVal->AsString() + TEXT("\n");
			}
			else if (ErrVal->AsObject().IsValid())
			{
				OutErrors += ErrVal->AsObject()->GetStringField(TEXT("message")) + TEXT("\n");
			}
		}
	}

	// Extract asset path
	JsonObj->TryGetStringField(TEXT("asset_path"), OutAssetPath);
	if (OutAssetPath.IsEmpty())
	{
		JsonObj->TryGetStringField(TEXT("blueprint_path"), OutAssetPath);
	}

	return true;
}

bool FOliveSelfCorrectionPolicy::HasToolFailure(const FString& ResultJson, FString& OutErrorCode, FString& OutErrorMessage) const
{
	TSharedPtr<FJsonObject> JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResultJson);
	if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
	{
		return false;
	}

	// Check for success field
	bool bSuccess = true;
	if (JsonObj->TryGetBoolField(TEXT("success"), bSuccess) && !bSuccess)
	{
		// Extract error info
		const TSharedPtr<FJsonObject>* ErrorObj;
		if (JsonObj->TryGetObjectField(TEXT("error"), ErrorObj))
		{
			(*ErrorObj)->TryGetStringField(TEXT("code"), OutErrorCode);
			(*ErrorObj)->TryGetStringField(TEXT("message"), OutErrorMessage);
		}
		else
		{
			OutErrorCode = TEXT("UNKNOWN");
			OutErrorMessage = TEXT("Tool execution failed");
		}
		return true;
	}

	return false;
}

FString FOliveSelfCorrectionPolicy::BuildCompileErrorMessage(
	const FString& ToolName,
	const FString& Errors,
	int32 AttemptNum,
	int32 MaxAttempts) const
{
	return FString::Printf(
		TEXT("[COMPILE FAILED - Attempt %d/%d] The Blueprint failed to compile after executing '%s'. Errors:\n%s\n"
			 "Please analyze the errors and fix the issue. You may re-call the write tool with corrections. "
			 "Focus on the FIRST error — later errors are often caused by the first one."),
		AttemptNum, MaxAttempts, *ToolName, *Errors);
}

FString FOliveSelfCorrectionPolicy::BuildToolErrorMessage(
	const FString& ToolName,
	const FString& ErrorCode,
	const FString& ErrorMessage,
	int32 AttemptNum,
	int32 MaxAttempts) const
{
	return FString::Printf(
		TEXT("[TOOL FAILED - Attempt %d/%d] Tool '%s' failed with error %s: %s\n"
			 "Please analyze the error and try a different approach. "
			 "If the parameters were wrong, read the asset first to verify its current state."),
		AttemptNum, MaxAttempts, *ToolName, *ErrorCode, *ErrorMessage);
}
