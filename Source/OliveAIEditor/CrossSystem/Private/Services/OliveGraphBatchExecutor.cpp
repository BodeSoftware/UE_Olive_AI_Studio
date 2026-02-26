// Copyright Bode Software. All Rights Reserved.

#include "Services/OliveGraphBatchExecutor.h"
#include "Writer/OliveGraphWriter.h"
#include "Services/OliveToolParamHelpers.h"

DEFINE_LOG_CATEGORY(LogOliveGraphBatchExecutor);

FOliveBlueprintWriteResult FOliveGraphBatchExecutor::DispatchWriterOp(
	const FString& ToolName,
	const FString& BlueprintPath,
	const TSharedPtr<FJsonObject>& OpParams)
{
	FOliveGraphWriter& Writer = FOliveGraphWriter::Get();

	FString GraphName = OliveToolParamHelpers::GetOptionalString(OpParams, TEXT("graph"), TEXT("EventGraph"));

	if (ToolName == TEXT("blueprint.add_node"))
	{
		FString NodeType = OliveToolParamHelpers::GetOptionalString(OpParams, TEXT("type"));
		if (NodeType.IsEmpty())
		{
			return FOliveBlueprintWriteResult::Error(TEXT("Missing required param 'type' for blueprint.add_node. Provide the node type, e.g. \"PrintString\", \"Delay\", \"K2Node_IfThenElse\"."));
		}

		TMap<FString, FString> NodeProperties = OliveToolParamHelpers::ParseNodeProperties(OpParams);

		int32 PosX = OliveToolParamHelpers::GetOptionalInt(OpParams, TEXT("pos_x"), 0);
		int32 PosY = OliveToolParamHelpers::GetOptionalInt(OpParams, TEXT("pos_y"), 0);

		return Writer.AddNode(BlueprintPath, GraphName, NodeType, NodeProperties, PosX, PosY);
	}
	else if (ToolName == TEXT("blueprint.connect_pins"))
	{
		FString Source = OliveToolParamHelpers::GetOptionalString(OpParams, TEXT("source"));
		FString Target = OliveToolParamHelpers::GetOptionalString(OpParams, TEXT("target"));
		if (Source.IsEmpty() || Target.IsEmpty())
		{
			return FOliveBlueprintWriteResult::Error(TEXT("Missing required params 'source' and/or 'target' for blueprint.connect_pins. Format: \"node_id.PinName\", e.g. \"node_1.ReturnValue\" -> \"node_2.Input\"."));
		}
		return Writer.ConnectPins(BlueprintPath, GraphName, Source, Target);
	}
	else if (ToolName == TEXT("blueprint.disconnect_pins"))
	{
		FString Source = OliveToolParamHelpers::GetOptionalString(OpParams, TEXT("source"));
		FString Target = OliveToolParamHelpers::GetOptionalString(OpParams, TEXT("target"));
		if (Source.IsEmpty() || Target.IsEmpty())
		{
			return FOliveBlueprintWriteResult::Error(TEXT("Missing required params 'source' and/or 'target' for blueprint.disconnect_pins. Format: \"node_id.PinName\"."));
		}
		return Writer.DisconnectPins(BlueprintPath, GraphName, Source, Target);
	}
	else if (ToolName == TEXT("blueprint.set_pin_default"))
	{
		FString Pin = OliveToolParamHelpers::GetOptionalString(OpParams, TEXT("pin"));
		FString Value = OliveToolParamHelpers::GetOptionalString(OpParams, TEXT("value"));
		if (Pin.IsEmpty())
		{
			return FOliveBlueprintWriteResult::Error(TEXT("Missing required param 'pin' for blueprint.set_pin_default. Format: \"node_id.PinName\", e.g. \"node_1.Duration\"."));
		}
		return Writer.SetPinDefault(BlueprintPath, GraphName, Pin, Value);
	}
	else if (ToolName == TEXT("blueprint.set_node_property"))
	{
		FString NodeId = OliveToolParamHelpers::GetOptionalString(OpParams, TEXT("node_id"));
		FString PropertyName = OliveToolParamHelpers::GetOptionalString(OpParams, TEXT("property"));
		FString PropertyValue = OliveToolParamHelpers::GetOptionalString(OpParams, TEXT("value"));
		if (NodeId.IsEmpty() || PropertyName.IsEmpty())
		{
			return FOliveBlueprintWriteResult::Error(TEXT("Missing required params 'node_id' and/or 'property' for blueprint.set_node_property. Provide the node ID and the UPROPERTY name."));
		}
		return Writer.SetNodeProperty(BlueprintPath, GraphName, NodeId, PropertyName, PropertyValue);
	}
	else if (ToolName == TEXT("blueprint.remove_node"))
	{
		FString NodeId = OliveToolParamHelpers::GetOptionalString(OpParams, TEXT("node_id"));
		if (NodeId.IsEmpty())
		{
			return FOliveBlueprintWriteResult::Error(TEXT("Missing required param 'node_id' for blueprint.remove_node. Provide the node ID from a prior add_node or from blueprint.read_graph."));
		}
		return Writer.RemoveNode(BlueprintPath, GraphName, NodeId);
	}

	return FOliveBlueprintWriteResult::Error(FString::Printf(TEXT("Unknown tool '%s' in batch dispatch"), *ToolName));
}

bool FOliveGraphBatchExecutor::ResolveTemplateReferences(
	TSharedPtr<FJsonObject>& OpParams,
	const TMap<FString, TSharedPtr<FJsonObject>>& OpResults,
	FString& OutError)
{
	if (!OpParams.IsValid())
	{
		return true;
	}

	// Collect resolved values separately to avoid mutating the map during iteration
	TArray<TPair<FString, FString>> ResolvedFields;

	for (const auto& Pair : OpParams->Values)
	{
		if (!Pair.Value.IsValid())
		{
			OutError = FString::Printf(TEXT("Invalid null JSON value for param '%s'"), *Pair.Key);
			return false;
		}

		if (Pair.Value->Type != EJson::String)
		{
			continue;
		}

		FString Value = Pair.Value->AsString();
		bool bModified = false;

		// Check for ${...} pattern
		int32 StartIdx = 0;
		while (StartIdx < Value.Len())
		{
			int32 TemplateStart = Value.Find(TEXT("${"), ESearchCase::CaseSensitive, ESearchDir::FromStart, StartIdx);
			if (TemplateStart == INDEX_NONE)
			{
				break;
			}

			int32 TemplateEnd = Value.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, TemplateStart + 2);
			if (TemplateEnd == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("Unclosed template reference at position %d in param '%s'"), TemplateStart, *Pair.Key);
				return false;
			}

			// Extract the content between ${ and }
			FString TemplateContent = Value.Mid(TemplateStart + 2, TemplateEnd - TemplateStart - 2);

			// Split on first dot: opId.fieldName
			FString OpId;
			FString FieldName;
			if (!TemplateContent.Split(TEXT("."), &OpId, &FieldName))
			{
				OutError = FString::Printf(TEXT("Invalid template format '${%s}' — expected ${opId.field}"), *TemplateContent);
				return false;
			}

			const TSharedPtr<FJsonObject>* FoundResult = OpResults.Find(OpId);
			if (!FoundResult || !(*FoundResult).IsValid())
			{
				OutError = FString::Printf(TEXT("Template '${%s}' references unknown op id '%s'"), *TemplateContent, *OpId);
				return false;
			}

			FString ResolvedValue;
			if (!(*FoundResult)->TryGetStringField(FieldName, ResolvedValue))
			{
				OutError = FString::Printf(TEXT("Template '${%s}' — field '%s' not found in op '%s' results"), *TemplateContent, *FieldName, *OpId);
				return false;
			}

			// Replace the template token with the resolved value
			FString TemplateToken = FString::Printf(TEXT("${%s}"), *TemplateContent);
			Value = Value.Replace(*TemplateToken, *ResolvedValue);
			bModified = true;

			// Advance past the replacement
			StartIdx = TemplateStart + ResolvedValue.Len();
		}

		if (bModified)
		{
			ResolvedFields.Emplace(Pair.Key, MoveTemp(Value));
		}
	}

	// Apply resolved values after iteration is complete
	for (const auto& Resolved : ResolvedFields)
	{
		OpParams->SetStringField(Resolved.Key, Resolved.Value);
	}

	return true;
}

const TSet<FString>& FOliveGraphBatchExecutor::GetBatchWriteAllowlist()
{
	static TSet<FString> Allowlist = {
		TEXT("blueprint.add_node"),
		TEXT("blueprint.connect_pins"),
		TEXT("blueprint.disconnect_pins"),
		TEXT("blueprint.set_pin_default"),
		TEXT("blueprint.set_node_property"),
		TEXT("blueprint.remove_node")
	};
	return Allowlist;
}
