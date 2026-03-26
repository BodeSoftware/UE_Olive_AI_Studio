// Copyright Bode Software. All Rights Reserved.

#include "OliveAnimGraphSerializer.h"
#include "OliveNodeSerializer.h"
#include "Animation/AnimBlueprint.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimStateNode.h"
#include "AnimStateConduitNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimStateEntryNode.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationGraph.h"
#include "AnimationStateGraph.h"
#include "AnimationTransitionGraph.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"

DEFINE_LOG_CATEGORY(LogOliveAnimSerializer);

FOliveAnimGraphSerializer::FOliveAnimGraphSerializer()
{
	// Create node serializer for generic node conversion
	NodeSerializer = MakeShared<FOliveNodeSerializer>();
}

// ============================================================================
// High-Level Read Methods
// ============================================================================

TArray<FOliveIRAnimStateMachine> FOliveAnimGraphSerializer::ReadStateMachines(const UAnimBlueprint* AnimBlueprint)
{
	TArray<FOliveIRAnimStateMachine> StateMachines;

	if (!AnimBlueprint)
	{
		UE_LOG(LogOliveAnimSerializer, Warning, TEXT("ReadStateMachines: AnimBlueprint is null"));
		return StateMachines;
	}

	// Find all state machine nodes in the AnimGraph
	TArray<UAnimGraphNode_StateMachine*> StateMachineNodes = FindStateMachineNodes(AnimBlueprint);

	// Serialize each state machine
	for (UAnimGraphNode_StateMachine* StateMachineNode : StateMachineNodes)
	{
		if (!StateMachineNode)
		{
			continue;
		}

		// Get the state machine graph
		UAnimationStateMachineGraph* StateMachineGraph = Cast<UAnimationStateMachineGraph>(StateMachineNode->EditorStateMachineGraph);
		if (StateMachineGraph)
		{
			FOliveIRAnimStateMachine StateMachineIR = SerializeStateMachine(StateMachineGraph, AnimBlueprint);
			StateMachines.Add(StateMachineIR);
		}
	}

	UE_LOG(LogOliveAnimSerializer, Log, TEXT("ReadStateMachines: Found %d state machines in %s"),
		StateMachines.Num(), *AnimBlueprint->GetName());

	return StateMachines;
}

TOptional<FOliveIRAnimStateMachine> FOliveAnimGraphSerializer::ReadStateMachine(
	const UAnimBlueprint* AnimBlueprint,
	const FString& StateMachineName)
{
	if (!AnimBlueprint)
	{
		UE_LOG(LogOliveAnimSerializer, Warning, TEXT("ReadStateMachine: AnimBlueprint is null"));
		return TOptional<FOliveIRAnimStateMachine>();
	}

	// Find all state machine nodes
	TArray<UAnimGraphNode_StateMachine*> StateMachineNodes = FindStateMachineNodes(AnimBlueprint);

	// Find the specific state machine by name
	for (UAnimGraphNode_StateMachine* StateMachineNode : StateMachineNodes)
	{
		if (!StateMachineNode || !StateMachineNode->EditorStateMachineGraph)
		{
			continue;
		}

		// Check if this is the requested state machine
		FString NodeName = StateMachineNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
		if (NodeName.Equals(StateMachineName, ESearchCase::IgnoreCase) ||
			StateMachineNode->EditorStateMachineGraph->GetName().Equals(StateMachineName, ESearchCase::IgnoreCase))
		{
			UAnimationStateMachineGraph* StateMachineGraph = Cast<UAnimationStateMachineGraph>(StateMachineNode->EditorStateMachineGraph);
			if (StateMachineGraph)
			{
				FOliveIRAnimStateMachine StateMachineIR = SerializeStateMachine(StateMachineGraph, AnimBlueprint);
				return StateMachineIR;
			}
		}
	}

	UE_LOG(LogOliveAnimSerializer, Warning, TEXT("ReadStateMachine: State machine '%s' not found in %s"),
		*StateMachineName, *AnimBlueprint->GetName());

	return TOptional<FOliveIRAnimStateMachine>();
}

TSharedPtr<FJsonObject> FOliveAnimGraphSerializer::ReadAnimGraphSummary(const UAnimBlueprint* AnimBlueprint)
{
	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();

	if (!AnimBlueprint)
	{
		UE_LOG(LogOliveAnimSerializer, Warning, TEXT("ReadAnimGraphSummary: AnimBlueprint is null"));
		return Summary;
	}

	// Get all state machine names
	TArray<UAnimGraphNode_StateMachine*> StateMachineNodes = FindStateMachineNodes(AnimBlueprint);
	TArray<TSharedPtr<FJsonValue>> StateMachineNames;

	for (UAnimGraphNode_StateMachine* StateMachineNode : StateMachineNodes)
	{
		if (StateMachineNode && StateMachineNode->EditorStateMachineGraph)
		{
			FString MachineName = StateMachineNode->EditorStateMachineGraph->GetName();
			StateMachineNames.Add(MakeShared<FJsonValueString>(MachineName));
		}
	}

	Summary->SetArrayField(TEXT("state_machines"), StateMachineNames);
	Summary->SetNumberField(TEXT("state_machine_count"), StateMachineNames.Num());

	return Summary;
}

FOliveIRGraph FOliveAnimGraphSerializer::ReadAnimGraphFull(const UAnimBlueprint* AnimBlueprint)
{
	FOliveIRGraph AnimGraphIR;

	if (!AnimBlueprint)
	{
		UE_LOG(LogOliveAnimSerializer, Warning, TEXT("ReadAnimGraphFull: AnimBlueprint is null"));
		return AnimGraphIR;
	}

	UEdGraph* AnimGraph = FindAnimGraph(AnimBlueprint);
	if (!AnimGraph)
	{
		UE_LOG(LogOliveAnimSerializer, Warning, TEXT("ReadAnimGraphFull: AnimGraph not found"));
		return AnimGraphIR;
	}

	// Set basic graph info
	AnimGraphIR.Name = AnimGraph->GetName();
	AnimGraphIR.GraphType = TEXT("AnimGraph");
	AnimGraphIR.Description = TEXT("Animation Graph");

	// Serialize all nodes in the AnimGraph
	for (UEdGraphNode* Node : AnimGraph->Nodes)
	{
		if (!Node)
		{
			continue;
		}

		// Check if this is an anim graph node
		if (UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node))
		{
			FOliveIRNode NodeIR = SerializeAnimNode(AnimNode);
			AnimGraphIR.Nodes.Add(NodeIR);
		}
	}

	AnimGraphIR.UpdateStatistics();

	UE_LOG(LogOliveAnimSerializer, Log, TEXT("ReadAnimGraphFull: Read %d nodes from AnimGraph"),
		AnimGraphIR.Nodes.Num());

	return AnimGraphIR;
}

// ============================================================================
// State Machine Reading
// ============================================================================

FOliveIRAnimStateMachine FOliveAnimGraphSerializer::SerializeStateMachine(
	const UAnimationStateMachineGraph* StateMachineGraph,
	const UAnimBlueprint* OwningBlueprint)
{
	FOliveIRAnimStateMachine StateMachineIR;

	if (!StateMachineGraph)
	{
		UE_LOG(LogOliveAnimSerializer, Warning, TEXT("SerializeStateMachine: StateMachineGraph is null"));
		return StateMachineIR;
	}

	// Set state machine name
	StateMachineIR.Name = StateMachineGraph->GetName();

	// Get entry state
	StateMachineIR.EntryState = GetEntryStateName(StateMachineGraph);

	// Iterate through all nodes in the state machine graph
	for (UEdGraphNode* Node : StateMachineGraph->Nodes)
	{
		if (!Node)
		{
			continue;
		}

		// Check if this is a state node
		if (UAnimStateNodeBase* StateNode = Cast<UAnimStateNodeBase>(Node))
		{
			// Skip entry nodes
			if (StateNode->IsA<UAnimStateEntryNode>())
			{
				continue;
			}

			// Serialize the state
			FOliveIRAnimState StateIR = SerializeState(StateNode, StateMachineGraph);
			StateMachineIR.States.Add(StateIR);
		}
	}

	UE_LOG(LogOliveAnimSerializer, Log, TEXT("SerializeStateMachine: '%s' has %d states, entry='%s'"),
		*StateMachineIR.Name, StateMachineIR.States.Num(), *StateMachineIR.EntryState);

	return StateMachineIR;
}

TArray<UAnimGraphNode_StateMachine*> FOliveAnimGraphSerializer::FindStateMachineNodes(const UAnimBlueprint* AnimBlueprint)
{
	TArray<UAnimGraphNode_StateMachine*> StateMachineNodes;

	if (!AnimBlueprint)
	{
		return StateMachineNodes;
	}

	// Find the AnimGraph
	UEdGraph* AnimGraph = FindAnimGraph(AnimBlueprint);
	if (!AnimGraph)
	{
		return StateMachineNodes;
	}

	// Find all state machine nodes in the AnimGraph
	for (UEdGraphNode* Node : AnimGraph->Nodes)
	{
		if (UAnimGraphNode_StateMachine* StateMachineNode = Cast<UAnimGraphNode_StateMachine>(Node))
		{
			StateMachineNodes.Add(StateMachineNode);
		}
	}

	return StateMachineNodes;
}

// ============================================================================
// State Reading
// ============================================================================

FOliveIRAnimState FOliveAnimGraphSerializer::SerializeState(
	const UAnimStateNodeBase* StateNode,
	const UAnimationStateMachineGraph* OwningGraph)
{
	FOliveIRAnimState StateIR;

	if (!StateNode)
	{
		UE_LOG(LogOliveAnimSerializer, Warning, TEXT("SerializeState: StateNode is null"));
		return StateIR;
	}

	// Get state name
	StateIR.Name = StateNode->GetStateName();

	// Check if this is a conduit
	StateIR.bIsConduit = IsConduitState(StateNode);

	// Get animation asset reference
	StateIR.AnimationAsset = GetStateAnimationAsset(StateNode);

	// Get transitions in and out
	if (OwningGraph)
	{
		StateIR.TransitionsIn = GetTransitionsIn(StateNode, OwningGraph);
		StateIR.TransitionsOut = GetTransitionsOut(StateNode, OwningGraph);
	}

	UE_LOG(LogOliveAnimSerializer, Verbose, TEXT("SerializeState: '%s' - Conduit=%d, Asset='%s', In=%d, Out=%d"),
		*StateIR.Name, StateIR.bIsConduit, *StateIR.AnimationAsset,
		StateIR.TransitionsIn.Num(), StateIR.TransitionsOut.Num());

	return StateIR;
}

FString FOliveAnimGraphSerializer::GetStateAnimationAsset(const UAnimStateNodeBase* StateNode)
{
	if (!StateNode)
	{
		return TEXT("");
	}

	// Get the bound graph for this state
	UEdGraph* BoundGraph = GetStateBoundGraph(StateNode);
	if (!BoundGraph)
	{
		return TEXT("");
	}

	// Look for animation asset references in the state's graph
	// This is a simplified implementation - could be enhanced to traverse the graph more thoroughly
	for (UEdGraphNode* Node : BoundGraph->Nodes)
	{
		if (!Node)
		{
			continue;
		}

		// Check node title for asset references (common pattern)
		FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();

		// Look for asset references in the title (e.g., "Play AnimSequence_Walk")
		// This is a heuristic - actual implementation would need to inspect node properties
		if (NodeTitle.Contains(TEXT("AnimSequence")) ||
			NodeTitle.Contains(TEXT("BlendSpace")) ||
			NodeTitle.Contains(TEXT("AnimMontage")))
		{
			return NodeTitle;
		}
	}

	return TEXT("");
}

bool FOliveAnimGraphSerializer::IsConduitState(const UAnimStateNodeBase* StateNode) const
{
	if (!StateNode)
	{
		return false;
	}

	// Check if this is a conduit node
	return Cast<UAnimStateConduitNode>(StateNode) != nullptr;
}

// ============================================================================
// Transition Reading
// ============================================================================

TArray<FString> FOliveAnimGraphSerializer::GetTransitionsIn(
	const UAnimStateNodeBase* StateNode,
	const UAnimationStateMachineGraph* Graph)
{
	TArray<FString> TransitionsIn;

	if (!StateNode || !Graph)
	{
		return TransitionsIn;
	}

	// Find all transition nodes in the graph
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (UAnimStateTransitionNode* TransitionNode = Cast<UAnimStateTransitionNode>(Node))
		{
			// Check if this transition targets our state
			if (TransitionNode->GetNextState() == StateNode)
			{
				// Get the source state name
				UAnimStateNodeBase* PrevState = TransitionNode->GetPreviousState();
				if (PrevState)
				{
					FString TransitionDesc = FString::Printf(TEXT("%s -> %s"),
						*PrevState->GetStateName(), *StateNode->GetStateName());
					TransitionsIn.Add(TransitionDesc);
				}
			}
		}
	}

	return TransitionsIn;
}

TArray<FString> FOliveAnimGraphSerializer::GetTransitionsOut(
	const UAnimStateNodeBase* StateNode,
	const UAnimationStateMachineGraph* Graph)
{
	TArray<FString> TransitionsOut;

	if (!StateNode || !Graph)
	{
		return TransitionsOut;
	}

	// Find all transition nodes in the graph
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (UAnimStateTransitionNode* TransitionNode = Cast<UAnimStateTransitionNode>(Node))
		{
			// Check if this transition originates from our state
			if (TransitionNode->GetPreviousState() == StateNode)
			{
				// Get the target state name
				UAnimStateNodeBase* NextState = TransitionNode->GetNextState();
				if (NextState)
				{
					FString TransitionDesc = FString::Printf(TEXT("%s -> %s"),
						*StateNode->GetStateName(), *NextState->GetStateName());
					TransitionsOut.Add(TransitionDesc);
				}
			}
		}
	}

	return TransitionsOut;
}

FString FOliveAnimGraphSerializer::SerializeTransitionRule(const UAnimStateTransitionNode* TransitionNode)
{
	if (!TransitionNode)
	{
		return TEXT("");
	}

	// Get the transition graph (contains the rule logic)
	UEdGraph* TransitionGraph = TransitionNode->GetBoundGraph();
	if (!TransitionGraph)
	{
		return TEXT("No rule defined");
	}

	// For now, return a simple description
	// A full implementation would traverse the transition graph and build a rule expression
	int32 NodeCount = TransitionGraph->Nodes.Num();
	return FString::Printf(TEXT("Rule with %d nodes"), NodeCount);
}

// ============================================================================
// AnimGraph Node Reading
// ============================================================================

FOliveIRNode FOliveAnimGraphSerializer::SerializeAnimNode(const UAnimGraphNode_Base* AnimNode)
{
	FOliveIRNode NodeIR;

	if (!AnimNode)
	{
		UE_LOG(LogOliveAnimSerializer, Warning, TEXT("SerializeAnimNode: AnimNode is null"));
		return NodeIR;
	}

	// Use the node serializer for basic node data
	if (NodeSerializer.IsValid())
	{
		const TMap<const UEdGraphNode*, FString> EmptyNodeIdMap;
		NodeIR = NodeSerializer->SerializeNode(AnimNode, EmptyNodeIdMap);
	}
	else
	{
		// Fallback: minimal serialization
		NodeIR.Id = FString::Printf(TEXT("node_%d"), AnimNode->NodeGuid.A);
		NodeIR.Type = AnimNode->GetClass()->GetName();
		NodeIR.Title = AnimNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
	}

	// Extract animation-specific properties
	TMap<FString, FString> AnimProperties = ExtractAnimNodeProperties(AnimNode);
	for (const auto& Pair : AnimProperties)
	{
		NodeIR.Properties.Add(Pair.Key, Pair.Value);
	}

	// Mark as animation node
	NodeIR.Category = TEXT("Animation");

	return NodeIR;
}

TMap<FString, FString> FOliveAnimGraphSerializer::ExtractAnimNodeProperties(const UAnimGraphNode_Base* AnimNode)
{
	TMap<FString, FString> Properties;

	if (!AnimNode)
	{
		return Properties;
	}

	// Add common animation node properties
	Properties.Add(TEXT("node_class"), AnimNode->GetClass()->GetName());

	// Check if this is a state machine node
	if (const UAnimGraphNode_StateMachine* StateMachineNode = Cast<UAnimGraphNode_StateMachine>(AnimNode))
	{
		if (StateMachineNode->EditorStateMachineGraph)
		{
			Properties.Add(TEXT("state_machine_name"), StateMachineNode->EditorStateMachineGraph->GetName());
		}
	}

	// Additional properties could be extracted here by reflection
	// For now, we keep it simple

	return Properties;
}

// ============================================================================
// Helper Methods
// ============================================================================

UEdGraph* FOliveAnimGraphSerializer::FindAnimGraph(const UAnimBlueprint* AnimBlueprint) const
{
	if (!AnimBlueprint)
	{
		return nullptr;
	}

	// Look through all function graphs
	for (UEdGraph* Graph : AnimBlueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetClass()->GetName().Contains(TEXT("AnimationGraph")))
		{
			return Graph;
		}
	}

	// Alternative: Look for graph by name
	for (UEdGraph* Graph : AnimBlueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetName().Equals(TEXT("AnimGraph"), ESearchCase::IgnoreCase))
		{
			return Graph;
		}
	}

	// Try the UbergraphPages (some AnimBPs store AnimGraph here)
	for (UEdGraph* Graph : AnimBlueprint->UbergraphPages)
	{
		if (Graph && Graph->GetClass()->GetName().Contains(TEXT("AnimationGraph")))
		{
			return Graph;
		}
	}

	return nullptr;
}

FString FOliveAnimGraphSerializer::GetEntryStateName(const UAnimationStateMachineGraph* StateMachineGraph) const
{
	if (!StateMachineGraph)
	{
		return TEXT("");
	}

	// Find the entry node
	for (UEdGraphNode* Node : StateMachineGraph->Nodes)
	{
		if (UAnimStateEntryNode* EntryNode = Cast<UAnimStateEntryNode>(Node))
		{
			// The entry node should have a connection to the first state
			for (UEdGraphPin* Pin : EntryNode->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Output && Pin->LinkedTo.Num() > 0)
				{
					UEdGraphPin* LinkedPin = Pin->LinkedTo[0];
					if (LinkedPin && LinkedPin->GetOwningNode())
					{
						if (UAnimStateNodeBase* StateNode = Cast<UAnimStateNodeBase>(LinkedPin->GetOwningNode()))
						{
							return StateNode->GetStateName();
						}
					}
				}
			}
		}
	}

	return TEXT("");
}

UEdGraph* FOliveAnimGraphSerializer::GetStateBoundGraph(const UAnimStateNodeBase* StateNode) const
{
	if (!StateNode)
	{
		return nullptr;
	}

	return StateNode->GetBoundGraph();
}
