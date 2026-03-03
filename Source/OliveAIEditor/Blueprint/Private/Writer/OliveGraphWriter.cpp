// Copyright Bode Software. All Rights Reserved.

#include "OliveGraphWriter.h"
#include "OliveNodeFactory.h"
#include "OlivePinConnector.h"
#include "Services/OliveTransactionManager.h"
#include "IR/CommonIR.h"

// Blueprint/Graph includes
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Tunnel.h"
#include "K2Node_Composite.h"

// JSON includes
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// Utility includes
#include "Kismet2/BlueprintEditorUtils.h"
#include "Editor.h"

DEFINE_LOG_CATEGORY(LogOliveGraphWriter);

// ============================================================================
// FOliveGraphWriter Singleton
// ============================================================================

FOliveGraphWriter& FOliveGraphWriter::Get()
{
	static FOliveGraphWriter Instance;
	return Instance;
}

FOliveGraphWriter::FOliveGraphWriter()
{
	// Initialize with empty caches
}

// ============================================================================
// Node Operations
// ============================================================================

FOliveBlueprintWriteResult FOliveGraphWriter::AddNode(
	const FString& BlueprintPath,
	const FString& GraphName,
	const FString& NodeType,
	const TMap<FString, FString>& NodeProperties,
	int32 PosX,
	int32 PosY)
{
	// Check PIE state
	if (IsPIEActive())
	{
		return FOliveBlueprintWriteResult::Error(
			TEXT("Cannot modify Blueprints while Play-In-Editor is active"));
	}

	// Load the Blueprint
	FString LoadError;
	UBlueprint* Blueprint = LoadBlueprintForEditing(BlueprintPath, LoadError);
	if (!Blueprint)
	{
		return FOliveBlueprintWriteResult::Error(LoadError, BlueprintPath);
	}

	// Find the graph
	UEdGraph* Graph = FindGraph(Blueprint, GraphName);
	if (!Graph)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Graph '%s' not found in Blueprint '%s'"), *GraphName, *BlueprintPath),
			BlueprintPath);
	}

	// Create transaction for undo support
	OLIVE_SCOPED_TRANSACTION(FText::Format(
		NSLOCTEXT("OliveGraphWriter", "AddNode", "Add {0} Node"),
		FText::FromString(NodeType)));

	Blueprint->Modify();

	// Create node via factory
	FOliveNodeFactory& Factory = FOliveNodeFactory::Get();
	UEdGraphNode* NewNode = Factory.CreateNode(Blueprint, Graph, NodeType, NodeProperties, PosX, PosY);

	if (!NewNode)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Failed to create node: %s"), *Factory.GetLastError()),
			BlueprintPath);
	}

	// Generate ID and cache the node
	FString NodeId = GenerateNodeId(BlueprintPath);
	CacheNode(BlueprintPath, NodeId, NewNode);

	// Mark the Blueprint as modified
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	UE_LOG(LogOliveGraphWriter, Log, TEXT("Added node '%s' (ID: %s) to graph '%s' in '%s' at (%d, %d)"),
		*NodeType, *NodeId, *GraphName, *BlueprintPath, PosX, PosY);

	return FOliveBlueprintWriteResult::SuccessWithNode(BlueprintPath, NodeId);
}

FOliveBlueprintWriteResult FOliveGraphWriter::AddNodes(
	const FString& BlueprintPath,
	const FString& GraphName,
	const TArray<FOliveIRNode>& Nodes)
{
	// Check PIE state
	if (IsPIEActive())
	{
		return FOliveBlueprintWriteResult::Error(
			TEXT("Cannot modify Blueprints while Play-In-Editor is active"));
	}

	if (Nodes.Num() == 0)
	{
		FOliveBlueprintWriteResult Result = FOliveBlueprintWriteResult::Success(BlueprintPath);
		Result.AddWarning(TEXT("No nodes specified for batch creation"));
		return Result;
	}

	// Load the Blueprint
	FString LoadError;
	UBlueprint* Blueprint = LoadBlueprintForEditing(BlueprintPath, LoadError);
	if (!Blueprint)
	{
		return FOliveBlueprintWriteResult::Error(LoadError, BlueprintPath);
	}

	// Find the graph
	UEdGraph* Graph = FindGraph(Blueprint, GraphName);
	if (!Graph)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Graph '%s' not found in Blueprint '%s'"), *GraphName, *BlueprintPath),
			BlueprintPath);
	}

	// Create transaction for undo support (all nodes in single transaction)
	OLIVE_SCOPED_TRANSACTION(FText::Format(
		NSLOCTEXT("OliveGraphWriter", "AddNodes", "Add {0} Nodes"),
		FText::AsNumber(Nodes.Num())));

	Blueprint->Modify();

	// Track created node IDs
	TArray<FString> CreatedNodeIds;
	TArray<FString> Warnings;

	FOliveNodeFactory& Factory = FOliveNodeFactory::Get();

	for (const FOliveIRNode& IRNode : Nodes)
	{
		// Convert IR properties to factory properties
		TMap<FString, FString> Properties = ConvertIRNodeProperties(IRNode);

		// Determine position - IR nodes may have position in properties
		int32 PosX = 0;
		int32 PosY = 0;
		if (const FString* XStr = IRNode.Properties.Find(TEXT("pos_x")))
		{
			PosX = FCString::Atoi(**XStr);
		}
		if (const FString* YStr = IRNode.Properties.Find(TEXT("pos_y")))
		{
			PosY = FCString::Atoi(**YStr);
		}

		// Create the node
		UEdGraphNode* NewNode = Factory.CreateNode(Blueprint, Graph, IRNode.Type, Properties, PosX, PosY);

		if (NewNode)
		{
			// If IR node has an ID, use it; otherwise generate one
			FString NodeId;
			if (!IRNode.Id.IsEmpty())
			{
				NodeId = IRNode.Id;
				// Update next index if needed to avoid collisions
				if (NodeId.StartsWith(TEXT("node_")))
				{
					FString IndexStr = NodeId.RightChop(5); // Remove "node_"
					int32 Index = FCString::Atoi(*IndexStr);
					FScopeLock Lock(&CacheLock);
					int32& NextIndex = NextNodeIndex.FindOrAdd(BlueprintPath);
					if (Index >= NextIndex)
					{
						NextIndex = Index + 1;
					}
				}
			}
			else
			{
				NodeId = GenerateNodeId(BlueprintPath);
			}

			CacheNode(BlueprintPath, NodeId, NewNode);
			CreatedNodeIds.Add(NodeId);

			UE_LOG(LogOliveGraphWriter, Verbose, TEXT("Created node '%s' (ID: %s)"), *IRNode.Type, *NodeId);
		}
		else
		{
			Warnings.Add(FString::Printf(TEXT("Failed to create node type '%s': %s"),
				*IRNode.Type, *Factory.GetLastError()));
		}
	}

	// Mark the Blueprint as modified
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	UE_LOG(LogOliveGraphWriter, Log, TEXT("Added %d/%d nodes to graph '%s' in '%s'"),
		CreatedNodeIds.Num(), Nodes.Num(), *GraphName, *BlueprintPath);

	// Build result
	FOliveBlueprintWriteResult Result;
	Result.bSuccess = CreatedNodeIds.Num() > 0;
	Result.AssetPath = BlueprintPath;
	Result.CreatedNodeIds = CreatedNodeIds;
	Result.Warnings = Warnings;

	if (CreatedNodeIds.Num() == 1)
	{
		Result.CreatedNodeId = CreatedNodeIds[0];
	}

	if (CreatedNodeIds.Num() < Nodes.Num())
	{
		Result.AddWarning(FString::Printf(TEXT("Only %d of %d nodes were created"),
			CreatedNodeIds.Num(), Nodes.Num()));
	}

	return Result;
}

FOliveBlueprintWriteResult FOliveGraphWriter::RemoveNode(
	const FString& BlueprintPath,
	const FString& GraphName,
	const FString& NodeId)
{
	// Check PIE state
	if (IsPIEActive())
	{
		return FOliveBlueprintWriteResult::Error(
			TEXT("Cannot modify Blueprints while Play-In-Editor is active"));
	}

	// Load the Blueprint
	FString LoadError;
	UBlueprint* Blueprint = LoadBlueprintForEditing(BlueprintPath, LoadError);
	if (!Blueprint)
	{
		return FOliveBlueprintWriteResult::Error(LoadError, BlueprintPath);
	}

	// Find the graph
	UEdGraph* Graph = FindGraph(Blueprint, GraphName);
	if (!Graph)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Graph '%s' not found in Blueprint '%s'"), *GraphName, *BlueprintPath),
			BlueprintPath);
	}

	// Find the node
	UEdGraphNode* Node = FindNodeById(Blueprint, Graph, BlueprintPath, NodeId);
	if (!Node)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Node '%s' not found in graph '%s'"), *NodeId, *GraphName),
			BlueprintPath);
	}

	// Create transaction for undo support
	OLIVE_SCOPED_TRANSACTION(FText::Format(
		NSLOCTEXT("OliveGraphWriter", "RemoveNode", "Remove Node {0}"),
		FText::FromString(NodeId)));

	Blueprint->Modify();

	// Remove the node from the graph
	FBlueprintEditorUtils::RemoveNode(Blueprint, Node, /*bDontRecompile=*/true);

	// Remove from cache
	RemoveFromCache(BlueprintPath, NodeId);

	// Mark the Blueprint as modified
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	UE_LOG(LogOliveGraphWriter, Log, TEXT("Removed node '%s' from graph '%s' in '%s'"),
		*NodeId, *GraphName, *BlueprintPath);

	return FOliveBlueprintWriteResult::Success(BlueprintPath, NodeId);
}

FOliveBlueprintWriteResult FOliveGraphWriter::SetNodeProperty(
	const FString& BlueprintPath,
	const FString& GraphName,
	const FString& NodeId,
	const FString& PropertyName,
	const FString& PropertyValue)
{
	// Check PIE state
	if (IsPIEActive())
	{
		return FOliveBlueprintWriteResult::Error(
			TEXT("Cannot modify Blueprints while Play-In-Editor is active"));
	}

	// Load the Blueprint
	FString LoadError;
	UBlueprint* Blueprint = LoadBlueprintForEditing(BlueprintPath, LoadError);
	if (!Blueprint)
	{
		return FOliveBlueprintWriteResult::Error(LoadError, BlueprintPath);
	}

	// Find the graph
	UEdGraph* Graph = FindGraph(Blueprint, GraphName);
	if (!Graph)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Graph '%s' not found in Blueprint '%s'"), *GraphName, *BlueprintPath),
			BlueprintPath);
	}

	// Find the node
	UEdGraphNode* Node = FindNodeById(Blueprint, Graph, BlueprintPath, NodeId);
	if (!Node)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Node '%s' not found in graph '%s'"), *NodeId, *GraphName),
			BlueprintPath);
	}

	// Create transaction for undo support
	OLIVE_SCOPED_TRANSACTION(FText::Format(
		NSLOCTEXT("OliveGraphWriter", "SetNodeProperty", "Set Node Property {0}"),
		FText::FromString(PropertyName)));

	Blueprint->Modify();
	Node->Modify();

	// Try to set the property using reflection
	FProperty* Property = Node->GetClass()->FindPropertyByName(FName(*PropertyName));
	if (Property)
	{
		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Node);

		if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
		{
			BoolProp->SetPropertyValue(ValuePtr, PropertyValue.ToBool());
		}
		else if (FIntProperty* IntProp = CastField<FIntProperty>(Property))
		{
			IntProp->SetPropertyValue(ValuePtr, FCString::Atoi(*PropertyValue));
		}
		else if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Property))
		{
			FloatProp->SetPropertyValue(ValuePtr, FCString::Atof(*PropertyValue));
		}
		else if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Property))
		{
			DoubleProp->SetPropertyValue(ValuePtr, FCString::Atod(*PropertyValue));
		}
		else if (FStrProperty* StrProp = CastField<FStrProperty>(Property))
		{
			StrProp->SetPropertyValue(ValuePtr, PropertyValue);
		}
		else if (FNameProperty* NameProp = CastField<FNameProperty>(Property))
		{
			NameProp->SetPropertyValue(ValuePtr, FName(*PropertyValue));
		}
		else if (FTextProperty* TextProp = CastField<FTextProperty>(Property))
		{
			TextProp->SetPropertyValue(ValuePtr, FText::FromString(PropertyValue));
		}
		else
		{
			// Try generic text import
			Property->ImportText_Direct(*PropertyValue, ValuePtr, Node, PPF_None);
		}

		// Reconstruct the node to apply changes
		Node->ReconstructNode();

		// Mark the Blueprint as modified
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

		UE_LOG(LogOliveGraphWriter, Log, TEXT("Set property '%s' = '%s' on node '%s' in '%s'"),
			*PropertyName, *PropertyValue, *NodeId, *BlueprintPath);

		return FOliveBlueprintWriteResult::Success(BlueprintPath, NodeId);
	}
	else
	{
		// Property not found - check if it's a node-specific property that needs special handling
		FOliveBlueprintWriteResult Result = FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Property '%s' not found on node '%s'"), *PropertyName, *NodeId),
			BlueprintPath);

		return Result;
	}
}

// ============================================================================
// Pin Operations
// ============================================================================

FOliveBlueprintWriteResult FOliveGraphWriter::ConnectPins(
	const FString& BlueprintPath,
	const FString& GraphName,
	const FString& SourcePinRef,
	const FString& TargetPinRef)
{
	// Check PIE state
	if (IsPIEActive())
	{
		return FOliveBlueprintWriteResult::Error(
			TEXT("Cannot modify Blueprints while Play-In-Editor is active"));
	}

	// Parse pin references
	FString SourceNodeId, SourcePinName;
	if (!ParsePinReference(SourcePinRef, SourceNodeId, SourcePinName))
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Invalid source pin reference format: '%s'. Expected 'node_id.pin_name'"), *SourcePinRef),
			BlueprintPath);
	}

	FString TargetNodeId, TargetPinName;
	if (!ParsePinReference(TargetPinRef, TargetNodeId, TargetPinName))
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Invalid target pin reference format: '%s'. Expected 'node_id.pin_name'"), *TargetPinRef),
			BlueprintPath);
	}

	UE_LOG(LogOliveGraphWriter, Log, TEXT("ConnectPins: '%s' (node='%s', pin='%s') -> '%s' (node='%s', pin='%s') in graph '%s'"),
		*SourcePinRef, *SourceNodeId, *SourcePinName,
		*TargetPinRef, *TargetNodeId, *TargetPinName,
		*GraphName);

	// Load the Blueprint
	FString LoadError;
	UBlueprint* Blueprint = LoadBlueprintForEditing(BlueprintPath, LoadError);
	if (!Blueprint)
	{
		return FOliveBlueprintWriteResult::Error(LoadError, BlueprintPath);
	}

	// Find the graph
	UEdGraph* Graph = FindGraph(Blueprint, GraphName);
	if (!Graph)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Graph '%s' not found in Blueprint '%s'"), *GraphName, *BlueprintPath),
			BlueprintPath);
	}

	// Find source node and pin
	UEdGraphNode* SourceNode = FindNodeById(Blueprint, Graph, BlueprintPath, SourceNodeId);
	if (!SourceNode)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Source node '%s' not found"), *SourceNodeId),
			BlueprintPath);
	}

	UEdGraphPin* SourcePin = FindPin(SourceNode, SourcePinName);
	if (!SourcePin && SourcePinName == TEXT("auto"))
	{
		// Resolve "auto" to first non-exec output pin (defensive fallback for v1.0 lowerer path)
		for (UEdGraphPin* TestPin : SourceNode->Pins)
		{
			if (TestPin && TestPin->Direction == EGPD_Output
				&& TestPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
				&& !TestPin->bHidden)
			{
				SourcePin = TestPin;
				UE_LOG(LogOliveGraphWriter, Log,
					TEXT("Auto-resolved source pin 'auto' on node '%s' to '%s'"),
					*SourceNodeId, *TestPin->GetName());
				break;
			}
		}
	}
	if (!SourcePin)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Source pin '%s' not found on node '%s'"), *SourcePinName, *SourceNodeId),
			BlueprintPath);
	}

	// Find target node and pin
	UEdGraphNode* TargetNode = FindNodeById(Blueprint, Graph, BlueprintPath, TargetNodeId);
	if (!TargetNode)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Target node '%s' not found"), *TargetNodeId),
			BlueprintPath);
	}

	UEdGraphPin* TargetPin = FindPin(TargetNode, TargetPinName);
	if (!TargetPin && TargetPinName == TEXT("auto"))
	{
		// Resolve "auto" to first non-exec input pin (defensive fallback for v1.0 lowerer path)
		for (UEdGraphPin* TestPin : TargetNode->Pins)
		{
			if (TestPin && TestPin->Direction == EGPD_Input
				&& TestPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
				&& !TestPin->bHidden)
			{
				TargetPin = TestPin;
				UE_LOG(LogOliveGraphWriter, Log,
					TEXT("Auto-resolved target pin 'auto' on node '%s' to '%s'"),
					*TargetNodeId, *TestPin->GetName());
				break;
			}
		}
	}
	if (!TargetPin)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Target pin '%s' not found on node '%s'"), *TargetPinName, *TargetNodeId),
			BlueprintPath);
	}

	// Use the pin connector to make the connection
	FOlivePinConnector& Connector = FOlivePinConnector::Get();
	FOliveBlueprintWriteResult Result = Connector.Connect(SourcePin, TargetPin, /*bAllowConversion=*/true);

	if (Result.bSuccess)
	{
		UE_LOG(LogOliveGraphWriter, Log, TEXT("Connected pins: %s -> %s in graph '%s' of '%s'"),
			*SourcePinRef, *TargetPinRef, *GraphName, *BlueprintPath);
	}

	return Result;
}

FOliveBlueprintWriteResult FOliveGraphWriter::DisconnectPins(
	const FString& BlueprintPath,
	const FString& GraphName,
	const FString& SourcePinRef,
	const FString& TargetPinRef)
{
	// Check PIE state
	if (IsPIEActive())
	{
		return FOliveBlueprintWriteResult::Error(
			TEXT("Cannot modify Blueprints while Play-In-Editor is active"));
	}

	// Parse pin references
	FString SourceNodeId, SourcePinName;
	if (!ParsePinReference(SourcePinRef, SourceNodeId, SourcePinName))
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Invalid source pin reference format: '%s'. Expected 'node_id.pin_name'"), *SourcePinRef),
			BlueprintPath);
	}

	FString TargetNodeId, TargetPinName;
	if (!ParsePinReference(TargetPinRef, TargetNodeId, TargetPinName))
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Invalid target pin reference format: '%s'. Expected 'node_id.pin_name'"), *TargetPinRef),
			BlueprintPath);
	}

	// Load the Blueprint
	FString LoadError;
	UBlueprint* Blueprint = LoadBlueprintForEditing(BlueprintPath, LoadError);
	if (!Blueprint)
	{
		return FOliveBlueprintWriteResult::Error(LoadError, BlueprintPath);
	}

	// Find the graph
	UEdGraph* Graph = FindGraph(Blueprint, GraphName);
	if (!Graph)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Graph '%s' not found in Blueprint '%s'"), *GraphName, *BlueprintPath),
			BlueprintPath);
	}

	// Find source node and pin
	UEdGraphNode* SourceNode = FindNodeById(Blueprint, Graph, BlueprintPath, SourceNodeId);
	if (!SourceNode)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Source node '%s' not found"), *SourceNodeId),
			BlueprintPath);
	}

	UEdGraphPin* SourcePin = FindPin(SourceNode, SourcePinName);
	if (!SourcePin)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Source pin '%s' not found on node '%s'"), *SourcePinName, *SourceNodeId),
			BlueprintPath);
	}

	// Find target node and pin
	UEdGraphNode* TargetNode = FindNodeById(Blueprint, Graph, BlueprintPath, TargetNodeId);
	if (!TargetNode)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Target node '%s' not found"), *TargetNodeId),
			BlueprintPath);
	}

	UEdGraphPin* TargetPin = FindPin(TargetNode, TargetPinName);
	if (!TargetPin)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Target pin '%s' not found on node '%s'"), *TargetPinName, *TargetNodeId),
			BlueprintPath);
	}

	// Use the pin connector to disconnect
	FOlivePinConnector& Connector = FOlivePinConnector::Get();
	FOliveBlueprintWriteResult Result = Connector.Disconnect(SourcePin, TargetPin);

	if (Result.bSuccess)
	{
		UE_LOG(LogOliveGraphWriter, Log, TEXT("Disconnected pins: %s <-> %s in graph '%s' of '%s'"),
			*SourcePinRef, *TargetPinRef, *GraphName, *BlueprintPath);
	}

	return Result;
}

FOliveBlueprintWriteResult FOliveGraphWriter::DisconnectAllFromPin(
	const FString& BlueprintPath,
	const FString& GraphName,
	const FString& PinRef)
{
	// Check PIE state
	if (IsPIEActive())
	{
		return FOliveBlueprintWriteResult::Error(
			TEXT("Cannot modify Blueprints while Play-In-Editor is active"));
	}

	// Parse pin reference
	FString NodeId, PinName;
	if (!ParsePinReference(PinRef, NodeId, PinName))
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Invalid pin reference format: '%s'. Expected 'node_id.pin_name'"), *PinRef),
			BlueprintPath);
	}

	// Load the Blueprint
	FString LoadError;
	UBlueprint* Blueprint = LoadBlueprintForEditing(BlueprintPath, LoadError);
	if (!Blueprint)
	{
		return FOliveBlueprintWriteResult::Error(LoadError, BlueprintPath);
	}

	// Find the graph
	UEdGraph* Graph = FindGraph(Blueprint, GraphName);
	if (!Graph)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Graph '%s' not found in Blueprint '%s'"), *GraphName, *BlueprintPath),
			BlueprintPath);
	}

	// Find node and pin
	UEdGraphNode* Node = FindNodeById(Blueprint, Graph, BlueprintPath, NodeId);
	if (!Node)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Node '%s' not found"), *NodeId),
			BlueprintPath);
	}

	UEdGraphPin* Pin = FindPin(Node, PinName);
	if (!Pin)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Pin '%s' not found on node '%s'"), *PinName, *NodeId),
			BlueprintPath);
	}

	// Use the pin connector to disconnect all
	FOlivePinConnector& Connector = FOlivePinConnector::Get();
	FOliveBlueprintWriteResult Result = Connector.DisconnectAll(Pin);

	if (Result.bSuccess)
	{
		UE_LOG(LogOliveGraphWriter, Log, TEXT("Disconnected all connections from pin: %s in graph '%s' of '%s'"),
			*PinRef, *GraphName, *BlueprintPath);
	}

	return Result;
}

FOliveBlueprintWriteResult FOliveGraphWriter::SetPinDefault(
	const FString& BlueprintPath,
	const FString& GraphName,
	const FString& PinRef,
	const FString& DefaultValue)
{
	// Check PIE state
	if (IsPIEActive())
	{
		return FOliveBlueprintWriteResult::Error(
			TEXT("Cannot modify Blueprints while Play-In-Editor is active"));
	}

	// Parse pin reference
	FString NodeId, PinName;
	if (!ParsePinReference(PinRef, NodeId, PinName))
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Invalid pin reference format: '%s'. Expected 'node_id.pin_name'"), *PinRef),
			BlueprintPath);
	}

	// Load the Blueprint
	FString LoadError;
	UBlueprint* Blueprint = LoadBlueprintForEditing(BlueprintPath, LoadError);
	if (!Blueprint)
	{
		return FOliveBlueprintWriteResult::Error(LoadError, BlueprintPath);
	}

	// Find the graph
	UEdGraph* Graph = FindGraph(Blueprint, GraphName);
	if (!Graph)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Graph '%s' not found in Blueprint '%s'"), *GraphName, *BlueprintPath),
			BlueprintPath);
	}

	// Find node and pin
	UEdGraphNode* Node = FindNodeById(Blueprint, Graph, BlueprintPath, NodeId);
	if (!Node)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Node '%s' not found"), *NodeId),
			BlueprintPath);
	}

	UEdGraphPin* Pin = FindPin(Node, PinName);
	if (!Pin)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Pin '%s' not found on node '%s'"), *PinName, *NodeId),
			BlueprintPath);
	}

	// Verify it's an input pin
	if (Pin->Direction != EGPD_Input)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Pin '%s' is not an input pin; cannot set default value"), *PinRef),
			BlueprintPath);
	}

	// Verify the pin is not connected
	if (Pin->HasAnyConnections())
	{
		FOliveBlueprintWriteResult Result = FOliveBlueprintWriteResult::Success(BlueprintPath);
		Result.AddWarning(FString::Printf(TEXT("Pin '%s' is connected; default value may be ignored"), *PinRef));
		// Continue anyway - the value can still be set
	}

	// Create transaction for undo support
	OLIVE_SCOPED_TRANSACTION(FText::Format(
		NSLOCTEXT("OliveGraphWriter", "SetPinDefault", "Set Pin Default: {0}"),
		FText::FromString(PinRef)));

	Blueprint->Modify();
	Node->Modify();

	// Get the schema to properly set the default value
	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (Schema)
	{
		Schema->TrySetDefaultValue(*Pin, DefaultValue);
	}
	else
	{
		// Fallback: set directly
		Pin->DefaultValue = DefaultValue;
	}

	// Mark the Blueprint as modified
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	UE_LOG(LogOliveGraphWriter, Log, TEXT("Set default value '%s' on pin '%s' in graph '%s' of '%s'"),
		*DefaultValue, *PinRef, *GraphName, *BlueprintPath);

	return FOliveBlueprintWriteResult::Success(BlueprintPath);
}

// ============================================================================
// Session Management
// ============================================================================

void FOliveGraphWriter::ClearNodeCache(const FString& BlueprintPath)
{
	FScopeLock Lock(&CacheLock);
	NodeIdCache.Remove(BlueprintPath);
	NextNodeIndex.Remove(BlueprintPath);

	UE_LOG(LogOliveGraphWriter, Log, TEXT("Cleared node cache for '%s'"), *BlueprintPath);
}

void FOliveGraphWriter::ClearAllCaches()
{
	FScopeLock Lock(&CacheLock);
	NodeIdCache.Empty();
	NextNodeIndex.Empty();

	UE_LOG(LogOliveGraphWriter, Log, TEXT("Cleared all node caches"));
}

UEdGraphNode* FOliveGraphWriter::GetCachedNode(const FString& BlueprintPath, const FString& NodeId)
{
	FScopeLock Lock(&CacheLock);

	if (const TMap<FString, TWeakObjectPtr<UEdGraphNode>>* BlueprintCache = NodeIdCache.Find(BlueprintPath))
	{
		if (const TWeakObjectPtr<UEdGraphNode>* WeakNode = BlueprintCache->Find(NodeId))
		{
			if (WeakNode->IsValid())
			{
				return WeakNode->Get();
			}
		}
	}

	return nullptr;
}

TArray<FString> FOliveGraphWriter::GetCachedNodeIds(const FString& BlueprintPath) const
{
	FScopeLock Lock(&CacheLock);

	TArray<FString> NodeIds;
	if (const TMap<FString, TWeakObjectPtr<UEdGraphNode>>* BlueprintCache = NodeIdCache.Find(BlueprintPath))
	{
		BlueprintCache->GetKeys(NodeIds);
	}

	return NodeIds;
}

bool FOliveGraphWriter::HasCachedNode(const FString& BlueprintPath, const FString& NodeId) const
{
	FScopeLock Lock(&CacheLock);

	if (const TMap<FString, TWeakObjectPtr<UEdGraphNode>>* BlueprintCache = NodeIdCache.Find(BlueprintPath))
	{
		return BlueprintCache->Contains(NodeId);
	}

	return false;
}

FString FOliveGraphWriter::CacheExternalNode(const FString& BlueprintPath, UEdGraphNode* Node)
{
	FString NodeId = GenerateNodeId(BlueprintPath);
	CacheNode(BlueprintPath, NodeId, Node);
	return NodeId;
}

// ============================================================================
// Private Helper Methods
// ============================================================================

UEdGraph* FOliveGraphWriter::FindGraph(UBlueprint* Blueprint, const FString& GraphName)
{
	if (!Blueprint)
	{
		return nullptr;
	}

	// Check Ubergraph pages (event graphs)
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph && Graph->GetName() == GraphName)
		{
			return Graph;
		}
	}

	// Check function graphs
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == GraphName)
		{
			return Graph;
		}
	}

	// Check interface implementation graphs
	for (FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
	{
		for (UEdGraph* Graph : InterfaceDesc.Graphs)
		{
			if (Graph && Graph->GetName() == GraphName)
			{
				return Graph;
			}
		}
	}

	// Check macro graphs
	for (UEdGraph* Graph : Blueprint->MacroGraphs)
	{
		if (Graph && Graph->GetName() == GraphName)
		{
			return Graph;
		}
	}

	// Special case: "EventGraph" might be the first Ubergraph page
	if (GraphName.Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase) && Blueprint->UbergraphPages.Num() > 0)
	{
		return Blueprint->UbergraphPages[0];
	}

	return nullptr;
}

UEdGraphNode* FOliveGraphWriter::FindNodeById(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const FString& BlueprintPath,
	const FString& NodeId)
{
	UE_LOG(LogOliveGraphWriter, Verbose, TEXT("FindNodeById: looking for '%s' in graph '%s' of '%s'"),
		*NodeId, *Graph->GetName(), *BlueprintPath);

	// First check the cache
	UEdGraphNode* CachedNode = GetCachedNode(BlueprintPath, NodeId);
	if (CachedNode)
	{
		UE_LOG(LogOliveGraphWriter, Verbose, TEXT("FindNodeById: '%s' found in cache"), *NodeId);

		// Verify the node is still in the graph
		if (Graph->Nodes.Contains(CachedNode))
		{
			return CachedNode;
		}
		else
		{
			UE_LOG(LogOliveGraphWriter, Verbose, TEXT("FindNodeById: '%s' was in cache but no longer in graph, removed from cache"), *NodeId);

			// Node was removed from graph - clear from cache
			RemoveFromCache(BlueprintPath, NodeId);
		}
	}

	// Node not in cache - try to find it by GUID if ID looks like a GUID
	// (This supports external tools that may use GUIDs as IDs)
	FGuid ParsedGuid;
	if (FGuid::Parse(NodeId, ParsedGuid))
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == ParsedGuid)
			{
				UE_LOG(LogOliveGraphWriter, Verbose, TEXT("FindNodeById: '%s' matched by GUID"), *NodeId);

				// Cache for future lookups
				CacheNode(BlueprintPath, NodeId, Node);
				return Node;
			}
		}
	}

	// Try to match by node_X format - rebuild the reader's sequential index
	if (NodeId.StartsWith(TEXT("node_")))
	{
		FString IndexStr = NodeId.RightChop(5);
		int32 TargetIndex = FCString::Atoi(*IndexStr);

		// Replicate the reader's sequential indexing: iterate graph nodes,
		// skip tunnel nodes (but not composites), count non-skipped nodes
		int32 CurrentIndex = 0;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			// Skip tunnel nodes (same logic as FOliveGraphReader::ShouldSkipNode)
			if (Node->IsA<UK2Node_Tunnel>() && !Node->IsA<UK2Node_Composite>())
			{
				continue;
			}

			if (CurrentIndex == TargetIndex)
			{
				UE_LOG(LogOliveGraphWriter, Verbose, TEXT("FindNodeById: '%s' matched by sequential index (%d)"), *NodeId, TargetIndex);

				// Cache for future lookups
				CacheNode(BlueprintPath, NodeId, Node);
				return Node;
			}
			CurrentIndex++;
		}
	}

	// Build available nodes list for diagnostic logging
	FString AvailableNodes;
	int32 Idx = 0;
	for (UEdGraphNode* N : Graph->Nodes)
	{
		if (!N) continue;
		// Skip tunnel nodes (same logic as above)
		if (N->IsA<UK2Node_Tunnel>() && !N->IsA<UK2Node_Composite>())
			continue;

		AvailableNodes += FString::Printf(TEXT("\n  node_%d: %s [%s]"),
			Idx, *N->GetNodeTitle(ENodeTitleType::ListView).ToString(),
			*N->GetClass()->GetName());
		Idx++;
	}
	UE_LOG(LogOliveGraphWriter, Warning,
		TEXT("FindNodeById: '%s' NOT FOUND in graph '%s' (%d nodes). Available nodes:%s"),
		*NodeId, *Graph->GetName(), Idx, *AvailableNodes);

	return nullptr;
}

UEdGraphPin* FOliveGraphWriter::FindPin(UEdGraphNode* Node, const FString& PinName)
{
	if (!Node)
	{
		return nullptr;
	}

	// Try exact name match first
	UEdGraphPin* Pin = Node->FindPin(FName(*PinName));
	if (Pin)
	{
		return Pin;
	}

	// Try searching by display name
	for (UEdGraphPin* TestPin : Node->Pins)
	{
		if (TestPin && TestPin->GetDisplayName().ToString() == PinName)
		{
			return TestPin;
		}
	}

	// Try case-insensitive match
	for (UEdGraphPin* TestPin : Node->Pins)
	{
		if (TestPin && TestPin->GetName().Equals(PinName, ESearchCase::IgnoreCase))
		{
			return TestPin;
		}
	}

	// Try partial match for common pin names (e.g., "execute" for "execute " or " execute")
	FString TrimmedPinName = PinName.TrimStartAndEnd();
	for (UEdGraphPin* TestPin : Node->Pins)
	{
		if (TestPin)
		{
			FString TestName = TestPin->GetName().TrimStartAndEnd();
			if (TestName.Equals(TrimmedPinName, ESearchCase::IgnoreCase))
			{
				return TestPin;
			}
		}
	}

	return nullptr;
}

bool FOliveGraphWriter::ParsePinReference(const FString& PinRef, FString& OutNodeId, FString& OutPinName)
{
	// Format: "node_id.pin_name"
	// Also accept "node_id:pin_name" (common AI mistake) with auto-correction
	int32 SepIndex;
	if (PinRef.FindChar(TEXT('.'), SepIndex))
	{
		OutNodeId = PinRef.Left(SepIndex);
		OutPinName = PinRef.Mid(SepIndex + 1);
		return !OutNodeId.IsEmpty() && !OutPinName.IsEmpty();
	}

	// Fallback: accept colon as separator (AI frequently uses this format)
	if (PinRef.FindChar(TEXT(':'), SepIndex))
	{
		OutNodeId = PinRef.Left(SepIndex);
		OutPinName = PinRef.Mid(SepIndex + 1);
		if (!OutNodeId.IsEmpty() && !OutPinName.IsEmpty())
		{
			UE_LOG(LogOliveGraphWriter, Warning,
				TEXT("Auto-corrected pin reference '%s' (colon->dot). Use 'node_id.pin_name' format."),
				*PinRef);
			return true;
		}
	}

	return false;
}

FString FOliveGraphWriter::GenerateNodeId(const FString& BlueprintPath)
{
	FScopeLock Lock(&CacheLock);

	int32& Index = NextNodeIndex.FindOrAdd(BlueprintPath);
	FString NodeId = FString::Printf(TEXT("node_%d"), Index);
	Index++;

	return NodeId;
}

void FOliveGraphWriter::CacheNode(const FString& BlueprintPath, const FString& NodeId, UEdGraphNode* Node)
{
	FScopeLock Lock(&CacheLock);

	TMap<FString, TWeakObjectPtr<UEdGraphNode>>& BlueprintCache = NodeIdCache.FindOrAdd(BlueprintPath);
	BlueprintCache.Add(NodeId, Node);

	UE_LOG(LogOliveGraphWriter, Verbose, TEXT("Cached node '%s' in Blueprint '%s'"), *NodeId, *BlueprintPath);
}

void FOliveGraphWriter::RemoveFromCache(const FString& BlueprintPath, const FString& NodeId)
{
	FScopeLock Lock(&CacheLock);

	if (TMap<FString, TWeakObjectPtr<UEdGraphNode>>* BlueprintCache = NodeIdCache.Find(BlueprintPath))
	{
		BlueprintCache->Remove(NodeId);
		UE_LOG(LogOliveGraphWriter, Verbose, TEXT("Removed node '%s' from cache for Blueprint '%s'"), *NodeId, *BlueprintPath);
	}
}

FString FOliveGraphWriter::ResolveNodeId(const FString& BlueprintPath, UEdGraphNode* Node) const
{
	if (!Node)
	{
		return TEXT("unknown");
	}

	FScopeLock Lock(&CacheLock);

	if (const TMap<FString, TWeakObjectPtr<UEdGraphNode>>* BlueprintCache = NodeIdCache.Find(BlueprintPath))
	{
		for (const auto& Pair : *BlueprintCache)
		{
			if (Pair.Value.IsValid() && Pair.Value.Get() == Node)
			{
				return Pair.Key;
			}
		}
	}

	// Fallback: use the node's UObject name
	return Node->GetName();
}

TArray<TSharedPtr<FJsonValue>> FOliveGraphWriter::CaptureNodeConnections(
	const FString& BlueprintPath,
	UEdGraph* Graph,
	UEdGraphNode* Node)
{
	TArray<TSharedPtr<FJsonValue>> BrokenLinks;

	if (!Node || !Graph)
	{
		return BrokenLinks;
	}

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin)
		{
			continue;
		}

		for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			if (!LinkedPin || !LinkedPin->GetOwningNode())
			{
				continue;
			}

			TSharedPtr<FJsonObject> ConnectedTo = MakeShareable(new FJsonObject());
			ConnectedTo->SetStringField(TEXT("node_id"), ResolveNodeId(BlueprintPath, LinkedPin->GetOwningNode()));
			ConnectedTo->SetStringField(TEXT("pin"), LinkedPin->GetName());

			TSharedPtr<FJsonObject> LinkObj = MakeShareable(new FJsonObject());
			LinkObj->SetStringField(TEXT("pin"), Pin->GetName());
			LinkObj->SetStringField(TEXT("direction"), (Pin->Direction == EGPD_Input) ? TEXT("input") : TEXT("output"));
			LinkObj->SetObjectField(TEXT("was_connected_to"), ConnectedTo);

			BrokenLinks.Add(MakeShareable(new FJsonValueObject(LinkObj)));
		}
	}

	UE_LOG(LogOliveGraphWriter, Verbose, TEXT("Captured %d connections on node in Blueprint '%s'"),
		BrokenLinks.Num(), *BlueprintPath);

	return BrokenLinks;
}

UBlueprint* FOliveGraphWriter::LoadBlueprintForEditing(const FString& AssetPath, FString& OutError)
{
	// Normalize the path
	FString NormalizedPath = AssetPath;
	if (!NormalizedPath.StartsWith(TEXT("/")))
	{
		NormalizedPath = TEXT("/") + NormalizedPath;
	}

	// Try to load the Blueprint
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *NormalizedPath);
	if (!Blueprint)
	{
		// Try with _C suffix removed if it's a class path
		FString CleanPath = NormalizedPath;
		if (CleanPath.EndsWith(TEXT("_C")))
		{
			CleanPath.LeftChopInline(2);
			Blueprint = LoadObject<UBlueprint>(nullptr, *CleanPath);
		}
	}

	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath);
		return nullptr;
	}

	return Blueprint;
}

bool FOliveGraphWriter::IsPIEActive() const
{
	return GEditor && GEditor->PlayWorld != nullptr;
}

TMap<FString, FString> FOliveGraphWriter::ConvertIRNodeProperties(const FOliveIRNode& IRNode)
{
	TMap<FString, FString> Properties;

	// Copy all properties from the IR node
	Properties = IRNode.Properties;

	// Add standard properties based on node type
	if (!IRNode.FunctionName.IsEmpty())
	{
		Properties.FindOrAdd(TEXT("function_name")) = IRNode.FunctionName;
	}

	if (!IRNode.OwningClass.IsEmpty())
	{
		Properties.FindOrAdd(TEXT("target_class")) = IRNode.OwningClass;
	}

	if (!IRNode.VariableName.IsEmpty())
	{
		Properties.FindOrAdd(TEXT("variable_name")) = IRNode.VariableName;
	}

	// Handle event name for custom events
	if (IRNode.Type == TEXT("CustomEvent") && !IRNode.Title.IsEmpty())
	{
		Properties.FindOrAdd(TEXT("event_name")) = IRNode.Title;
	}

	return Properties;
}
