// Copyright Bode Software. All Rights Reserved.

/**
 * OliveBuildTool.cpp
 *
 * Implements the olive.build batch executor tool. Executes an ordered list of
 * tool operations in a single MCP call, creating an optional snapshot before
 * execution and returning aggregated per-step results.
 */

#include "MCP/OliveBuildTool.h"
#include "MCP/OliveToolRegistry.h"
#include "OliveSnapshotManager.h"
#include "OliveAIEditorModule.h"
#include "Serialization/JsonSerializer.h"

// Static member initialization
bool FOliveBuildTool::bIsExecuting = false;

void FOliveBuildTool::RegisterTool()
{
	// Build the input schema
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// description (optional)
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetStringField(TEXT("description"), TEXT("What this build does"));
		Properties->SetObjectField(TEXT("description"), Prop);
	}

	// snapshot (optional, default true)
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("boolean"));
		Prop->SetStringField(TEXT("description"), TEXT("Create snapshot before execution for rollback"));
		Prop->SetBoolField(TEXT("default"), true);
		Properties->SetObjectField(TEXT("snapshot"), Prop);
	}

	// on_error (optional, enum)
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetStringField(TEXT("description"), TEXT("stop = halt on first failure, continue_remaining = skip failed steps"));
		Prop->SetStringField(TEXT("default"), TEXT("stop"));

		TArray<TSharedPtr<FJsonValue>> EnumValues;
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("stop")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("continue_remaining")));
		Prop->SetArrayField(TEXT("enum"), EnumValues);

		Properties->SetObjectField(TEXT("on_error"), Prop);
	}

	// steps (required, array of objects)
	{
		// Item schema for each step
		TSharedPtr<FJsonObject> StepItemProps = MakeShared<FJsonObject>();

		TSharedPtr<FJsonObject> IdProp = MakeShared<FJsonObject>();
		IdProp->SetStringField(TEXT("type"), TEXT("string"));
		IdProp->SetStringField(TEXT("description"), TEXT("Unique step identifier for result correlation"));
		StepItemProps->SetObjectField(TEXT("id"), IdProp);

		TSharedPtr<FJsonObject> ToolProp = MakeShared<FJsonObject>();
		ToolProp->SetStringField(TEXT("type"), TEXT("string"));
		ToolProp->SetStringField(TEXT("description"), TEXT("Tool name to execute (e.g., 'blueprint.add_variable', 'blueprint.apply_plan_json')"));
		StepItemProps->SetObjectField(TEXT("tool"), ToolProp);

		TSharedPtr<FJsonObject> ArgsProp = MakeShared<FJsonObject>();
		ArgsProp->SetStringField(TEXT("type"), TEXT("object"));
		ArgsProp->SetStringField(TEXT("description"), TEXT("Arguments to pass to the tool"));
		StepItemProps->SetObjectField(TEXT("args"), ArgsProp);

		TSharedPtr<FJsonObject> StepItemSchema = MakeShared<FJsonObject>();
		StepItemSchema->SetStringField(TEXT("type"), TEXT("object"));
		StepItemSchema->SetObjectField(TEXT("properties"), StepItemProps);

		TArray<TSharedPtr<FJsonValue>> StepRequired;
		StepRequired.Add(MakeShared<FJsonValueString>(TEXT("tool")));
		StepItemSchema->SetArrayField(TEXT("required"), StepRequired);

		TSharedPtr<FJsonObject> StepsProp = MakeShared<FJsonObject>();
		StepsProp->SetStringField(TEXT("type"), TEXT("array"));
		StepsProp->SetStringField(TEXT("description"), TEXT("Ordered list of operations to execute"));
		StepsProp->SetObjectField(TEXT("items"), StepItemSchema);

		Properties->SetObjectField(TEXT("steps"), StepsProp);
	}

	Schema->SetObjectField(TEXT("properties"), Properties);

	// Required fields
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("steps")));
	Schema->SetArrayField(TEXT("required"), Required);

	FOliveToolRegistry::Get().RegisterTool(
		TEXT("olive.build"),
		TEXT("Batch executor. Use this whenever you need 3 or more tool calls -- create, add_component, "
			 "add_variable, apply_plan_json, compile, read, verify all count. Single call, atomic, faster "
			 "than calling tools individually. Snapshot created automatically for rollback."),
		Schema,
		FOliveToolHandler::CreateStatic(&FOliveBuildTool::HandleBuild),
		{ TEXT("olive"), TEXT("build"), TEXT("batch") },
		TEXT("olive")
	);
}

FOliveToolResult FOliveBuildTool::HandleBuild(const TSharedPtr<FJsonObject>& Params)
{
	const double StartTime = FPlatformTime::Seconds();

	// ---- Recursion guard ----
	if (bIsExecuting)
	{
		return FOliveToolResult::Error(
			TEXT("BUILD_RECURSION"),
			TEXT("olive.build cannot be called from within olive.build"),
			TEXT("Remove the nested olive.build call and list all steps in a single olive.build invocation."));
	}

	// ---- Parse params ----
	FString Description;
	Params->TryGetStringField(TEXT("description"), Description);

	bool bSnapshot = true;
	if (Params->HasField(TEXT("snapshot")))
	{
		bSnapshot = Params->GetBoolField(TEXT("snapshot"));
	}

	FString OnError = TEXT("stop");
	Params->TryGetStringField(TEXT("on_error"), OnError);
	const bool bStopOnError = (OnError != TEXT("continue_remaining"));

	const TArray<TSharedPtr<FJsonValue>>* StepsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("steps"), StepsArray) || !StepsArray || StepsArray->Num() == 0)
	{
		return FOliveToolResult::Error(
			TEXT("BUILD_MISSING_STEPS"),
			TEXT("'steps' array is required and must not be empty"),
			TEXT("Provide an array of {id, tool, args} objects."));
	}

	// ---- Parse and validate all steps upfront ----
	struct FBuildStep
	{
		FString Id;
		FString ToolName;
		TSharedPtr<FJsonObject> Args;
	};
	TArray<FBuildStep> Steps;
	Steps.Reserve(StepsArray->Num());

	for (int32 i = 0; i < StepsArray->Num(); i++)
	{
		const TSharedPtr<FJsonObject>* StepObj = nullptr;
		if (!(*StepsArray)[i]->TryGetObject(StepObj) || !StepObj || !(*StepObj).IsValid())
		{
			return FOliveToolResult::Error(
				TEXT("BUILD_INVALID_STEP"),
				FString::Printf(TEXT("Step %d is not a valid object"), i),
				TEXT("Each step must be {id, tool, args}."));
		}

		FBuildStep Step;
		if (!(*StepObj)->TryGetStringField(TEXT("id"), Step.Id) || Step.Id.IsEmpty())
		{
			Step.Id = FString::Printf(TEXT("step_%d"), i);
		}

		if (!(*StepObj)->TryGetStringField(TEXT("tool"), Step.ToolName) || Step.ToolName.IsEmpty())
		{
			return FOliveToolResult::Error(
				TEXT("BUILD_MISSING_TOOL"),
				FString::Printf(TEXT("Step '%s' (index %d) missing 'tool' field"), *Step.Id, i),
				TEXT("Each step must specify a tool name."));
		}

		// Reject recursive olive.build calls
		if (Step.ToolName == TEXT("olive.build"))
		{
			return FOliveToolResult::Error(
				TEXT("BUILD_RECURSION"),
				FString::Printf(TEXT("Step '%s' (index %d) calls olive.build -- nesting is not allowed"), *Step.Id, i),
				TEXT("List all operations as flat steps in a single olive.build call."));
		}

		// Validate tool exists (checks aliases too)
		FString ResolvedName = Step.ToolName;
		TSharedPtr<FJsonObject> DummyParams = MakeShared<FJsonObject>();
		FOliveToolRegistry::Get().ResolveAlias(ResolvedName, DummyParams);

		if (!FOliveToolRegistry::Get().HasTool(ResolvedName))
		{
			return FOliveToolResult::Error(
				TEXT("BUILD_UNKNOWN_TOOL"),
				FString::Printf(TEXT("Step '%s': tool '%s' not found"), *Step.Id, *Step.ToolName),
				TEXT("Check tool name spelling. Use tools/list to see available tools."));
		}

		const TSharedPtr<FJsonObject>* ArgsObj = nullptr;
		if ((*StepObj)->TryGetObjectField(TEXT("args"), ArgsObj) && ArgsObj && (*ArgsObj).IsValid())
		{
			Step.Args = *ArgsObj;
		}
		else
		{
			Step.Args = MakeShared<FJsonObject>();
		}

		Steps.Add(MoveTemp(Step));
	}

	// ---- Create snapshot if requested ----
	FString SnapshotId;
	if (bSnapshot)
	{
		// Collect unique asset paths from steps for snapshot
		TSet<FString> AssetPaths;
		for (const FBuildStep& Step : Steps)
		{
			FString Path;
			if (Step.Args->TryGetStringField(TEXT("path"), Path) ||
				Step.Args->TryGetStringField(TEXT("asset_path"), Path))
			{
				AssetPaths.Add(Path);
			}
		}

		if (AssetPaths.Num() > 0)
		{
			FOliveToolResult SnapResult = FOliveSnapshotManager::Get().CreateSnapshot(
				TEXT("olive.build"),
				AssetPaths.Array(),
				Description.IsEmpty() ? TEXT("Batch build") : Description);

			if (SnapResult.bSuccess && SnapResult.Data.IsValid())
			{
				SnapResult.Data->TryGetStringField(TEXT("snapshot_id"), SnapshotId);
			}
		}
	}

	// ---- Execute steps ----
	int32 CompletedCount = 0;
	int32 FailedCount = 0;
	TArray<TSharedPtr<FJsonValue>> StepResults;
	StepResults.Reserve(Steps.Num());

	// Batch-level error pattern tracking for enriched failure guidance
	int32 PinNodeErrorCount = 0;
	bool bHasPlanJsonRollback = false;

	UE_LOG(LogOliveAI, Log, TEXT("olive.build: executing %d steps%s"),
		Steps.Num(),
		*(!Description.IsEmpty() ? FString::Printf(TEXT(" (%s)"), *Description) : TEXT("")));

	// Set recursion guard
	bIsExecuting = true;

	for (int32 i = 0; i < Steps.Num(); i++)
	{
		const FBuildStep& Step = Steps[i];
		const double StepStart = FPlatformTime::Seconds();

		FOliveToolResult StepResult = FOliveToolRegistry::Get().ExecuteTool(Step.ToolName, Step.Args);

		const double StepDuration = (FPlatformTime::Seconds() - StepStart) * 1000.0;

		// Build step result JSON
		TSharedPtr<FJsonObject> StepJson = MakeShared<FJsonObject>();
		StepJson->SetStringField(TEXT("id"), Step.Id);
		StepJson->SetStringField(TEXT("tool"), Step.ToolName);
		StepJson->SetBoolField(TEXT("success"), StepResult.bSuccess);
		StepJson->SetNumberField(TEXT("duration_ms"), FMath::RoundToInt(StepDuration));

		if (StepResult.bSuccess)
		{
			CompletedCount++;

			// Include a compact summary from the result data
			if (StepResult.Data.IsValid())
			{
				FString Summary;
				if (StepResult.Data->TryGetStringField(TEXT("message"), Summary) ||
					StepResult.Data->TryGetStringField(TEXT("summary"), Summary))
				{
					StepJson->SetStringField(TEXT("summary"), Summary);
				}
			}

			UE_LOG(LogOliveAI, Log, TEXT("  [%d/%d] %s (%s) - OK (%.1fms)"),
				i + 1, Steps.Num(), *Step.Id, *Step.ToolName, StepDuration);
		}
		else
		{
			FailedCount++;

			// Include error details
			FString ErrorMsg;
			FString ErrorCode;
			if (StepResult.Messages.Num() > 0)
			{
				ErrorMsg = StepResult.Messages[0].Message;
				ErrorCode = StepResult.Messages[0].Code;
			}
			StepJson->SetStringField(TEXT("error"), ErrorMsg);

			if (!StepResult.NextStepGuidance.IsEmpty())
			{
				StepJson->SetStringField(TEXT("suggestion"), StepResult.NextStepGuidance);
			}

			if (StepResult.Data.IsValid())
			{
				StepJson->SetObjectField(TEXT("data"), StepResult.Data);
			}

			// Track error patterns for batch-level analysis
			if (ErrorCode == TEXT("BP_CONNECT_PINS_FAILED") ||
				ErrorCode == TEXT("BP_SET_PIN_DEFAULT_FAILED") ||
				ErrorMsg.Contains(TEXT("pin")) || ErrorMsg.Contains(TEXT("node_")))
			{
				PinNodeErrorCount++;
			}
			if (Step.ToolName == TEXT("blueprint.apply_plan_json") &&
				ErrorMsg.Contains(TEXT("ROLLED BACK")))
			{
				bHasPlanJsonRollback = true;
			}

			// Error-code-aware fix guidance for MCP clients
			FString FixGuidance;
			if (ErrorCode == TEXT("BP_CONNECT_PINS_FAILED") || ErrorCode == TEXT("BP_SET_PIN_DEFAULT_FAILED"))
			{
				FixGuidance = TEXT("Call blueprint.read(section:'graph', mode:'full') to get current node IDs and pin names before retrying.");
			}
			else if (ErrorCode == TEXT("BP_ADD_NODE_FAILED"))
			{
				FixGuidance = TEXT("Check node type and required properties. Use blueprint.describe_node_type to see available properties.");
			}
			else if (ErrorCode == TEXT("PLAN_VALIDATE_FAILED") || ErrorCode == TEXT("PLAN_VALIDATION_FAILED"))
			{
				FixGuidance = TEXT("Check exec_after and exec_outputs syntax. exec_outputs goes ON the branch step, not as exec_after on downstream steps.");
			}
			else if (ErrorCode == TEXT("PLAN_EXECUTION_FAILED") || ErrorCode == TEXT("PLAN_LOWER_FAILED"))
			{
				FixGuidance = TEXT("Plan execution failed and nodes were rolled back. Node IDs from step_to_node_map are INVALID. Call blueprint.read(section:'graph', mode:'full') to get current graph state.");
			}
			else if (ErrorCode == TEXT("FUNCTION_NOT_FOUND"))
			{
				FixGuidance = TEXT("Use blueprint.describe_function(function_name, target_class) to verify the function exists and get exact pin names.");
			}
			else if (ErrorCode == TEXT("ASSET_NOT_FOUND"))
			{
				FixGuidance = TEXT("Asset not found at given path. Call project.search to find the correct asset path.");
			}
			else if (ErrorCode == TEXT("DUPLICATE_NATIVE_EVENT"))
			{
				FixGuidance = TEXT("This event already exists in the graph. Use blueprint.read to find the existing event node and wire from it instead of creating a new one.");
			}

			if (!FixGuidance.IsEmpty())
			{
				StepJson->SetStringField(TEXT("fix_guidance"), FixGuidance);
			}

			UE_LOG(LogOliveAI, Warning, TEXT("  [%d/%d] %s (%s) - FAILED: %s"),
				i + 1, Steps.Num(), *Step.Id, *Step.ToolName, *ErrorMsg);

			// Add the failed step result BEFORE marking remaining as skipped
			StepResults.Add(MakeShared<FJsonValueObject>(StepJson));

			if (bStopOnError)
			{
				// Mark remaining steps as skipped
				for (int32 j = i + 1; j < Steps.Num(); j++)
				{
					TSharedPtr<FJsonObject> SkipJson = MakeShared<FJsonObject>();
					SkipJson->SetStringField(TEXT("id"), Steps[j].Id);
					SkipJson->SetStringField(TEXT("tool"), Steps[j].ToolName);
					SkipJson->SetBoolField(TEXT("success"), false);
					SkipJson->SetStringField(TEXT("error"),
						TEXT("Skipped (previous step failed with on_error='stop')"));
					StepResults.Add(MakeShared<FJsonValueObject>(SkipJson));
				}
				break;
			}

			// In continue_remaining mode, fall through to next step
			continue;
		}

		StepResults.Add(MakeShared<FJsonValueObject>(StepJson));
	}

	// Clear recursion guard
	bIsExecuting = false;

	const double TotalDuration = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	UE_LOG(LogOliveAI, Log, TEXT("olive.build: complete -- %d/%d succeeded, %d failed (%.1fms)"),
		CompletedCount, Steps.Num(), FailedCount, TotalDuration);

	// ---- Build result ----
	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetBoolField(TEXT("success"), FailedCount == 0);
	ResultData->SetNumberField(TEXT("total_steps"), Steps.Num());
	ResultData->SetNumberField(TEXT("completed"), CompletedCount);
	ResultData->SetNumberField(TEXT("failed"), FailedCount);
	ResultData->SetNumberField(TEXT("duration_ms"), FMath::RoundToInt(TotalDuration));
	ResultData->SetArrayField(TEXT("steps"), StepResults);

	if (!SnapshotId.IsEmpty())
	{
		ResultData->SetStringField(TEXT("snapshot_id"), SnapshotId);
	}

	if (FailedCount == 0)
	{
		return FOliveToolResult::Success(ResultData);
	}
	else
	{
		// Return with embedded failure info so the caller gets full step details,
		// but mark bSuccess = false so it knows errors occurred
		FOliveToolResult Result;
		Result.bSuccess = false;
		Result.Data = ResultData;

		FOliveIRMessage ErrorMessage;
		ErrorMessage.Severity = EOliveIRSeverity::Error;
		ErrorMessage.Code = TEXT("BUILD_PARTIAL_FAILURE");
		ErrorMessage.Message = FString::Printf(TEXT("%d of %d steps failed"), FailedCount, Steps.Num());
		Result.Messages.Add(MoveTemp(ErrorMessage));

		// Build batch-level guidance based on error patterns observed during execution
		FString BatchGuidance;

		if (bHasPlanJsonRollback)
		{
			BatchGuidance += TEXT("PLAN_JSON ROLLBACK: All nodes from the failed apply_plan_json were removed. "
				"Node IDs from step_to_node_map are INVALID. Call blueprint.read(section:'graph', mode:'full') "
				"to get the current graph state before referencing any nodes. ");
		}

		if (PinNodeErrorCount >= 2)
		{
			BatchGuidance += TEXT("BEFORE RETRYING: call blueprint.read(section:'graph', mode:'full') "
				"on the target to get actual node IDs and pin names. ");
		}

		if (FailedCount >= 3 && CompletedCount == 0)
		{
			BatchGuidance += TEXT("ALL STEPS FAILED. Switch to granular tools (add_node + connect_pins "
				"one at a time) instead of retrying the same batch. ");
		}
		else if (FailedCount > 0 && CompletedCount > 0)
		{
			BatchGuidance += TEXT("Re-run olive.build with ONLY the corrected failed steps "
				"(successful steps are already done). ");
		}

		if (BatchGuidance.IsEmpty())
		{
			BatchGuidance = TEXT("Review each failed step's 'fix_guidance' for specific corrections.");
		}

		Result.NextStepGuidance = BatchGuidance;
		return Result;
	}
}
