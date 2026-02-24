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
	const FOliveRetryPolicy& Policy,
	const FString& AssetContext)
{
	FOliveCorrectionDecision Decision;

	// Check for compile failure
	FString CompileErrors, AssetPath;
	if (HasCompileFailure(ResultJson, CompileErrors, AssetPath))
	{
		// Build error signature and record attempt (per-signature tracking)
		const FString Signature = FOliveLoopDetector::BuildCompileErrorSignature(AssetPath, CompileErrors);
		LoopDetector.RecordAttempt(Signature, FString::Printf(TEXT("compile_retry_%d"), LoopDetector.GetAttemptCount(Signature)));

		const int32 SignatureAttempts = LoopDetector.GetAttemptCount(Signature);
		Decision.AttemptNumber = SignatureAttempts;
		Decision.MaxAttempts = Policy.MaxRetriesPerError;

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
		Decision.EnrichedMessage = BuildCompileErrorMessage(ToolName, CompileErrors, SignatureAttempts, Policy.MaxRetriesPerError);
		UE_LOG(LogOliveAI, Log, TEXT("SelfCorrection: Compile failure on '%s', attempt %d/%d"),
			*AssetPath, SignatureAttempts, Policy.MaxRetriesPerError);
		UE_LOG(LogOliveAI, Log, TEXT("SelfCorrection: Injecting compile correction:\n%s"), *Decision.EnrichedMessage);
		return Decision;
	}

	// Check for tool failure
	FString ErrorCode, ErrorMessage;
	if (HasToolFailure(ResultJson, ErrorCode, ErrorMessage))
	{
		// Build error signature and record attempt (per-signature tracking)
		const FString Signature = FOliveLoopDetector::BuildToolErrorSignature(ToolName, ErrorCode, AssetContext);
		LoopDetector.RecordAttempt(Signature, FString::Printf(TEXT("tool_retry_%d"), LoopDetector.GetAttemptCount(Signature)));

		const int32 SignatureAttempts = LoopDetector.GetAttemptCount(Signature);
		Decision.AttemptNumber = SignatureAttempts;
		Decision.MaxAttempts = Policy.MaxRetriesPerError;

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
		Decision.EnrichedMessage = BuildToolErrorMessage(ToolName, ErrorCode, ErrorMessage, SignatureAttempts, Policy.MaxRetriesPerError);
		UE_LOG(LogOliveAI, Log, TEXT("SelfCorrection: Tool failure '%s' error '%s', attempt %d/%d"),
			*ToolName, *ErrorCode, SignatureAttempts, Policy.MaxRetriesPerError);
		UE_LOG(LogOliveAI, Log, TEXT("SelfCorrection: Injecting tool correction:\n%s"), *Decision.EnrichedMessage);
		return Decision;
	}

	// Success — tool worked fine
	Decision.Action = EOliveCorrectionAction::Continue;
	return Decision;
}

void FOliveSelfCorrectionPolicy::Reset()
{
	// Per-signature counting is owned by FOliveLoopDetector; nothing to reset here
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

	UE_LOG(LogOliveAI, Log, TEXT("SelfCorrection: Extracted compile failure — asset='%s', errors:\n%s"), *OutAssetPath, *OutErrors);
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
		UE_LOG(LogOliveAI, Log, TEXT("SelfCorrection: Extracted tool failure — code='%s', message='%s'"), *OutErrorCode, *OutErrorMessage);
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
	FString Header = FString::Printf(
		TEXT("[TOOL FAILED - Attempt %d/%d] Tool '%s' failed with error %s: %s"),
		AttemptNum, MaxAttempts, *ToolName, *ErrorCode, *ErrorMessage);

	FString Guidance;

	if (ErrorCode == TEXT("VALIDATION_MISSING_PARAM"))
	{
		Guidance = TEXT("Check the tool schema for required parameters. Re-call the tool with all required parameters filled in.");
	}
	else if (ErrorCode == TEXT("ASSET_NOT_FOUND"))
	{
		Guidance = TEXT("The asset path is wrong. Use project.search_assets to find the correct path, then retry with the corrected path.");
	}
	else if (ErrorCode == TEXT("NODE_TYPE_UNKNOWN") || ErrorCode == TEXT("BP_ADD_NODE_FAILED"))
	{
		if (ErrorMessage.Contains(TEXT("class")) && ErrorMessage.Contains(TEXT("not found")))
		{
			Guidance = TEXT("The specified class was not found. "
				"If this is a Blueprint class, provide the full asset path (e.g., '/Game/Blueprints/BP_Bullet'). "
				"Use project.search_assets to find the correct path. "
				"If this is a native C++ class, use the full prefixed name (e.g., 'ACharacter', 'APawn'). "
				"Do NOT guess class names — search first.");
		}
		else
		{
			Guidance = TEXT("The node type was not found. Use blueprint.search_nodes to find the correct node type identifier, then retry.");
		}
	}
	else if (ErrorCode == TEXT("FUNCTION_NOT_FOUND"))
	{
		Guidance = TEXT("The function was not found. Use blueprint.search_nodes to find the correct function name. Check for K2_ prefixes and class membership.");
	}
	else if (ErrorCode == TEXT("DUPLICATE_NATIVE_EVENT"))
	{
		Guidance = TEXT("This event already exists in the graph. Use blueprint.read to see existing nodes. Reuse the existing event node instead of creating a new one.");
	}
	else if (ErrorCode == TEXT("DATA_PIN_NOT_FOUND") || ErrorCode == TEXT("EXEC_PIN_NOT_FOUND") || ErrorCode == TEXT("DATA_PIN_AMBIGUOUS"))
	{
		Guidance = TEXT("Pin name mismatch. Use blueprint.read with include_pins:true to see the actual pin names on the target node, then retry with the correct pin name.");
	}
	else if (ErrorCode == TEXT("BP_REMOVE_NODE_FAILED"))
	{
		Guidance = TEXT("The node was not found in the specified graph. "
			"IMPORTANT: node_ids (node_0, node_1, etc.) are scoped to the graph "
			"where they were created. A node created by apply_plan_json in the 'Fire' "
			"function graph does NOT exist in 'EventGraph'. "
			"Use blueprint.read_event_graph or blueprint.read_function to find the "
			"correct node_id in the target graph.");
	}
	else if (ErrorCode == TEXT("USER_DENIED"))
	{
		Guidance = TEXT("The user denied this operation. Ask the user how they would like to proceed instead of retrying.");
	}
	else if (ErrorCode == TEXT("PREVIEW_REQUIRED"))
	{
		Guidance = TEXT("This operation requires a preview first. Call blueprint.preview_plan_json with the plan to get a preview, then confirm execution.");
	}
	else if (ErrorCode == TEXT("PLAN_INVALID_INPUT_REF") || ErrorCode == TEXT("PLAN_FORWARD_INPUT_REF"))
	{
		Guidance = TEXT("A step references an input from a step that has not been defined yet. Reorder steps so that data-providing steps come before the steps that consume their outputs.");
	}
	else if (ErrorCode == TEXT("BP_MODIFY_COMPONENT_FAILED") || ErrorCode == TEXT("BP_CONSTRAINT_COMPONENT_NOT_FOUND"))
	{
		Guidance = TEXT("Component operation failed. Use blueprint.read to check which components exist on the Blueprint and verify the component name and owning Blueprint.");
	}
	else if (ErrorCode == TEXT("PLAN_INVALID_OP") || ErrorCode == TEXT("PLAN_MISSING_OP"))
	{
		Guidance = TEXT("The plan contains an unknown or missing operation type. Check the tool schema for the list of valid operation types and correct the plan.");
	}
	else if (ErrorCode == TEXT("PLAN_DUPLICATE_STEP_ID"))
	{
		Guidance = TEXT("Two or more steps share the same step_id. Each step_id must be unique within the plan. Rename the duplicate step_ids.");
	}
	else if (ErrorCode == TEXT("PLAN_RESOLVE_FAILED") || ErrorCode == TEXT("PLAN_LOWER_FAILED") || ErrorCode == TEXT("PLAN_EXECUTION_FAILED"))
	{
		Guidance = TEXT("The plan failed during resolution or execution. Check the error details for which step failed, fix that step, and resubmit the corrected plan.");
	}
	else if (ErrorCode == TEXT("GRAPH_DRIFT"))
	{
		Guidance = TEXT(
			"The graph fingerprint did not match because other operations modified the Blueprint "
			"between preview and apply. BEST PRACTICE: prefer calling apply_plan_json directly — "
			"it validates inline. If you do use preview, call it in a separate turn BEFORE apply, "
			"never in the same batch.");
	}
	else if (ErrorCode == TEXT("BP_CONNECT_PINS_FAILED"))
	{
		Guidance = TEXT("Pin connection failed. BEFORE RETRYING: call blueprint.read "
			"with include_pins:true on the target graph to get the ACTUAL pin names. "
			"Do NOT guess or hallucinate pin names. "
			"Common causes: "
			"1) Pin name mismatch — pins often have internal names different from display names "
			"(e.g., 'SpawnTransform' not 'Location', 'ReturnValue' not 'Result'). "
			"2) Node not found — node_ids are scoped per graph. Use blueprint.read to verify. "
			"3) Pin format — use 'node_id.pin_name' (dot separator, NOT colon). "
			"4) Type mismatch — ensure compatible pin types. "
			"MANDATORY: your next tool call MUST be blueprint.read (or blueprint.read_function / "
			"blueprint.read_event_graph) with include_pins:true. Never retry connect_pins "
			"with the same pin names that just failed.");
	}
	else if (ErrorCode == TEXT("INVALID_EXEC_REF"))
	{
		Guidance = TEXT("exec_after references a step_id that doesn't exist in the plan. "
			"IMPORTANT: exec_after expects step_ids from YOUR plan (e.g. 'evt', 'spawn'), "
			"NOT K2Node IDs from blueprint.read (e.g. 'K2Node_Event_1'). "
			"If adding to an existing graph, create a new event/custom_event step in your plan "
			"as the entry point — don't reference existing graph nodes in exec_after.");
	}
	else
	{
		Guidance = TEXT("Analyze the error and try a different approach. If the parameters were wrong, read the asset first to verify its current state.");
	}

	return Header + TEXT("\nFIX GUIDANCE: ") + Guidance;
}
