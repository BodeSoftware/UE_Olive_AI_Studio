// Copyright Bode Software. All Rights Reserved.

#include "Brain/OliveSelfCorrectionPolicy.h"
#include "Brain/OliveRetryPolicy.h"
#include "Index/OliveProjectIndex.h"
#include "OliveAIEditorModule.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"

FOliveSelfCorrectionPolicy::FOliveSelfCorrectionPolicy()
{
}

FOliveCorrectionDecision FOliveSelfCorrectionPolicy::Evaluate(
	const FString& ToolName,
	const FString& ResultJson,
	FOliveLoopDetector& LoopDetector,
	const FOliveRetryPolicy& Policy,
	const FString& AssetContext,
	const TSharedPtr<FJsonObject>& ToolCallArgs)
{
	FOliveCorrectionDecision Decision;

	// Plan deduplication: detect when AI submits identical plans
	if ((ToolName == TEXT("blueprint.apply_plan_json") || ToolName == TEXT("blueprint.preview_plan_json"))
		&& ToolCallArgs.IsValid())
	{
		const FString PlanHash = BuildPlanHash(ToolName, ToolCallArgs);

		if (!PlanHash.IsEmpty())
		{
			int32& SubmitCount = PreviousPlanHashes.FindOrAdd(PlanHash, 0);
			SubmitCount++;

			if (SubmitCount > 1)
			{
				// Extract error info from this attempt's result for context
				FString ErrorCode, ErrorMessage;
				HasToolFailure(ResultJson, ErrorCode, ErrorMessage);

				Decision.Action = EOliveCorrectionAction::FeedBackErrors;
				Decision.EnrichedMessage = FString::Printf(
					TEXT("[IDENTICAL PLAN - Seen %d time(s)] Previous error: %s %s\n"
						 "Change the failing step's approach or call olive.get_recipe for the correct pattern."),
					SubmitCount, *ErrorCode, *ErrorMessage);

				UE_LOG(LogOliveAI, Warning,
					TEXT("SelfCorrection: Identical plan hash=%s, submission #%d"),
					*PlanHash, SubmitCount);

				// Escalate to stop after 3 identical submissions
				if (SubmitCount >= 3)
				{
					Decision.Action = EOliveCorrectionAction::StopWorker;
					Decision.LoopReport = FString::Printf(
						TEXT("Stopped: identical plan submitted %d times without changes."),
						SubmitCount);
				}

				return Decision;
			}
		}
	}

	// Check for compile failure
	FString CompileErrors, AssetPath;
	bool bHasStaleErrors = false;
	int32 RolledBackNodeCount = 0;
	if (HasCompileFailure(ResultJson, CompileErrors, AssetPath, bHasStaleErrors, RolledBackNodeCount))
	{
		// Stale error: compile error is NOT caused by this plan's steps.
		// Do NOT count toward loop detector -- the AI did nothing wrong this turn.
		if (bHasStaleErrors)
		{
			Decision.Action = EOliveCorrectionAction::FeedBackErrors;
			Decision.EnrichedMessage = FString::Printf(
				TEXT("[STALE COMPILE ERROR] The compile error is NOT caused by your current plan. "
					 "It comes from leftover nodes in the graph from a previous operation.\n"
					 "Errors:\n%s\n"
					 "RECOMMENDED: Resubmit your plan with mode: \"replace\" to clear the graph "
					 "before creating new nodes. Alternatively, use blueprint.read to find the "
					 "offending nodes and remove them with blueprint.remove_node."),
				*CompileErrors);
			Decision.AttemptNumber = 1; // Do not count this toward attempts
			Decision.MaxAttempts = Policy.MaxRetriesPerError;

			UE_LOG(LogOliveAI, Log,
				TEXT("SelfCorrection: Stale compile error on '%s' -- not counting toward loop detector"),
				*AssetPath);
			return Decision;
		}

		// Build error signature and record attempt (per-signature tracking)
		const FString Signature = FOliveLoopDetector::BuildCompileErrorSignature(AssetPath, CompileErrors);
		LoopDetector.RecordAttempt(Signature, FString::Printf(TEXT("compile_retry_%d"), LoopDetector.GetAttemptCount(Signature)));

		const int32 SignatureAttempts = LoopDetector.GetAttemptCount(Signature);
		Decision.AttemptNumber = SignatureAttempts;
		Decision.MaxAttempts = Policy.MaxRetriesPerError;

		// Check for loops
		if (LoopDetector.IsLooping(Signature, Policy) || LoopDetector.IsOscillating())
		{
			if (!bIsInGranularFallback && Policy.bAllowGranularFallback)
			{
				// Switch to granular fallback instead of dying
				bIsInGranularFallback = true;
				LastPlanFailureReason = CompileErrors;

				// Reset the loop detector so granular attempts get a fresh error budget
				LoopDetector.Reset();

				Decision.Action = EOliveCorrectionAction::FeedBackErrors;
				Decision.EnrichedMessage = BuildGranularFallbackMessage(
					ToolName, CompileErrors, AssetPath, RolledBackNodeCount);

				UE_LOG(LogOliveAI, Warning,
					TEXT("SelfCorrection: Plan loop detected for '%s'. Switching to GRANULAR FALLBACK mode."),
					*AssetPath);
				return Decision;
			}

			// Already in granular fallback, or fallback disabled -- truly stop
			Decision.Action = EOliveCorrectionAction::StopWorker;
			Decision.LoopReport = LoopDetector.BuildLoopReport();
			UE_LOG(LogOliveAI, Warning,
				TEXT("SelfCorrection: Loop detected for compile failure on '%s' (granular=%s). Stopping."),
				*AssetPath, bIsInGranularFallback ? TEXT("true") : TEXT("false"));
			return Decision;
		}
		// Retain IsBudgetExhausted as hard backstop (now at 20 cycles)
		if (LoopDetector.IsBudgetExhausted(Policy))
		{
			Decision.Action = EOliveCorrectionAction::StopWorker;
			Decision.LoopReport = LoopDetector.BuildLoopReport();
			UE_LOG(LogOliveAI, Warning,
				TEXT("SelfCorrection: Global budget exhausted on '%s'. Stopping."), *AssetPath);
			return Decision;
		}

		// Feed back errors for retry
		Decision.Action = EOliveCorrectionAction::FeedBackErrors;
		if (RolledBackNodeCount > 0)
		{
			Decision.EnrichedMessage = BuildRollbackAwareMessage(
				ToolName, CompileErrors, SignatureAttempts, Policy.MaxRetriesPerError, RolledBackNodeCount);
		}
		else
		{
			Decision.EnrichedMessage = BuildCompileErrorMessage(
				ToolName, CompileErrors, SignatureAttempts, Policy.MaxRetriesPerError);
		}
		// Track failure for asset-switch detection
		if (!AssetPath.IsEmpty())
		{
			CurrentWorkingAsset = AssetPath;
			bCurrentAssetHasFailure = true;
		}

		UE_LOG(LogOliveAI, Log, TEXT("SelfCorrection: Compile failure on '%s', attempt %d/%d"),
			*AssetPath, SignatureAttempts, Policy.MaxRetriesPerError);
		UE_LOG(LogOliveAI, Log, TEXT("SelfCorrection: Injecting compile correction:\n%s"), *Decision.EnrichedMessage);
		return Decision;
	}

	// Check for tool failure
	FString ErrorCode, ErrorMessage;
	if (HasToolFailure(ResultJson, ErrorCode, ErrorMessage))
	{
		// Extract rollback info from result (same pattern as compile failure path)
		int32 ToolRolledBackNodeCount = 0;
		{
			TSharedPtr<FJsonObject> ToolJsonObj;
			TSharedRef<TJsonReader<>> ToolReader = TJsonReaderFactory<>::Create(ResultJson);
			if (FJsonSerializer::Deserialize(ToolReader, ToolJsonObj) && ToolJsonObj.IsValid())
			{
				const TSharedPtr<FJsonObject>* ToolDataObj = nullptr;
				if (ToolJsonObj->TryGetObjectField(TEXT("data"), ToolDataObj) && ToolDataObj && (*ToolDataObj).IsValid())
				{
					double RBDouble = 0;
					if ((*ToolDataObj)->TryGetNumberField(TEXT("rolled_back_nodes"), RBDouble))
					{
						ToolRolledBackNodeCount = static_cast<int32>(RBDouble);
					}
				}
			}
		}

		// Classify error before deciding retry behavior
		const EOliveErrorCategory Category = ClassifyErrorCode(ErrorCode, ErrorMessage);

		// Category B: Unsupported Feature -- do NOT retry, suggest alternative.
		// Skip loop detector recording entirely -- there is nothing to retry.
		if (Category == EOliveErrorCategory::UnsupportedFeature)
		{
			Decision.Action = EOliveCorrectionAction::FeedBackErrors;
			Decision.EnrichedMessage = BuildToolErrorMessage(
				ToolName, ErrorCode, ErrorMessage, 1, 1, AssetContext);
			Decision.EnrichedMessage += TEXT("\n\n[UNSUPPORTED] This error indicates a feature "
				"limitation, not a fixable mistake. Do NOT retry the same operation. "
				"Choose a fundamentally different approach or ask the user for guidance.");

			UE_LOG(LogOliveAI, Log, TEXT("SelfCorrection: Category B (Unsupported) for '%s' error '%s'. "
				"Returning guidance without retry encouragement."), *ToolName, *ErrorCode);
			return Decision;
		}

		// Build error signature and record attempt (per-signature tracking)
		const FString Signature = FOliveLoopDetector::BuildToolErrorSignature(ToolName, ErrorCode, AssetContext);
		LoopDetector.RecordAttempt(Signature, FString::Printf(TEXT("tool_retry_%d"), LoopDetector.GetAttemptCount(Signature)));

		const int32 SignatureAttempts = LoopDetector.GetAttemptCount(Signature);
		Decision.AttemptNumber = SignatureAttempts;
		Decision.MaxAttempts = Policy.MaxRetriesPerError;

		// Check for loops
		if (LoopDetector.IsLooping(Signature, Policy) || LoopDetector.IsOscillating())
		{
			if (!bIsInGranularFallback && Policy.bAllowGranularFallback)
			{
				// Switch to granular fallback instead of dying
				bIsInGranularFallback = true;
				LastPlanFailureReason = ErrorMessage;

				// Reset the loop detector so granular attempts get a fresh error budget
				LoopDetector.Reset();

				Decision.Action = EOliveCorrectionAction::FeedBackErrors;
				Decision.EnrichedMessage = BuildGranularFallbackMessage(
					ToolName, ErrorMessage, AssetContext, ToolRolledBackNodeCount);

				UE_LOG(LogOliveAI, Warning,
					TEXT("SelfCorrection: Tool loop detected for '%s' error '%s'. Switching to GRANULAR FALLBACK mode."),
					*ToolName, *ErrorCode);
				return Decision;
			}

			// Already in granular fallback, or fallback disabled -- truly stop
			Decision.Action = EOliveCorrectionAction::StopWorker;
			Decision.LoopReport = LoopDetector.BuildLoopReport();
			UE_LOG(LogOliveAI, Warning,
				TEXT("SelfCorrection: Loop detected for tool '%s' error '%s' (granular=%s). Stopping."),
				*ToolName, *ErrorCode, bIsInGranularFallback ? TEXT("true") : TEXT("false"));
			return Decision;
		}
		// Retain IsBudgetExhausted as hard backstop (now at 20 cycles)
		if (LoopDetector.IsBudgetExhausted(Policy))
		{
			Decision.Action = EOliveCorrectionAction::StopWorker;
			Decision.LoopReport = LoopDetector.BuildLoopReport();
			UE_LOG(LogOliveAI, Warning,
				TEXT("SelfCorrection: Global budget exhausted for tool '%s'. Stopping."), *ToolName);
			return Decision;
		}

		// Category C: Ambiguous -- allow 1 retry, then escalate
		if (Category == EOliveErrorCategory::Ambiguous && SignatureAttempts > 1)
		{
			Decision.Action = EOliveCorrectionAction::FeedBackErrors;
			Decision.EnrichedMessage = BuildToolErrorMessage(
				ToolName, ErrorCode, ErrorMessage, SignatureAttempts, Policy.MaxRetriesPerError, AssetContext);
			Decision.EnrichedMessage += TEXT("\n\n[ESCALATION] This error may indicate a fundamental "
				"limitation rather than a fixable mistake. If the same approach keeps failing, "
				"try add_node with the exact UK2Node class name, or ask the user.");

			if (ToolRolledBackNodeCount > 0)
			{
				Decision.EnrichedMessage += FString::Printf(
					TEXT("\nNOTE: %d nodes were ROLLED BACK. Do NOT reference node IDs from the failed operation."),
					ToolRolledBackNodeCount);
			}

			UE_LOG(LogOliveAI, Log, TEXT("SelfCorrection: Category C escalation for '%s' error '%s', "
				"attempt %d."), *ToolName, *ErrorCode, SignatureAttempts);
			return Decision;
		}

		// Category A (and first attempt of Category C): standard retry
		Decision.Action = EOliveCorrectionAction::FeedBackErrors;
		Decision.EnrichedMessage = BuildToolErrorMessage(ToolName, ErrorCode, ErrorMessage, SignatureAttempts, Policy.MaxRetriesPerError, AssetContext);
		if (ToolRolledBackNodeCount > 0)
		{
			Decision.EnrichedMessage += FString::Printf(
				TEXT("\nNOTE: %d nodes were ROLLED BACK. Do NOT reference node IDs from the failed operation."),
				ToolRolledBackNodeCount);
		}
		// Track failure for asset-switch detection
		if (!AssetContext.IsEmpty())
		{
			CurrentWorkingAsset = AssetContext;
			bCurrentAssetHasFailure = true;
		}

		UE_LOG(LogOliveAI, Log, TEXT("SelfCorrection: Tool failure '%s' error '%s', attempt %d/%d"),
			*ToolName, *ErrorCode, SignatureAttempts, Policy.MaxRetriesPerError);
		UE_LOG(LogOliveAI, Log, TEXT("SelfCorrection: Injecting tool correction:\n%s"), *Decision.EnrichedMessage);
		return Decision;
	}

	// Success — check for premature asset switching
	if (bCurrentAssetHasFailure && !CurrentWorkingAsset.IsEmpty())
	{
		const FString TargetAsset = ExtractAssetFromResult(ResultJson);
		if (!TargetAsset.IsEmpty() && TargetAsset != CurrentWorkingAsset)
		{
			// Agent switched assets while the previous one still has errors — inform softly
			Decision.Action = EOliveCorrectionAction::FeedBackErrors;
			Decision.EnrichedMessage = FString::Printf(
				TEXT("Note: '%s' had unresolved errors from your last operation. "
					 "You may want to address those before continuing with '%s'. "
					 "Use blueprint.read to check its current state, or if the errors are "
					 "unfixable, let the user know what went wrong."),
				*FPaths::GetBaseFilename(CurrentWorkingAsset),
				*FPaths::GetBaseFilename(TargetAsset));

			// Reset after one informational nudge — don't block
			bCurrentAssetHasFailure = false;
			CurrentWorkingAsset = TargetAsset;

			UE_LOG(LogOliveAI, Log,
				TEXT("SelfCorrection: Asset switch detected — '%s' (with failures) → '%s'. Soft nudge injected."),
				*CurrentWorkingAsset, *TargetAsset);
			return Decision;
		}

		if (!TargetAsset.IsEmpty() && TargetAsset == CurrentWorkingAsset)
		{
			// Success on the same asset — failures resolved
			bCurrentAssetHasFailure = false;
		}
	}

	// Update current working asset on success
	{
		const FString TargetAsset = ExtractAssetFromResult(ResultJson);
		if (!TargetAsset.IsEmpty())
		{
			CurrentWorkingAsset = TargetAsset;
		}
	}

	Decision.Action = EOliveCorrectionAction::Continue;
	return Decision;
}

void FOliveSelfCorrectionPolicy::Reset()
{
	PreviousPlanHashes.Empty();
	bIsInGranularFallback = false;
	LastPlanFailureReason.Empty();
	CurrentWorkingAsset.Empty();
	bCurrentAssetHasFailure = false;
}

bool FOliveSelfCorrectionPolicy::HasCompileFailure(const FString& ResultJson, FString& OutErrors, FString& OutAssetPath, bool& OutHasStaleErrors, int32& OutRolledBackNodeCount) const
{
	OutHasStaleErrors = false;
	OutRolledBackNodeCount = 0;

	TSharedPtr<FJsonObject> JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResultJson);
	if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
	{
		return false;
	}

	// Look for compile_result -- it may be at top level OR nested inside "data"
	// (FOliveToolResult::ToJson nests pipeline ResultData inside "data")
	const TSharedPtr<FJsonObject>* CompileResult = nullptr;
	TSharedPtr<FJsonObject> DataObj;

	if (JsonObj->TryGetObjectField(TEXT("compile_result"), CompileResult))
	{
		// Found at top level (direct compile tool result or legacy format)
	}
	else
	{
		// Try inside "data" (standard write pipeline result format)
		const TSharedPtr<FJsonObject>* DataPtr = nullptr;
		if (JsonObj->TryGetObjectField(TEXT("data"), DataPtr) && DataPtr && (*DataPtr).IsValid())
		{
			DataObj = *DataPtr;
			if (!DataObj->TryGetObjectField(TEXT("compile_result"), CompileResult))
			{
				return false;
			}
		}
		else
		{
			return false;
		}
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

	// Extract asset path -- check top level first, then inside "data"
	JsonObj->TryGetStringField(TEXT("asset_path"), OutAssetPath);
	if (OutAssetPath.IsEmpty())
	{
		JsonObj->TryGetStringField(TEXT("blueprint_path"), OutAssetPath);
	}
	if (OutAssetPath.IsEmpty() && DataObj.IsValid())
	{
		DataObj->TryGetStringField(TEXT("asset_path"), OutAssetPath);
		if (OutAssetPath.IsEmpty())
		{
			DataObj->TryGetStringField(TEXT("blueprint_path"), OutAssetPath);
		}
	}

	// Extract rolled_back_nodes count from "data" if present (set by plan executor rollback)
	if (DataObj.IsValid())
	{
		double RolledBackDouble = 0;
		if (DataObj->TryGetNumberField(TEXT("rolled_back_nodes"), RolledBackDouble))
		{
			OutRolledBackNodeCount = static_cast<int32>(RolledBackDouble);
		}
	}

	// ------------------------------------------------------------------
	// Stale error detection: cross-reference compile errors against
	// plan_class_names / plan_function_names from the result data.
	// If the plan has metadata but NO compile error mentions any of the
	// plan's classes or functions, the errors are stale (caused by
	// pre-existing issues or previous operations, not this plan).
	// ------------------------------------------------------------------

	// Determine the effective data object to search for plan metadata.
	// plan_class_names / plan_function_names are set on ToolResult.Data,
	// which is nested under "data" in the serialized JSON (via ToJson).
	// Also check top level for non-nested formats (e.g. direct results).
	TArray<FString> PlanClasses;
	TArray<FString> PlanFunctions;

	// Helper lambda to extract plan metadata arrays from a JSON object
	auto ExtractPlanMetadata = [&PlanClasses, &PlanFunctions](const TSharedPtr<FJsonObject>& Obj)
	{
		if (!Obj.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* ClassArr = nullptr;
		if (Obj->TryGetArrayField(TEXT("plan_class_names"), ClassArr) && ClassArr)
		{
			for (const auto& Val : *ClassArr)
			{
				FString ClassName = Val->AsString();
				if (!ClassName.IsEmpty())
				{
					PlanClasses.Add(MoveTemp(ClassName));
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* FuncArr = nullptr;
		if (Obj->TryGetArrayField(TEXT("plan_function_names"), FuncArr) && FuncArr)
		{
			for (const auto& Val : *FuncArr)
			{
				FString FuncName = Val->AsString();
				if (!FuncName.IsEmpty())
				{
					PlanFunctions.Add(MoveTemp(FuncName));
				}
			}
		}
	};

	// Try "data" object first (standard ToolResult->ToJson nesting)
	ExtractPlanMetadata(DataObj);

	// If not found in data, try top level (legacy or direct format)
	if (PlanClasses.Num() == 0 && PlanFunctions.Num() == 0)
	{
		ExtractPlanMetadata(JsonObj);
	}

	// Only classify as stale if we HAVE plan metadata to compare against.
	// If plan metadata is missing (old-style result without T5 fields),
	// default to OutHasStaleErrors = false (current behavior preserved).
	if (PlanClasses.Num() > 0 || PlanFunctions.Num() > 0)
	{
		bool bAnyErrorMatchesPlan = false;
		TArray<FString> ErrorLines;
		OutErrors.ParseIntoArrayLines(ErrorLines);

		for (const FString& Line : ErrorLines)
		{
			if (Line.IsEmpty())
			{
				continue;
			}

			for (const FString& ClassName : PlanClasses)
			{
				if (Line.Contains(ClassName))
				{
					bAnyErrorMatchesPlan = true;
					break;
				}
			}
			if (bAnyErrorMatchesPlan)
			{
				break;
			}

			for (const FString& FuncName : PlanFunctions)
			{
				if (Line.Contains(FuncName))
				{
					bAnyErrorMatchesPlan = true;
					break;
				}
			}
			if (bAnyErrorMatchesPlan)
			{
				break;
			}
		}

		OutHasStaleErrors = !bAnyErrorMatchesPlan;

		if (OutHasStaleErrors)
		{
			UE_LOG(LogOliveAI, Log,
				TEXT("SelfCorrection: Compile errors on '%s' classified as STALE -- "
					 "no error line references plan classes [%s] or functions [%s]"),
				*OutAssetPath,
				*FString::Join(PlanClasses, TEXT(", ")),
				*FString::Join(PlanFunctions, TEXT(", ")));
		}
	}

	UE_LOG(LogOliveAI, Log, TEXT("SelfCorrection: Extracted compile failure -- asset='%s', stale=%s, errors:\n%s"),
		*OutAssetPath, OutHasStaleErrors ? TEXT("true") : TEXT("false"), *OutErrors);
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

		// Enrich with step-level diagnostics when available (plan failures).
		const TSharedPtr<FJsonObject>* DataObj = nullptr;
		if (JsonObj->TryGetObjectField(TEXT("data"), DataObj) && DataObj && (*DataObj).IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* StepErrors = nullptr;
			if ((*DataObj)->TryGetArrayField(TEXT("step_errors"), StepErrors) && StepErrors && StepErrors->Num() > 0)
			{
				TSharedPtr<FJsonObject> FirstStepError = (*StepErrors)[0]->AsObject();
				if (FirstStepError.IsValid())
				{
					FString StepId;
					FString StepCode;
					FString StepMessage;
					FirstStepError->TryGetStringField(TEXT("step_id"), StepId);
					FirstStepError->TryGetStringField(TEXT("error_code"), StepCode);
					FirstStepError->TryGetStringField(TEXT("message"), StepMessage);

					if (!StepCode.IsEmpty() && OutErrorCode == TEXT("PLAN_RESOLVE_FAILED"))
					{
						OutErrorCode = StepCode;
					}

					if (!StepMessage.IsEmpty())
					{
						OutErrorMessage += StepId.IsEmpty()
							? FString::Printf(TEXT(" | First step error: %s"), *StepMessage)
							: FString::Printf(TEXT(" | First step error (%s): %s"), *StepId, *StepMessage);
					}
				}
			}
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
			 "REQUIRED ACTION: Do NOT declare success. Fix the compile error before finishing.\n"
			 "1. Call blueprint.read on the affected graph with include_pins:true to see the current node/pin state.\n"
			 "2. Focus on the FIRST error — later errors are often caused by the first one.\n"
			 "3. Use connect_pins or set_pin_default to fix the issue, then compile again."),
		AttemptNum, MaxAttempts, *ToolName, *Errors);
}

FString FOliveSelfCorrectionPolicy::BuildToolErrorMessage(
	const FString& ToolName,
	const FString& ErrorCode,
	const FString& ErrorMessage,
	int32 AttemptNum,
	int32 MaxAttempts,
	const FString& AssetContext) const
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
		Guidance = TEXT("The asset path is wrong.");

		// Auto-search the project index to suggest correct paths
		const FString ShortName = FPaths::GetBaseFilename(AssetContext);
		if (!ShortName.IsEmpty() && FOliveProjectIndex::Get().IsReady())
		{
			static constexpr int32 MaxSuggestions = 5;
			const TArray<FOliveAssetInfo> SearchResults = FOliveProjectIndex::Get().SearchAssets(ShortName, MaxSuggestions);

			if (SearchResults.Num() > 0)
			{
				Guidance += TEXT("\n\nDid you mean one of these?");
				for (const FOliveAssetInfo& Result : SearchResults)
				{
					Guidance += FString::Printf(TEXT("\n  - %s  (%s)"), *Result.Path, *Result.AssetClass.ToString());
				}
				Guidance += TEXT("\n\nUse the correct path from above and retry.");
			}
			else
			{
				Guidance += FString::Printf(
					TEXT("\n\nNo assets found matching '%s'. Use project.search_assets with different keywords to find the correct path."),
					*ShortName);
			}
		}
		else
		{
			Guidance += TEXT(" Use project.search_assets to find the correct path, then retry.");
		}
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
			Guidance = TEXT("The node type was not recognized as a curated type. "
				"You can also pass the exact UK2Node class name as the type "
				"(e.g., 'K2Node_ComponentBoundEvent', 'K2Node_Timeline', 'K2Node_Select'). "
				"Use blueprint.search_nodes to find available node types. "
				"After creation, use blueprint.get_node_pins to discover the actual pin names.");
		}
	}
	else if (ErrorCode == TEXT("FUNCTION_NOT_FOUND"))
	{
		if (AttemptNum <= 1)
		{
			// Extract "Did you mean:" alternatives from error message and emphasize them
			if (ErrorMessage.Contains(TEXT("Did you mean:")))
			{
				Guidance = TEXT("The error lists suggested function names after 'Did you mean:'. "
					"USE one of those names exactly — do NOT guess a different name. "
					"If a property match is shown, use set_var or get_var instead of call.");
			}
			else
			{
				Guidance = TEXT("This function does not exist in UE's Blueprint API. "
					"Do NOT retry with the same name or a variation. "
					"Call blueprint.describe_function with the name you intended to verify it exists, "
					"or use set_var/get_var if you meant to access a property.");
			}
		}
		else if (AttemptNum == 2)
		{
			Guidance = TEXT("You already failed to find this function. STOP guessing names. "
				"Call blueprint.describe_function(function_name, target_class) to verify "
				"the actual UE function name before retrying. "
				"If describe_function also fails, the function does not exist — "
				"use set_var/get_var for properties, or find an alternative approach.");
		}
		else
		{
			Guidance = TEXT("This function does not exist after multiple attempts. "
				"Do NOT retry. Choose a fundamentally different approach: "
				"set_var/get_var for properties, a different function, or editor.run_python.");
		}
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
	else if (ErrorCode == TEXT("RATE_LIMITED"))
	{
		Guidance = TEXT("Write rate limit reached (30 operations/minute). "
			"Do NOT retry immediately — the same operation will fail again. "
			"Options:\n"
			"1. Wait 30-60 seconds, then retry\n"
			"2. Continue with READ-ONLY operations (blueprint.read, project.search) while waiting\n"
			"3. Work on a different Blueprint that doesn't need writes\n"
			"The rate limit resets on a rolling 60-second window.");
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
	else if (ErrorCode == TEXT("PLAN_RESOLVE_FAILED") && ErrorMessage.Contains(TEXT("event")))
	{
		Guidance = TEXT("The plan failed to resolve an event operation. "
			"If you need a component delegate event (e.g., OnComponentHit, OnComponentBeginOverlap), "
			"plan JSON does not support component_bound_event. Use add_node instead:\n"
			"  blueprint.add_node type:\"K2Node_ComponentBoundEvent\" "
			"properties:{\"DelegatePropertyName\":\"OnComponentHit\",\"ComponentPropertyName\":\"CollisionComp\"}\n"
			"Then wire with blueprint.connect_pins.");
	}
	else if (ErrorCode == TEXT("PLAN_RESOLVE_FAILED") || ErrorCode == TEXT("PLAN_LOWER_FAILED") || ErrorCode == TEXT("PLAN_EXECUTION_FAILED"))
	{
		Guidance = TEXT("The plan failed during resolution or execution. Check the error details for which step failed. "
			"Common mistakes: set_var on a component (use get_var to read, then call setter), "
			"invented function names (search with blueprint.search_nodes first), "
			"wrong pin names (use @step.auto instead of guessing). "
			"Fix the failing step and resubmit the corrected plan.");

		// Enrich with SCS component list when asset context is available
		if (!AssetContext.IsEmpty())
		{
			UBlueprint* BP = Cast<UBlueprint>(
				StaticLoadObject(UBlueprint::StaticClass(), nullptr, *AssetContext));
			if (BP && BP->SimpleConstructionScript)
			{
				TArray<FString> ComponentNames;
				for (USCS_Node* SCSNode : BP->SimpleConstructionScript->GetAllNodes())
				{
					if (SCSNode && SCSNode->ComponentClass)
					{
						ComponentNames.Add(FString::Printf(TEXT("%s (%s)"),
							*SCSNode->GetVariableName().ToString(),
							*SCSNode->ComponentClass->GetName()));
					}
				}
				if (ComponentNames.Num() > 0)
				{
					Guidance += FString::Printf(
						TEXT("\nThis Blueprint's components: %s. "
							 "If calling a component function, wire a get_var for the component to the Target input."),
						*FString::Join(ComponentNames, TEXT(", ")));
				}
			}
		}
	}
	else if (ErrorCode == TEXT("GRAPH_DRIFT"))
	{
		Guidance = TEXT(
			"The graph fingerprint did not match because other operations modified the Blueprint "
			"between preview and apply. BEST PRACTICE: prefer calling apply_plan_json directly — "
			"it validates inline. If you do use preview, call it in a separate turn BEFORE apply, "
			"never in the same batch.");
	}
	else if (ErrorCode == TEXT("BP_CONNECT_PINS_INCOMPATIBLE"))
	{
		Guidance = TEXT("Pin types are incompatible and no automatic conversion exists. "
			"The error response includes 'alternatives' with specific fixes ordered by confidence. "
			"Try the first 'high' confidence alternative. "
			"Common patterns:\n"
			"- Struct -> Scalar (e.g., Vector -> Float): use break_struct op or ~PinName_X suffix\n"
			"- Scalar -> Struct (e.g., Float -> Vector): use make_struct op or Conv_ call\n"
			"- Object type mismatch: add a cast step\n"
			"- Container mismatch (Array -> single): add a Get/GetCopy call step\n"
			"- If all alternatives fail, use editor.run_python");
	}
	else if (ErrorCode == TEXT("DATA_WIRE_INCOMPATIBLE"))
	{
		Guidance = TEXT("Two pins in the plan have incompatible types and no autocast exists. "
			"The wiring_errors array contains specific alternatives. "
			"Common fixes:\n"
			"- Vector -> Float: use ~suffix for sub-pin (e.g., '@get_loc.~ReturnValue_X'). "
			"SplitPin is also available as a fallback.\n"
			"- Scalar -> Struct: add a make_struct intermediate step\n"
			"- Struct -> individual values: add a break_struct step\n"
			"- If all else fails, try editor.run_python for direct pin manipulation");
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
	else if (ErrorCode == TEXT("COMPONENT_NOT_VARIABLE"))
	{
		Guidance = TEXT("You tried to use set_var on a component. Components are read-only and "
			"cannot be assigned with set_var. To READ a component, use get_var (this works). "
			"To MODIFY a component property, use get_var to get the reference, then call "
			"the setter function with Target wired to the get_var output.");
	}
	else if (ErrorCode == TEXT("COMPILE_FAILED"))
	{
		Guidance = TEXT("The Blueprint compiled with errors after this operation. "
			"Call blueprint.read on the affected graph with include_pins:true to see node/pin state. "
			"Focus on the FIRST compile error. Use connect_pins or set_pin_default to fix it, then compile again.");
	}
	else if (ErrorCode == TEXT("PLAN_VALIDATION_FAILED"))
	{
		Guidance = TEXT("The plan has structural issues detected before execution. Read the error details carefully. "
			"COMPONENT_FUNCTION_ON_ACTOR: if only one matching component exists, "
			"the executor will auto-wire it (no action needed). If multiple components "
			"match, add a get_var step for the specific component and wire its output to Target. "
			"EXEC_WIRING_CONFLICT: remove exec_after and restructure using exec_outputs on the branch node. "
			"Fix ONLY the failing step and resubmit the COMPLETE plan. "
			"Keep ALL working steps intact -- do NOT drop steps that were not causing errors.");
	}
	else
	{
		Guidance = TEXT("Analyze the error and try a different approach. If the parameters were wrong, read the asset first to verify its current state.");
	}

	// Progressive error disclosure based on attempt number
	FString Result;

	if (AttemptNum == 1)
	{
		// First attempt: header (error code only) + guidance.
		// Omit raw error message to avoid overwhelming context.
		FString ShortHeader = FString::Printf(
			TEXT("[TOOL FAILED - Attempt %d/%d] Tool '%s' error: %s"),
			AttemptNum, MaxAttempts, *ToolName, *ErrorCode);
		Result = ShortHeader + TEXT("\nFIX GUIDANCE: ") + Guidance;
	}
	else if (AttemptNum == 2)
	{
		// Second attempt: full header (with error message) + guidance.
		Result = Header + TEXT("\nFIX GUIDANCE: ") + Guidance;
	}
	else
	{
		// Third+ attempt: full header + guidance + escalation directive.
		Result = Header + TEXT("\nFIX GUIDANCE: ") + Guidance;
		Result += FString::Printf(
			TEXT("\n\n[ESCALATION - Attempt %d/%d] Previous approaches have not worked. "
				 "You MUST try a fundamentally different strategy:\n"
				 "- Use olive.get_recipe to find the correct pattern for this task\n"
				 "- Use @step.auto for ALL data wires instead of explicit pin names\n"
				 "- Simplify the plan by breaking it into smaller operations\n"
				 "- Read the Blueprint state with blueprint.read before retrying"),
			AttemptNum, MaxAttempts);
	}

	return Result;
}

FString FOliveSelfCorrectionPolicy::BuildRollbackAwareMessage(
	const FString& ToolName,
	const FString& Errors,
	int32 AttemptNum,
	int32 MaxAttempts,
	int32 RolledBackNodeCount) const
{
	return FString::Printf(
		TEXT("[COMPILE FAILED + ROLLBACK - Attempt %d/%d] The Blueprint failed to compile after executing '%s'. "
			 "%d nodes were ROLLED BACK -- the graph is restored to its pre-plan state.\n"
			 "Errors:\n%s\n"
			 "REQUIRED ACTION: Fix ONLY the failing step and resubmit the COMPLETE plan with apply_plan_json.\n"
			 "CRITICAL: Keep ALL working steps intact. Only remove or modify the step that caused the error. "
			 "Do NOT simplify by dropping steps that were working -- that causes missing functionality.\n"
			 "Do NOT use connect_pins or reference any node IDs from the failed plan -- those nodes no longer exist.\n"
			 "Common fixes:\n"
			 "- Latent calls (Delay, AI MoveTo, etc.) CANNOT be in function graphs -- use a Custom Event in EventGraph instead\n"
			 "- Function parameters are NOT class variables -- if in a function graph, parameters are pins on the entry node\n"
			 "- Missing variables -- ensure add_variable succeeded before referencing in a plan"),
		AttemptNum, MaxAttempts, *ToolName, RolledBackNodeCount, *Errors);
}

FString FOliveSelfCorrectionPolicy::BuildGranularFallbackMessage(
	const FString& ToolName,
	const FString& Errors,
	const FString& AssetPath,
	int32 RolledBackNodeCount) const
{
	return FString::Printf(
		TEXT("[PLAN APPROACH FAILED -- SWITCHING TO STEP-BY-STEP MODE]\n"
			 "The plan_json approach failed 3 times with the same error. %d nodes were rolled back.\n"
			 "Last error:\n%s\n\n"
			 "You MUST now switch to granular node-by-node building:\n"
			 "1. Call blueprint.read or blueprint.read_function on '%s' to see current graph state\n"
			 "2. Use blueprint.add_node to create nodes ONE AT A TIME -- each call returns the node_id and pin manifest\n"
			 "3. Use blueprint.connect_pins to wire them using the node_ids from step 2\n"
			 "4. Use blueprint.set_pin_default for any default values\n"
			 "5. Call blueprint.compile to verify\n\n"
			 "This is slower but gives you accurate state after each operation.\n"
			 "Do NOT use apply_plan_json again for this graph -- it has failed repeatedly."),
		RolledBackNodeCount, *Errors, *AssetPath);
}

EOliveErrorCategory FOliveSelfCorrectionPolicy::ClassifyErrorCode(
	const FString& ErrorCode,
	const FString& ErrorMessage)
{
	// ============================================================
	// Category B: Unsupported Feature -- do NOT retry
	// ============================================================

	// These represent features that genuinely cannot be done with current tools.
	// Retrying will never help; the AI needs to choose a different approach.
	if (ErrorCode == TEXT("USER_DENIED")
		|| ErrorCode == TEXT("TEMPLATE_NOT_FOUND")
		|| ErrorCode == TEXT("TEMPLATE_NOT_FACTORY")
		|| ErrorCode == TEXT("RATE_LIMITED"))
	{
		return EOliveErrorCategory::UnsupportedFeature;
	}

	// ============================================================
	// Category C: Ambiguous -- retry once, then escalate
	// ============================================================

	// These might be fixable with a different approach, but might also
	// indicate a fundamental limitation. One retry with diagnostics.
	if (ErrorCode == TEXT("BP_ADD_NODE_FAILED"))
	{
		// Could be a typo in class name (fixable) or a genuinely unsupported
		// node type (unsupported). One retry with class lookup guidance.
		return EOliveErrorCategory::Ambiguous;
	}
	if (ErrorCode == TEXT("PLAN_EXECUTION_FAILED"))
	{
		return EOliveErrorCategory::Ambiguous;
	}

	// ============================================================
	// Category A: Fixable Mistake -- standard retry (default)
	// ============================================================

	// Everything else is treated as a fixable mistake:
	// VALIDATION_MISSING_PARAM, ASSET_NOT_FOUND, NODE_TYPE_UNKNOWN,
	// FUNCTION_NOT_FOUND, DUPLICATE_NATIVE_EVENT, DATA_PIN_NOT_FOUND,
	// DATA_WIRE_INCOMPATIBLE, EXEC_PIN_NOT_FOUND, BP_CONNECT_PINS_FAILED,
	// BP_CONNECT_PINS_INCOMPATIBLE, PLAN_RESOLVE_FAILED,
	// COMPILE_FAILED, COMPONENT_FUNCTION_ON_ACTOR, etc.
	return EOliveErrorCategory::FixableMistake;
}

FString FOliveSelfCorrectionPolicy::BuildPlanHash(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& ToolCallArgs) const
{
	if (!ToolCallArgs.IsValid())
	{
		return FString();
	}

	// Extract identifying fields
	FString AssetPath;
	ToolCallArgs->TryGetStringField(TEXT("asset_path"), AssetPath);

	FString GraphName;
	ToolCallArgs->TryGetStringField(TEXT("graph_name"), GraphName);

	// Extract and serialize the plan object
	const TSharedPtr<FJsonObject>* PlanObj = nullptr;
	FString PlanString;
	if (ToolCallArgs->TryGetObjectField(TEXT("plan"), PlanObj) && PlanObj && (*PlanObj).IsValid())
	{
		auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&PlanString);
		FJsonSerializer::Serialize((*PlanObj).ToSharedRef(), Writer);
		Writer->Close();
	}

	if (PlanString.IsEmpty())
	{
		return FString();
	}

	// Composite key: tool + asset + graph + plan content
	const FString Composite = FString::Printf(TEXT("%s|%s|%s|%s"),
		*ToolName, *AssetPath, *GraphName, *PlanString);

	return FOliveLoopDetector::HashString(Composite);
}

FString FOliveSelfCorrectionPolicy::ExtractAssetFromResult(const FString& ResultJson) const
{
	TSharedPtr<FJsonObject> JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResultJson);
	if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
	{
		return FString();
	}

	// Check top-level fields
	FString AssetPath;
	if (JsonObj->TryGetStringField(TEXT("asset_path"), AssetPath) && !AssetPath.IsEmpty())
	{
		return AssetPath;
	}
	if (JsonObj->TryGetStringField(TEXT("blueprint_path"), AssetPath) && !AssetPath.IsEmpty())
	{
		return AssetPath;
	}

	// Check inside "data" (standard write pipeline result format)
	const TSharedPtr<FJsonObject>* DataObj = nullptr;
	if (JsonObj->TryGetObjectField(TEXT("data"), DataObj) && DataObj && (*DataObj).IsValid())
	{
		if ((*DataObj)->TryGetStringField(TEXT("asset_path"), AssetPath) && !AssetPath.IsEmpty())
		{
			return AssetPath;
		}
		if ((*DataObj)->TryGetStringField(TEXT("blueprint_path"), AssetPath) && !AssetPath.IsEmpty())
		{
			return AssetPath;
		}
	}

	return FString();
}
