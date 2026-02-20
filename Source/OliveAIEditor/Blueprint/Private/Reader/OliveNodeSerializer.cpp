// Copyright Bode Software. All Rights Reserved.

#include "OliveNodeSerializer.h"
#include "OlivePinSerializer.h"

// UE Graph includes
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"

// K2 Node includes
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Timeline.h"
#include "K2Node_Knot.h"

// ============================================================================
// OliveSupportedNodes Namespace
// ============================================================================

namespace OliveSupportedNodes
{
	static const TArray<FName> SupportedClasses = {
		FName(TEXT("UK2Node_CallFunction")),
		FName(TEXT("UK2Node_VariableGet")),
		FName(TEXT("UK2Node_VariableSet")),
		FName(TEXT("UK2Node_Event")),
		FName(TEXT("UK2Node_CustomEvent")),
		FName(TEXT("UK2Node_FunctionEntry")),
		FName(TEXT("UK2Node_FunctionResult")),
		FName(TEXT("UK2Node_IfThenElse")),
		FName(TEXT("UK2Node_ExecutionSequence")),
		FName(TEXT("UK2Node_DynamicCast")),
		FName(TEXT("UK2Node_MacroInstance")),
		FName(TEXT("UK2Node_Timeline")),
		FName(TEXT("UK2Node_Knot")),
		FName(TEXT("UEdGraphNode_Comment"))
	};

	bool IsSupported(FName ClassName)
	{
		return SupportedClasses.Contains(ClassName);
	}

	TArray<FName> GetAllSupported()
	{
		return SupportedClasses;
	}
}

// ============================================================================
// Constructor
// ============================================================================

FOliveNodeSerializer::FOliveNodeSerializer()
{
	PinSerializer = MakeShared<FOlivePinSerializer>();
}

// ============================================================================
// Main Serialization Methods
// ============================================================================

FOliveIRNode FOliveNodeSerializer::SerializeNode(
	const UEdGraphNode* Node,
	const TMap<const UEdGraphNode*, FString>& NodeIdMap) const
{
	if (!Node)
	{
		FOliveIRNode EmptyNode;
		EmptyNode.Type = TEXT("Unknown");
		EmptyNode.NodeCategory = EOliveIRNodeCategory::Unknown;
		return EmptyNode;
	}

	// Try specialized serializers first (check from most specific to least specific)

	// Comment nodes (not a K2Node)
	if (const UEdGraphNode_Comment* CommentNode = Cast<UEdGraphNode_Comment>(Node))
	{
		return SerializeComment(CommentNode);
	}

	// Function call
	if (const UK2Node_CallFunction* CallFunctionNode = Cast<UK2Node_CallFunction>(Node))
	{
		return SerializeCallFunction(CallFunctionNode, NodeIdMap);
	}

	// Custom event (must check before UK2Node_Event since it inherits from it)
	if (const UK2Node_CustomEvent* CustomEventNode = Cast<UK2Node_CustomEvent>(Node))
	{
		return SerializeCustomEvent(CustomEventNode, NodeIdMap);
	}

	// Native event
	if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
	{
		return SerializeEvent(EventNode, NodeIdMap);
	}

	// Variable get
	if (const UK2Node_VariableGet* VarGetNode = Cast<UK2Node_VariableGet>(Node))
	{
		return SerializeVariableGet(VarGetNode, NodeIdMap);
	}

	// Variable set
	if (const UK2Node_VariableSet* VarSetNode = Cast<UK2Node_VariableSet>(Node))
	{
		return SerializeVariableSet(VarSetNode, NodeIdMap);
	}

	// Function entry
	if (const UK2Node_FunctionEntry* FunctionEntryNode = Cast<UK2Node_FunctionEntry>(Node))
	{
		return SerializeFunctionEntry(FunctionEntryNode, NodeIdMap);
	}

	// Function result
	if (const UK2Node_FunctionResult* FunctionResultNode = Cast<UK2Node_FunctionResult>(Node))
	{
		return SerializeFunctionResult(FunctionResultNode, NodeIdMap);
	}

	// Branch
	if (const UK2Node_IfThenElse* BranchNode = Cast<UK2Node_IfThenElse>(Node))
	{
		return SerializeBranch(BranchNode, NodeIdMap);
	}

	// Sequence
	if (const UK2Node_ExecutionSequence* SequenceNode = Cast<UK2Node_ExecutionSequence>(Node))
	{
		return SerializeSequence(SequenceNode, NodeIdMap);
	}

	// Cast
	if (const UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
	{
		return SerializeCast(CastNode, NodeIdMap);
	}

	// Macro instance
	if (const UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node))
	{
		return SerializeMacroInstance(MacroNode, NodeIdMap);
	}

	// Timeline
	if (const UK2Node_Timeline* TimelineNode = Cast<UK2Node_Timeline>(Node))
	{
		return SerializeTimeline(TimelineNode, NodeIdMap);
	}

	// Knot (reroute)
	if (const UK2Node_Knot* KnotNode = Cast<UK2Node_Knot>(Node))
	{
		return SerializeKnot(KnotNode, NodeIdMap);
	}

	// Fallback to generic serialization
	return SerializeGenericNode(Node, NodeIdMap);
}

bool FOliveNodeSerializer::HasSpecializedSerializer(const UEdGraphNode* Node) const
{
	if (!Node)
	{
		return false;
	}

	// Check against known types
	return Node->IsA<UK2Node_CallFunction>()
		|| Node->IsA<UK2Node_VariableGet>()
		|| Node->IsA<UK2Node_VariableSet>()
		|| Node->IsA<UK2Node_Event>()
		|| Node->IsA<UK2Node_CustomEvent>()
		|| Node->IsA<UK2Node_FunctionEntry>()
		|| Node->IsA<UK2Node_FunctionResult>()
		|| Node->IsA<UK2Node_IfThenElse>()
		|| Node->IsA<UK2Node_ExecutionSequence>()
		|| Node->IsA<UK2Node_DynamicCast>()
		|| Node->IsA<UK2Node_MacroInstance>()
		|| Node->IsA<UK2Node_Timeline>()
		|| Node->IsA<UK2Node_Knot>()
		|| Node->IsA<UEdGraphNode_Comment>();
}

TArray<FName> FOliveNodeSerializer::GetSupportedNodeClasses()
{
	return OliveSupportedNodes::GetAllSupported();
}

// ============================================================================
// Specialized Serializers
// ============================================================================

FOliveIRNode FOliveNodeSerializer::SerializeCallFunction(
	const UK2Node_CallFunction* Node,
	const TMap<const UEdGraphNode*, FString>& NodeIdMap) const
{
	FOliveIRNode IRNode;
	PopulateCommonProperties(Node, IRNode, NodeIdMap);

	IRNode.Type = TEXT("CallFunction");
	IRNode.NodeCategory = EOliveIRNodeCategory::CallFunction;

	// Get function information
	if (UFunction* Function = Node->GetTargetFunction())
	{
		IRNode.FunctionName = Function->GetName();

		// Get owning class
		if (UClass* OwningClass = Function->GetOwnerClass())
		{
			IRNode.OwningClass = OwningClass->GetName();
		}

		// Additional properties
		IRNode.Properties.Add(TEXT("IsPure"), Node->IsNodePure() ? TEXT("true") : TEXT("false"));
		IRNode.Properties.Add(TEXT("IsLatent"), Function->HasMetaData(TEXT("Latent")) ? TEXT("true") : TEXT("false"));

		// Check if it's a static function
		if (Function->HasAnyFunctionFlags(FUNC_Static))
		{
			IRNode.Properties.Add(TEXT("IsStatic"), TEXT("true"));
		}
	}
	else
	{
		// Function reference might be stored differently
		IRNode.FunctionName = Node->GetFunctionName().ToString();
	}

	// Serialize pins
	SerializePins(Node, IRNode, NodeIdMap);

	return IRNode;
}

FOliveIRNode FOliveNodeSerializer::SerializeVariableGet(
	const UK2Node_VariableGet* Node,
	const TMap<const UEdGraphNode*, FString>& NodeIdMap) const
{
	FOliveIRNode IRNode;
	PopulateCommonProperties(Node, IRNode, NodeIdMap);

	IRNode.Type = TEXT("VariableGet");
	IRNode.NodeCategory = EOliveIRNodeCategory::VariableGet;

	// Get variable name
	IRNode.VariableName = Node->GetVarName().ToString();

	// Check if this is a self-context variable or external
	if (Node->VariableReference.IsSelfContext())
	{
		IRNode.Properties.Add(TEXT("Scope"), TEXT("self"));
	}
	else if (UClass* MemberParentClass = Node->VariableReference.GetMemberParentClass())
	{
		IRNode.Properties.Add(TEXT("Scope"), MemberParentClass->GetName());
	}

	// Check if it's a local variable
	if (Node->VariableReference.IsLocalScope())
	{
		IRNode.Properties.Add(TEXT("IsLocal"), TEXT("true"));
	}

	// Serialize pins
	SerializePins(Node, IRNode, NodeIdMap);

	return IRNode;
}

FOliveIRNode FOliveNodeSerializer::SerializeVariableSet(
	const UK2Node_VariableSet* Node,
	const TMap<const UEdGraphNode*, FString>& NodeIdMap) const
{
	FOliveIRNode IRNode;
	PopulateCommonProperties(Node, IRNode, NodeIdMap);

	IRNode.Type = TEXT("VariableSet");
	IRNode.NodeCategory = EOliveIRNodeCategory::VariableSet;

	// Get variable name
	IRNode.VariableName = Node->GetVarName().ToString();

	// Check scope
	if (Node->VariableReference.IsSelfContext())
	{
		IRNode.Properties.Add(TEXT("Scope"), TEXT("self"));
	}
	else if (UClass* MemberParentClass = Node->VariableReference.GetMemberParentClass())
	{
		IRNode.Properties.Add(TEXT("Scope"), MemberParentClass->GetName());
	}

	// Check if it's a local variable
	if (Node->VariableReference.IsLocalScope())
	{
		IRNode.Properties.Add(TEXT("IsLocal"), TEXT("true"));
	}

	// Serialize pins
	SerializePins(Node, IRNode, NodeIdMap);

	return IRNode;
}

FOliveIRNode FOliveNodeSerializer::SerializeEvent(
	const UK2Node_Event* Node,
	const TMap<const UEdGraphNode*, FString>& NodeIdMap) const
{
	FOliveIRNode IRNode;
	PopulateCommonProperties(Node, IRNode, NodeIdMap);

	IRNode.Type = TEXT("Event");
	IRNode.NodeCategory = EOliveIRNodeCategory::Event;

	// Get event name
	IRNode.FunctionName = Node->GetFunctionName().ToString();

	// Check if it's an override
	if (UFunction* Function = Node->FindEventSignatureFunction())
	{
		if (UClass* OwningClass = Function->GetOwnerClass())
		{
			IRNode.OwningClass = OwningClass->GetName();
			IRNode.Properties.Add(TEXT("IsOverride"), TEXT("true"));
		}
	}

	// Serialize pins
	SerializePins(Node, IRNode, NodeIdMap);

	return IRNode;
}

FOliveIRNode FOliveNodeSerializer::SerializeCustomEvent(
	const UK2Node_CustomEvent* Node,
	const TMap<const UEdGraphNode*, FString>& NodeIdMap) const
{
	FOliveIRNode IRNode;
	PopulateCommonProperties(Node, IRNode, NodeIdMap);

	IRNode.Type = TEXT("CustomEvent");
	IRNode.NodeCategory = EOliveIRNodeCategory::CustomEvent;

	// Get custom event name
	IRNode.FunctionName = Node->CustomFunctionName.ToString();

	// Custom events can be replicated
	if (Node->IsOverride())
	{
		IRNode.Properties.Add(TEXT("IsOverride"), TEXT("true"));
	}

	// Serialize pins
	SerializePins(Node, IRNode, NodeIdMap);

	return IRNode;
}

FOliveIRNode FOliveNodeSerializer::SerializeFunctionEntry(
	const UK2Node_FunctionEntry* Node,
	const TMap<const UEdGraphNode*, FString>& NodeIdMap) const
{
	FOliveIRNode IRNode;
	PopulateCommonProperties(Node, IRNode, NodeIdMap);

	IRNode.Type = TEXT("FunctionEntry");
	IRNode.NodeCategory = EOliveIRNodeCategory::FunctionEntry;

	// Get function name from the signature
	if (const UEdGraph* Graph = Node->GetGraph())
	{
		IRNode.FunctionName = Graph->GetName();
	}

	// Extract function flags from metadata
	const FKismetUserDeclaredFunctionMetadata& Metadata = Node->MetaData;
	if (!Metadata.Category.IsEmpty())
	{
		IRNode.Properties.Add(TEXT("Category"), Metadata.Category.ToString());
	}
	if (!Metadata.ToolTip.IsEmpty())
	{
		IRNode.Properties.Add(TEXT("Description"), Metadata.ToolTip.ToString());
	}

	// Serialize pins (these represent the function inputs)
	SerializePins(Node, IRNode, NodeIdMap);

	return IRNode;
}

FOliveIRNode FOliveNodeSerializer::SerializeFunctionResult(
	const UK2Node_FunctionResult* Node,
	const TMap<const UEdGraphNode*, FString>& NodeIdMap) const
{
	FOliveIRNode IRNode;
	PopulateCommonProperties(Node, IRNode, NodeIdMap);

	IRNode.Type = TEXT("FunctionResult");
	IRNode.NodeCategory = EOliveIRNodeCategory::FunctionResult;

	// Get function name from the graph
	if (const UEdGraph* Graph = Node->GetGraph())
	{
		IRNode.FunctionName = Graph->GetName();
	}

	// Serialize pins (these represent the function outputs)
	SerializePins(Node, IRNode, NodeIdMap);

	return IRNode;
}

FOliveIRNode FOliveNodeSerializer::SerializeBranch(
	const UK2Node_IfThenElse* Node,
	const TMap<const UEdGraphNode*, FString>& NodeIdMap) const
{
	FOliveIRNode IRNode;
	PopulateCommonProperties(Node, IRNode, NodeIdMap);

	IRNode.Type = TEXT("Branch");
	IRNode.NodeCategory = EOliveIRNodeCategory::Branch;

	// Serialize pins
	SerializePins(Node, IRNode, NodeIdMap);

	return IRNode;
}

FOliveIRNode FOliveNodeSerializer::SerializeSequence(
	const UK2Node_ExecutionSequence* Node,
	const TMap<const UEdGraphNode*, FString>& NodeIdMap) const
{
	FOliveIRNode IRNode;
	PopulateCommonProperties(Node, IRNode, NodeIdMap);

	IRNode.Type = TEXT("Sequence");
	IRNode.NodeCategory = EOliveIRNodeCategory::Sequence;

	// Store number of outputs
	int32 OutputCount = 0;
	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == TEXT("exec"))
		{
			OutputCount++;
		}
	}
	IRNode.Properties.Add(TEXT("OutputCount"), FString::FromInt(OutputCount));

	// Serialize pins
	SerializePins(Node, IRNode, NodeIdMap);

	return IRNode;
}

FOliveIRNode FOliveNodeSerializer::SerializeCast(
	const UK2Node_DynamicCast* Node,
	const TMap<const UEdGraphNode*, FString>& NodeIdMap) const
{
	FOliveIRNode IRNode;
	PopulateCommonProperties(Node, IRNode, NodeIdMap);

	IRNode.Type = TEXT("Cast");
	IRNode.NodeCategory = EOliveIRNodeCategory::Cast;

	// Get target class
	if (UClass* TargetClass = Node->TargetType)
	{
		IRNode.Properties.Add(TEXT("TargetClass"), TargetClass->GetName());
		IRNode.Properties.Add(TEXT("TargetClassPath"), TargetClass->GetPathName());
	}

	// Check if it's a pure cast (no exec pins)
	IRNode.Properties.Add(TEXT("IsPureCast"), Node->IsNodePure() ? TEXT("true") : TEXT("false"));

	// Serialize pins
	SerializePins(Node, IRNode, NodeIdMap);

	return IRNode;
}

FOliveIRNode FOliveNodeSerializer::SerializeMacroInstance(
	const UK2Node_MacroInstance* Node,
	const TMap<const UEdGraphNode*, FString>& NodeIdMap) const
{
	FOliveIRNode IRNode;
	PopulateCommonProperties(Node, IRNode, NodeIdMap);

	IRNode.Type = TEXT("MacroInstance");
	IRNode.NodeCategory = EOliveIRNodeCategory::MacroInstance;

	// Get macro graph reference
	if (UEdGraph* MacroGraph = Node->GetMacroGraph())
	{
		IRNode.FunctionName = MacroGraph->GetName();

		// Get the Blueprint that owns the macro
		if (UObject* Outer = MacroGraph->GetOuter())
		{
			IRNode.Properties.Add(TEXT("MacroOwner"), Outer->GetName());
			IRNode.Properties.Add(TEXT("MacroPath"), Outer->GetPathName());
		}
	}

	// Serialize pins
	SerializePins(Node, IRNode, NodeIdMap);

	return IRNode;
}

FOliveIRNode FOliveNodeSerializer::SerializeTimeline(
	const UK2Node_Timeline* Node,
	const TMap<const UEdGraphNode*, FString>& NodeIdMap) const
{
	FOliveIRNode IRNode;
	PopulateCommonProperties(Node, IRNode, NodeIdMap);

	IRNode.Type = TEXT("Timeline");
	IRNode.NodeCategory = EOliveIRNodeCategory::Timeline;

	// Get timeline name
	IRNode.FunctionName = Node->TimelineName.ToString();

	// Timeline properties
	IRNode.Properties.Add(TEXT("TimelineName"), Node->TimelineName.ToString());
	IRNode.Properties.Add(TEXT("bAutoPlay"), Node->bAutoPlay ? TEXT("true") : TEXT("false"));
	IRNode.Properties.Add(TEXT("bLoop"), Node->bLoop ? TEXT("true") : TEXT("false"));
	IRNode.Properties.Add(TEXT("bReplicated"), Node->bReplicated ? TEXT("true") : TEXT("false"));
	IRNode.Properties.Add(TEXT("bIgnoreTimeDilation"), Node->bIgnoreTimeDilation ? TEXT("true") : TEXT("false"));

	// Serialize pins
	SerializePins(Node, IRNode, NodeIdMap);

	return IRNode;
}

FOliveIRNode FOliveNodeSerializer::SerializeKnot(
	const UK2Node_Knot* Node,
	const TMap<const UEdGraphNode*, FString>& NodeIdMap) const
{
	FOliveIRNode IRNode;
	PopulateCommonProperties(Node, IRNode, NodeIdMap);

	IRNode.Type = TEXT("Reroute");
	IRNode.NodeCategory = EOliveIRNodeCategory::Reroute;

	// Serialize pins
	SerializePins(Node, IRNode, NodeIdMap);

	return IRNode;
}

FOliveIRNode FOliveNodeSerializer::SerializeComment(
	const UEdGraphNode_Comment* Node) const
{
	FOliveIRNode IRNode;

	// Comment nodes don't need node ID map since they have no connections
	IRNode.Id = FString::Printf(TEXT("comment_%p"), Node);
	IRNode.Type = TEXT("Comment");
	IRNode.NodeCategory = EOliveIRNodeCategory::Comment;
	IRNode.Title = TEXT("Comment");

	// Store comment text
	IRNode.Comment = Node->NodeComment;

	// Store comment dimensions
	IRNode.Properties.Add(TEXT("Width"), FString::FromInt(Node->NodeWidth));
	IRNode.Properties.Add(TEXT("Height"), FString::FromInt(Node->NodeHeight));

	// Comment color
	IRNode.Properties.Add(TEXT("CommentColor"),
		FString::Printf(TEXT("(%f,%f,%f,%f)"),
			Node->CommentColor.R,
			Node->CommentColor.G,
			Node->CommentColor.B,
			Node->CommentColor.A));

	// Whether the comment bubble is visible (the property is for the details panel toggle)
	IRNode.Properties.Add(TEXT("bCommentBubbleVisible"), Node->bCommentBubbleVisible ? TEXT("true") : TEXT("false"));
	IRNode.Properties.Add(TEXT("bCommentBubblePinned"), Node->bCommentBubblePinned ? TEXT("true") : TEXT("false"));

	// No pins on comment nodes, so we don't serialize pins

	return IRNode;
}

FOliveIRNode FOliveNodeSerializer::SerializeGenericNode(
	const UEdGraphNode* Node,
	const TMap<const UEdGraphNode*, FString>& NodeIdMap) const
{
	FOliveIRNode IRNode;
	PopulateCommonProperties(Node, IRNode, NodeIdMap);

	// Use class name as type
	IRNode.Type = Node->GetClass()->GetName();
	IRNode.NodeCategory = DetermineCategory(Node);

	// Serialize pins
	SerializePins(Node, IRNode, NodeIdMap);

	return IRNode;
}

// ============================================================================
// Helper Methods
// ============================================================================

EOliveIRNodeCategory FOliveNodeSerializer::DetermineCategory(const UEdGraphNode* Node) const
{
	if (!Node)
	{
		return EOliveIRNodeCategory::Unknown;
	}

	// Try to determine category from node class name
	FString ClassName = Node->GetClass()->GetName();

	// Flow control
	if (ClassName.Contains(TEXT("ForLoop")))
	{
		return EOliveIRNodeCategory::ForLoop;
	}
	if (ClassName.Contains(TEXT("ForEach")))
	{
		return EOliveIRNodeCategory::ForEachLoop;
	}
	if (ClassName.Contains(TEXT("While")))
	{
		return EOliveIRNodeCategory::WhileLoop;
	}
	if (ClassName.Contains(TEXT("Switch")))
	{
		return EOliveIRNodeCategory::Switch;
	}
	if (ClassName.Contains(TEXT("Select")))
	{
		return EOliveIRNodeCategory::Select;
	}
	if (ClassName.Contains(TEXT("Gate")))
	{
		return EOliveIRNodeCategory::Gate;
	}
	if (ClassName.Contains(TEXT("DoOnce")))
	{
		return EOliveIRNodeCategory::DoOnce;
	}
	if (ClassName.Contains(TEXT("FlipFlop")))
	{
		return EOliveIRNodeCategory::FlipFlop;
	}
	if (ClassName.Contains(TEXT("Delay")))
	{
		return EOliveIRNodeCategory::Delay;
	}

	// Struct operations
	if (ClassName.Contains(TEXT("MakeStruct")) || ClassName.Contains(TEXT("Make")))
	{
		return EOliveIRNodeCategory::MakeStruct;
	}
	if (ClassName.Contains(TEXT("BreakStruct")) || ClassName.Contains(TEXT("Break")))
	{
		return EOliveIRNodeCategory::BreakStruct;
	}

	// Array operations
	if (ClassName.Contains(TEXT("Array")))
	{
		return EOliveIRNodeCategory::ArrayOperation;
	}

	// Delegate operations
	if (ClassName.Contains(TEXT("CreateDelegate")))
	{
		return EOliveIRNodeCategory::CreateDelegate;
	}
	if (ClassName.Contains(TEXT("Bind")))
	{
		return EOliveIRNodeCategory::BindDelegate;
	}
	if (ClassName.Contains(TEXT("Delegate")))
	{
		return EOliveIRNodeCategory::CallDelegate;
	}

	// Object operations
	if (ClassName.Contains(TEXT("IsValid")))
	{
		return EOliveIRNodeCategory::IsValid;
	}
	if (ClassName.Contains(TEXT("Spawn")))
	{
		return EOliveIRNodeCategory::SpawnActor;
	}

	// Math/comparison
	if (ClassName.Contains(TEXT("Math")) || ClassName.Contains(TEXT("Arithmetic")))
	{
		return EOliveIRNodeCategory::MathExpression;
	}
	if (ClassName.Contains(TEXT("Compare")) || ClassName.Contains(TEXT("Equal")))
	{
		return EOliveIRNodeCategory::Comparison;
	}
	if (ClassName.Contains(TEXT("And")) || ClassName.Contains(TEXT("Or")) || ClassName.Contains(TEXT("Not")))
	{
		return EOliveIRNodeCategory::BooleanOp;
	}

	// Literals
	if (ClassName.Contains(TEXT("Literal")) || ClassName.Contains(TEXT("MakeLiteral")))
	{
		return EOliveIRNodeCategory::Literal;
	}

	// Default to CallFunction for K2Nodes, Unknown otherwise
	if (Node->IsA<UK2Node>())
	{
		return EOliveIRNodeCategory::CallFunction;
	}

	return EOliveIRNodeCategory::Unknown;
}

void FOliveNodeSerializer::SerializePins(
	const UEdGraphNode* Node,
	FOliveIRNode& OutIR,
	const TMap<const UEdGraphNode*, FString>& NodeIdMap) const
{
	if (!Node || !PinSerializer.IsValid())
	{
		return;
	}

	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->bHidden)
		{
			continue;
		}

		FOliveIRPin IRPin = PinSerializer->SerializePin(Pin, NodeIdMap);

		if (Pin->Direction == EGPD_Input)
		{
			OutIR.InputPins.Add(MoveTemp(IRPin));
		}
		else
		{
			OutIR.OutputPins.Add(MoveTemp(IRPin));
		}
	}
}

FString FOliveNodeSerializer::GetNodeId(
	const UEdGraphNode* Node,
	const TMap<const UEdGraphNode*, FString>& NodeIdMap) const
{
	if (!Node)
	{
		return FString();
	}

	const FString* FoundId = NodeIdMap.Find(Node);
	if (FoundId)
	{
		return *FoundId;
	}

	// Generate a fallback ID based on pointer (not ideal but prevents crashes)
	return FString::Printf(TEXT("node_%p"), Node);
}

void FOliveNodeSerializer::PopulateCommonProperties(
	const UEdGraphNode* Node,
	FOliveIRNode& OutIR,
	const TMap<const UEdGraphNode*, FString>& NodeIdMap) const
{
	if (!Node)
	{
		return;
	}

	// Set ID
	OutIR.Id = GetNodeId(Node, NodeIdMap);

	// Set title
	OutIR.Title = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();

	// Set comment if present
	if (!Node->NodeComment.IsEmpty())
	{
		OutIR.Comment = Node->NodeComment;
	}

	// Store category string version
	OutIR.Category = FOliveIRNode::NodeCategoryToString(OutIR.NodeCategory);
}
