// Copyright Bode Software. All Rights Reserved.

#include "IR/CommonIR.h"
#include "Serialization/JsonSerializer.h"

// FOliveIRAssetRef

TSharedPtr<FJsonObject> FOliveIRAssetRef::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Name);
	Json->SetStringField(TEXT("path"), Path);
	Json->SetStringField(TEXT("asset_class"), AssetClass);
	return Json;
}

FOliveIRAssetRef FOliveIRAssetRef::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRAssetRef Ref;
	if (JsonObject.IsValid())
	{
		Ref.Name = JsonObject->GetStringField(TEXT("name"));
		Ref.Path = JsonObject->GetStringField(TEXT("path"));
		Ref.AssetClass = JsonObject->GetStringField(TEXT("asset_class"));
	}
	return Ref;
}

// FOliveIRPinRef

FString FOliveIRPinRef::ToConnectionString() const
{
	return FString::Printf(TEXT("%s.%s"), *NodeId, *PinName);
}

FOliveIRPinRef FOliveIRPinRef::FromConnectionString(const FString& ConnectionString)
{
	FOliveIRPinRef Ref;
	int32 DotIndex;
	if (ConnectionString.FindChar('.', DotIndex))
	{
		Ref.NodeId = ConnectionString.Left(DotIndex);
		Ref.PinName = ConnectionString.Mid(DotIndex + 1);
	}
	return Ref;
}

TSharedPtr<FJsonObject> FOliveIRPinRef::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("node_id"), NodeId);
	Json->SetStringField(TEXT("pin_name"), PinName);
	return Json;
}

FOliveIRPinRef FOliveIRPinRef::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRPinRef Ref;
	if (JsonObject.IsValid())
	{
		Ref.NodeId = JsonObject->GetStringField(TEXT("node_id"));
		Ref.PinName = JsonObject->GetStringField(TEXT("pin_name"));
	}
	return Ref;
}

// FOliveIRFunctionParam

TSharedPtr<FJsonObject> FOliveIRFunctionParam::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Name);
	Json->SetObjectField(TEXT("type"), Type.ToJson());

	if (!DefaultValue.IsEmpty())
	{
		Json->SetStringField(TEXT("default_value"), DefaultValue);
	}
	if (bIsOutParam)
	{
		Json->SetBoolField(TEXT("is_out_param"), true);
	}
	if (bIsReference)
	{
		Json->SetBoolField(TEXT("is_reference"), true);
	}

	return Json;
}

FOliveIRFunctionParam FOliveIRFunctionParam::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRFunctionParam Param;
	if (!JsonObject.IsValid())
	{
		return Param;
	}

	Param.Name = JsonObject->GetStringField(TEXT("name"));

	const TSharedPtr<FJsonObject>* TypeJson;
	if (JsonObject->TryGetObjectField(TEXT("type"), TypeJson))
	{
		Param.Type = FOliveIRType::FromJson(*TypeJson);
	}

	Param.DefaultValue = JsonObject->GetStringField(TEXT("default_value"));
	Param.bIsOutParam = JsonObject->GetBoolField(TEXT("is_out_param"));
	Param.bIsReference = JsonObject->GetBoolField(TEXT("is_reference"));

	return Param;
}

// FOliveIRFunctionSignature

TSharedPtr<FJsonObject> FOliveIRFunctionSignature::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Name);

	if (Inputs.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> InputsArray;
		for (const FOliveIRFunctionParam& Param : Inputs)
		{
			InputsArray.Add(MakeShared<FJsonValueObject>(Param.ToJson()));
		}
		Json->SetArrayField(TEXT("inputs"), InputsArray);
	}

	if (Outputs.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> OutputsArray;
		for (const FOliveIRFunctionParam& Param : Outputs)
		{
			OutputsArray.Add(MakeShared<FJsonValueObject>(Param.ToJson()));
		}
		Json->SetArrayField(TEXT("outputs"), OutputsArray);
	}

	// Flags object
	TSharedPtr<FJsonObject> FlagsJson = MakeShared<FJsonObject>();
	if (bIsStatic) FlagsJson->SetBoolField(TEXT("static"), true);
	if (bIsPure) FlagsJson->SetBoolField(TEXT("pure"), true);
	if (bIsConst) FlagsJson->SetBoolField(TEXT("const"), true);
	if (!bIsPublic) FlagsJson->SetBoolField(TEXT("public"), false);
	if (bCallInEditor) FlagsJson->SetBoolField(TEXT("call_in_editor"), true);
	if (bIsOverride) FlagsJson->SetBoolField(TEXT("override"), true);
	if (bIsEvent) FlagsJson->SetBoolField(TEXT("event"), true);

	if (FlagsJson->Values.Num() > 0)
	{
		Json->SetObjectField(TEXT("flags"), FlagsJson);
	}

	if (!Category.IsEmpty())
	{
		Json->SetStringField(TEXT("category"), Category);
	}
	if (!Description.IsEmpty())
	{
		Json->SetStringField(TEXT("description"), Description);
	}
	if (!Keywords.IsEmpty())
	{
		Json->SetStringField(TEXT("keywords"), Keywords);
	}
	if (!DefinedIn.IsEmpty())
	{
		Json->SetStringField(TEXT("defined_in"), DefinedIn);
	}

	return Json;
}

FOliveIRFunctionSignature FOliveIRFunctionSignature::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRFunctionSignature Sig;
	if (!JsonObject.IsValid())
	{
		return Sig;
	}

	Sig.Name = JsonObject->GetStringField(TEXT("name"));
	Sig.Category = JsonObject->GetStringField(TEXT("category"));
	Sig.Description = JsonObject->GetStringField(TEXT("description"));
	Sig.Keywords = JsonObject->GetStringField(TEXT("keywords"));
	Sig.DefinedIn = JsonObject->GetStringField(TEXT("defined_in"));

	const TArray<TSharedPtr<FJsonValue>>* InputsArray;
	if (JsonObject->TryGetArrayField(TEXT("inputs"), InputsArray))
	{
		for (const auto& Value : *InputsArray)
		{
			Sig.Inputs.Add(FOliveIRFunctionParam::FromJson(Value->AsObject()));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* OutputsArray;
	if (JsonObject->TryGetArrayField(TEXT("outputs"), OutputsArray))
	{
		for (const auto& Value : *OutputsArray)
		{
			Sig.Outputs.Add(FOliveIRFunctionParam::FromJson(Value->AsObject()));
		}
	}

	const TSharedPtr<FJsonObject>* FlagsJson;
	if (JsonObject->TryGetObjectField(TEXT("flags"), FlagsJson))
	{
		Sig.bIsStatic = (*FlagsJson)->GetBoolField(TEXT("static"));
		Sig.bIsPure = (*FlagsJson)->GetBoolField(TEXT("pure"));
		Sig.bIsConst = (*FlagsJson)->GetBoolField(TEXT("const"));

		// Public defaults to true, so only set false if explicitly false
		bool bPublicFound = false;
		(*FlagsJson)->TryGetBoolField(TEXT("public"), bPublicFound);
		if (bPublicFound)
		{
			Sig.bIsPublic = (*FlagsJson)->GetBoolField(TEXT("public"));
		}

		Sig.bCallInEditor = (*FlagsJson)->GetBoolField(TEXT("call_in_editor"));
		Sig.bIsOverride = (*FlagsJson)->GetBoolField(TEXT("override"));
		Sig.bIsEvent = (*FlagsJson)->GetBoolField(TEXT("event"));
	}

	return Sig;
}

// FOliveIRPin

TSharedPtr<FJsonObject> FOliveIRPin::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Name);

	if (!DisplayName.IsEmpty())
	{
		Json->SetStringField(TEXT("display_name"), DisplayName);
	}

	Json->SetObjectField(TEXT("type"), Type.ToJson());

	if (bIsExec)
	{
		Json->SetBoolField(TEXT("is_exec"), true);
	}

	if (!DefaultValue.IsEmpty())
	{
		Json->SetStringField(TEXT("default"), DefaultValue);
	}

	if (!Connection.IsEmpty())
	{
		Json->SetStringField(TEXT("connection"), Connection);
	}

	if (Connections.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ConnectionsArray;
		for (const FString& Conn : Connections)
		{
			ConnectionsArray.Add(MakeShared<FJsonValueString>(Conn));
		}
		Json->SetArrayField(TEXT("connections"), ConnectionsArray);
	}

	return Json;
}

FOliveIRPin FOliveIRPin::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRPin Pin;
	if (!JsonObject.IsValid())
	{
		return Pin;
	}

	Pin.Name = JsonObject->GetStringField(TEXT("name"));
	Pin.DisplayName = JsonObject->GetStringField(TEXT("display_name"));

	const TSharedPtr<FJsonObject>* TypeJson;
	if (JsonObject->TryGetObjectField(TEXT("type"), TypeJson))
	{
		Pin.Type = FOliveIRType::FromJson(*TypeJson);
	}

	Pin.bIsExec = JsonObject->GetBoolField(TEXT("is_exec"));
	Pin.DefaultValue = JsonObject->GetStringField(TEXT("default"));
	Pin.Connection = JsonObject->GetStringField(TEXT("connection"));

	const TArray<TSharedPtr<FJsonValue>>* ConnectionsArray;
	if (JsonObject->TryGetArrayField(TEXT("connections"), ConnectionsArray))
	{
		for (const auto& ConnValue : *ConnectionsArray)
		{
			Pin.Connections.Add(ConnValue->AsString());
		}
	}

	return Pin;
}

// FOliveIRNode

FString FOliveIRNode::NodeCategoryToString(EOliveIRNodeCategory InCategory)
{
	switch (InCategory)
	{
	case EOliveIRNodeCategory::Event: return TEXT("Event");
	case EOliveIRNodeCategory::CustomEvent: return TEXT("CustomEvent");
	case EOliveIRNodeCategory::FunctionEntry: return TEXT("FunctionEntry");
	case EOliveIRNodeCategory::FunctionResult: return TEXT("FunctionResult");
	case EOliveIRNodeCategory::Branch: return TEXT("Branch");
	case EOliveIRNodeCategory::Sequence: return TEXT("Sequence");
	case EOliveIRNodeCategory::ForLoop: return TEXT("ForLoop");
	case EOliveIRNodeCategory::ForEachLoop: return TEXT("ForEachLoop");
	case EOliveIRNodeCategory::WhileLoop: return TEXT("WhileLoop");
	case EOliveIRNodeCategory::Switch: return TEXT("Switch");
	case EOliveIRNodeCategory::Select: return TEXT("Select");
	case EOliveIRNodeCategory::Gate: return TEXT("Gate");
	case EOliveIRNodeCategory::DoOnce: return TEXT("DoOnce");
	case EOliveIRNodeCategory::FlipFlop: return TEXT("FlipFlop");
	case EOliveIRNodeCategory::Delay: return TEXT("Delay");
	case EOliveIRNodeCategory::CallFunction: return TEXT("CallFunction");
	case EOliveIRNodeCategory::CallParentFunction: return TEXT("CallParentFunction");
	case EOliveIRNodeCategory::VariableGet: return TEXT("VariableGet");
	case EOliveIRNodeCategory::VariableSet: return TEXT("VariableSet");
	case EOliveIRNodeCategory::LocalVariable: return TEXT("LocalVariable");
	case EOliveIRNodeCategory::Cast: return TEXT("Cast");
	case EOliveIRNodeCategory::IsValid: return TEXT("IsValid");
	case EOliveIRNodeCategory::SpawnActor: return TEXT("SpawnActor");
	case EOliveIRNodeCategory::MakeStruct: return TEXT("MakeStruct");
	case EOliveIRNodeCategory::BreakStruct: return TEXT("BreakStruct");
	case EOliveIRNodeCategory::SetMember: return TEXT("SetMember");
	case EOliveIRNodeCategory::ArrayOperation: return TEXT("ArrayOperation");
	case EOliveIRNodeCategory::CreateDelegate: return TEXT("CreateDelegate");
	case EOliveIRNodeCategory::BindDelegate: return TEXT("BindDelegate");
	case EOliveIRNodeCategory::CallDelegate: return TEXT("CallDelegate");
	case EOliveIRNodeCategory::MacroInstance: return TEXT("MacroInstance");
	case EOliveIRNodeCategory::Comment: return TEXT("Comment");
	case EOliveIRNodeCategory::Reroute: return TEXT("Reroute");
	case EOliveIRNodeCategory::Timeline: return TEXT("Timeline");
	case EOliveIRNodeCategory::Literal: return TEXT("Literal");
	case EOliveIRNodeCategory::MathExpression: return TEXT("MathExpression");
	case EOliveIRNodeCategory::Comparison: return TEXT("Comparison");
	case EOliveIRNodeCategory::BooleanOp: return TEXT("BooleanOp");
	case EOliveIRNodeCategory::Unknown:
	default: return TEXT("Unknown");
	}
}

EOliveIRNodeCategory FOliveIRNode::StringToNodeCategory(const FString& InString)
{
	if (InString == TEXT("Event")) return EOliveIRNodeCategory::Event;
	if (InString == TEXT("CustomEvent")) return EOliveIRNodeCategory::CustomEvent;
	if (InString == TEXT("FunctionEntry")) return EOliveIRNodeCategory::FunctionEntry;
	if (InString == TEXT("FunctionResult")) return EOliveIRNodeCategory::FunctionResult;
	if (InString == TEXT("Branch")) return EOliveIRNodeCategory::Branch;
	if (InString == TEXT("Sequence")) return EOliveIRNodeCategory::Sequence;
	if (InString == TEXT("ForLoop")) return EOliveIRNodeCategory::ForLoop;
	if (InString == TEXT("ForEachLoop")) return EOliveIRNodeCategory::ForEachLoop;
	if (InString == TEXT("WhileLoop")) return EOliveIRNodeCategory::WhileLoop;
	if (InString == TEXT("Switch")) return EOliveIRNodeCategory::Switch;
	if (InString == TEXT("Select")) return EOliveIRNodeCategory::Select;
	if (InString == TEXT("Gate")) return EOliveIRNodeCategory::Gate;
	if (InString == TEXT("DoOnce")) return EOliveIRNodeCategory::DoOnce;
	if (InString == TEXT("FlipFlop")) return EOliveIRNodeCategory::FlipFlop;
	if (InString == TEXT("Delay")) return EOliveIRNodeCategory::Delay;
	if (InString == TEXT("CallFunction")) return EOliveIRNodeCategory::CallFunction;
	if (InString == TEXT("CallParentFunction")) return EOliveIRNodeCategory::CallParentFunction;
	if (InString == TEXT("VariableGet")) return EOliveIRNodeCategory::VariableGet;
	if (InString == TEXT("VariableSet")) return EOliveIRNodeCategory::VariableSet;
	if (InString == TEXT("LocalVariable")) return EOliveIRNodeCategory::LocalVariable;
	if (InString == TEXT("Cast")) return EOliveIRNodeCategory::Cast;
	if (InString == TEXT("IsValid")) return EOliveIRNodeCategory::IsValid;
	if (InString == TEXT("SpawnActor")) return EOliveIRNodeCategory::SpawnActor;
	if (InString == TEXT("MakeStruct")) return EOliveIRNodeCategory::MakeStruct;
	if (InString == TEXT("BreakStruct")) return EOliveIRNodeCategory::BreakStruct;
	if (InString == TEXT("SetMember")) return EOliveIRNodeCategory::SetMember;
	if (InString == TEXT("ArrayOperation")) return EOliveIRNodeCategory::ArrayOperation;
	if (InString == TEXT("CreateDelegate")) return EOliveIRNodeCategory::CreateDelegate;
	if (InString == TEXT("BindDelegate")) return EOliveIRNodeCategory::BindDelegate;
	if (InString == TEXT("CallDelegate")) return EOliveIRNodeCategory::CallDelegate;
	if (InString == TEXT("MacroInstance")) return EOliveIRNodeCategory::MacroInstance;
	if (InString == TEXT("Comment")) return EOliveIRNodeCategory::Comment;
	if (InString == TEXT("Reroute")) return EOliveIRNodeCategory::Reroute;
	if (InString == TEXT("Timeline")) return EOliveIRNodeCategory::Timeline;
	if (InString == TEXT("Literal")) return EOliveIRNodeCategory::Literal;
	if (InString == TEXT("MathExpression")) return EOliveIRNodeCategory::MathExpression;
	if (InString == TEXT("Comparison")) return EOliveIRNodeCategory::Comparison;
	if (InString == TEXT("BooleanOp")) return EOliveIRNodeCategory::BooleanOp;
	return EOliveIRNodeCategory::Unknown;
}

TSharedPtr<FJsonObject> FOliveIRNode::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("id"), Id);
	Json->SetStringField(TEXT("type"), Type);
	Json->SetStringField(TEXT("title"), Title);

	if (!FunctionName.IsEmpty())
	{
		Json->SetStringField(TEXT("function"), FunctionName);
	}
	if (!OwningClass.IsEmpty())
	{
		Json->SetStringField(TEXT("owning_class"), OwningClass);
	}
	if (!VariableName.IsEmpty())
	{
		Json->SetStringField(TEXT("variable"), VariableName);
	}
	if (!Category.IsEmpty())
	{
		Json->SetStringField(TEXT("category"), Category);
	}

	// Add node category enum as string
	if (NodeCategory != EOliveIRNodeCategory::Unknown)
	{
		Json->SetStringField(TEXT("node_category"), NodeCategoryToString(NodeCategory));
	}

	if (InputPins.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> PinsArray;
		for (const FOliveIRPin& Pin : InputPins)
		{
			PinsArray.Add(MakeShared<FJsonValueObject>(Pin.ToJson()));
		}
		Json->SetArrayField(TEXT("pins_in"), PinsArray);
	}

	if (OutputPins.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> PinsArray;
		for (const FOliveIRPin& Pin : OutputPins)
		{
			PinsArray.Add(MakeShared<FJsonValueObject>(Pin.ToJson()));
		}
		Json->SetArrayField(TEXT("pins_out"), PinsArray);
	}

	if (Properties.Num() > 0)
	{
		TSharedPtr<FJsonObject> PropsJson = MakeShared<FJsonObject>();
		for (const auto& Pair : Properties)
		{
			PropsJson->SetStringField(Pair.Key, Pair.Value);
		}
		Json->SetObjectField(TEXT("properties"), PropsJson);
	}

	if (!Comment.IsEmpty())
	{
		Json->SetStringField(TEXT("comment"), Comment);
	}

	return Json;
}

FOliveIRNode FOliveIRNode::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRNode Node;
	if (!JsonObject.IsValid())
	{
		return Node;
	}

	Node.Id = JsonObject->GetStringField(TEXT("id"));
	Node.Type = JsonObject->GetStringField(TEXT("type"));
	Node.Title = JsonObject->GetStringField(TEXT("title"));
	Node.FunctionName = JsonObject->GetStringField(TEXT("function"));
	Node.OwningClass = JsonObject->GetStringField(TEXT("owning_class"));
	Node.VariableName = JsonObject->GetStringField(TEXT("variable"));
	Node.Category = JsonObject->GetStringField(TEXT("category"));
	Node.Comment = JsonObject->GetStringField(TEXT("comment"));

	// Parse node category enum
	FString NodeCategoryStr = JsonObject->GetStringField(TEXT("node_category"));
	if (!NodeCategoryStr.IsEmpty())
	{
		Node.NodeCategory = StringToNodeCategory(NodeCategoryStr);
	}

	const TArray<TSharedPtr<FJsonValue>>* PinsArray;
	if (JsonObject->TryGetArrayField(TEXT("pins_in"), PinsArray))
	{
		for (const auto& PinValue : *PinsArray)
		{
			Node.InputPins.Add(FOliveIRPin::FromJson(PinValue->AsObject()));
		}
	}
	if (JsonObject->TryGetArrayField(TEXT("pins_out"), PinsArray))
	{
		for (const auto& PinValue : *PinsArray)
		{
			Node.OutputPins.Add(FOliveIRPin::FromJson(PinValue->AsObject()));
		}
	}

	const TSharedPtr<FJsonObject>* PropsJson;
	if (JsonObject->TryGetObjectField(TEXT("properties"), PropsJson))
	{
		for (const auto& Pair : (*PropsJson)->Values)
		{
			Node.Properties.Add(Pair.Key, Pair.Value->AsString());
		}
	}

	return Node;
}

// FOliveIRGraph

void FOliveIRGraph::UpdateStatistics()
{
	NodeCount = Nodes.Num();
	ConnectionCount = 0;

	for (const FOliveIRNode& Node : Nodes)
	{
		// Count connections from input pins
		for (const FOliveIRPin& Pin : Node.InputPins)
		{
			if (!Pin.Connection.IsEmpty())
			{
				ConnectionCount++;
			}
			ConnectionCount += Pin.Connections.Num();
		}
		// Note: We don't count output pins to avoid double-counting
		// since each connection appears on both the output and input side
	}
}

TSharedPtr<FJsonObject> FOliveIRGraph::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Name);
	Json->SetStringField(TEXT("graph_type"), GraphType);

	if (Inputs.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> InputsArray;
		for (const FOliveIRPin& Pin : Inputs)
		{
			InputsArray.Add(MakeShared<FJsonValueObject>(Pin.ToJson()));
		}
		Json->SetArrayField(TEXT("inputs"), InputsArray);
	}

	if (Outputs.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> OutputsArray;
		for (const FOliveIRPin& Pin : Outputs)
		{
			OutputsArray.Add(MakeShared<FJsonValueObject>(Pin.ToJson()));
		}
		Json->SetArrayField(TEXT("outputs"), OutputsArray);
	}

	if (!Access.IsEmpty())
	{
		Json->SetStringField(TEXT("access"), Access);
	}

	if (bIsPure)
	{
		Json->SetBoolField(TEXT("pure"), true);
	}
	if (bIsStatic)
	{
		Json->SetBoolField(TEXT("static"), true);
	}
	if (bIsConst)
	{
		Json->SetBoolField(TEXT("const"), true);
	}

	if (Nodes.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> NodesArray;
		for (const FOliveIRNode& Node : Nodes)
		{
			NodesArray.Add(MakeShared<FJsonValueObject>(Node.ToJson()));
		}
		Json->SetArrayField(TEXT("nodes"), NodesArray);
	}

	if (!Description.IsEmpty())
	{
		Json->SetStringField(TEXT("description"), Description);
	}
	if (!Category.IsEmpty())
	{
		Json->SetStringField(TEXT("category"), Category);
	}
	if (Keywords.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> KeywordsArray;
		for (const FString& Keyword : Keywords)
		{
			KeywordsArray.Add(MakeShared<FJsonValueString>(Keyword));
		}
		Json->SetArrayField(TEXT("keywords"), KeywordsArray);
	}

	// Statistics
	Json->SetNumberField(TEXT("node_count"), NodeCount);
	Json->SetNumberField(TEXT("connection_count"), ConnectionCount);

	return Json;
}

FOliveIRGraph FOliveIRGraph::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRGraph Graph;
	if (!JsonObject.IsValid())
	{
		return Graph;
	}

	Graph.Name = JsonObject->GetStringField(TEXT("name"));
	Graph.GraphType = JsonObject->GetStringField(TEXT("graph_type"));
	Graph.Access = JsonObject->GetStringField(TEXT("access"));
	Graph.bIsPure = JsonObject->GetBoolField(TEXT("pure"));
	Graph.bIsStatic = JsonObject->GetBoolField(TEXT("static"));
	Graph.bIsConst = JsonObject->GetBoolField(TEXT("const"));
	Graph.Description = JsonObject->GetStringField(TEXT("description"));
	Graph.Category = JsonObject->GetStringField(TEXT("category"));
	Graph.NodeCount = static_cast<int32>(JsonObject->GetNumberField(TEXT("node_count")));
	Graph.ConnectionCount = static_cast<int32>(JsonObject->GetNumberField(TEXT("connection_count")));

	const TArray<TSharedPtr<FJsonValue>>* Array;
	if (JsonObject->TryGetArrayField(TEXT("inputs"), Array))
	{
		for (const auto& Value : *Array)
		{
			Graph.Inputs.Add(FOliveIRPin::FromJson(Value->AsObject()));
		}
	}
	if (JsonObject->TryGetArrayField(TEXT("outputs"), Array))
	{
		for (const auto& Value : *Array)
		{
			Graph.Outputs.Add(FOliveIRPin::FromJson(Value->AsObject()));
		}
	}
	if (JsonObject->TryGetArrayField(TEXT("nodes"), Array))
	{
		for (const auto& Value : *Array)
		{
			Graph.Nodes.Add(FOliveIRNode::FromJson(Value->AsObject()));
		}
	}
	if (JsonObject->TryGetArrayField(TEXT("keywords"), Array))
	{
		for (const auto& Value : *Array)
		{
			Graph.Keywords.Add(Value->AsString());
		}
	}

	return Graph;
}

// FOliveIRVariable

TSharedPtr<FJsonObject> FOliveIRVariable::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Name);
	Json->SetObjectField(TEXT("type"), Type.ToJson());

	if (!DefaultValue.IsEmpty())
	{
		Json->SetStringField(TEXT("default_value"), DefaultValue);
	}
	if (!Category.IsEmpty())
	{
		Json->SetStringField(TEXT("category"), Category);
	}
	if (!Description.IsEmpty())
	{
		Json->SetStringField(TEXT("description"), Description);
	}
	if (!DefinedIn.IsEmpty())
	{
		Json->SetStringField(TEXT("defined_in"), DefinedIn);
	}

	TSharedPtr<FJsonObject> FlagsJson = MakeShared<FJsonObject>();
	FlagsJson->SetBoolField(TEXT("blueprint_read_write"), bBlueprintReadWrite);
	FlagsJson->SetBoolField(TEXT("expose_on_spawn"), bExposeOnSpawn);
	FlagsJson->SetBoolField(TEXT("replicated"), bReplicated);
	FlagsJson->SetBoolField(TEXT("save_game"), bSaveGame);
	FlagsJson->SetBoolField(TEXT("edit_anywhere"), bEditAnywhere);
	FlagsJson->SetBoolField(TEXT("blueprint_visible"), bBlueprintVisible);
	Json->SetObjectField(TEXT("flags"), FlagsJson);

	if (!ReplicationCondition.IsEmpty())
	{
		Json->SetStringField(TEXT("replication_condition"), ReplicationCondition);
	}

	return Json;
}

FOliveIRVariable FOliveIRVariable::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRVariable Var;
	if (!JsonObject.IsValid())
	{
		return Var;
	}

	Var.Name = JsonObject->GetStringField(TEXT("name"));

	const TSharedPtr<FJsonObject>* TypeJson;
	if (JsonObject->TryGetObjectField(TEXT("type"), TypeJson))
	{
		Var.Type = FOliveIRType::FromJson(*TypeJson);
	}

	Var.DefaultValue = JsonObject->GetStringField(TEXT("default_value"));
	Var.Category = JsonObject->GetStringField(TEXT("category"));
	Var.Description = JsonObject->GetStringField(TEXT("description"));
	Var.DefinedIn = JsonObject->GetStringField(TEXT("defined_in"));
	Var.ReplicationCondition = JsonObject->GetStringField(TEXT("replication_condition"));

	const TSharedPtr<FJsonObject>* FlagsJson;
	if (JsonObject->TryGetObjectField(TEXT("flags"), FlagsJson))
	{
		Var.bBlueprintReadWrite = (*FlagsJson)->GetBoolField(TEXT("blueprint_read_write"));
		Var.bExposeOnSpawn = (*FlagsJson)->GetBoolField(TEXT("expose_on_spawn"));
		Var.bReplicated = (*FlagsJson)->GetBoolField(TEXT("replicated"));
		Var.bSaveGame = (*FlagsJson)->GetBoolField(TEXT("save_game"));
		Var.bEditAnywhere = (*FlagsJson)->GetBoolField(TEXT("edit_anywhere"));
		Var.bBlueprintVisible = (*FlagsJson)->GetBoolField(TEXT("blueprint_visible"));
	}

	return Var;
}

// FOliveIRComponent

TSharedPtr<FJsonObject> FOliveIRComponent::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Name);
	Json->SetStringField(TEXT("class"), ComponentClass);

	if (bIsRoot)
	{
		Json->SetBoolField(TEXT("is_root"), true);
	}

	if (Children.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ChildrenArray;
		for (const FOliveIRComponent& Child : Children)
		{
			ChildrenArray.Add(MakeShared<FJsonValueObject>(Child.ToJson()));
		}
		Json->SetArrayField(TEXT("children"), ChildrenArray);
	}

	if (Properties.Num() > 0)
	{
		TSharedPtr<FJsonObject> PropsJson = MakeShared<FJsonObject>();
		for (const auto& Pair : Properties)
		{
			PropsJson->SetStringField(Pair.Key, Pair.Value);
		}
		Json->SetObjectField(TEXT("properties"), PropsJson);
	}

	return Json;
}

FOliveIRComponent FOliveIRComponent::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRComponent Comp;
	if (!JsonObject.IsValid())
	{
		return Comp;
	}

	Comp.Name = JsonObject->GetStringField(TEXT("name"));
	Comp.ComponentClass = JsonObject->GetStringField(TEXT("class"));
	Comp.bIsRoot = JsonObject->GetBoolField(TEXT("is_root"));

	const TArray<TSharedPtr<FJsonValue>>* ChildrenArray;
	if (JsonObject->TryGetArrayField(TEXT("children"), ChildrenArray))
	{
		for (const auto& ChildValue : *ChildrenArray)
		{
			Comp.Children.Add(FOliveIRComponent::FromJson(ChildValue->AsObject()));
		}
	}

	const TSharedPtr<FJsonObject>* PropsJson;
	if (JsonObject->TryGetObjectField(TEXT("properties"), PropsJson))
	{
		for (const auto& Pair : (*PropsJson)->Values)
		{
			Comp.Properties.Add(Pair.Key, Pair.Value->AsString());
		}
	}

	return Comp;
}
