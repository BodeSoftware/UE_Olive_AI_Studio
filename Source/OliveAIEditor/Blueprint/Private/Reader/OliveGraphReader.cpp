// Copyright Bode Software. All Rights Reserved.

#include "OliveGraphReader.h"
#include "OliveNodeSerializer.h"
#include "OlivePinSerializer.h"

// UE Graph includes
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"

// Blueprint includes
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"

// K2 Node includes
#include "K2Node.h"
#include "K2Node_Tunnel.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Composite.h"

DEFINE_LOG_CATEGORY(LogOliveGraphReader);

// ============================================================================
// Constructor
// ============================================================================

FOliveGraphReader::FOliveGraphReader()
{
	NodeSerializer = MakeShared<FOliveNodeSerializer>();
	PinSerializer = MakeShared<FOlivePinSerializer>();
}

// ============================================================================
// Main Reading Methods
// ============================================================================

FOliveIRGraph FOliveGraphReader::ReadGraph(const UEdGraph* Graph, const UBlueprint* OwningBlueprint)
{
	FOliveIRGraph Result;

	if (!Graph)
	{
		UE_LOG(LogOliveGraphReader, Warning, TEXT("ReadGraph called with null Graph"));
		return Result;
	}

	// Clear previous state
	ClearCache();

	// Set basic graph info
	Result.Name = Graph->GetName();
	Result.GraphType = DetermineGraphType(Graph, OwningBlueprint);

	UE_LOG(LogOliveGraphReader, Log, TEXT("Reading graph: %s (Type: %s, Nodes: %d)"),
		*Result.Name,
		*Result.GraphType,
		Graph->Nodes.Num());

	// Build the node ID map first - this must be done before serializing nodes
	// so that connection references can be resolved
	BuildNodeIdMap(Graph);

	// Serialize all nodes
	for (const auto& Pair : NodeIdMap)
	{
		const UEdGraphNode* Node = Pair.Key;
		const FString& NodeId = Pair.Value;

		if (!Node)
		{
			continue;
		}

		FOliveIRNode NodeIR = NodeSerializer->SerializeNode(Node, NodeIdMap);
		NodeIR.Id = NodeId;
		Result.Nodes.Add(MoveTemp(NodeIR));
	}

	// For function graphs, extract the function signature
	if (Result.GraphType == TEXT("Function"))
	{
		ReadFunctionSignature(Graph, Result);
	}

	// Calculate statistics
	CalculateStatistics(Result);

	UE_LOG(LogOliveGraphReader, Log, TEXT("Graph read complete: %s (Nodes: %d, Connections: %d)"),
		*Result.Name,
		Result.NodeCount,
		Result.ConnectionCount);

	return Result;
}

FOliveIRGraph FOliveGraphReader::ReadGraphSummary(const UEdGraph* Graph, const UBlueprint* OwningBlueprint)
{
	FOliveIRGraph Result;

	if (!Graph)
	{
		UE_LOG(LogOliveGraphReader, Warning, TEXT("ReadGraphSummary called with null Graph"));
		return Result;
	}

	// Clear previous state and rebuild ID map
	ClearCache();

	Result.Name = Graph->GetName();
	Result.GraphType = DetermineGraphType(Graph, OwningBlueprint);

	// Build the full node ID map for ID stability (needed if caller later requests pages)
	BuildNodeIdMap(Graph);

	// Set node count from the ID map (which filters out skipped nodes)
	Result.NodeCount = NodeIdMap.Num();

	// Count connections by traversing output pins without serializing nodes
	int32 ConnCount = 0;
	for (const auto& Pair : NodeIdMap)
	{
		const UEdGraphNode* Node = Pair.Key;
		if (!Node)
		{
			continue;
		}

		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output)
			{
				ConnCount += Pin->LinkedTo.Num();
			}
		}
	}
	Result.ConnectionCount = ConnCount;

	// For function graphs, extract the function signature
	if (Result.GraphType == TEXT("Function"))
	{
		ReadFunctionSignature(Graph, Result);
	}

	// Nodes array intentionally left empty for summary mode
	UE_LOG(LogOliveGraphReader, Log, TEXT("Graph summary read: %s (Nodes: %d, Connections: %d)"),
		*Result.Name,
		Result.NodeCount,
		Result.ConnectionCount);

	return Result;
}

FOliveIRGraph FOliveGraphReader::ReadGraphPage(
	const UEdGraph* Graph,
	const UBlueprint* OwningBlueprint,
	int32 Offset,
	int32 Limit)
{
	FOliveIRGraph Result;

	if (!Graph)
	{
		UE_LOG(LogOliveGraphReader, Warning, TEXT("ReadGraphPage called with null Graph"));
		return Result;
	}

	// Clear previous state
	ClearCache();

	Result.Name = Graph->GetName();
	Result.GraphType = DetermineGraphType(Graph, OwningBlueprint);

	// Build the FULL node ID map -- this is required even for paged reads
	// so that cross-page connection references resolve correctly
	BuildNodeIdMap(Graph);

	// Serialize only the requested slice of nodes
	int32 Index = 0;
	for (const auto& Pair : NodeIdMap)
	{
		if (Index >= Offset + Limit)
		{
			break;
		}

		if (Index >= Offset)
		{
			const UEdGraphNode* Node = Pair.Key;
			if (Node)
			{
				FOliveIRNode NodeIR = NodeSerializer->SerializeNode(Node, NodeIdMap);
				NodeIR.Id = Pair.Value;
				Result.Nodes.Add(MoveTemp(NodeIR));
			}
		}
		Index++;
	}

	// For function graphs, extract the function signature
	if (Result.GraphType == TEXT("Function"))
	{
		ReadFunctionSignature(Graph, Result);
	}

	// Calculate connection count for the page nodes only
	CalculateStatistics(Result);

	// Override NodeCount with total count (not page count)
	// so the caller knows the full graph size
	Result.NodeCount = NodeIdMap.Num();

	UE_LOG(LogOliveGraphReader, Log, TEXT("Graph page read: %s (Page nodes: %d, Total nodes: %d, Offset: %d)"),
		*Result.Name,
		Result.Nodes.Num(),
		Result.NodeCount,
		Offset);

	return Result;
}

TArray<FOliveIRNode> FOliveGraphReader::ReadNodes(
	const TArray<UEdGraphNode*>& Nodes,
	const FString& IdPrefix)
{
	TArray<FOliveIRNode> Result;

	// Build a temporary node ID map
	TMap<const UEdGraphNode*, FString> TempNodeIdMap;
	int32 NodeIndex = 0;

	for (UEdGraphNode* Node : Nodes)
	{
		if (!Node || ShouldSkipNode(Node))
		{
			continue;
		}

		FString NodeId = FString::Printf(TEXT("%s%d"), *IdPrefix, NodeIndex++);
		TempNodeIdMap.Add(Node, NodeId);
	}

	// Serialize nodes
	for (const auto& Pair : TempNodeIdMap)
	{
		const UEdGraphNode* Node = Pair.Key;
		const FString& NodeId = Pair.Value;

		FOliveIRNode NodeIR = NodeSerializer->SerializeNode(Node, TempNodeIdMap);
		NodeIR.Id = NodeId;
		Result.Add(MoveTemp(NodeIR));
	}

	return Result;
}

void FOliveGraphReader::ClearCache()
{
	NodeIdMap.Empty();
}

// ============================================================================
// Internal Helper Methods
// ============================================================================

FString FOliveGraphReader::GenerateNodeId(const UEdGraphNode* Node, int32 Index)
{
	// Simple sequential ID generation
	return FString::Printf(TEXT("node_%d"), Index);
}

FString FOliveGraphReader::ResolveConnection(const UEdGraphPin* Pin)
{
	if (!Pin || Pin->LinkedTo.Num() == 0)
	{
		return FString();
	}

	// Get the first connected pin
	const UEdGraphPin* LinkedPin = Pin->LinkedTo[0];
	if (!LinkedPin)
	{
		return FString();
	}

	// Get the owning node of the linked pin
	const UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
	if (!LinkedNode)
	{
		return FString();
	}

	// Look up the node ID
	const FString* NodeId = NodeIdMap.Find(LinkedNode);
	if (!NodeId)
	{
		UE_LOG(LogOliveGraphReader, Warning, TEXT("Could not resolve connection - node not in ID map: %s"),
			*LinkedNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
		return FString();
	}

	// Format: node_id.pin_name
	return FString::Printf(TEXT("%s.%s"), **NodeId, *LinkedPin->GetName());
}

void FOliveGraphReader::CalculateStatistics(FOliveIRGraph& Graph)
{
	Graph.NodeCount = Graph.Nodes.Num();

	// Count all connections (each connection is counted once, from output to input)
	int32 ConnectionCount = 0;

	for (const FOliveIRNode& Node : Graph.Nodes)
	{
		// Count output pin connections
		for (const FOliveIRPin& Pin : Node.OutputPins)
		{
			// Single connection
			if (!Pin.Connection.IsEmpty())
			{
				ConnectionCount++;
			}

			// Multiple connections (for data pins)
			ConnectionCount += Pin.Connections.Num();
		}
	}

	Graph.ConnectionCount = ConnectionCount;
}

FString FOliveGraphReader::DetermineGraphType(const UEdGraph* Graph, const UBlueprint* OwningBlueprint) const
{
	if (!Graph)
	{
		return TEXT("Unknown");
	}

	// Check if it's an event graph (Ubergraph page)
	if (OwningBlueprint)
	{
		if (OwningBlueprint->UbergraphPages.Contains(const_cast<UEdGraph*>(Graph)))
		{
			return TEXT("EventGraph");
		}

		if (OwningBlueprint->FunctionGraphs.Contains(const_cast<UEdGraph*>(Graph)))
		{
			return TEXT("Function");
		}

		if (OwningBlueprint->MacroGraphs.Contains(const_cast<UEdGraph*>(Graph)))
		{
			return TEXT("Macro");
		}

		// Check for delegate signature graphs
		if (OwningBlueprint->DelegateSignatureGraphs.Contains(const_cast<UEdGraph*>(Graph)))
		{
			return TEXT("DelegateSignature");
		}
	}

	// Fallback: use FBlueprintEditorUtils
	if (FBlueprintEditorUtils::IsEventGraph(Graph))
	{
		return TEXT("EventGraph");
	}

	// Check the schema
	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (Schema)
	{
		// The graph name can hint at its type
		FString GraphName = Graph->GetName();
		if (GraphName.StartsWith(TEXT("AnimGraph")))
		{
			return TEXT("AnimGraph");
		}
	}

	// Default to Function if we can't determine
	return TEXT("Unknown");
}

bool FOliveGraphReader::ShouldSkipNode(const UEdGraphNode* Node) const
{
	if (!Node)
	{
		return true;
	}

	// Skip tunnel nodes - they are internal implementation details
	if (Node->IsA<UK2Node_Tunnel>())
	{
		// But don't skip composite nodes (collapsed graphs)
		if (!Node->IsA<UK2Node_Composite>())
		{
			return true;
		}
	}

	// Skip nodes that are flagged for deprecation and are non-functional
	// (bDeprecated alone doesn't mean skip - the node still exists)

	return false;
}

void FOliveGraphReader::ReadFunctionSignature(const UEdGraph* Graph, FOliveIRGraph& OutGraph)
{
	if (!Graph || !PinSerializer.IsValid())
	{
		return;
	}

	// Find the entry and result nodes
	UK2Node_FunctionEntry* EntryNode = FindFunctionEntryNode(Graph);
	UK2Node_FunctionResult* ResultNode = FindFunctionResultNode(Graph);

	if (!EntryNode)
	{
		UE_LOG(LogOliveGraphReader, Warning, TEXT("Function graph '%s' has no entry node"), *Graph->GetName());
		return;
	}

	// Extract function metadata from entry node
	const FKismetUserDeclaredFunctionMetadata& Metadata = EntryNode->MetaData;

	if (!Metadata.Category.IsEmpty())
	{
		OutGraph.Category = Metadata.Category.ToString();
	}
	if (!Metadata.ToolTip.IsEmpty())
	{
		OutGraph.Description = Metadata.ToolTip.ToString();
	}

	// Extract function flags
	if (const UFunction* SignatureFunction = EntryNode->FindSignatureFunction())
	{
		OutGraph.bIsPure = SignatureFunction->HasAnyFunctionFlags(FUNC_BlueprintPure);
		OutGraph.bIsStatic = SignatureFunction->HasAnyFunctionFlags(FUNC_Static);
		OutGraph.bIsConst = SignatureFunction->HasAnyFunctionFlags(FUNC_Const);

		// Access specifier
		if (SignatureFunction->HasAnyFunctionFlags(FUNC_Public))
		{
			OutGraph.Access = TEXT("public");
		}
		else if (SignatureFunction->HasAnyFunctionFlags(FUNC_Protected))
		{
			OutGraph.Access = TEXT("protected");
		}
		else
		{
			OutGraph.Access = TEXT("private");
		}

		// Keywords
		if (SignatureFunction->HasMetaData(FBlueprintMetadata::MD_FunctionKeywords))
		{
			FString Keywords = SignatureFunction->GetMetaData(FBlueprintMetadata::MD_FunctionKeywords);
			Keywords.ParseIntoArray(OutGraph.Keywords, TEXT(" "), true);
		}
	}

	// Extract inputs from entry node's output pins
	// (Entry node outputs are the function's input parameters)
	for (UEdGraphPin* Pin : EntryNode->Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Output)
		{
			continue;
		}

		// Skip exec and self pins
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec ||
			Pin->PinName == UEdGraphSchema_K2::PN_Self)
		{
			continue;
		}

		FOliveIRPin InputPin;
		InputPin.Name = Pin->GetName();
		InputPin.DisplayName = Pin->GetDisplayName().ToString();
		InputPin.Type = PinSerializer->SerializePinType(Pin->PinType);
		InputPin.bIsInput = true;
		InputPin.bIsExec = false;
		InputPin.DefaultValue = Pin->DefaultValue;

		OutGraph.Inputs.Add(MoveTemp(InputPin));
	}

	// Extract outputs from result node's input pins
	// (Result node inputs are the function's return values)
	if (ResultNode)
	{
		for (UEdGraphPin* Pin : ResultNode->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input)
			{
				continue;
			}

			// Skip exec pins
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				continue;
			}

			FOliveIRPin OutputPin;
			OutputPin.Name = Pin->GetName();
			OutputPin.DisplayName = Pin->GetDisplayName().ToString();
			OutputPin.Type = PinSerializer->SerializePinType(Pin->PinType);
			OutputPin.bIsInput = false;
			OutputPin.bIsExec = false;

			OutGraph.Outputs.Add(MoveTemp(OutputPin));
		}
	}
}

UK2Node_FunctionEntry* FOliveGraphReader::FindFunctionEntryNode(const UEdGraph* Graph) const
{
	if (!Graph)
	{
		return nullptr;
	}

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
		{
			return EntryNode;
		}
	}

	return nullptr;
}

UK2Node_FunctionResult* FOliveGraphReader::FindFunctionResultNode(const UEdGraph* Graph) const
{
	if (!Graph)
	{
		return nullptr;
	}

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (UK2Node_FunctionResult* ResultNode = Cast<UK2Node_FunctionResult>(Node))
		{
			return ResultNode;
		}
	}

	return nullptr;
}

void FOliveGraphReader::BuildNodeIdMap(const UEdGraph* Graph)
{
	if (!Graph)
	{
		return;
	}

	NodeIdMap.Empty();
	int32 NodeIndex = 0;

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node || ShouldSkipNode(Node))
		{
			continue;
		}

		FString NodeId = GenerateNodeId(Node, NodeIndex);
		NodeIdMap.Add(Node, NodeId);
		NodeIndex++;
	}

	UE_LOG(LogOliveGraphReader, Verbose, TEXT("Built node ID map with %d entries for graph: %s"),
		NodeIdMap.Num(),
		*Graph->GetName());
}
