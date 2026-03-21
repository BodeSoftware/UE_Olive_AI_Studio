// Copyright Bode Software. All Rights Reserved.

#include "IR/OliveIRSchema.h"
#include "IR/BlueprintPlanIR.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogOliveIRSchema, Log, All);

// ============================================================================
// Forbidden Field Names (Rule R4 - Positions omitted)
// ============================================================================

static TArray<FString> GForbiddenFieldNames = {
	TEXT("position_x"),
	TEXT("position_y"),
	TEXT("node_pos_x"),
	TEXT("node_pos_y"),
	TEXT("pos_x"),
	TEXT("pos_y"),
	TEXT("location_x"),
	TEXT("location_y"),
	TEXT("node_position"),
	TEXT("graph_position"),
	TEXT("visual_position"),
	TEXT("editor_position")
};

const TArray<FString>& FOliveIRValidator::GetForbiddenFieldNames()
{
	return GForbiddenFieldNames;
}

// ============================================================================
// FOliveIRValidator - Blueprint IR Validation
// ============================================================================

FOliveIRResult FOliveIRValidator::ValidateBlueprintIR(const TSharedPtr<FJsonObject>& Json, bool bStrict)
{
	FOliveIRResult Result;
	Result.bSuccess = true;

	if (!Json.IsValid())
	{
		Result.bSuccess = false;
		Result.ErrorCode = TEXT("IR_INVALID_JSON");
		Result.ErrorMessage = TEXT("Blueprint IR JSON is null or invalid");
		return Result;
	}

	// Check required fields
	if (!Json->HasField(TEXT("name")))
	{
		Result.bSuccess = false;
		Result.ErrorCode = TEXT("IR_MISSING_FIELD");
		Result.ErrorMessage = TEXT("Blueprint IR missing required field: name");
		Result.Suggestion = TEXT("Ensure the Blueprint IR includes a 'name' field");
		return Result;
	}

	if (!Json->HasField(TEXT("path")))
	{
		Result.bSuccess = false;
		Result.ErrorCode = TEXT("IR_MISSING_FIELD");
		Result.ErrorMessage = TEXT("Blueprint IR missing required field: path");
		Result.Suggestion = TEXT("Ensure the Blueprint IR includes a 'path' field");
		return Result;
	}

	// Check schema version if present
	if (Json->HasField(TEXT("schema_version")))
	{
		FString Version = Json->GetStringField(TEXT("schema_version"));
		if (!IsSchemaVersionCompatible(Version))
		{
			Result.bSuccess = false;
			Result.ErrorCode = TEXT("IR_INCOMPATIBLE_VERSION");
			Result.ErrorMessage = FString::Printf(TEXT("Blueprint IR schema version '%s' is incompatible with current version '%s'"),
				*Version, *OliveIR::SchemaVersion);
			Result.Suggestion = TEXT("Update the IR to use a compatible schema version");
			return Result;
		}
	}

	// Strict mode: Check for forbidden fields at Blueprint level
	if (bStrict)
	{
		FString ForbiddenField;
		if (HasForbiddenFields(Json, ForbiddenField))
		{
			Result.bSuccess = false;
			Result.ErrorCode = TEXT("IR_FORBIDDEN_FIELD");
			Result.ErrorMessage = FString::Printf(TEXT("Blueprint IR contains forbidden layout field: '%s'"), *ForbiddenField);
			Result.Suggestion = TEXT("Remove position/layout fields. IR uses auto-layout on write (Rule R4)");
			return Result;
		}
	}

	// Validate variables if present (check defined_in in strict mode)
	if (Json->HasField(TEXT("variables")))
	{
		const TArray<TSharedPtr<FJsonValue>>* VarsArray;
		if (Json->TryGetArrayField(TEXT("variables"), VarsArray))
		{
			for (int32 i = 0; i < VarsArray->Num(); ++i)
			{
				const TSharedPtr<FJsonObject>* VarObj;
				if ((*VarsArray)[i]->TryGetObject(VarObj))
				{
					FOliveIRResult VarResult = ValidateVariableIR(*VarObj, bStrict);
					if (!VarResult.bSuccess)
					{
						Result.bSuccess = false;
						Result.ErrorCode = VarResult.ErrorCode;
						Result.ErrorMessage = FString::Printf(TEXT("Variable %d: %s"), i, *VarResult.ErrorMessage);
						Result.Suggestion = VarResult.Suggestion;
						return Result;
					}
				}
			}
		}
	}

	// Validate graphs if present
	if (Json->HasField(TEXT("graphs")))
	{
		const TArray<TSharedPtr<FJsonValue>>* GraphsArray;
		if (Json->TryGetArrayField(TEXT("graphs"), GraphsArray))
		{
			for (int32 i = 0; i < GraphsArray->Num(); ++i)
			{
				const TSharedPtr<FJsonObject>* GraphObj;
				if ((*GraphsArray)[i]->TryGetObject(GraphObj))
				{
					FOliveIRResult GraphResult = ValidateGraphIR(*GraphObj, bStrict);
					if (!GraphResult.bSuccess)
					{
						Result.bSuccess = false;
						Result.ErrorCode = GraphResult.ErrorCode;
						Result.ErrorMessage = FString::Printf(TEXT("Graph %d: %s"), i, *GraphResult.ErrorMessage);
						Result.Suggestion = GraphResult.Suggestion;
						return Result;
					}
				}
			}
		}
	}

	return Result;
}

// ============================================================================
// FOliveIRValidator - Graph IR Validation
// ============================================================================

FOliveIRResult FOliveIRValidator::ValidateGraphIR(const TSharedPtr<FJsonObject>& Json, bool bStrict)
{
	FOliveIRResult Result;
	Result.bSuccess = true;

	if (!Json.IsValid())
	{
		Result.bSuccess = false;
		Result.ErrorCode = TEXT("IR_INVALID_JSON");
		Result.ErrorMessage = TEXT("Graph IR JSON is null or invalid");
		return Result;
	}

	// Check required fields
	if (!Json->HasField(TEXT("name")))
	{
		Result.bSuccess = false;
		Result.ErrorCode = TEXT("IR_MISSING_FIELD");
		Result.ErrorMessage = TEXT("Graph IR missing required field: name");
		Result.Suggestion = TEXT("Ensure the Graph IR includes a 'name' field");
		return Result;
	}

	// Strict mode: Check for forbidden fields at graph level
	if (bStrict)
	{
		FString ForbiddenField;
		if (HasForbiddenFields(Json, ForbiddenField))
		{
			Result.bSuccess = false;
			Result.ErrorCode = TEXT("IR_FORBIDDEN_FIELD");
			Result.ErrorMessage = FString::Printf(TEXT("Graph IR contains forbidden layout field: '%s'"), *ForbiddenField);
			Result.Suggestion = TEXT("Remove position/layout fields. IR uses auto-layout on write (Rule R4)");
			return Result;
		}
	}

	// Validate nodes if present
	if (Json->HasField(TEXT("nodes")))
	{
		const TArray<TSharedPtr<FJsonValue>>* NodesArray;
		if (Json->TryGetArrayField(TEXT("nodes"), NodesArray))
		{
			for (int32 i = 0; i < NodesArray->Num(); ++i)
			{
				const TSharedPtr<FJsonObject>* NodeObj;
				if ((*NodesArray)[i]->TryGetObject(NodeObj))
				{
					FOliveIRResult NodeResult = ValidateNodeIR(*NodeObj, bStrict);
					if (!NodeResult.bSuccess)
					{
						Result.bSuccess = false;
						Result.ErrorCode = NodeResult.ErrorCode;
						Result.ErrorMessage = FString::Printf(TEXT("Node %d: %s"), i, *NodeResult.ErrorMessage);
						Result.Suggestion = NodeResult.Suggestion;
						return Result;
					}
				}
			}
		}
	}

	return Result;
}

// ============================================================================
// FOliveIRValidator - Node IR Validation
// ============================================================================

FOliveIRResult FOliveIRValidator::ValidateNodeIR(const TSharedPtr<FJsonObject>& Json, bool bStrict)
{
	FOliveIRResult Result;
	Result.bSuccess = true;

	if (!Json.IsValid())
	{
		Result.bSuccess = false;
		Result.ErrorCode = TEXT("IR_INVALID_JSON");
		Result.ErrorMessage = TEXT("Node IR JSON is null or invalid");
		return Result;
	}

	// Check required field: id
	if (!Json->HasField(TEXT("id")))
	{
		Result.bSuccess = false;
		Result.ErrorCode = TEXT("IR_MISSING_FIELD");
		Result.ErrorMessage = TEXT("Node IR missing required field: id");
		Result.Suggestion = TEXT("Ensure every node has a unique 'id' field (e.g., 'node_1', 'entry')");
		return Result;
	}

	FString NodeId = Json->GetStringField(TEXT("id"));

	// Strict mode: Validate node ID format (Rule R1)
	if (bStrict)
	{
		if (!IsValidNodeId(NodeId))
		{
			Result.bSuccess = false;
			Result.ErrorCode = TEXT("IR_INVALID_NODE_ID");
			Result.ErrorMessage = FString::Printf(TEXT("Node ID '%s' has invalid format"), *NodeId);
			Result.Suggestion = TEXT("Node IDs must be 'entry', 'result', 'result_<suffix>', or 'node_<number>' (Rule R1). GUID-style IDs are forbidden.");
			return Result;
		}

		// Check for forbidden fields
		FString ForbiddenField;
		if (HasForbiddenFields(Json, ForbiddenField))
		{
			Result.bSuccess = false;
			Result.ErrorCode = TEXT("IR_FORBIDDEN_FIELD");
			Result.ErrorMessage = FString::Printf(TEXT("Node '%s' contains forbidden layout field: '%s'"), *NodeId, *ForbiddenField);
			Result.Suggestion = TEXT("Remove position/layout fields. IR uses auto-layout on write (Rule R4)");
			return Result;
		}
	}

	// Validate connection strings in pins
	auto ValidatePins = [&Result, bStrict](const TArray<TSharedPtr<FJsonValue>>* PinsArray, const FString& PinType) -> bool
	{
		if (!PinsArray) return true;

		for (int32 i = 0; i < PinsArray->Num(); ++i)
		{
			const TSharedPtr<FJsonObject>* PinObj;
			if ((*PinsArray)[i]->TryGetObject(PinObj))
			{
				// Strict mode: Check for forbidden fields in pins
				if (bStrict)
				{
					FString ForbiddenField;
					if (HasForbiddenFields(*PinObj, ForbiddenField))
					{
						Result.bSuccess = false;
						Result.ErrorCode = TEXT("IR_FORBIDDEN_FIELD");
						Result.ErrorMessage = FString::Printf(TEXT("%s pin %d contains forbidden field: '%s'"),
							*PinType, i, *ForbiddenField);
						Result.Suggestion = TEXT("Remove position/layout fields from pins");
						return false;
					}
				}

				// Check connection field
				if ((*PinObj)->HasField(TEXT("connection")))
				{
					FString Connection = (*PinObj)->GetStringField(TEXT("connection"));
					if (!Connection.IsEmpty() && !IsValidConnectionString(Connection))
					{
						Result.bSuccess = false;
						Result.ErrorCode = TEXT("IR_INVALID_CONNECTION");
						Result.ErrorMessage = FString::Printf(TEXT("%s pin %d has invalid connection format: '%s'"),
							*PinType, i, *Connection);
						Result.Suggestion = TEXT("Connection strings must use 'node_id.pin_name' format (Rule R2)");
						return false;
					}
				}

				// Check connections array
				if ((*PinObj)->HasField(TEXT("connections")))
				{
					const TArray<TSharedPtr<FJsonValue>>* ConnectionsArray;
					if ((*PinObj)->TryGetArrayField(TEXT("connections"), ConnectionsArray))
					{
						for (int32 j = 0; j < ConnectionsArray->Num(); ++j)
						{
							FString Connection = (*ConnectionsArray)[j]->AsString();
							if (!Connection.IsEmpty() && !IsValidConnectionString(Connection))
							{
								Result.bSuccess = false;
								Result.ErrorCode = TEXT("IR_INVALID_CONNECTION");
								Result.ErrorMessage = FString::Printf(TEXT("%s pin %d connection %d has invalid format: '%s'"),
									*PinType, i, j, *Connection);
								Result.Suggestion = TEXT("Connection strings must use 'node_id.pin_name' format (Rule R2)");
								return false;
							}
						}
					}
				}
			}
		}
		return true;
	};

	// Validate input pins
	const TArray<TSharedPtr<FJsonValue>>* InputPins;
	if (Json->TryGetArrayField(TEXT("input_pins"), InputPins))
	{
		if (!ValidatePins(InputPins, TEXT("Input")))
		{
			return Result;
		}
	}

	// Validate output pins
	const TArray<TSharedPtr<FJsonValue>>* OutputPins;
	if (Json->TryGetArrayField(TEXT("output_pins"), OutputPins))
	{
		if (!ValidatePins(OutputPins, TEXT("Output")))
		{
			return Result;
		}
	}

	return Result;
}

// ============================================================================
// FOliveIRValidator - Variable IR Validation
// ============================================================================

FOliveIRResult FOliveIRValidator::ValidateVariableIR(const TSharedPtr<FJsonObject>& Json, bool bStrict)
{
	FOliveIRResult Result;
	Result.bSuccess = true;

	if (!Json.IsValid())
	{
		Result.bSuccess = false;
		Result.ErrorCode = TEXT("IR_INVALID_JSON");
		Result.ErrorMessage = TEXT("Variable IR JSON is null or invalid");
		return Result;
	}

	// Check required field: name
	if (!Json->HasField(TEXT("name")))
	{
		Result.bSuccess = false;
		Result.ErrorCode = TEXT("IR_MISSING_FIELD");
		Result.ErrorMessage = TEXT("Variable IR missing required field: name");
		Result.Suggestion = TEXT("Ensure every variable has a 'name' field");
		return Result;
	}

	// Strict mode: Require defined_in field (Rule R5)
	if (bStrict)
	{
		if (!Json->HasField(TEXT("defined_in")))
		{
			FString VarName = Json->GetStringField(TEXT("name"));
			Result.bSuccess = false;
			Result.ErrorCode = TEXT("IR_MISSING_DEFINED_IN");
			Result.ErrorMessage = FString::Printf(TEXT("Variable '%s' missing required 'defined_in' field"), *VarName);
			Result.Suggestion = TEXT("Add 'defined_in' field with value 'self' for Blueprint-defined or parent class name for inherited (Rule R5)");
			return Result;
		}
	}

	return Result;
}

// ============================================================================
// FOliveIRValidator - Node ID Validation (Rule R1)
// ============================================================================

bool FOliveIRValidator::IsValidNodeId(const FString& NodeId)
{
	if (NodeId.IsEmpty())
	{
		return false;
	}

	// Reject GUID-style IDs
	if (IsGuidFormat(NodeId))
	{
		return false;
	}

	// Allowed patterns:
	// - "entry" (function entry point)
	// - "result" or "result_<suffix>" (function return nodes)
	// - "node_<number>" (regular nodes)

	if (NodeId == TEXT("entry"))
	{
		return true;
	}

	if (NodeId == TEXT("result"))
	{
		return true;
	}

	if (NodeId.StartsWith(TEXT("result_")))
	{
		// result_true, result_false, result_0, etc.
		return NodeId.Len() > 7;  // Must have something after "result_"
	}

	if (NodeId.StartsWith(TEXT("node_")))
	{
		// Must have a number after "node_"
		FString Suffix = NodeId.Mid(5);
		if (Suffix.IsEmpty())
		{
			return false;
		}

		// Check that suffix is numeric
		for (const TCHAR& Char : Suffix)
		{
			if (!FChar::IsDigit(Char))
			{
				return false;
			}
		}
		return true;
	}

	// Reject anything else
	return false;
}

bool FOliveIRValidator::IsGuidFormat(const FString& Str)
{
	// GUID format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (8-4-4-4-12)
	// Total length with dashes: 36

	if (Str.Len() != 36)
	{
		return false;
	}

	// Check for dashes at expected positions
	if (Str[8] != TEXT('-') || Str[13] != TEXT('-') ||
		Str[18] != TEXT('-') || Str[23] != TEXT('-'))
	{
		return false;
	}

	// Check that other characters are hex digits
	for (int32 i = 0; i < 36; ++i)
	{
		if (i == 8 || i == 13 || i == 18 || i == 23)
		{
			continue;  // Skip dash positions
		}

		TCHAR C = Str[i];
		if (!FChar::IsHexDigit(C))
		{
			return false;
		}
	}

	return true;
}

// ============================================================================
// FOliveIRValidator - Connection Validation (Rule R2)
// ============================================================================

bool FOliveIRValidator::IsValidConnectionString(const FString& Connection)
{
	if (Connection.IsEmpty())
	{
		return false;
	}

	// Connection format: node_id.pin_name
	int32 DotIndex;
	if (!Connection.FindChar(TEXT('.'), DotIndex))
	{
		return false;
	}

	// Must have content before and after the dot
	if (DotIndex == 0 || DotIndex == Connection.Len() - 1)
	{
		return false;
	}

	return true;
}

bool FOliveIRValidator::ParseConnectionString(
	const FString& Connection,
	FString& OutNodeId,
	FString& OutPinName)
{
	if (!IsValidConnectionString(Connection))
	{
		return false;
	}

	int32 DotIndex;
	Connection.FindChar(TEXT('.'), DotIndex);

	OutNodeId = Connection.Left(DotIndex);
	OutPinName = Connection.Mid(DotIndex + 1);

	return true;
}

// ============================================================================
// FOliveIRValidator - Forbidden Fields (Rule R4)
// ============================================================================

bool FOliveIRValidator::HasForbiddenFields(const TSharedPtr<FJsonObject>& Json, FString& OutForbiddenField)
{
	if (!Json.IsValid())
	{
		return false;
	}

	const TArray<FString>& Forbidden = GetForbiddenFieldNames();

	for (const FString& FieldName : Forbidden)
	{
		if (Json->HasField(FieldName))
		{
			OutForbiddenField = FieldName;
			return true;
		}
	}

	return false;
}

// ============================================================================
// FOliveIRValidator - Schema Version (Rule R8)
// ============================================================================

bool FOliveIRValidator::IsSchemaVersionCompatible(const FString& Version)
{
	int32 Major, Minor;
	if (!ParseVersionString(Version, Major, Minor))
	{
		return false;
	}

	// Major version must match or be within supported range
	if (Major < OliveIR::MinSupportedMajor || Major > OliveIR::SchemaVersionMajor)
	{
		return false;
	}

	// If same major version, minor must be <= current
	if (Major == OliveIR::SchemaVersionMajor && Minor > OliveIR::SchemaVersionMinor)
	{
		return false;
	}

	return true;
}

bool FOliveIRValidator::ParseVersionString(
	const FString& Version,
	int32& OutMajor,
	int32& OutMinor)
{
	TArray<FString> Parts;
	Version.ParseIntoArray(Parts, TEXT("."));

	if (Parts.Num() != 2)
	{
		return false;
	}

	// Validate that both parts are numeric
	if (!Parts[0].IsNumeric() || !Parts[1].IsNumeric())
	{
		return false;
	}

	OutMajor = FCString::Atoi(*Parts[0]);
	OutMinor = FCString::Atoi(*Parts[1]);

	return true;
}

// ============================================================================
// FOliveIRValidator - Blueprint Plan JSON Validation
// ============================================================================

namespace
{
	/**
	 * Ops that require a non-empty "target" field.
	 * All other ops either have no required target or use target optionally.
	 */
	static const TSet<FString>& GetOpsRequiringTarget()
	{
		static const TSet<FString> Ops = {
			TEXT("call"),
			TEXT("get_var"),
			TEXT("set_var"),
			TEXT("event"),
			TEXT("custom_event"),
			TEXT("cast"),
			TEXT("spawn_actor"),
			TEXT("make_struct"),
			TEXT("break_struct"),
			TEXT("call_delegate"),
			TEXT("call_dispatcher"),
			TEXT("bind_dispatcher")
		};
		return Ops;
	}

	/** Helper to create an error JSON object with consistent structure. */
	TSharedPtr<FJsonObject> MakePlanValidationError(
		const FString& Code,
		const FString& Location,
		const FString& Message,
		const FString& Suggestion = TEXT(""))
	{
		TSharedPtr<FJsonObject> ErrorObj = MakeShareable(new FJsonObject());
		ErrorObj->SetStringField(TEXT("code"), Code);
		ErrorObj->SetStringField(TEXT("location"), Location);
		ErrorObj->SetStringField(TEXT("message"), Message);
		if (!Suggestion.IsEmpty())
		{
			ErrorObj->SetStringField(TEXT("suggestion"), Suggestion);
		}
		return ErrorObj;
	}
}

FOliveIRResult FOliveIRValidator::ValidateBlueprintPlanJson(
	const TSharedPtr<FJsonObject>& Json,
	int32 MaxSteps)
{
	UE_LOG(LogOliveIRSchema, Verbose, TEXT("ValidateBlueprintPlanJson: validating plan with MaxSteps=%d"), MaxSteps);

	// Collect all errors rather than failing fast, so the caller gets a complete picture
	TArray<TSharedPtr<FJsonValue>> Errors;

	auto AddError = [&Errors](const FString& Code, const FString& Location,
		const FString& Message, const FString& Suggestion = TEXT(""))
	{
		Errors.Add(MakeShareable(new FJsonValueObject(
			MakePlanValidationError(Code, Location, Message, Suggestion))));
		UE_LOG(LogOliveIRSchema, Warning, TEXT("  Schema validation error [%s] at '%s': %s"), *Code, *Location, *Message);
	};

	// --- Null check ---
	if (!Json.IsValid())
	{
		return FOliveIRResult::Error(
			TEXT("PLAN_INVALID_JSON"),
			TEXT("Plan JSON is null or invalid"));
	}

	// --- schema_version ---
	if (!Json->HasField(TEXT("schema_version")))
	{
		// Auto-default to "1.0" — the struct default handles this.
		// Log a note but do not block validation.
		UE_LOG(LogOliveIRSchema, Log, TEXT("Plan JSON missing schema_version, defaulting to 1.0"));
	}
	else
	{
		FString Version = Json->GetStringField(TEXT("schema_version"));

		// Use plan-specific version acceptance rather than the general IR version check.
		// Plan JSON v2.0 was introduced in Phase A-D; both v1.x and v2.x are valid.
		int32 PlanMajor = 0, PlanMinor = 0;
		if (!ParseVersionString(Version, PlanMajor, PlanMinor))
		{
			AddError(
				TEXT("PLAN_INVALID_VERSION_FORMAT"),
				TEXT("/schema_version"),
				FString::Printf(TEXT("Plan schema_version '%s' is not a valid version string"), *Version),
				TEXT("schema_version must be in \"major.minor\" format, e.g. \"1.0\" or \"2.0\""));
		}
		else if (PlanMajor < OliveIR::MinSupportedMajor || PlanMajor > OliveIR::MaxPlanSchemaVersionMajor)
		{
			AddError(
				TEXT("PLAN_INCOMPATIBLE_VERSION"),
				TEXT("/schema_version"),
				FString::Printf(TEXT("Plan schema version '%s' is not supported (supported major versions: %d-%d)"),
					*Version, OliveIR::MinSupportedMajor, OliveIR::MaxPlanSchemaVersionMajor),
				FString::Printf(TEXT("Use schema_version \"1.0\" or \"%d.0\""), OliveIR::MaxPlanSchemaVersionMajor));
		}
		// else: version is within supported range, continue validation
	}

	// --- steps array ---
	const TArray<TSharedPtr<FJsonValue>>* StepsArray = nullptr;
	if (!Json->HasField(TEXT("steps")))
	{
		AddError(
			TEXT("PLAN_MISSING_FIELD"),
			TEXT("/steps"),
			TEXT("Plan is missing required field 'steps'"),
			TEXT("Add a \"steps\" array containing plan step objects"));
	}
	else if (!Json->TryGetArrayField(TEXT("steps"), StepsArray) || StepsArray == nullptr)
	{
		AddError(
			TEXT("PLAN_INVALID_TYPE"),
			TEXT("/steps"),
			TEXT("'steps' must be a JSON array"),
			TEXT("Ensure 'steps' is an array of step objects, not a single object or string"));
	}
	else if (StepsArray->Num() == 0)
	{
		AddError(
			TEXT("PLAN_EMPTY_STEPS"),
			TEXT("/steps"),
			TEXT("'steps' array is empty; a plan must contain at least one step"),
			TEXT("Add at least one step to the steps array"));
	}
	else if (StepsArray->Num() > MaxSteps)
	{
		AddError(
			TEXT("PLAN_TOO_MANY_STEPS"),
			TEXT("/steps"),
			FString::Printf(TEXT("Plan has %d steps, exceeding the maximum of %d"), StepsArray->Num(), MaxSteps),
			FString::Printf(TEXT("Reduce the plan to %d steps or fewer, or split into multiple plans"), MaxSteps));
	}

	// If we don't have a valid steps array, we can't validate individual steps
	if (StepsArray == nullptr || StepsArray->Num() == 0)
	{
		// Build result from collected errors
		if (Errors.Num() > 0)
		{
			TSharedPtr<FJsonObject> ErrorData = MakeShareable(new FJsonObject());
			ErrorData->SetArrayField(TEXT("errors"), Errors);
			ErrorData->SetNumberField(TEXT("error_count"), Errors.Num());

			// First error populates primary fields
			TSharedPtr<FJsonObject> FirstError = Errors[0]->AsObject();
			return FOliveIRResult::Error(
				FirstError->GetStringField(TEXT("code")),
				FirstError->GetStringField(TEXT("message")),
				FirstError->HasField(TEXT("suggestion")) ? FirstError->GetStringField(TEXT("suggestion")) : TEXT(""));
		}
		return FOliveIRResult::Success();
	}

	// --- Per-step validation ---
	// Track step IDs for uniqueness and reference validation
	TSet<FString> DeclaredStepIds;
	// Ordered list for forward-reference checking on exec_after
	TArray<FString> StepIdOrder;

	for (int32 i = 0; i < StepsArray->Num(); ++i)
	{
		const FString StepLocation = FString::Printf(TEXT("/steps/%d"), i);

		const TSharedPtr<FJsonObject>* StepObj = nullptr;
		if (!(*StepsArray)[i]->TryGetObject(StepObj) || StepObj == nullptr || !(*StepObj).IsValid())
		{
			AddError(
				TEXT("PLAN_INVALID_STEP"),
				StepLocation,
				FString::Printf(TEXT("Step %d is not a valid JSON object"), i),
				TEXT("Each step must be a JSON object with at least 'step_id' and 'op' fields"));
			continue;
		}

		const TSharedPtr<FJsonObject>& Step = *StepObj;

		// --- step_id ---
		FString StepId;
		if (!Step->HasField(TEXT("step_id")))
		{
			AddError(
				TEXT("PLAN_MISSING_STEP_ID"),
				StepLocation + TEXT("/step_id"),
				FString::Printf(TEXT("Step %d is missing required field 'step_id'"), i),
				TEXT("Add a unique 'step_id' string to identify this step (e.g., \"s1\", \"print_node\")"));
		}
		else
		{
			StepId = Step->GetStringField(TEXT("step_id"));
			if (StepId.IsEmpty())
			{
				AddError(
					TEXT("PLAN_EMPTY_STEP_ID"),
					StepLocation + TEXT("/step_id"),
					FString::Printf(TEXT("Step %d has an empty 'step_id'"), i),
					TEXT("Provide a non-empty unique identifier for this step"));
			}
			else if (DeclaredStepIds.Contains(StepId))
			{
				AddError(
					TEXT("PLAN_DUPLICATE_STEP_ID"),
					StepLocation + TEXT("/step_id"),
					FString::Printf(TEXT("Step %d has duplicate step_id '%s'"), i, *StepId),
					FString::Printf(TEXT("Choose a unique step_id; '%s' is already used by an earlier step"), *StepId));
			}
			else
			{
				DeclaredStepIds.Add(StepId);
				StepIdOrder.Add(StepId);
			}
		}

		// --- op ---
		FString Op;
		if (!Step->HasField(TEXT("op")))
		{
			AddError(
				TEXT("PLAN_MISSING_OP"),
				StepLocation + TEXT("/op"),
				FString::Printf(TEXT("Step %d ('%s') is missing required field 'op'"), i, *StepId),
				TEXT("Add an 'op' field with a valid operation (e.g., \"call\", \"branch\", \"event\")"));
		}
		else
		{
			Op = Step->GetStringField(TEXT("op"));
			if (!OlivePlanOps::IsValidOp(Op))
			{
				// Build a hint with all valid ops
				const TSet<FString>& AllOps = OlivePlanOps::GetAllOps();
				TArray<FString> OpsList = AllOps.Array();
				OpsList.Sort();
				FString ValidOpsStr = FString::Join(OpsList, TEXT(", "));

				AddError(
					TEXT("PLAN_INVALID_OP"),
					StepLocation + TEXT("/op"),
					FString::Printf(TEXT("Step %d ('%s') has unknown op '%s'"), i, *StepId, *Op),
					FString::Printf(TEXT("Valid ops: %s"), *ValidOpsStr));
			}
		}

		// --- Per-op required fields (target) ---
		if (!Op.IsEmpty() && OlivePlanOps::IsValidOp(Op))
		{
			if (GetOpsRequiringTarget().Contains(Op))
			{
				if (!Step->HasField(TEXT("target")) || Step->GetStringField(TEXT("target")).IsEmpty())
				{
					AddError(
						TEXT("PLAN_MISSING_TARGET"),
						StepLocation + TEXT("/target"),
						FString::Printf(TEXT("Step %d ('%s'): op '%s' requires a non-empty 'target' field"), i, *StepId, *Op),
						FString::Printf(TEXT("Add a 'target' field (e.g., function name for 'call', variable name for '%s')"), *Op));
				}
			}
		}

		// --- exec_after: must reference a step_id declared EARLIER ---
		if (Step->HasField(TEXT("exec_after")))
		{
			FString ExecAfter = Step->GetStringField(TEXT("exec_after"));
			if (!ExecAfter.IsEmpty())
			{
				// 'entry' is a valid virtual target — refers to FunctionEntry node
				// in function graphs. The executor's auto-chain handles wiring.
				const bool bIsEntryRef = ExecAfter.Equals(TEXT("entry"), ESearchCase::IgnoreCase);

				// Check that the referenced step exists earlier in the array
				// (StepIdOrder contains IDs of steps processed so far, EXCLUDING current step)
				int32 CurrentStepOrderIndex = StepIdOrder.Find(StepId);
				bool bFoundEarlier = bIsEntryRef; // 'entry' is always valid

				for (int32 j = 0; j < StepIdOrder.Num(); ++j)
				{
					if (StepIdOrder[j] == ExecAfter)
					{
						// Ensure it was declared before the current step
						if (j < CurrentStepOrderIndex || CurrentStepOrderIndex == INDEX_NONE)
						{
							bFoundEarlier = true;
						}
						break;
					}
				}

				if (!bFoundEarlier)
				{
					if (!DeclaredStepIds.Contains(ExecAfter))
					{
						AddError(
							TEXT("PLAN_INVALID_EXEC_AFTER"),
							StepLocation + TEXT("/exec_after"),
							FString::Printf(TEXT("Step %d ('%s'): exec_after references unknown step_id '%s'"), i, *StepId, *ExecAfter),
							TEXT("exec_after must reference a step_id that exists in the steps array and is declared before this step"));
					}
					else
					{
						AddError(
							TEXT("PLAN_FORWARD_EXEC_AFTER"),
							StepLocation + TEXT("/exec_after"),
							FString::Printf(TEXT("Step %d ('%s'): exec_after references step '%s' which is not declared earlier in the array"), i, *StepId, *ExecAfter),
							TEXT("exec_after must reference a step_id that appears before this step in the steps array"));
					}
				}
			}
		}

		// --- inputs: validate @ref references ---
		if (Step->HasField(TEXT("inputs")))
		{
			const TSharedPtr<FJsonObject>* InputsObj = nullptr;
			if (Step->TryGetObjectField(TEXT("inputs"), InputsObj) && InputsObj != nullptr && (*InputsObj).IsValid())
			{
				for (const auto& Pair : (*InputsObj)->Values)
				{
					const FString& PinName = Pair.Key;
					FString Value;
					if (Pair.Value->TryGetString(Value) && Value.StartsWith(TEXT("@")))
					{
						// Parse "@stepId.pinName" format
						FString RefBody = Value.Mid(1); // strip leading @
						int32 DotIndex = INDEX_NONE;
						if (!RefBody.FindChar(TEXT('.'), DotIndex) || DotIndex == 0 || DotIndex == RefBody.Len() - 1)
						{
							// FIX RC2: Dotless @refs (e.g., "@InteractionSphere") may be
							// component or variable names that the plan resolver will
							// auto-expand into get_var steps. Only warn, don't hard-reject.
							// The resolver's ExpandComponentRefs pass handles these.
							UE_LOG(LogOliveIRSchema, Log,
								TEXT("Step %d ('%s'): input '%s' has dotless @ref '%s' — will be treated as component/variable reference by resolver"),
								i, *StepId, *PinName, *Value);
							continue;
						}

						FString RefStepId = RefBody.Left(DotIndex);

						// Referenced step must be declared EARLIER in the array
						int32 CurrentIdx = StepIdOrder.Find(StepId);
						bool bRefFoundEarlier = false;

						for (int32 j = 0; j < StepIdOrder.Num(); ++j)
						{
							if (StepIdOrder[j] == RefStepId)
							{
								if (j < CurrentIdx || CurrentIdx == INDEX_NONE)
								{
									bRefFoundEarlier = true;
								}
								break;
							}
						}

						if (!bRefFoundEarlier)
						{
							FString RefPinHint = RefBody.Mid(DotIndex + 1);

							if (!DeclaredStepIds.Contains(RefStepId))
							{
								// FIX RC2/RC3: Unknown step refs may be component names
								// or function parameter names that the resolver's
								// ExpandComponentRefs pass will synthesize get_var steps
								// for. Only log, don't hard-reject.
								UE_LOG(LogOliveIRSchema, Log,
									TEXT("Step %d ('%s'): input '%s' ref '@%s.%s' references unknown step '%s' — may be component/param name (resolver will handle)"),
									i, *StepId, *PinName, *RefStepId, *RefPinHint, *RefStepId);
							}
							else
							{
								AddError(
									TEXT("PLAN_FORWARD_INPUT_REF"),
									StepLocation + TEXT("/inputs/") + PinName,
									FString::Printf(TEXT("Step %d ('%s'): input '%s' references step '%s' which is not declared earlier"), i, *StepId, *PinName, *RefStepId),
									TEXT("@ref data references must point to steps declared before this step in the array"));

								FString DeclaredList = FString::Join(DeclaredStepIds.Array(), TEXT(", "));
								UE_LOG(LogOliveIRSchema, Warning,
									TEXT("  Declared step_ids at this point: [%s]. Ref '@%s.%s' is a forward reference to step '%s' (exists but declared later)"),
									*DeclaredList, *RefStepId, *RefPinHint, *RefStepId);
							}
						}
					}
				}
			}
		}

		// --- exec_outputs: verify referenced stepIds exist in the steps array (forward refs allowed) ---
		if (Step->HasField(TEXT("exec_outputs")))
		{
			const TSharedPtr<FJsonObject>* ExecOutputsObj = nullptr;
			if (Step->TryGetObjectField(TEXT("exec_outputs"), ExecOutputsObj) && ExecOutputsObj != nullptr && (*ExecOutputsObj).IsValid())
			{
				// We need to collect all step IDs first to allow forward references.
				// Build the full set by scanning ahead if we haven't already.
				// Since we're iterating step-by-step, we need all step_ids from the whole array.
				TSet<FString> AllStepIds;
				for (int32 k = 0; k < StepsArray->Num(); ++k)
				{
					const TSharedPtr<FJsonObject>* OtherStepObj = nullptr;
					if ((*StepsArray)[k]->TryGetObject(OtherStepObj) && OtherStepObj != nullptr)
					{
						FString OtherStepId;
						if ((*OtherStepObj)->TryGetStringField(TEXT("step_id"), OtherStepId) && !OtherStepId.IsEmpty())
						{
							AllStepIds.Add(OtherStepId);
						}
					}
				}

				for (const auto& Pair : (*ExecOutputsObj)->Values)
				{
					const FString& ExecPinName = Pair.Key;
					FString TargetStepId;
					if (Pair.Value->TryGetString(TargetStepId) && !TargetStepId.IsEmpty())
					{
						if (!AllStepIds.Contains(TargetStepId))
						{
							AddError(
								TEXT("PLAN_INVALID_EXEC_OUTPUT_REF"),
								StepLocation + TEXT("/exec_outputs/") + ExecPinName,
								FString::Printf(TEXT("Step %d ('%s'): exec_outputs pin '%s' references unknown step_id '%s'"),
									i, *StepId, *ExecPinName, *TargetStepId),
								TEXT("exec_outputs must reference step_ids that exist in the steps array"));
						}
					}
				}
			}
		}
	}

	// --- Build final result ---
	if (Errors.Num() > 0)
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShareable(new FJsonObject());
		ErrorData->SetArrayField(TEXT("errors"), Errors);
		ErrorData->SetNumberField(TEXT("error_count"), Errors.Num());

		// Primary fields from first error
		TSharedPtr<FJsonObject> FirstError = Errors[0]->AsObject();

		FOliveIRResult Result;
		Result.bSuccess = false;
		Result.ErrorCode = FirstError->GetStringField(TEXT("code"));
		Result.ErrorMessage = FirstError->GetStringField(TEXT("message"));
		if (FirstError->HasField(TEXT("suggestion")))
		{
			Result.Suggestion = FirstError->GetStringField(TEXT("suggestion"));
		}
		Result.Data = ErrorData;
		return Result;
	}

	return FOliveIRResult::Success();
}
