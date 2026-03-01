// Copyright Bode Software. All Rights Reserved.

#include "IR/BlueprintPlanIR.h"
#include "OliveAIRuntimeModule.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// ---------------------------------------------------------------------------
// OlivePlanOps namespace
// ---------------------------------------------------------------------------

namespace OlivePlanOps
{
	bool IsValidOp(const FString& Op)
	{
		return GetAllOps().Contains(Op);
	}

	const TSet<FString>& GetAllOps()
	{
		static const TSet<FString> Ops = {
			Call, GetVar, SetVar, Branch, Sequence, Cast,
			Event, CustomEvent, ForLoop, ForEachLoop, Delay,
			IsValid, PrintString, SpawnActor, MakeStruct,
			BreakStruct, Return, Comment,
			WhileLoop, DoOnce, FlipFlop, Gate,
			CallDelegate, CallDispatcher, BindDispatcher
		};
		return Ops;
	}
}

// ---------------------------------------------------------------------------
// Helper: serialize TMap<FString, FString> to JSON object
// ---------------------------------------------------------------------------

namespace
{
	TSharedPtr<FJsonObject> StringMapToJson(const TMap<FString, FString>& Map)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		for (const auto& Pair : Map)
		{
			Json->SetStringField(Pair.Key, Pair.Value);
		}
		return Json;
	}

	/**
	 * Convert a JSON object to a string map, handling type mismatches gracefully.
	 * String, Number, and Boolean values are handled via TryGetString (all three
	 * JSON value types implement it). Array values are coerced to their first
	 * element with a warning log, preventing silent data loss when the AI sends
	 * e.g. "exec_outputs": {"True": ["step_a", "step_b"]}.
	 */
	TMap<FString, FString> JsonToStringMap(const TSharedPtr<FJsonObject>& Json)
	{
		TMap<FString, FString> Map;
		if (Json.IsValid())
		{
			for (const auto& Pair : Json->Values)
			{
				FString StringValue;
				if (Pair.Value->TryGetString(StringValue))
				{
					// String, Number, Boolean all implement TryGetString
					Map.Add(Pair.Key, StringValue);
				}
				else if (Pair.Value->Type == EJson::Array)
				{
					// AI sent array where string expected -- take first element
					const TArray<TSharedPtr<FJsonValue>>& Arr = Pair.Value->AsArray();
					if (Arr.Num() > 0 && Arr[0]->TryGetString(StringValue))
					{
						Map.Add(Pair.Key, StringValue);
						UE_LOG(LogOliveAIRuntime, Warning,
							TEXT("JsonToStringMap: Key '%s' has Array value, "
								 "coerced to first element '%s' (%d elements total)"),
							*Pair.Key, *StringValue, Arr.Num());
					}
					else
					{
						UE_LOG(LogOliveAIRuntime, Warning,
							TEXT("JsonToStringMap: Key '%s' has empty or non-string Array, skipping"),
							*Pair.Key);
					}
				}
				else
				{
					// Null or Object -- skip with warning
					UE_LOG(LogOliveAIRuntime, Warning,
						TEXT("JsonToStringMap: Key '%s' has unsupported JSON type (EJson=%d), skipping"),
						*Pair.Key, static_cast<int32>(Pair.Value->Type));
				}
			}
		}
		return Map;
	}

	TArray<TSharedPtr<FJsonValue>> StringArrayToJsonArray(const TArray<FString>& Arr)
	{
		TArray<TSharedPtr<FJsonValue>> JsonArr;
		JsonArr.Reserve(Arr.Num());
		for (const FString& Str : Arr)
		{
			JsonArr.Add(MakeShared<FJsonValueString>(Str));
		}
		return JsonArr;
	}

	TArray<FString> JsonArrayToStringArray(const TArray<TSharedPtr<FJsonValue>>& JsonArr)
	{
		TArray<FString> Arr;
		Arr.Reserve(JsonArr.Num());
		for (const TSharedPtr<FJsonValue>& Val : JsonArr)
		{
			Arr.Add(Val->AsString());
		}
		return Arr;
	}
}

// ---------------------------------------------------------------------------
// FOliveIRBlueprintPlanStep
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FOliveIRBlueprintPlanStep::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

	Json->SetStringField(TEXT("step_id"), StepId);
	Json->SetStringField(TEXT("op"), Op);

	if (!Target.IsEmpty())
	{
		Json->SetStringField(TEXT("target"), Target);
	}

	if (!TargetClass.IsEmpty())
	{
		Json->SetStringField(TEXT("target_class"), TargetClass);
	}

	if (Inputs.Num() > 0)
	{
		Json->SetObjectField(TEXT("inputs"), StringMapToJson(Inputs));
	}

	if (Properties.Num() > 0)
	{
		Json->SetObjectField(TEXT("properties"), StringMapToJson(Properties));
	}

	if (!ExecAfter.IsEmpty())
	{
		Json->SetStringField(TEXT("exec_after"), ExecAfter);
	}

	if (ExecOutputs.Num() > 0)
	{
		Json->SetObjectField(TEXT("exec_outputs"), StringMapToJson(ExecOutputs));
	}

	return Json;
}

FOliveIRBlueprintPlanStep FOliveIRBlueprintPlanStep::FromJson(const TSharedPtr<FJsonObject>& Json)
{
	FOliveIRBlueprintPlanStep Step;
	if (!Json.IsValid())
	{
		return Step;
	}

	Json->TryGetStringField(TEXT("step_id"), Step.StepId);
	Json->TryGetStringField(TEXT("op"), Step.Op);
	Json->TryGetStringField(TEXT("target"), Step.Target);
	Json->TryGetStringField(TEXT("target_class"), Step.TargetClass);
	Json->TryGetStringField(TEXT("exec_after"), Step.ExecAfter);

	// Fallback: if exec_after was sent as an array, take first element.
	// exec_after is semantically a single predecessor step ID; if the AI
	// sends an array, the first element is the primary predecessor.
	if (Step.ExecAfter.IsEmpty())
	{
		const TArray<TSharedPtr<FJsonValue>>* ExecAfterArr = nullptr;
		if (Json->TryGetArrayField(TEXT("exec_after"), ExecAfterArr)
			&& ExecAfterArr && ExecAfterArr->Num() > 0)
		{
			FString FirstVal;
			if ((*ExecAfterArr)[0]->TryGetString(FirstVal) && !FirstVal.IsEmpty())
			{
				Step.ExecAfter = FirstVal;
				UE_LOG(LogOliveAIRuntime, Warning,
					TEXT("Plan step '%s': exec_after was Array, coerced to first element '%s'"),
					*Step.StepId, *FirstVal);
			}
		}
	}

	const TSharedPtr<FJsonObject>* InputsObj = nullptr;
	if (Json->TryGetObjectField(TEXT("inputs"), InputsObj))
	{
		Step.Inputs = JsonToStringMap(*InputsObj);
	}

	const TSharedPtr<FJsonObject>* PropertiesObj = nullptr;
	if (Json->TryGetObjectField(TEXT("properties"), PropertiesObj))
	{
		Step.Properties = JsonToStringMap(*PropertiesObj);
	}

	const TSharedPtr<FJsonObject>* ExecOutputsObj = nullptr;
	if (Json->TryGetObjectField(TEXT("exec_outputs"), ExecOutputsObj))
	{
		Step.ExecOutputs = JsonToStringMap(*ExecOutputsObj);
	}

	return Step;
}

// ---------------------------------------------------------------------------
// FOliveIRBlueprintPlan
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FOliveIRBlueprintPlan::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

	Json->SetStringField(TEXT("schema_version"), SchemaVersion);

	TArray<TSharedPtr<FJsonValue>> StepsArr;
	StepsArr.Reserve(Steps.Num());
	for (const FOliveIRBlueprintPlanStep& Step : Steps)
	{
		StepsArr.Add(MakeShared<FJsonValueObject>(Step.ToJson()));
	}
	Json->SetArrayField(TEXT("steps"), StepsArr);

	return Json;
}

FOliveIRBlueprintPlan FOliveIRBlueprintPlan::FromJson(const TSharedPtr<FJsonObject>& Json)
{
	FOliveIRBlueprintPlan Plan;
	if (!Json.IsValid())
	{
		return Plan;
	}

	Json->TryGetStringField(TEXT("schema_version"), Plan.SchemaVersion);

	const TArray<TSharedPtr<FJsonValue>>* StepsArr = nullptr;
	if (Json->TryGetArrayField(TEXT("steps"), StepsArr))
	{
		Plan.Steps.Reserve(StepsArr->Num());
		for (const TSharedPtr<FJsonValue>& StepVal : *StepsArr)
		{
			const TSharedPtr<FJsonObject>* StepObj = nullptr;
			if (StepVal->TryGetObject(StepObj))
			{
				Plan.Steps.Add(FOliveIRBlueprintPlanStep::FromJson(*StepObj));
			}
		}
	}

	return Plan;
}

// ---------------------------------------------------------------------------
// FOliveIRBlueprintPlanError
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FOliveIRBlueprintPlanError::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

	Json->SetStringField(TEXT("error_code"), ErrorCode);

	if (!StepId.IsEmpty())
	{
		Json->SetStringField(TEXT("step_id"), StepId);
	}

	if (!LocationPointer.IsEmpty())
	{
		Json->SetStringField(TEXT("location"), LocationPointer);
	}

	Json->SetStringField(TEXT("message"), Message);

	if (!Suggestion.IsEmpty())
	{
		Json->SetStringField(TEXT("suggestion"), Suggestion);
	}

	if (Alternatives.Num() > 0)
	{
		Json->SetArrayField(TEXT("alternatives"), StringArrayToJsonArray(Alternatives));
	}

	return Json;
}

FOliveIRBlueprintPlanError FOliveIRBlueprintPlanError::FromJson(const TSharedPtr<FJsonObject>& Json)
{
	FOliveIRBlueprintPlanError Error;
	if (!Json.IsValid())
	{
		return Error;
	}

	Json->TryGetStringField(TEXT("error_code"), Error.ErrorCode);
	Json->TryGetStringField(TEXT("step_id"), Error.StepId);
	Json->TryGetStringField(TEXT("location"), Error.LocationPointer);
	Json->TryGetStringField(TEXT("message"), Error.Message);
	Json->TryGetStringField(TEXT("suggestion"), Error.Suggestion);

	const TArray<TSharedPtr<FJsonValue>>* AlternativesArr = nullptr;
	if (Json->TryGetArrayField(TEXT("alternatives"), AlternativesArr))
	{
		Error.Alternatives = JsonArrayToStringArray(*AlternativesArr);
	}

	return Error;
}

FOliveIRBlueprintPlanError FOliveIRBlueprintPlanError::MakeStepError(
	const FString& Code,
	const FString& InStepId,
	const FString& InLocation,
	const FString& InMessage,
	const FString& InSuggestion)
{
	FOliveIRBlueprintPlanError Error;
	Error.ErrorCode = Code;
	Error.StepId = InStepId;
	Error.LocationPointer = InLocation;
	Error.Message = InMessage;
	Error.Suggestion = InSuggestion;
	return Error;
}

FOliveIRBlueprintPlanError FOliveIRBlueprintPlanError::MakePlanError(
	const FString& Code,
	const FString& InMessage,
	const FString& InSuggestion)
{
	FOliveIRBlueprintPlanError Error;
	Error.ErrorCode = Code;
	Error.Message = InMessage;
	Error.Suggestion = InSuggestion;
	return Error;
}

// ---------------------------------------------------------------------------
// FOliveIRBlueprintPlanResult
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FOliveIRBlueprintPlanResult::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

	Json->SetBoolField(TEXT("success"), bSuccess);
	Json->SetNumberField(TEXT("applied_ops_count"), AppliedOpsCount);
	Json->SetBoolField(TEXT("partial"), bPartial);
	Json->SetNumberField(TEXT("connections_succeeded"), ConnectionsSucceeded);
	Json->SetNumberField(TEXT("connections_failed"), ConnectionsFailed);
	Json->SetNumberField(TEXT("defaults_succeeded"), DefaultsSucceeded);
	Json->SetNumberField(TEXT("defaults_failed"), DefaultsFailed);

	if (StepToNodeMap.Num() > 0)
	{
		Json->SetObjectField(TEXT("step_to_node_map"), StringMapToJson(StepToNodeMap));
	}

	if (CompileResult.IsSet())
	{
		Json->SetObjectField(TEXT("compile_result"), CompileResult.GetValue().ToJson());
	}

	if (Errors.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ErrorsArr;
		ErrorsArr.Reserve(Errors.Num());
		for (const FOliveIRBlueprintPlanError& Error : Errors)
		{
			ErrorsArr.Add(MakeShared<FJsonValueObject>(Error.ToJson()));
		}
		Json->SetArrayField(TEXT("errors"), ErrorsArr);
	}

	if (Warnings.Num() > 0)
	{
		Json->SetArrayField(TEXT("warnings"), StringArrayToJsonArray(Warnings));
	}

	if (PinManifestJsons.Num() > 0)
	{
		TSharedPtr<FJsonObject> ManifestsObj = MakeShared<FJsonObject>();
		for (const auto& Pair : PinManifestJsons)
		{
			ManifestsObj->SetObjectField(Pair.Key, Pair.Value);
		}
		Json->SetObjectField(TEXT("pin_manifests"), ManifestsObj);
	}

	if (ConversionNotesJson.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> NotesArr;
		NotesArr.Reserve(ConversionNotesJson.Num());
		for (const TSharedPtr<FJsonObject>& NoteJson : ConversionNotesJson)
		{
			if (NoteJson.IsValid())
			{
				NotesArr.Add(MakeShared<FJsonValueObject>(NoteJson));
			}
		}
		Json->SetArrayField(TEXT("conversion_notes"), NotesArr);
	}

	return Json;
}

FOliveIRBlueprintPlanResult FOliveIRBlueprintPlanResult::FromJson(const TSharedPtr<FJsonObject>& Json)
{
	FOliveIRBlueprintPlanResult Result;
	if (!Json.IsValid())
	{
		return Result;
	}

	Json->TryGetBoolField(TEXT("success"), Result.bSuccess);
	Json->TryGetNumberField(TEXT("applied_ops_count"), Result.AppliedOpsCount);
	Json->TryGetBoolField(TEXT("partial"), Result.bPartial);
	Json->TryGetNumberField(TEXT("connections_succeeded"), Result.ConnectionsSucceeded);
	Json->TryGetNumberField(TEXT("connections_failed"), Result.ConnectionsFailed);
	Json->TryGetNumberField(TEXT("defaults_succeeded"), Result.DefaultsSucceeded);
	Json->TryGetNumberField(TEXT("defaults_failed"), Result.DefaultsFailed);

	const TSharedPtr<FJsonObject>* StepMapObj = nullptr;
	if (Json->TryGetObjectField(TEXT("step_to_node_map"), StepMapObj))
	{
		Result.StepToNodeMap = JsonToStringMap(*StepMapObj);
	}

	const TSharedPtr<FJsonObject>* CompileObj = nullptr;
	if (Json->TryGetObjectField(TEXT("compile_result"), CompileObj))
	{
		Result.CompileResult = FOliveIRCompileResult::FromJson(*CompileObj);
	}

	const TArray<TSharedPtr<FJsonValue>>* ErrorsArr = nullptr;
	if (Json->TryGetArrayField(TEXT("errors"), ErrorsArr))
	{
		Result.Errors.Reserve(ErrorsArr->Num());
		for (const TSharedPtr<FJsonValue>& Val : *ErrorsArr)
		{
			const TSharedPtr<FJsonObject>* ErrObj = nullptr;
			if (Val->TryGetObject(ErrObj))
			{
				Result.Errors.Add(FOliveIRBlueprintPlanError::FromJson(*ErrObj));
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* WarningsArr = nullptr;
	if (Json->TryGetArrayField(TEXT("warnings"), WarningsArr))
	{
		Result.Warnings = JsonArrayToStringArray(*WarningsArr);
	}

	const TSharedPtr<FJsonObject>* ManifestsObj = nullptr;
	if (Json->TryGetObjectField(TEXT("pin_manifests"), ManifestsObj))
	{
		for (const auto& Pair : (*ManifestsObj)->Values)
		{
			const TSharedPtr<FJsonObject>* ManifestObj = nullptr;
			if (Pair.Value->TryGetObject(ManifestObj))
			{
				Result.PinManifestJsons.Add(Pair.Key, *ManifestObj);
			}
		}
	}

	return Result;
}

FOliveIRBlueprintPlanResult FOliveIRBlueprintPlanResult::Success(
	const TMap<FString, FString>& InStepToNodeMap,
	int32 InAppliedOpsCount)
{
	FOliveIRBlueprintPlanResult Result;
	Result.bSuccess = true;
	Result.StepToNodeMap = InStepToNodeMap;
	Result.AppliedOpsCount = InAppliedOpsCount;
	return Result;
}

FOliveIRBlueprintPlanResult FOliveIRBlueprintPlanResult::Failure(
	const TArray<FOliveIRBlueprintPlanError>& InErrors)
{
	FOliveIRBlueprintPlanResult Result;
	Result.bSuccess = false;
	Result.Errors = InErrors;
	return Result;
}
