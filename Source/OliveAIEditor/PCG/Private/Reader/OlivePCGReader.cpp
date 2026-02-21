// Copyright Bode Software. All Rights Reserved.

#include "OlivePCGReader.h"
#include "PCGGraph.h"
#include "PCGEdge.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "PCGSubgraph.h"

DEFINE_LOG_CATEGORY_STATIC(LogOlivePCGReader, Log, All);

FOlivePCGReader& FOlivePCGReader::Get()
{
	static FOlivePCGReader Instance;
	return Instance;
}

TOptional<FOliveIRPCGGraph> FOlivePCGReader::ReadPCGGraph(const FString& AssetPath)
{
	UPCGGraph* Graph = LoadPCGGraph(AssetPath);
	if (!Graph)
	{
		UE_LOG(LogOlivePCGReader, Warning, TEXT("Failed to load PCG graph: %s"), *AssetPath);
		return {};
	}

	return ReadPCGGraph(Graph);
}

TOptional<FOliveIRPCGGraph> FOlivePCGReader::ReadPCGGraph(const UPCGGraph* Graph)
{
	if (!Graph)
	{
		return {};
	}

	FOliveIRPCGGraph IR;
	IR.Name = Graph->GetName();
	IR.Path = Graph->GetPathName();

	// Build node ID map: InputNode, OutputNode, then GetNodes()
	TMap<const UPCGNode*, FString> NodeIdMap;
	int32 NodeIndex = 0;

	// Input node
	const UPCGNode* InputNode = Graph->GetInputNode();
	if (InputNode)
	{
		FString InputId = TEXT("input");
		IR.InputNodeId = InputId;
		NodeIdMap.Add(InputNode, InputId);
		IR.Nodes.Add(SerializeNode(InputNode, InputId));
	}

	// Output node
	const UPCGNode* OutputNode = Graph->GetOutputNode();
	if (OutputNode)
	{
		FString OutputId = TEXT("output");
		IR.OutputNodeId = OutputId;
		NodeIdMap.Add(OutputNode, OutputId);
		IR.Nodes.Add(SerializeNode(OutputNode, OutputId));
	}

	// Regular nodes
	const TArray<UPCGNode*>& Nodes = Graph->GetNodes();
	for (const UPCGNode* Node : Nodes)
	{
		if (!Node)
		{
			continue;
		}

		FString NodeId = FString::Printf(TEXT("node_%d"), NodeIndex++);
		NodeIdMap.Add(Node, NodeId);
		IR.Nodes.Add(SerializeNode(Node, NodeId));

		// Check for subgraph references
		const UPCGSettings* Settings = Node->GetSettings();
		if (const UPCGBaseSubgraphSettings* SubSettings = Cast<UPCGBaseSubgraphSettings>(Settings))
		{
			if (UPCGGraphInterface* SubGraph = SubSettings->GetSubgraphInterface())
			{
				IR.SubgraphPaths.AddUnique(SubGraph->GetPathName());
			}
		}
	}

	// Build edge list by walking all output pins
	auto ProcessNodeEdges = [&](const UPCGNode* Node)
	{
		if (!Node)
		{
			return;
		}

		const FString* SourceId = NodeIdMap.Find(Node);
		if (!SourceId)
		{
			return;
		}

		for (const UPCGPin* OutputPin : Node->GetOutputPins())
		{
			if (!OutputPin)
			{
				continue;
			}

			for (const UPCGEdge* Edge : OutputPin->Edges)
			{
				if (!Edge)
				{
					continue;
				}

				const UPCGPin* OtherPin = Edge->GetOtherPin(OutputPin);
				if (!OtherPin)
				{
					continue;
				}

				const UPCGNode* TargetNode = OtherPin->Node;
				const FString* TargetId = NodeIdMap.Find(TargetNode);
				if (!TargetId)
				{
					continue;
				}

				FOliveIRPCGEdge IREdge;
				IREdge.SourceNodeId = *SourceId;
				IREdge.SourcePinName = OutputPin->Properties.Label.ToString();
				IREdge.TargetNodeId = *TargetId;
				IREdge.TargetPinName = OtherPin->Properties.Label.ToString();
				IR.Edges.Add(IREdge);
			}
		}
	};

	// Process edges for all nodes including input/output
	if (InputNode)
	{
		ProcessNodeEdges(InputNode);
	}
	if (OutputNode)
	{
		ProcessNodeEdges(OutputNode);
	}
	for (const UPCGNode* Node : Nodes)
	{
		ProcessNodeEdges(Node);
	}

	// Serialize graph interface
	if (InputNode)
	{
		for (const UPCGPin* Pin : InputNode->GetOutputPins())
		{
			if (Pin)
			{
				FOliveIRPCGPin IRPin = SerializePin(Pin);
				IRPin.bIsInput = true;
				IR.Interface.InputPins.Add(IRPin);
			}
		}
	}
	if (OutputNode)
	{
		for (const UPCGPin* Pin : OutputNode->GetInputPins())
		{
			if (Pin)
			{
				FOliveIRPCGPin IRPin = SerializePin(Pin);
				IRPin.bIsInput = false;
				IR.Interface.OutputPins.Add(IRPin);
			}
		}
	}

	UE_LOG(LogOlivePCGReader, Log, TEXT("Read PCG graph '%s': %d nodes, %d edges"),
		*IR.Name, IR.Nodes.Num(), IR.Edges.Num());

	return IR;
}

UPCGGraph* FOlivePCGReader::LoadPCGGraph(const FString& AssetPath) const
{
	return Cast<UPCGGraph>(StaticLoadObject(UPCGGraph::StaticClass(), nullptr, *AssetPath));
}

FOliveIRPCGNode FOlivePCGReader::SerializeNode(const UPCGNode* Node, const FString& NodeId) const
{
	FOliveIRPCGNode IRNode;
	IRNode.Id = NodeId;

	const UPCGSettings* Settings = Node->GetSettings();
	if (Settings)
	{
		IRNode.NodeType = Settings->GetClass()->GetName();
		IRNode.Title = Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString();
	}
	else
	{
		IRNode.NodeType = TEXT("Unknown");
		IRNode.Title = Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString();
	}

#if WITH_EDITOR
	int32 PosX = 0, PosY = 0;
	Node->GetNodePosition(PosX, PosY);
	IRNode.PositionX = PosX;
	IRNode.PositionY = PosY;
#endif

	IRNode.Comment = Node->NodeComment;

	// Serialize pins
	for (const UPCGPin* Pin : Node->GetInputPins())
	{
		if (Pin)
		{
			FOliveIRPCGPin IRPin = SerializePin(Pin);
			IRPin.bIsInput = true;
			IRNode.InputPins.Add(IRPin);
		}
	}

	for (const UPCGPin* Pin : Node->GetOutputPins())
	{
		if (Pin)
		{
			FOliveIRPCGPin IRPin = SerializePin(Pin);
			IRPin.bIsInput = false;
			IRNode.OutputPins.Add(IRPin);
		}
	}

	// Serialize settings properties
	if (Settings)
	{
		SerializeSettings(Settings, IRNode.Settings);
		IRNode.bEnabled = Settings->bEnabled;
		IRNode.bDebug = Settings->bDebug;
	}

	return IRNode;
}

FOliveIRPCGPin FOlivePCGReader::SerializePin(const UPCGPin* Pin) const
{
	FOliveIRPCGPin IRPin;
	IRPin.Name = Pin->Properties.Label.ToString();
	IRPin.DataType = MapDataType(Pin->Properties.AllowedTypes);
	IRPin.bAllowMultipleConnections = Pin->Properties.bAllowMultipleData;
	IRPin.bIsInput = Pin->IsOutputPin() ? false : true;

	// Connection info is captured via the edge list instead
	return IRPin;
}

EOliveIRPCGDataType FOlivePCGReader::MapDataType(EPCGDataType EngineType) const
{
	// EPCGDataType is a bitmask; map the dominant/simple types
	if (EngineType == EPCGDataType::Point) return EOliveIRPCGDataType::Point;
	if (EngineType == EPCGDataType::Spline) return EOliveIRPCGDataType::Spline;
	if (EngineType == EPCGDataType::Surface) return EOliveIRPCGDataType::Surface;
	if (EngineType == EPCGDataType::Volume) return EOliveIRPCGDataType::Volume;
	if (EngineType == EPCGDataType::Landscape) return EOliveIRPCGDataType::Landscape;
	if (EngineType == EPCGDataType::Primitive) return EOliveIRPCGDataType::Primitive;
	if (EngineType == EPCGDataType::Concrete) return EOliveIRPCGDataType::Concrete;
	if (EngineType == EPCGDataType::Param) return EOliveIRPCGDataType::Param;
	if (EngineType == EPCGDataType::Spatial) return EOliveIRPCGDataType::Spatial;
	if (EngineType == EPCGDataType::PolyLine) return EOliveIRPCGDataType::PolyLine;
	if (EngineType == EPCGDataType::Any) return EOliveIRPCGDataType::Any;

	return EOliveIRPCGDataType::Unknown;
}

void FOlivePCGReader::SerializeSettings(const UPCGSettings* Settings, TMap<FString, FString>& OutSettings) const
{
	if (!Settings)
	{
		return;
	}

	const UClass* SettingsClass = Settings->GetClass();

	for (TFieldIterator<FProperty> PropIt(SettingsClass); PropIt; ++PropIt)
	{
		FProperty* Property = *PropIt;

		// Skip properties from base UPCGSettings and above
		if (Property->GetOwnerClass() == UPCGSettings::StaticClass() ||
			Property->GetOwnerClass() == UObject::StaticClass())
		{
			continue;
		}

		// Only include editable properties
		if (!Property->HasAnyPropertyFlags(CPF_Edit))
		{
			continue;
		}

		FString ValueStr;
		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Settings);
		Property->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, nullptr, PPF_None);

		if (!ValueStr.IsEmpty())
		{
			OutSettings.Add(Property->GetName(), ValueStr);
		}
	}
}
