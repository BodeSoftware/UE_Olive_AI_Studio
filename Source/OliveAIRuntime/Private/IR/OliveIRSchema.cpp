// Copyright Bode Software. All Rights Reserved.

#include "IR/OliveIRSchema.h"

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
