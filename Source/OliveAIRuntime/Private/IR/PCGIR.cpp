// Copyright Bode Software. All Rights Reserved.

#include "IR/PCGIR.h"
#include "Serialization/JsonSerializer.h"

// FOliveIRPCGPin

TSharedPtr<FJsonObject> FOliveIRPCGPin::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Name);

	FString TypeStr;
	switch (DataType)
	{
		case EOliveIRPCGDataType::Point: TypeStr = TEXT("point"); break;
		case EOliveIRPCGDataType::Spline: TypeStr = TEXT("spline"); break;
		case EOliveIRPCGDataType::Surface: TypeStr = TEXT("surface"); break;
		case EOliveIRPCGDataType::Volume: TypeStr = TEXT("volume"); break;
		case EOliveIRPCGDataType::Landscape: TypeStr = TEXT("landscape"); break;
		case EOliveIRPCGDataType::Primitive: TypeStr = TEXT("primitive"); break;
		case EOliveIRPCGDataType::Concrete: TypeStr = TEXT("concrete"); break;
		case EOliveIRPCGDataType::Attribute: TypeStr = TEXT("attribute"); break;
		case EOliveIRPCGDataType::Param: TypeStr = TEXT("param"); break;
		case EOliveIRPCGDataType::Spatial: TypeStr = TEXT("spatial"); break;
		case EOliveIRPCGDataType::LandscapeSpline: TypeStr = TEXT("landscape_spline"); break;
		case EOliveIRPCGDataType::PolyLine: TypeStr = TEXT("polyline"); break;
		case EOliveIRPCGDataType::Texture: TypeStr = TEXT("texture"); break;
		case EOliveIRPCGDataType::RenderTarget: TypeStr = TEXT("render_target"); break;
		case EOliveIRPCGDataType::DynamicMesh: TypeStr = TEXT("dynamic_mesh"); break;
		case EOliveIRPCGDataType::Composite: TypeStr = TEXT("composite"); break;
		case EOliveIRPCGDataType::Any: TypeStr = TEXT("any"); break;
		default: TypeStr = TEXT("unknown"); break;
	}
	Json->SetStringField(TEXT("data_type"), TypeStr);

	if (bAllowMultipleConnections)
	{
		Json->SetBoolField(TEXT("allow_multiple"), true);
	}

	if (Connections.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ConnArray;
		for (const FString& Conn : Connections)
		{
			ConnArray.Add(MakeShared<FJsonValueString>(Conn));
		}
		Json->SetArrayField(TEXT("connections"), ConnArray);
	}

	return Json;
}

FOliveIRPCGPin FOliveIRPCGPin::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRPCGPin Pin;
	if (!JsonObject.IsValid())
	{
		return Pin;
	}

	Pin.Name = JsonObject->GetStringField(TEXT("name"));

	FString TypeStr = JsonObject->GetStringField(TEXT("data_type"));
	if (TypeStr == TEXT("point")) Pin.DataType = EOliveIRPCGDataType::Point;
	else if (TypeStr == TEXT("spline")) Pin.DataType = EOliveIRPCGDataType::Spline;
	else if (TypeStr == TEXT("surface")) Pin.DataType = EOliveIRPCGDataType::Surface;
	else if (TypeStr == TEXT("volume")) Pin.DataType = EOliveIRPCGDataType::Volume;
	else if (TypeStr == TEXT("landscape")) Pin.DataType = EOliveIRPCGDataType::Landscape;
	else if (TypeStr == TEXT("primitive")) Pin.DataType = EOliveIRPCGDataType::Primitive;
	else if (TypeStr == TEXT("concrete")) Pin.DataType = EOliveIRPCGDataType::Concrete;
	else if (TypeStr == TEXT("attribute")) Pin.DataType = EOliveIRPCGDataType::Attribute;
	else if (TypeStr == TEXT("param")) Pin.DataType = EOliveIRPCGDataType::Param;
	else if (TypeStr == TEXT("spatial")) Pin.DataType = EOliveIRPCGDataType::Spatial;
	else if (TypeStr == TEXT("landscape_spline")) Pin.DataType = EOliveIRPCGDataType::LandscapeSpline;
	else if (TypeStr == TEXT("polyline")) Pin.DataType = EOliveIRPCGDataType::PolyLine;
	else if (TypeStr == TEXT("texture")) Pin.DataType = EOliveIRPCGDataType::Texture;
	else if (TypeStr == TEXT("render_target")) Pin.DataType = EOliveIRPCGDataType::RenderTarget;
	else if (TypeStr == TEXT("dynamic_mesh")) Pin.DataType = EOliveIRPCGDataType::DynamicMesh;
	else if (TypeStr == TEXT("composite")) Pin.DataType = EOliveIRPCGDataType::Composite;
	else if (TypeStr == TEXT("any")) Pin.DataType = EOliveIRPCGDataType::Any;
	else Pin.DataType = EOliveIRPCGDataType::Unknown;

	Pin.bAllowMultipleConnections = JsonObject->GetBoolField(TEXT("allow_multiple"));

	const TArray<TSharedPtr<FJsonValue>>* ConnArray;
	if (JsonObject->TryGetArrayField(TEXT("connections"), ConnArray))
	{
		for (const auto& Value : *ConnArray)
		{
			Pin.Connections.Add(Value->AsString());
		}
	}

	return Pin;
}

// FOliveIRPCGNode

TSharedPtr<FJsonObject> FOliveIRPCGNode::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("id"), Id);
	Json->SetStringField(TEXT("type"), NodeType);
	Json->SetStringField(TEXT("title"), Title);

	if (PositionX != 0 || PositionY != 0)
	{
		Json->SetNumberField(TEXT("pos_x"), PositionX);
		Json->SetNumberField(TEXT("pos_y"), PositionY);
	}

	if (!Comment.IsEmpty())
	{
		Json->SetStringField(TEXT("comment"), Comment);
	}

	if (InputPins.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> PinsArray;
		for (const FOliveIRPCGPin& Pin : InputPins)
		{
			PinsArray.Add(MakeShared<FJsonValueObject>(Pin.ToJson()));
		}
		Json->SetArrayField(TEXT("inputs"), PinsArray);
	}

	if (OutputPins.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> PinsArray;
		for (const FOliveIRPCGPin& Pin : OutputPins)
		{
			PinsArray.Add(MakeShared<FJsonValueObject>(Pin.ToJson()));
		}
		Json->SetArrayField(TEXT("outputs"), PinsArray);
	}

	if (Settings.Num() > 0)
	{
		TSharedPtr<FJsonObject> SettingsJson = MakeShared<FJsonObject>();
		for (const auto& Pair : Settings)
		{
			SettingsJson->SetStringField(Pair.Key, Pair.Value);
		}
		Json->SetObjectField(TEXT("settings"), SettingsJson);
	}

	if (!bEnabled)
	{
		Json->SetBoolField(TEXT("enabled"), false);
	}
	if (bDebug)
	{
		Json->SetBoolField(TEXT("debug"), true);
	}

	return Json;
}

FOliveIRPCGNode FOliveIRPCGNode::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRPCGNode Node;
	if (!JsonObject.IsValid())
	{
		return Node;
	}

	Node.Id = JsonObject->GetStringField(TEXT("id"));
	Node.NodeType = JsonObject->GetStringField(TEXT("type"));
	Node.Title = JsonObject->GetStringField(TEXT("title"));

	if (JsonObject->HasField(TEXT("pos_x")))
	{
		Node.PositionX = (int32)JsonObject->GetNumberField(TEXT("pos_x"));
	}
	if (JsonObject->HasField(TEXT("pos_y")))
	{
		Node.PositionY = (int32)JsonObject->GetNumberField(TEXT("pos_y"));
	}
	JsonObject->TryGetStringField(TEXT("comment"), Node.Comment);

	Node.bEnabled = JsonObject->GetBoolField(TEXT("enabled"));
	Node.bDebug = JsonObject->GetBoolField(TEXT("debug"));

	// Default enabled to true if not present
	if (!JsonObject->HasField(TEXT("enabled")))
	{
		Node.bEnabled = true;
	}

	const TArray<TSharedPtr<FJsonValue>>* PinsArray;
	if (JsonObject->TryGetArrayField(TEXT("inputs"), PinsArray))
	{
		for (const auto& Value : *PinsArray)
		{
			FOliveIRPCGPin Pin = FOliveIRPCGPin::FromJson(Value->AsObject());
			Pin.bIsInput = true;
			Node.InputPins.Add(Pin);
		}
	}
	if (JsonObject->TryGetArrayField(TEXT("outputs"), PinsArray))
	{
		for (const auto& Value : *PinsArray)
		{
			FOliveIRPCGPin Pin = FOliveIRPCGPin::FromJson(Value->AsObject());
			Pin.bIsInput = false;
			Node.OutputPins.Add(Pin);
		}
	}

	const TSharedPtr<FJsonObject>* SettingsJson;
	if (JsonObject->TryGetObjectField(TEXT("settings"), SettingsJson))
	{
		for (const auto& Pair : (*SettingsJson)->Values)
		{
			Node.Settings.Add(Pair.Key, Pair.Value->AsString());
		}
	}

	return Node;
}

// FOliveIRPCGEdge

TSharedPtr<FJsonObject> FOliveIRPCGEdge::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("source_node"), SourceNodeId);
	Json->SetStringField(TEXT("source_pin"), SourcePinName);
	Json->SetStringField(TEXT("target_node"), TargetNodeId);
	Json->SetStringField(TEXT("target_pin"), TargetPinName);
	return Json;
}

FOliveIRPCGEdge FOliveIRPCGEdge::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRPCGEdge Edge;
	if (!JsonObject.IsValid())
	{
		return Edge;
	}

	Edge.SourceNodeId = JsonObject->GetStringField(TEXT("source_node"));
	Edge.SourcePinName = JsonObject->GetStringField(TEXT("source_pin"));
	Edge.TargetNodeId = JsonObject->GetStringField(TEXT("target_node"));
	Edge.TargetPinName = JsonObject->GetStringField(TEXT("target_pin"));
	return Edge;
}

// FOliveIRPCGGraphInterface

TSharedPtr<FJsonObject> FOliveIRPCGGraphInterface::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

	if (InputPins.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> PinsArray;
		for (const FOliveIRPCGPin& Pin : InputPins)
		{
			PinsArray.Add(MakeShared<FJsonValueObject>(Pin.ToJson()));
		}
		Json->SetArrayField(TEXT("inputs"), PinsArray);
	}

	if (OutputPins.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> PinsArray;
		for (const FOliveIRPCGPin& Pin : OutputPins)
		{
			PinsArray.Add(MakeShared<FJsonValueObject>(Pin.ToJson()));
		}
		Json->SetArrayField(TEXT("outputs"), PinsArray);
	}

	return Json;
}

FOliveIRPCGGraphInterface FOliveIRPCGGraphInterface::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRPCGGraphInterface Interface;
	if (!JsonObject.IsValid())
	{
		return Interface;
	}

	const TArray<TSharedPtr<FJsonValue>>* PinsArray;
	if (JsonObject->TryGetArrayField(TEXT("inputs"), PinsArray))
	{
		for (const auto& Value : *PinsArray)
		{
			FOliveIRPCGPin Pin = FOliveIRPCGPin::FromJson(Value->AsObject());
			Pin.bIsInput = true;
			Interface.InputPins.Add(Pin);
		}
	}
	if (JsonObject->TryGetArrayField(TEXT("outputs"), PinsArray))
	{
		for (const auto& Value : *PinsArray)
		{
			FOliveIRPCGPin Pin = FOliveIRPCGPin::FromJson(Value->AsObject());
			Pin.bIsInput = false;
			Interface.OutputPins.Add(Pin);
		}
	}

	return Interface;
}

// FOliveIRPCGGraph

TSharedPtr<FJsonObject> FOliveIRPCGGraph::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Name);
	Json->SetStringField(TEXT("path"), Path);

	if (bIsSubgraph)
	{
		Json->SetBoolField(TEXT("is_subgraph"), true);
	}

	Json->SetObjectField(TEXT("interface"), Interface.ToJson());

	if (Nodes.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> NodesArray;
		for (const FOliveIRPCGNode& Node : Nodes)
		{
			NodesArray.Add(MakeShared<FJsonValueObject>(Node.ToJson()));
		}
		Json->SetArrayField(TEXT("nodes"), NodesArray);
	}

	if (!InputNodeId.IsEmpty())
	{
		Json->SetStringField(TEXT("input_node"), InputNodeId);
	}
	if (!OutputNodeId.IsEmpty())
	{
		Json->SetStringField(TEXT("output_node"), OutputNodeId);
	}

	if (Edges.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> EdgesArray;
		for (const FOliveIRPCGEdge& Edge : Edges)
		{
			EdgesArray.Add(MakeShared<FJsonValueObject>(Edge.ToJson()));
		}
		Json->SetArrayField(TEXT("edges"), EdgesArray);
	}

	if (SubgraphPaths.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> SubgraphsArray;
		for (const FString& SubgraphPath : SubgraphPaths)
		{
			SubgraphsArray.Add(MakeShared<FJsonValueString>(SubgraphPath));
		}
		Json->SetArrayField(TEXT("subgraphs"), SubgraphsArray);
	}

	return Json;
}

FOliveIRPCGGraph FOliveIRPCGGraph::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRPCGGraph Graph;
	if (!JsonObject.IsValid())
	{
		return Graph;
	}

	Graph.Name = JsonObject->GetStringField(TEXT("name"));
	Graph.Path = JsonObject->GetStringField(TEXT("path"));
	Graph.bIsSubgraph = JsonObject->GetBoolField(TEXT("is_subgraph"));
	Graph.InputNodeId = JsonObject->GetStringField(TEXT("input_node"));
	Graph.OutputNodeId = JsonObject->GetStringField(TEXT("output_node"));

	const TSharedPtr<FJsonObject>* InterfaceJson;
	if (JsonObject->TryGetObjectField(TEXT("interface"), InterfaceJson))
	{
		Graph.Interface = FOliveIRPCGGraphInterface::FromJson(*InterfaceJson);
	}

	const TArray<TSharedPtr<FJsonValue>>* NodesArray;
	if (JsonObject->TryGetArrayField(TEXT("nodes"), NodesArray))
	{
		for (const auto& NodeValue : *NodesArray)
		{
			Graph.Nodes.Add(FOliveIRPCGNode::FromJson(NodeValue->AsObject()));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* EdgesArray;
	if (JsonObject->TryGetArrayField(TEXT("edges"), EdgesArray))
	{
		for (const auto& EdgeValue : *EdgesArray)
		{
			Graph.Edges.Add(FOliveIRPCGEdge::FromJson(EdgeValue->AsObject()));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* SubgraphsArray;
	if (JsonObject->TryGetArrayField(TEXT("subgraphs"), SubgraphsArray))
	{
		for (const auto& Value : *SubgraphsArray)
		{
			Graph.SubgraphPaths.Add(Value->AsString());
		}
	}

	return Graph;
}
