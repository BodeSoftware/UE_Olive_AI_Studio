// Copyright Bode Software. All Rights Reserved.

#include "OliveNodeFactory.h"
#include "OliveBlueprintTypes.h"
#include "OliveClassResolver.h"

// Blueprint/Graph includes
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"

// K2 Node includes
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_Knot.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_InputKey.h"
#include "EdGraphNode_Comment.h"
#include "InputCoreTypes.h"

// Utility includes
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetArrayLibrary.h"
#include "GameFramework/Actor.h"

DEFINE_LOG_CATEGORY_STATIC(LogOliveNodeFactory, Log, All);

// ============================================================================
// FOliveNodeFactory Singleton
// ============================================================================

FOliveNodeFactory& FOliveNodeFactory::Get()
{
	static FOliveNodeFactory Instance;
	return Instance;
}

FOliveNodeFactory::FOliveNodeFactory()
{
	InitializeNodeCreators();
}

// ============================================================================
// Node Creation
// ============================================================================

UEdGraphNode* FOliveNodeFactory::CreateNode(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const FString& NodeType,
	const TMap<FString, FString>& Properties,
	int32 PosX,
	int32 PosY)
{
	LastError.Empty();

	// Validate inputs
	if (!Blueprint)
	{
		LastError = TEXT("Blueprint is null");
		UE_LOG(LogOliveNodeFactory, Error, TEXT("%s"), *LastError);
		return nullptr;
	}

	if (!Graph)
	{
		LastError = TEXT("Graph is null");
		UE_LOG(LogOliveNodeFactory, Error, TEXT("%s"), *LastError);
		return nullptr;
	}

	// Validate node type and property-level resolution before attempting creation
	if (!ValidateNodeType(NodeType, Properties))
	{
		// LastError is already set by ValidateNodeType with a structured message
		UE_LOG(LogOliveNodeFactory, Error, TEXT("%s"), *LastError);
		return nullptr;
	}

	// Call the appropriate creator
	const FNodeCreator& Creator = NodeCreators[NodeType];
	UEdGraphNode* NewNode = Creator(Blueprint, Graph, Properties);

	if (!NewNode)
	{
		// LastError should be set by the creator
		if (LastError.IsEmpty())
		{
			LastError = FString::Printf(TEXT("Failed to create node of type '%s'"), *NodeType);
		}
		UE_LOG(LogOliveNodeFactory, Error, TEXT("%s"), *LastError);
		return nullptr;
	}

	// Set position
	SetNodePosition(NewNode, PosX, PosY);

	UE_LOG(LogOliveNodeFactory, Log, TEXT("Created node of type '%s' at (%d, %d)"),
		*NodeType, PosX, PosY);

	return NewNode;
}

bool FOliveNodeFactory::IsNodeTypeSupported(const FString& NodeType) const
{
	return NodeCreators.Contains(NodeType);
}

TArray<FString> FOliveNodeFactory::GetSupportedNodeTypes() const
{
	TArray<FString> Types;
	NodeCreators.GetKeys(Types);
	return Types;
}

TMap<FString, FString> FOliveNodeFactory::GetRequiredProperties(const FString& NodeType) const
{
	if (const TMap<FString, FString>* Props = RequiredPropertiesMap.Find(NodeType))
	{
		return *Props;
	}
	return TMap<FString, FString>();
}

bool FOliveNodeFactory::ValidateNodeType(const FString& NodeType, const TMap<FString, FString>& Properties) const
{
	UE_LOG(LogOliveNodeFactory, Verbose, TEXT("ValidateNodeType: type='%s', properties=%d"), *NodeType, Properties.Num());

	// Step 1: Check if the node type exists in the creator map at all
	if (!NodeCreators.Contains(NodeType))
	{
		// Cast away const for LastError -- ValidateNodeType is logically a query
		// but LastError is a diagnostic side-channel, consistent with existing pattern
		const_cast<FOliveNodeFactory*>(this)->LastError = FString::Printf(
			TEXT("Unknown node type: '%s'. Use blueprint.node_catalog_search to find available node types."),
			*NodeType);
		return false;
	}

	// Step 2: For CallFunction, validate that function_name resolves
	if (NodeType == OliveNodeTypes::CallFunction)
	{
		const FString* FunctionNamePtr = Properties.Find(TEXT("function_name"));
		if (!FunctionNamePtr || FunctionNamePtr->IsEmpty())
		{
			const_cast<FOliveNodeFactory*>(this)->LastError =
				TEXT("CallFunction node requires 'function_name' property");
			return false;
		}

		FString TargetClassName;
		if (const FString* TargetClassPtr = Properties.Find(TEXT("target_class")))
		{
			TargetClassName = *TargetClassPtr;
		}

		// Use the factory's own FindFunction to check resolution
		UFunction* Function = const_cast<FOliveNodeFactory*>(this)->FindFunction(*FunctionNamePtr, TargetClassName);
		if (!Function)
		{
			const_cast<FOliveNodeFactory*>(this)->LastError = FString::Printf(
				TEXT("Function '%s' not found%s. Verify the function name and target class."),
				**FunctionNamePtr,
				TargetClassName.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" in class '%s'"), *TargetClassName));
			return false;
		}
	}

	// Step 3: For Event, validate that event_name resolves in parent class
	// Note: Cannot fully validate without a Blueprint (no parent class available),
	// but we can check if the event_name property is provided
	if (NodeType == OliveNodeTypes::Event)
	{
		const FString* EventNamePtr = Properties.Find(TEXT("event_name"));
		if (!EventNamePtr || EventNamePtr->IsEmpty())
		{
			const_cast<FOliveNodeFactory*>(this)->LastError =
				TEXT("Event node requires 'event_name' property");
			return false;
		}
	}

	return true;
}

// ============================================================================
// Type-Specific Node Creators
// ============================================================================

UK2Node* FOliveNodeFactory::CreateCallFunctionNode(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const TMap<FString, FString>& Properties)
{
	// Get required property
	const FString* FunctionNamePtr = Properties.Find(TEXT("function_name"));
	if (!FunctionNamePtr || FunctionNamePtr->IsEmpty())
	{
		LastError = TEXT("CallFunction node requires 'function_name' property");
		return nullptr;
	}

	// Get optional target class
	FString TargetClassName;
	if (const FString* TargetClassPtr = Properties.Find(TEXT("target_class")))
	{
		TargetClassName = *TargetClassPtr;
	}

	// Find the function
	UFunction* Function = FindFunction(*FunctionNamePtr, TargetClassName);
	if (!Function)
	{
		LastError = FString::Printf(TEXT("Function '%s' not found"), **FunctionNamePtr);
		return nullptr;
	}

	// Create the node
	UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(Graph);
	CallNode->SetFromFunction(Function);
	CallNode->AllocateDefaultPins();
	Graph->AddNode(CallNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

	return CallNode;
}

UK2Node* FOliveNodeFactory::CreateVariableGetNode(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const TMap<FString, FString>& Properties)
{
	const FString* VarNamePtr = Properties.Find(TEXT("variable_name"));
	if (!VarNamePtr || VarNamePtr->IsEmpty())
	{
		LastError = TEXT("GetVariable node requires 'variable_name' property");
		return nullptr;
	}

	// Find the property in the Blueprint
	FName VarName(**VarNamePtr);
	FProperty* Property = nullptr;

	// Check Blueprint variables
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (Var.VarName == VarName)
		{
			// Variable found - property will be resolved from the generated class
			break;
		}
	}

	// Create the node
	UK2Node_VariableGet* GetNode = NewObject<UK2Node_VariableGet>(Graph);
	GetNode->VariableReference.SetSelfMember(VarName);
	GetNode->AllocateDefaultPins();
	Graph->AddNode(GetNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

	return GetNode;
}

UK2Node* FOliveNodeFactory::CreateVariableSetNode(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const TMap<FString, FString>& Properties)
{
	const FString* VarNamePtr = Properties.Find(TEXT("variable_name"));
	if (!VarNamePtr || VarNamePtr->IsEmpty())
	{
		LastError = TEXT("SetVariable node requires 'variable_name' property");
		return nullptr;
	}

	FName VarName(**VarNamePtr);

	// Create the node
	UK2Node_VariableSet* SetNode = NewObject<UK2Node_VariableSet>(Graph);
	SetNode->VariableReference.SetSelfMember(VarName);
	SetNode->AllocateDefaultPins();
	Graph->AddNode(SetNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

	return SetNode;
}

UK2Node* FOliveNodeFactory::CreateEventNode(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const TMap<FString, FString>& Properties)
{
	const FString* EventNamePtr = Properties.Find(TEXT("event_name"));
	if (!EventNamePtr || EventNamePtr->IsEmpty())
	{
		LastError = TEXT("Event node requires 'event_name' property");
		return nullptr;
	}

	FName EventName(**EventNamePtr);

	// Check if parent class has this event
	if (!Blueprint->ParentClass)
	{
		LastError = TEXT("Blueprint has no parent class for event override");
		return nullptr;
	}

	// Find the event function in parent class
	UE_LOG(LogOliveNodeFactory, Log, TEXT("CreateEventNode: looking for event '%s' in parent class '%s'"),
		**EventNamePtr, *Blueprint->ParentClass->GetName());

	UFunction* EventFunction = Blueprint->ParentClass->FindFunctionByName(EventName);
	if (!EventFunction)
	{
		UE_LOG(LogOliveNodeFactory, Warning,
			TEXT("CreateEventNode: event '%s' not found via FindFunctionByName on '%s'. Note: only native events are supported, not Enhanced Input Actions."),
			**EventNamePtr, *Blueprint->ParentClass->GetName());
		LastError = FString::Printf(TEXT("Event '%s' not found in parent class"), **EventNamePtr);
		return nullptr;
	}

	// Check if this native event override already exists in the Blueprint.
	// Each native event can only appear once -- silently returning the existing
	// node would mislead the caller into thinking a new node was created.
	UK2Node_Event* ExistingEvent = FBlueprintEditorUtils::FindOverrideForFunction(
		Blueprint, Blueprint->ParentClass, EventName);

	if (ExistingEvent)
	{
		UE_LOG(LogOliveNodeFactory, Warning, TEXT("CreateEventNode: native event '%s' already exists at (%d, %d)"),
			**EventNamePtr, ExistingEvent->NodePosX, ExistingEvent->NodePosY);
		LastError = FString::Printf(
			TEXT("Native event '%s' already exists in this Blueprint (node at %d, %d). "
				 "Each native event can only appear once."),
			**EventNamePtr, ExistingEvent->NodePosX, ExistingEvent->NodePosY);
		return nullptr;
	}

	// Create new event node
	UK2Node_Event* EventNode = NewObject<UK2Node_Event>(Graph);
	EventNode->EventReference.SetExternalMember(EventName, Blueprint->ParentClass);
	EventNode->bOverrideFunction = true;
	EventNode->AllocateDefaultPins();
	Graph->AddNode(EventNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

	return EventNode;
}

UK2Node* FOliveNodeFactory::CreateCustomEventNode(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const TMap<FString, FString>& Properties)
{
	const FString* EventNamePtr = Properties.Find(TEXT("event_name"));
	if (!EventNamePtr || EventNamePtr->IsEmpty())
	{
		LastError = TEXT("CustomEvent node requires 'event_name' property");
		return nullptr;
	}

	// Create custom event node
	UK2Node_CustomEvent* CustomEvent = NewObject<UK2Node_CustomEvent>(Graph);
	CustomEvent->CustomFunctionName = FName(**EventNamePtr);
	CustomEvent->AllocateDefaultPins();
	Graph->AddNode(CustomEvent, /*bFromUI=*/false, /*bSelectNewNode=*/false);

	return CustomEvent;
}

UK2Node* FOliveNodeFactory::CreateBranchNode(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const TMap<FString, FString>& Properties)
{
	UK2Node_IfThenElse* BranchNode = NewObject<UK2Node_IfThenElse>(Graph);
	BranchNode->AllocateDefaultPins();
	Graph->AddNode(BranchNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

	return BranchNode;
}

UK2Node* FOliveNodeFactory::CreateSequenceNode(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const TMap<FString, FString>& Properties)
{
	UK2Node_ExecutionSequence* SequenceNode = NewObject<UK2Node_ExecutionSequence>(Graph);
	SequenceNode->AllocateDefaultPins();
	Graph->AddNode(SequenceNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

	// Add additional outputs if specified
	if (const FString* NumOutputsStr = Properties.Find(TEXT("num_outputs")))
	{
		int32 NumOutputs = FCString::Atoi(**NumOutputsStr);
		// Sequence starts with 2 outputs, add more if needed
		for (int32 i = 2; i < NumOutputs; ++i)
		{
			SequenceNode->AddInputPin();
		}
	}

	return SequenceNode;
}

UK2Node* FOliveNodeFactory::CreateCastNode(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const TMap<FString, FString>& Properties)
{
	const FString* TargetClassPtr = Properties.Find(TEXT("target_class"));
	if (!TargetClassPtr || TargetClassPtr->IsEmpty())
	{
		LastError = TEXT("Cast node requires 'target_class' property");
		return nullptr;
	}

	UClass* TargetClass = FindClass(*TargetClassPtr);
	if (!TargetClass)
	{
		LastError = FString::Printf(TEXT("Target class '%s' not found"), **TargetClassPtr);
		return nullptr;
	}

	UK2Node_DynamicCast* CastNode = NewObject<UK2Node_DynamicCast>(Graph);
	CastNode->TargetType = TargetClass;
	CastNode->AllocateDefaultPins();
	Graph->AddNode(CastNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

	return CastNode;
}

UK2Node* FOliveNodeFactory::CreateSpawnActorNode(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const TMap<FString, FString>& Properties)
{
	const FString* ActorClassPtr = Properties.Find(TEXT("actor_class"));
	if (!ActorClassPtr || ActorClassPtr->IsEmpty())
	{
		LastError = TEXT("SpawnActor node requires 'actor_class' property");
		return nullptr;
	}

	UE_LOG(LogOliveNodeFactory, Log, TEXT("CreateSpawnActorNode: resolving actor class '%s'"), **ActorClassPtr);

	UClass* ActorClass = FindClass(*ActorClassPtr);
	if (!ActorClass)
	{
		LastError = FString::Printf(TEXT("Actor class '%s' not found"), **ActorClassPtr);
		return nullptr;
	}

	UE_LOG(LogOliveNodeFactory, Verbose, TEXT("CreateSpawnActorNode: class '%s' resolved to '%s', checking Actor inheritance"),
		**ActorClassPtr, *ActorClass->GetName());

	if (!ActorClass->IsChildOf(AActor::StaticClass()))
	{
		LastError = FString::Printf(TEXT("'%s' is not an Actor class"), **ActorClassPtr);
		return nullptr;
	}

	UK2Node_SpawnActorFromClass* SpawnNode = NewObject<UK2Node_SpawnActorFromClass>(Graph);
	// Note: The class pin needs to be set after allocation
	SpawnNode->AllocateDefaultPins();
	Graph->AddNode(SpawnNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

	// Set the class on the class pin
	UEdGraphPin* ClassPin = SpawnNode->GetClassPin();
	if (ClassPin)
	{
		ClassPin->DefaultObject = ActorClass;
	}

	return SpawnNode;
}

UK2Node* FOliveNodeFactory::CreateMakeStructNode(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const TMap<FString, FString>& Properties)
{
	const FString* StructTypePtr = Properties.Find(TEXT("struct_type"));
	if (!StructTypePtr || StructTypePtr->IsEmpty())
	{
		LastError = TEXT("MakeStruct node requires 'struct_type' property");
		return nullptr;
	}

	UScriptStruct* Struct = FindStruct(*StructTypePtr);
	if (!Struct)
	{
		LastError = FString::Printf(TEXT("Struct type '%s' not found"), **StructTypePtr);
		return nullptr;
	}

	UK2Node_MakeStruct* MakeNode = NewObject<UK2Node_MakeStruct>(Graph);
	MakeNode->StructType = Struct;
	MakeNode->AllocateDefaultPins();
	Graph->AddNode(MakeNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

	return MakeNode;
}

UK2Node* FOliveNodeFactory::CreateBreakStructNode(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const TMap<FString, FString>& Properties)
{
	const FString* StructTypePtr = Properties.Find(TEXT("struct_type"));
	if (!StructTypePtr || StructTypePtr->IsEmpty())
	{
		LastError = TEXT("BreakStruct node requires 'struct_type' property");
		return nullptr;
	}

	UScriptStruct* Struct = FindStruct(*StructTypePtr);
	if (!Struct)
	{
		LastError = FString::Printf(TEXT("Struct type '%s' not found"), **StructTypePtr);
		return nullptr;
	}

	UK2Node_BreakStruct* BreakNode = NewObject<UK2Node_BreakStruct>(Graph);
	BreakNode->StructType = Struct;
	BreakNode->AllocateDefaultPins();
	Graph->AddNode(BreakNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

	return BreakNode;
}

UK2Node* FOliveNodeFactory::CreateForLoopNode(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const TMap<FString, FString>& Properties)
{
	// ForLoop is typically a macro - we use CallFunction on a macro instance
	// Find the ForLoop macro from the Kismet library
	UFunction* ForLoopFunc = FindFunction(TEXT("ForLoop"), TEXT("KismetSystemLibrary"));
	if (!ForLoopFunc)
	{
		// Try ForLoopWithBreak if standard ForLoop not found
		ForLoopFunc = FindFunction(TEXT("ForLoopWithBreak"), TEXT("KismetSystemLibrary"));
	}

	if (ForLoopFunc)
	{
		UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(Graph);
		CallNode->SetFromFunction(ForLoopFunc);
		CallNode->AllocateDefaultPins();
		Graph->AddNode(CallNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		return CallNode;
	}

	// If not found as function, create as macro instance
	LastError = TEXT("ForLoop function not found - macro support not yet implemented");
	return nullptr;
}

UK2Node* FOliveNodeFactory::CreateForEachLoopNode(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const TMap<FString, FString>& Properties)
{
	// ForEachLoop is typically a macro - we try to find it as a function first
	UFunction* ForEachFunc = FindFunction(TEXT("ForEachLoop"), TEXT("KismetArrayLibrary"));
	if (!ForEachFunc)
	{
		// Try alternate names
		ForEachFunc = FindFunction(TEXT("Array_ForEach"), TEXT("KismetArrayLibrary"));
	}

	if (ForEachFunc)
	{
		UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(Graph);
		CallNode->SetFromFunction(ForEachFunc);
		CallNode->AllocateDefaultPins();
		Graph->AddNode(CallNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		return CallNode;
	}

	// ForEachLoop is implemented as a macro in Blueprints
	// Full macro support requires additional implementation
	LastError = TEXT("ForEachLoop function not found - macro support not yet implemented");
	return nullptr;
}

UK2Node* FOliveNodeFactory::CreateDelayNode(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const TMap<FString, FString>& Properties)
{
	// Delay is a latent function from KismetSystemLibrary
	UFunction* DelayFunc = FindFunction(TEXT("Delay"), TEXT("KismetSystemLibrary"));
	if (!DelayFunc)
	{
		LastError = TEXT("Delay function not found");
		return nullptr;
	}

	UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(Graph);
	CallNode->SetFromFunction(DelayFunc);
	CallNode->AllocateDefaultPins();
	Graph->AddNode(CallNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

	return CallNode;
}

UK2Node* FOliveNodeFactory::CreateIsValidNode(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const TMap<FString, FString>& Properties)
{
	// IsValid is from KismetSystemLibrary
	UFunction* IsValidFunc = FindFunction(TEXT("IsValid"), TEXT("KismetSystemLibrary"));
	if (!IsValidFunc)
	{
		// Try the object version
		IsValidFunc = FindFunction(TEXT("IsValidObject"), TEXT("KismetSystemLibrary"));
	}

	if (!IsValidFunc)
	{
		LastError = TEXT("IsValid function not found");
		return nullptr;
	}

	UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(Graph);
	CallNode->SetFromFunction(IsValidFunc);
	CallNode->AllocateDefaultPins();
	Graph->AddNode(CallNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

	return CallNode;
}

UK2Node* FOliveNodeFactory::CreatePrintStringNode(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const TMap<FString, FString>& Properties)
{
	UFunction* PrintFunc = FindFunction(TEXT("PrintString"), TEXT("KismetSystemLibrary"));
	if (!PrintFunc)
	{
		LastError = TEXT("PrintString function not found");
		return nullptr;
	}

	UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(Graph);
	CallNode->SetFromFunction(PrintFunc);
	CallNode->AllocateDefaultPins();
	Graph->AddNode(CallNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

	return CallNode;
}

UEdGraphNode* FOliveNodeFactory::CreateCommentNode(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const TMap<FString, FString>& Properties)
{
	UEdGraphNode_Comment* CommentNode = NewObject<UEdGraphNode_Comment>(Graph);

	// Set comment text
	if (const FString* TextPtr = Properties.Find(TEXT("text")))
	{
		CommentNode->NodeComment = *TextPtr;
	}

	// Set dimensions
	if (const FString* WidthPtr = Properties.Find(TEXT("width")))
	{
		CommentNode->NodeWidth = FCString::Atoi(**WidthPtr);
	}
	else
	{
		CommentNode->NodeWidth = 400; // Default width
	}

	if (const FString* HeightPtr = Properties.Find(TEXT("height")))
	{
		CommentNode->NodeHeight = FCString::Atoi(**HeightPtr);
	}
	else
	{
		CommentNode->NodeHeight = 100; // Default height
	}

	Graph->AddNode(CommentNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

	return CommentNode;
}

UK2Node* FOliveNodeFactory::CreateRerouteNode(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const TMap<FString, FString>& Properties)
{
	UK2Node_Knot* KnotNode = NewObject<UK2Node_Knot>(Graph);
	KnotNode->AllocateDefaultPins();
	Graph->AddNode(KnotNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

	return KnotNode;
}

UK2Node* FOliveNodeFactory::CreateInputKeyNode(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const TMap<FString, FString>& Properties)
{
	const FString* KeyPtr = Properties.Find(TEXT("key"));
	if (!KeyPtr || KeyPtr->IsEmpty())
	{
		LastError = TEXT("InputKey node requires 'key' property (e.g., \"E\", \"SpaceBar\")");
		return nullptr;
	}

	// Resolve the key from string
	FKey ResolvedKey(**KeyPtr);
	if (!ResolvedKey.IsValid())
	{
		LastError = FString::Printf(TEXT("Key '%s' is not a valid FKey name"), **KeyPtr);
		return nullptr;
	}

	UK2Node_InputKey* InputKeyNode = NewObject<UK2Node_InputKey>(Graph);
	InputKeyNode->InputKey = ResolvedKey;
	InputKeyNode->bConsumeInput = true;
	InputKeyNode->bOverrideParentBinding = false;
	InputKeyNode->AllocateDefaultPins();
	Graph->AddNode(InputKeyNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

	return InputKeyNode;
}

// ============================================================================
// Helper Methods
// ============================================================================

void FOliveNodeFactory::SetNodePosition(UEdGraphNode* Node, int32 PosX, int32 PosY)
{
	if (Node)
	{
		Node->NodePosX = PosX;
		Node->NodePosY = PosY;
	}
}

UClass* FOliveNodeFactory::FindClass(const FString& ClassName)
{
	FOliveClassResolveResult Result = FOliveClassResolver::Resolve(ClassName);
	if (Result.IsValid())
	{
		UE_LOG(LogOliveNodeFactory, Verbose, TEXT("FindClass('%s'): resolved -> %s"),
			*ClassName, *Result.Class->GetName());
		return Result.Class;
	}

	UE_LOG(LogOliveNodeFactory, Warning, TEXT("FindClass('%s'): FAILED — all resolution strategies exhausted"), *ClassName);
	return nullptr;
}

UFunction* FOliveNodeFactory::FindFunction(const FString& FunctionName, const FString& ClassName)
{
	// If class name specified, search in that class
	if (!ClassName.IsEmpty())
	{
		UE_LOG(LogOliveNodeFactory, Verbose, TEXT("FindFunction('%s', class='%s'): searching specified class"), *FunctionName, *ClassName);
		UClass* Class = FindClass(ClassName);
		if (Class)
		{
			UFunction* Func = Class->FindFunctionByName(FName(*FunctionName));
			if (Func)
			{
				UE_LOG(LogOliveNodeFactory, Verbose, TEXT("FindFunction('%s'): found in class '%s'"), *FunctionName, *Class->GetName());
				return Func;
			}
		}
	}

	// Search in common library classes
	TArray<UClass*> LibraryClasses = {
		UKismetSystemLibrary::StaticClass(),
		UObject::StaticClass(),
		AActor::StaticClass(),
	};

	for (UClass* LibClass : LibraryClasses)
	{
		UE_LOG(LogOliveNodeFactory, Verbose, TEXT("FindFunction('%s'): trying library class '%s'"), *FunctionName, *LibClass->GetName());
		UFunction* Func = LibClass->FindFunctionByName(FName(*FunctionName));
		if (Func)
		{
			UE_LOG(LogOliveNodeFactory, Verbose, TEXT("FindFunction('%s'): found in library class '%s'"), *FunctionName, *LibClass->GetName());
			return Func;
		}
	}

	UE_LOG(LogOliveNodeFactory, Warning, TEXT("FindFunction('%s', class='%s'): FAILED — searched specified class + library classes [KismetSystemLibrary, Object, Actor]"),
		*FunctionName, *ClassName);
	return nullptr;
}

UScriptStruct* FOliveNodeFactory::FindStruct(const FString& StructName)
{
	// Try direct lookup
	UScriptStruct* Struct = FindFirstObject<UScriptStruct>(*StructName, EFindFirstObjectOptions::NativeFirst);
	if (Struct)
	{
		return Struct;
	}

	// Try with F prefix
	if (!StructName.StartsWith(TEXT("F")))
	{
		FString PrefixedName = TEXT("F") + StructName;
		Struct = FindFirstObject<UScriptStruct>(*PrefixedName, EFindFirstObjectOptions::NativeFirst);
		if (Struct)
		{
			return Struct;
		}
	}

	// Try common engine structs
	TArray<FString> CommonStructs = {
		TEXT("FVector"),
		TEXT("FRotator"),
		TEXT("FTransform"),
		TEXT("FColor"),
		TEXT("FLinearColor"),
		TEXT("FVector2D"),
		TEXT("FHitResult"),
		TEXT("FTimerHandle"),
	};

	for (const FString& CommonName : CommonStructs)
	{
		if (StructName.Equals(CommonName, ESearchCase::IgnoreCase) ||
			StructName.Equals(CommonName.Mid(1), ESearchCase::IgnoreCase))
		{
			Struct = FindFirstObject<UScriptStruct>(*CommonName, EFindFirstObjectOptions::NativeFirst);
			if (Struct)
			{
				return Struct;
			}
		}
	}

	return nullptr;
}

void FOliveNodeFactory::InitializeNodeCreators()
{
	// ============================================================================
	// Register Node Creators
	// ============================================================================

	// Control Flow
	NodeCreators.Add(OliveNodeTypes::Branch, [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
		return CreateBranchNode(BP, G, P);
	});

	NodeCreators.Add(OliveNodeTypes::Sequence, [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
		return CreateSequenceNode(BP, G, P);
	});

	NodeCreators.Add(OliveNodeTypes::ForLoop, [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
		return CreateForLoopNode(BP, G, P);
	});

	NodeCreators.Add(OliveNodeTypes::ForEachLoop, [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
		return CreateForEachLoopNode(BP, G, P);
	});

	NodeCreators.Add(OliveNodeTypes::Delay, [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
		return CreateDelayNode(BP, G, P);
	});

	// Function/Event
	NodeCreators.Add(OliveNodeTypes::CallFunction, [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
		return CreateCallFunctionNode(BP, G, P);
	});

	NodeCreators.Add(OliveNodeTypes::GetVariable, [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
		return CreateVariableGetNode(BP, G, P);
	});

	NodeCreators.Add(OliveNodeTypes::SetVariable, [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
		return CreateVariableSetNode(BP, G, P);
	});

	NodeCreators.Add(OliveNodeTypes::Event, [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
		return CreateEventNode(BP, G, P);
	});

	NodeCreators.Add(OliveNodeTypes::CustomEvent, [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
		return CreateCustomEventNode(BP, G, P);
	});

	// Casting/Type
	NodeCreators.Add(OliveNodeTypes::Cast, [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
		return CreateCastNode(BP, G, P);
	});

	NodeCreators.Add(OliveNodeTypes::IsValid, [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
		return CreateIsValidNode(BP, G, P);
	});

	// Object Creation
	NodeCreators.Add(OliveNodeTypes::SpawnActor, [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
		return CreateSpawnActorNode(BP, G, P);
	});

	// Struct Operations
	NodeCreators.Add(OliveNodeTypes::MakeStruct, [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
		return CreateMakeStructNode(BP, G, P);
	});

	NodeCreators.Add(OliveNodeTypes::BreakStruct, [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
		return CreateBreakStructNode(BP, G, P);
	});

	// Utility
	NodeCreators.Add(OliveNodeTypes::PrintString, [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
		return CreatePrintStringNode(BP, G, P);
	});

	NodeCreators.Add(OliveNodeTypes::Comment, [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
		return CreateCommentNode(BP, G, P);
	});

	NodeCreators.Add(OliveNodeTypes::Reroute, [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
		return CreateRerouteNode(BP, G, P);
	});

	// Input
	NodeCreators.Add(OliveNodeTypes::InputKey, [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
		return CreateInputKeyNode(BP, G, P);
	});

	// ============================================================================
	// Register Required Properties
	// ============================================================================

	// CallFunction
	TMap<FString, FString> CallFunctionProps;
	CallFunctionProps.Add(TEXT("function_name"), TEXT("Name of the function to call (required)"));
	CallFunctionProps.Add(TEXT("target_class"), TEXT("Class containing the function (optional)"));
	RequiredPropertiesMap.Add(OliveNodeTypes::CallFunction, CallFunctionProps);

	// GetVariable
	TMap<FString, FString> GetVarProps;
	GetVarProps.Add(TEXT("variable_name"), TEXT("Name of the variable to get (required)"));
	RequiredPropertiesMap.Add(OliveNodeTypes::GetVariable, GetVarProps);

	// SetVariable
	TMap<FString, FString> SetVarProps;
	SetVarProps.Add(TEXT("variable_name"), TEXT("Name of the variable to set (required)"));
	RequiredPropertiesMap.Add(OliveNodeTypes::SetVariable, SetVarProps);

	// Event
	TMap<FString, FString> EventProps;
	EventProps.Add(TEXT("event_name"), TEXT("Name of the event to override (required)"));
	RequiredPropertiesMap.Add(OliveNodeTypes::Event, EventProps);

	// CustomEvent
	TMap<FString, FString> CustomEventProps;
	CustomEventProps.Add(TEXT("event_name"), TEXT("Name for the custom event (required)"));
	RequiredPropertiesMap.Add(OliveNodeTypes::CustomEvent, CustomEventProps);

	// Sequence
	TMap<FString, FString> SequenceProps;
	SequenceProps.Add(TEXT("num_outputs"), TEXT("Number of output pins (optional, default 2)"));
	RequiredPropertiesMap.Add(OliveNodeTypes::Sequence, SequenceProps);

	// Cast
	TMap<FString, FString> CastProps;
	CastProps.Add(TEXT("target_class"), TEXT("Class to cast to (required)"));
	RequiredPropertiesMap.Add(OliveNodeTypes::Cast, CastProps);

	// SpawnActor
	TMap<FString, FString> SpawnActorProps;
	SpawnActorProps.Add(TEXT("actor_class"), TEXT("Actor class to spawn (required)"));
	RequiredPropertiesMap.Add(OliveNodeTypes::SpawnActor, SpawnActorProps);

	// MakeStruct
	TMap<FString, FString> MakeStructProps;
	MakeStructProps.Add(TEXT("struct_type"), TEXT("Struct type to create (required)"));
	RequiredPropertiesMap.Add(OliveNodeTypes::MakeStruct, MakeStructProps);

	// BreakStruct
	TMap<FString, FString> BreakStructProps;
	BreakStructProps.Add(TEXT("struct_type"), TEXT("Struct type to break (required)"));
	RequiredPropertiesMap.Add(OliveNodeTypes::BreakStruct, BreakStructProps);

	// InputKey
	TMap<FString, FString> InputKeyProps;
	InputKeyProps.Add(TEXT("key"), TEXT("Key name to bind (required, e.g., \"E\", \"SpaceBar\", \"Gamepad_FaceButton_Bottom\")"));
	RequiredPropertiesMap.Add(OliveNodeTypes::InputKey, InputKeyProps);

	// Comment
	TMap<FString, FString> CommentProps;
	CommentProps.Add(TEXT("text"), TEXT("Comment text (optional)"));
	CommentProps.Add(TEXT("width"), TEXT("Comment width in pixels (optional, default 400)"));
	CommentProps.Add(TEXT("height"), TEXT("Comment height in pixels (optional, default 100)"));
	RequiredPropertiesMap.Add(OliveNodeTypes::Comment, CommentProps);

	// Nodes with no required properties get empty maps
	RequiredPropertiesMap.Add(OliveNodeTypes::Branch, TMap<FString, FString>());
	RequiredPropertiesMap.Add(OliveNodeTypes::ForLoop, TMap<FString, FString>());
	RequiredPropertiesMap.Add(OliveNodeTypes::ForEachLoop, TMap<FString, FString>());
	RequiredPropertiesMap.Add(OliveNodeTypes::Delay, TMap<FString, FString>());
	RequiredPropertiesMap.Add(OliveNodeTypes::IsValid, TMap<FString, FString>());
	RequiredPropertiesMap.Add(OliveNodeTypes::PrintString, TMap<FString, FString>());
	RequiredPropertiesMap.Add(OliveNodeTypes::Reroute, TMap<FString, FString>());
}
