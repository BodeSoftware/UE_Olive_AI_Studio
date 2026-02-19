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
