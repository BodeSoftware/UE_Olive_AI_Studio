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
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_Message.h"
#include "EdGraphNode_Comment.h"
#include "InputCoreTypes.h"

// SCS includes (for component delegate event detection)
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"

// Enhanced Input includes (for UK2Node_EnhancedInputAction + UInputAction)
#include "K2Node_EnhancedInputAction.h"
#include "InputAction.h"
#include "AssetRegistry/AssetRegistryModule.h"

// Utility includes
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/KismetArrayLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "Components/SceneComponent.h"
#include "Components/PrimitiveComponent.h"

// Universal node creation includes
#include "UObject/UObjectGlobals.h"  // For StaticLoadClass

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

	// Validate node type and property-level resolution before attempting creation.
	// Pass Blueprint so ValidateNodeType can find Blueprint-defined functions.
	if (!ValidateNodeType(NodeType, Properties, Blueprint))
	{
		// LastError is already set by ValidateNodeType with a structured message
		UE_LOG(LogOliveNodeFactory, Error, TEXT("%s"), *LastError);
		return nullptr;
	}

	// Call the appropriate creator (curated types) or universal fallback
	UEdGraphNode* NewNode = nullptr;
	if (const FNodeCreator* Creator = NodeCreators.Find(NodeType))
	{
		NewNode = (*Creator)(Blueprint, Graph, Properties);
	}
	else
	{
		// Universal fallback: type passed ValidateNodeType, so it's a valid K2Node class.
		// Position is NOT set here -- the existing SetNodePosition below handles it uniformly.
		NewNode = CreateNodeByClass(Blueprint, Graph, NodeType, Properties);
	}

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

bool FOliveNodeFactory::ValidateNodeType(const FString& NodeType, const TMap<FString, FString>& Properties, UBlueprint* Blueprint) const
{
	UE_LOG(LogOliveNodeFactory, Verbose, TEXT("ValidateNodeType: type='%s', properties=%d"), *NodeType, Properties.Num());

	// Step 1: Check if the node type exists in the curated creator map
	if (!NodeCreators.Contains(NodeType))
	{
		// Step 1b: Universal fallback -- try resolving as a UK2Node subclass name
		UClass* NodeClass = FindK2NodeClass(NodeType);
		if (NodeClass)
		{
			UE_LOG(LogOliveNodeFactory, Log,
				TEXT("ValidateNodeType: '%s' not in curated map, but resolved as UK2Node subclass %s"),
				*NodeType, *NodeClass->GetName());
			return true; // Valid as a universal node class
		}

		// Cast away const for LastError -- ValidateNodeType is logically a query
		// but LastError is a diagnostic side-channel, consistent with existing pattern
		const_cast<FOliveNodeFactory*>(this)->LastError = FString::Printf(
			TEXT("Unknown node type: '%s'. Not found as a curated type or UK2Node subclass. "
				 "Use blueprint.search_nodes to find available node types, or pass the exact "
				 "UK2Node class name (e.g., 'K2Node_ComponentBoundEvent')."),
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

		// Use the factory's own FindFunction to check resolution.
		// Blueprint may be nullptr for pre-pipeline checks (before asset load),
		// in which case Blueprint-defined functions won't be found here but will
		// be found later in CreateCallFunctionNode when Blueprint is available.
		UFunction* Function = const_cast<FOliveNodeFactory*>(this)->FindFunction(*FunctionNamePtr, TargetClassName, Blueprint);
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

	// Check if caller explicitly requests an interface message call
	bool bForceInterfaceCall = false;
	if (const FString* InterfaceFlag = Properties.Find(TEXT("is_interface_call")))
	{
		bForceInterfaceCall = InterfaceFlag->Equals(TEXT("true"), ESearchCase::IgnoreCase);
	}

	// Find the function (pass Blueprint to search GeneratedClass for Blueprint-defined functions)
	EOliveFunctionMatchMethod MatchMethod = EOliveFunctionMatchMethod::None;
	UFunction* Function = FindFunction(*FunctionNamePtr, TargetClassName, Blueprint, &MatchMethod);
	if (!Function)
	{
		LastError = FString::Printf(TEXT("Function '%s' not found"), **FunctionNamePtr);
		return nullptr;
	}

	// Determine if this should be an interface message call.
	// UK2Node_Message is used when calling a function defined on an interface --
	// it creates a generic UObject self pin and handles the cast-to-interface
	// dispatch at compile time. This is how Blueprint "Call Interface Function"
	// nodes work in the editor.
	const bool bIsInterfaceCall = bForceInterfaceCall
		|| (MatchMethod == EOliveFunctionMatchMethod::InterfaceSearch);

	if (bIsInterfaceCall)
	{
		// Create an interface message node (UK2Node_Message inherits UK2Node_CallFunction)
		UK2Node_Message* MessageNode = NewObject<UK2Node_Message>(Graph);

		// SetFromField with bIsConsideredSelfContext=false ensures the interface
		// class is preserved as the MemberParent on the FunctionReference.
		// This is the same pattern the engine uses in BlueprintActionDatabase.cpp
		// when spawning message nodes from the context menu.
		MessageNode->FunctionReference.SetFromField<UFunction>(Function, /*bIsConsideredSelfContext=*/false);
		MessageNode->AllocateDefaultPins();
		Graph->AddNode(MessageNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

		UE_LOG(LogOliveNodeFactory, Log,
			TEXT("CreateCallFunctionNode: created UK2Node_Message for interface function '%s' on '%s'"),
			*Function->GetName(), *Function->GetOwnerClass()->GetName());

		return MessageNode;
	}

	// Create a standard CallFunction node
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

	FName VarName(**VarNamePtr);

	// Create the node -- variable resolution happens at AllocateDefaultPins/compile
	// time via SetSelfMember, not at creation time.
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
		// ----------------------------------------------------------------
		// FIX RC1: Check if this is a component delegate event (e.g.,
		// OnComponentBeginOverlap, OnComponentEndOverlap, OnComponentHit).
		// These are bound via UK2Node_ComponentBoundEvent, not regular
		// event override nodes. Scan SCS components for a matching
		// multicast delegate property.
		// ----------------------------------------------------------------
		if (Blueprint->SimpleConstructionScript)
		{
			// Optional: extract component name hint from properties
			FString ComponentNameHint;
			if (const FString* CompName = Properties.Find(TEXT("component_name")))
			{
				ComponentNameHint = *CompName;
			}

			FObjectProperty* FoundComponentProp = nullptr;
			FMulticastDelegateProperty* FoundDelegateProp = nullptr;
			FString FoundComponentName;

			TArray<USCS_Node*> AllSCSNodes = Blueprint->SimpleConstructionScript->GetAllNodes();
			for (USCS_Node* SCSNode : AllSCSNodes)
			{
				if (!SCSNode || !SCSNode->ComponentClass)
				{
					continue;
				}

				// If a component name hint was given, only match that component
				if (!ComponentNameHint.IsEmpty()
					&& !SCSNode->GetVariableName().ToString().Equals(ComponentNameHint, ESearchCase::IgnoreCase))
				{
					continue;
				}

				// Search for a multicast delegate property matching the event name
				for (TFieldIterator<FMulticastDelegateProperty> PropIt(SCSNode->ComponentClass); PropIt; ++PropIt)
				{
					FMulticastDelegateProperty* DelegateProp = *PropIt;
					if (DelegateProp && DelegateProp->GetFName() == EventName)
					{
						// Find the FObjectProperty on the generated class that
						// corresponds to this SCS component variable
						if (Blueprint->GeneratedClass)
						{
							FObjectProperty* CompProp = FindFProperty<FObjectProperty>(
								Blueprint->GeneratedClass, SCSNode->GetVariableName());

							if (CompProp)
							{
								FoundComponentProp = CompProp;
								FoundDelegateProp = DelegateProp;
								FoundComponentName = SCSNode->GetVariableName().ToString();
								break;
							}
						}
					}
				}

				if (FoundDelegateProp)
				{
					break;
				}
			}

			if (FoundComponentProp && FoundDelegateProp)
			{
				UE_LOG(LogOliveNodeFactory, Log,
					TEXT("CreateEventNode: '%s' is a component delegate on '%s' (%s) — creating UK2Node_ComponentBoundEvent"),
					**EventNamePtr, *FoundComponentName, *FoundComponentProp->PropertyClass->GetName());

				UK2Node_ComponentBoundEvent* BoundEventNode = NewObject<UK2Node_ComponentBoundEvent>(Graph);
				BoundEventNode->InitializeComponentBoundEventParams(FoundComponentProp, FoundDelegateProp);
				BoundEventNode->AllocateDefaultPins();
				Graph->AddNode(BoundEventNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

				return BoundEventNode;
			}
		}

		// ----------------------------------------------------------------
		// Check 3: Enhanced Input Action event (e.g., "IA_Interact", "IA_Jump")
		// Creates a UK2Node_EnhancedInputAction for player input handling.
		// ----------------------------------------------------------------
		{
			FString InputActionName = *EventNamePtr;

			// Normalize: strip "IA_" prefix to get the action base name,
			// or if the name doesn't have it, use as-is for searching.
			// We always search for the asset with IA_ prefix.
			if (!InputActionName.StartsWith(TEXT("IA_")))
			{
				InputActionName = FString::Printf(TEXT("IA_%s"), **EventNamePtr);
			}

			// Delegate to the dedicated creator method
			TMap<FString, FString> InputActionProps;
			InputActionProps.Add(TEXT("input_action_name"), InputActionName);
			UK2Node* InputActionNode = CreateEnhancedInputActionNode(Blueprint, Graph, InputActionProps);
			if (InputActionNode)
			{
				UE_LOG(LogOliveNodeFactory, Log,
					TEXT("CreateEventNode: '%s' resolved as Enhanced Input Action '%s'"),
					**EventNamePtr, *InputActionName);
				return InputActionNode;
			}
			// If we tried to find an IA_ asset and failed, check if the original
			// event name was IA_ prefixed. If so, give a specific error about
			// missing Input Action assets rather than falling through to the
			// generic "not found" error.
			if (EventNamePtr->StartsWith(TEXT("IA_")))
			{
				// LastError is already set by CreateEnhancedInputActionNode
				return nullptr;
			}
		}

		UE_LOG(LogOliveNodeFactory, Warning,
			TEXT("CreateEventNode: event '%s' not found via FindFunctionByName on '%s', "
				 "not a component delegate, and not an Enhanced Input Action."),
			**EventNamePtr, *Blueprint->ParentClass->GetName());
		LastError = FString::Printf(
			TEXT("Event '%s' not found in parent class, as a component delegate, "
				 "or as an Enhanced Input Action (IA_ asset). "
				 "Verify the event name or use project.search with type 'InputAction' "
				 "to find available Input Action assets."),
			**EventNamePtr);
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
	return CreateMacroInstanceNode(Blueprint, Graph, TEXT("ForLoop"));
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
	return CreateMacroInstanceNode(Blueprint, Graph, TEXT("ForEachLoop"));
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

UK2Node* FOliveNodeFactory::CreateCallDelegateNode(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const TMap<FString, FString>& Properties)
{
	const FString* DelegateNamePtr = Properties.Find(TEXT("delegate_name"));
	if (!DelegateNamePtr || DelegateNamePtr->IsEmpty())
	{
		LastError = TEXT("CallDelegate node requires 'delegate_name' property");
		return nullptr;
	}

	const FName DelegateFName(**DelegateNamePtr);

	// Search for the FMulticastDelegateProperty on the Blueprint's generated class.
	// SkeletonGeneratedClass is preferred (it may be more up-to-date if the Blueprint
	// has not been fully compiled yet), with fallback to GeneratedClass.
	FMulticastDelegateProperty* DelegateProp = nullptr;
	UClass* OwnerClass = nullptr;

	// Try SkeletonGeneratedClass first
	if (Blueprint->SkeletonGeneratedClass)
	{
		for (TFieldIterator<FMulticastDelegateProperty> It(Blueprint->SkeletonGeneratedClass); It; ++It)
		{
			if (It->GetFName() == DelegateFName)
			{
				DelegateProp = *It;
				OwnerClass = Blueprint->SkeletonGeneratedClass;
				break;
			}
		}
	}

	// Fallback to GeneratedClass
	if (!DelegateProp && Blueprint->GeneratedClass)
	{
		for (TFieldIterator<FMulticastDelegateProperty> It(Blueprint->GeneratedClass); It; ++It)
		{
			if (It->GetFName() == DelegateFName)
			{
				DelegateProp = *It;
				OwnerClass = Blueprint->GeneratedClass;
				break;
			}
		}
	}

	if (!DelegateProp)
	{
		// Build a list of available dispatchers for the error message
		TArray<FString> AvailableDispatchers;
		UClass* SearchClass = Blueprint->SkeletonGeneratedClass
			? Blueprint->SkeletonGeneratedClass
			: Blueprint->GeneratedClass;
		if (SearchClass)
		{
			for (TFieldIterator<FMulticastDelegateProperty> It(SearchClass); It; ++It)
			{
				AvailableDispatchers.Add(It->GetName());
			}
		}

		if (AvailableDispatchers.Num() > 0)
		{
			LastError = FString::Printf(
				TEXT("Event dispatcher '%s' not found on Blueprint '%s'. "
					 "Available dispatchers: %s"),
				**DelegateNamePtr, *Blueprint->GetName(),
				*FString::Join(AvailableDispatchers, TEXT(", ")));
		}
		else
		{
			LastError = FString::Printf(
				TEXT("Event dispatcher '%s' not found on Blueprint '%s'. "
					 "This Blueprint has no event dispatchers. "
					 "Use blueprint.add_function with function_type='event_dispatcher' to create one first."),
				**DelegateNamePtr, *Blueprint->GetName());
		}
		return nullptr;
	}

	UE_LOG(LogOliveNodeFactory, Log,
		TEXT("CreateCallDelegateNode: Found delegate '%s' on class '%s'"),
		**DelegateNamePtr, *OwnerClass->GetName());

	// Create the CallDelegate node
	UK2Node_CallDelegate* CallDelegateNode = NewObject<UK2Node_CallDelegate>(Graph);
	CallDelegateNode->SetFromProperty(DelegateProp, /*bSelfContext=*/true, OwnerClass);
	CallDelegateNode->AllocateDefaultPins();
	Graph->AddNode(CallDelegateNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

	return CallDelegateNode;
}

UK2Node* FOliveNodeFactory::CreateBindDelegateNode(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const TMap<FString, FString>& Properties)
{
	const FString* DelegateNamePtr = Properties.Find(TEXT("delegate_name"));
	if (!DelegateNamePtr || DelegateNamePtr->IsEmpty())
	{
		LastError = TEXT("BindDelegate node requires 'delegate_name' property");
		return nullptr;
	}

	const FName DelegateFName(**DelegateNamePtr);

	// Search for the FMulticastDelegateProperty on the Blueprint's generated class.
	// SkeletonGeneratedClass is preferred (it may be more up-to-date if the Blueprint
	// has not been fully compiled yet), with fallback to GeneratedClass.
	FMulticastDelegateProperty* DelegateProp = nullptr;
	UClass* OwnerClass = nullptr;

	// Try SkeletonGeneratedClass first
	if (Blueprint->SkeletonGeneratedClass)
	{
		for (TFieldIterator<FMulticastDelegateProperty> It(Blueprint->SkeletonGeneratedClass); It; ++It)
		{
			if (It->GetFName() == DelegateFName)
			{
				DelegateProp = *It;
				OwnerClass = Blueprint->SkeletonGeneratedClass;
				break;
			}
		}
	}

	// Fallback to GeneratedClass
	if (!DelegateProp && Blueprint->GeneratedClass)
	{
		for (TFieldIterator<FMulticastDelegateProperty> It(Blueprint->GeneratedClass); It; ++It)
		{
			if (It->GetFName() == DelegateFName)
			{
				DelegateProp = *It;
				OwnerClass = Blueprint->GeneratedClass;
				break;
			}
		}
	}

	if (!DelegateProp)
	{
		// Build a list of available dispatchers for the error message
		TArray<FString> AvailableDispatchers;
		UClass* SearchClass = Blueprint->SkeletonGeneratedClass
			? Blueprint->SkeletonGeneratedClass
			: Blueprint->GeneratedClass;
		if (SearchClass)
		{
			for (TFieldIterator<FMulticastDelegateProperty> It(SearchClass); It; ++It)
			{
				AvailableDispatchers.Add(It->GetName());
			}
		}

		if (AvailableDispatchers.Num() > 0)
		{
			LastError = FString::Printf(
				TEXT("Event dispatcher '%s' not found on Blueprint '%s'. "
					 "Available dispatchers: %s"),
				**DelegateNamePtr, *Blueprint->GetName(),
				*FString::Join(AvailableDispatchers, TEXT(", ")));
		}
		else
		{
			LastError = FString::Printf(
				TEXT("Event dispatcher '%s' not found on Blueprint '%s'. "
					 "This Blueprint has no event dispatchers. "
					 "Use blueprint.add_function with function_type='event_dispatcher' to create one first."),
				**DelegateNamePtr, *Blueprint->GetName());
		}
		return nullptr;
	}

	UE_LOG(LogOliveNodeFactory, Log,
		TEXT("CreateBindDelegateNode: Found delegate '%s' on class '%s'"),
		**DelegateNamePtr, *OwnerClass->GetName());

	// Create the AddDelegate (bind event) node
	UK2Node_AddDelegate* BindDelegateNode = NewObject<UK2Node_AddDelegate>(Graph);
	BindDelegateNode->SetFromProperty(DelegateProp, /*bSelfContext=*/true, OwnerClass);
	BindDelegateNode->AllocateDefaultPins();
	Graph->AddNode(BindDelegateNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

	return BindDelegateNode;
}

UK2Node* FOliveNodeFactory::CreateEnhancedInputActionNode(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const TMap<FString, FString>& Properties)
{
	const FString* ActionNamePtr = Properties.Find(TEXT("input_action_name"));
	if (!ActionNamePtr || ActionNamePtr->IsEmpty())
	{
		LastError = TEXT("EnhancedInputAction node requires 'input_action_name' property "
			"(e.g., \"IA_Interact\", \"IA_Jump\", \"IA_Fire\")");
		return nullptr;
	}

	const FString& InputActionName = *ActionNamePtr;

	// Try to find the UInputAction asset via asset registry search.
	// Input Actions are UDataAsset subclasses stored anywhere in the project.
	UInputAction* FoundAction = nullptr;

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
		TEXT("AssetRegistry")).Get();

	TArray<FAssetData> AssetResults;
	AssetRegistry.GetAssetsByClass(UInputAction::StaticClass()->GetClassPathName(), AssetResults);

	// Strategy 1: exact name match
	for (const FAssetData& Asset : AssetResults)
	{
		if (Asset.AssetName.ToString().Equals(InputActionName, ESearchCase::IgnoreCase))
		{
			FoundAction = Cast<UInputAction>(Asset.GetAsset());
			if (FoundAction)
			{
				break;
			}
		}
	}

	// Strategy 2: if the name has IA_ prefix, also try without it (and vice versa)
	if (!FoundAction)
	{
		FString AlternateName;
		if (InputActionName.StartsWith(TEXT("IA_")))
		{
			AlternateName = InputActionName.Mid(3); // Strip IA_ prefix
		}
		else
		{
			AlternateName = FString::Printf(TEXT("IA_%s"), *InputActionName);
		}

		for (const FAssetData& Asset : AssetResults)
		{
			if (Asset.AssetName.ToString().Equals(AlternateName, ESearchCase::IgnoreCase))
			{
				FoundAction = Cast<UInputAction>(Asset.GetAsset());
				if (FoundAction)
				{
					break;
				}
			}
		}
	}

	// Strategy 3: direct LoadObject with common project paths
	if (!FoundAction)
	{
		const TArray<FString> SearchPaths = {
			FString::Printf(TEXT("/Game/Input/Actions/%s.%s"), *InputActionName, *InputActionName),
			FString::Printf(TEXT("/Game/Input/%s.%s"), *InputActionName, *InputActionName),
			FString::Printf(TEXT("/Game/%s.%s"), *InputActionName, *InputActionName),
		};

		for (const FString& Path : SearchPaths)
		{
			FoundAction = LoadObject<UInputAction>(nullptr, *Path);
			if (FoundAction)
			{
				break;
			}
		}
	}

	if (!FoundAction)
	{
		// Build a helpful error with available Input Actions
		TArray<FString> AvailableActions;
		for (const FAssetData& Asset : AssetResults)
		{
			AvailableActions.Add(Asset.AssetName.ToString());
		}

		if (AvailableActions.Num() > 0)
		{
			AvailableActions.Sort();
			// Cap the list at 20 entries to avoid huge error messages
			const int32 MaxToShow = FMath::Min(AvailableActions.Num(), 20);
			TArray<FString> DisplayActions(AvailableActions.GetData(), MaxToShow);
			FString AvailableStr = FString::Join(DisplayActions, TEXT(", "));
			if (AvailableActions.Num() > MaxToShow)
			{
				AvailableStr += FString::Printf(TEXT(" ... and %d more"), AvailableActions.Num() - MaxToShow);
			}

			LastError = FString::Printf(
				TEXT("Enhanced Input Action '%s' not found. "
					 "Available Input Actions in project: [%s]. "
					 "Ensure the Input Action asset exists, or use project.search "
					 "with type filter 'InputAction' to locate it."),
				*InputActionName, *AvailableStr);
		}
		else
		{
			LastError = FString::Printf(
				TEXT("Enhanced Input Action '%s' not found, and no Input Action assets "
					 "exist in this project. Create an Input Action asset first "
					 "(Content Browser > Right-click > Input > Input Action), "
					 "then reference it in a plan step."),
				*InputActionName);
		}
		return nullptr;
	}

	// Check for duplicate: only one UK2Node_EnhancedInputAction per InputAction per graph
	for (UEdGraphNode* ExistingNode : Graph->Nodes)
	{
		if (UK2Node_EnhancedInputAction* ExistingIA = Cast<UK2Node_EnhancedInputAction>(ExistingNode))
		{
			if (ExistingIA->InputAction == FoundAction)
			{
				UE_LOG(LogOliveNodeFactory, Warning,
					TEXT("CreateEnhancedInputActionNode: Enhanced Input Action '%s' "
						 "already exists in graph at (%d, %d)"),
					*InputActionName, ExistingIA->NodePosX, ExistingIA->NodePosY);
				LastError = FString::Printf(
					TEXT("Enhanced Input Action '%s' already exists in this graph "
						 "(node at %d, %d). Each Input Action event can only appear once per graph."),
					*InputActionName, ExistingIA->NodePosX, ExistingIA->NodePosY);
				return nullptr;
			}
		}
	}

	// Create the Enhanced Input Action node
	UK2Node_EnhancedInputAction* InputNode = NewObject<UK2Node_EnhancedInputAction>(Graph);
	InputNode->InputAction = FoundAction;
	InputNode->AllocateDefaultPins();
	Graph->AddNode(InputNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

	UE_LOG(LogOliveNodeFactory, Log,
		TEXT("CreateEnhancedInputActionNode: Created node for '%s' (asset: %s) with %d pins"),
		*InputActionName, *FoundAction->GetPathName(), InputNode->Pins.Num());

	return InputNode;
}

// ============================================================================
// Universal Node Creation
// ============================================================================

UClass* FOliveNodeFactory::FindK2NodeClass(const FString& ClassName) const
{
	// Strategy 1: Direct FindFirstObject (handles fully-qualified and short names)
	UClass* Found = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
	if (Found && Found->IsChildOf(UK2Node::StaticClass()))
	{
		return Found;
	}

	// Strategy 2: Try with "K2Node_" and "UK2Node_" prefix variants
	// The AI might pass "ComponentBoundEvent" without the K2Node_ prefix
	if (!ClassName.StartsWith(TEXT("K2Node_")) && !ClassName.StartsWith(TEXT("UK2Node_")))
	{
		// Try K2Node_X
		FString Prefixed = TEXT("K2Node_") + ClassName;
		Found = FindFirstObject<UClass>(*Prefixed, EFindFirstObjectOptions::NativeFirst);
		if (Found && Found->IsChildOf(UK2Node::StaticClass()))
		{
			return Found;
		}
		// Try UK2Node_X
		Prefixed = TEXT("UK2Node_") + ClassName;
		Found = FindFirstObject<UClass>(*Prefixed, EFindFirstObjectOptions::NativeFirst);
		if (Found && Found->IsChildOf(UK2Node::StaticClass()))
		{
			return Found;
		}
	}

	// Strategy 3: Strip "U" prefix if present (AI might pass "UK2Node_Timeline")
	if (ClassName.StartsWith(TEXT("U")))
	{
		FString Stripped = ClassName.Mid(1);
		Found = FindFirstObject<UClass>(*Stripped, EFindFirstObjectOptions::NativeFirst);
		if (Found && Found->IsChildOf(UK2Node::StaticClass()))
		{
			return Found;
		}
	}

	// Strategy 4: Try common engine packages explicitly via StaticLoadClass.
	// K2Nodes live in multiple modules. FindFirstObject searches loaded modules,
	// but some may not be loaded yet. StaticLoadClass forces loading.
	static const TCHAR* PackagePaths[] = {
		TEXT("/Script/BlueprintGraph"),
		TEXT("/Script/UnrealEd"),
		TEXT("/Script/AnimGraph"),
		TEXT("/Script/GameplayAbilitiesEditor"),
		TEXT("/Script/EnhancedInput"),
		TEXT("/Script/AIModule"),
	};

	// Build the class name to search for (strip U prefix for StaticLoadClass)
	FString SearchName = ClassName;
	if (SearchName.StartsWith(TEXT("U")))
	{
		SearchName = SearchName.Mid(1);
	}

	for (const TCHAR* Package : PackagePaths)
	{
		FString FullPath = FString::Printf(TEXT("%s.%s"), Package, *SearchName);
		Found = StaticLoadClass(UK2Node::StaticClass(), nullptr, *FullPath, nullptr, LOAD_Quiet);
		if (Found)
		{
			return Found;
		}
	}

	return nullptr;
}

int32 FOliveNodeFactory::SetNodePropertiesViaReflection(
	UEdGraphNode* Node,
	const TMap<FString, FString>& Properties,
	TArray<FString>& OutSetProperties,
	TMap<FString, FString>& OutSkippedProperties)
{
	int32 SetCount = 0;

	for (const auto& Pair : Properties)
	{
		const FString& PropName = Pair.Key;
		const FString& PropValue = Pair.Value;

		FProperty* Property = Node->GetClass()->FindPropertyByName(FName(*PropName));
		if (!Property)
		{
			OutSkippedProperties.Add(PropName, TEXT("Property not found on node class"));
			UE_LOG(LogOliveNodeFactory, Warning,
				TEXT("SetNodePropertiesViaReflection: Property '%s' not found on %s"),
				*PropName, *Node->GetClass()->GetName());
			continue;
		}

		// Note: We do NOT block on CPF_Edit -- many K2Node properties are set
		// programmatically and don't have CPF_Edit. ImportText will work regardless.
		if (!Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible | CPF_Config))
		{
			UE_LOG(LogOliveNodeFactory, Verbose,
				TEXT("SetNodePropertiesViaReflection: Property '%s' is not CPF_Edit, "
					 "attempting ImportText anyway"), *PropName);
		}

		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Node);

		// Type-specific fast paths (same pattern as OliveGraphWriter::SetNodeProperty)
		bool bSet = false;
		if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
		{
			BoolProp->SetPropertyValue(ValuePtr, PropValue.ToBool());
			bSet = true;
		}
		else if (FIntProperty* IntProp = CastField<FIntProperty>(Property))
		{
			IntProp->SetPropertyValue(ValuePtr, FCString::Atoi(*PropValue));
			bSet = true;
		}
		else if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Property))
		{
			FloatProp->SetPropertyValue(ValuePtr, FCString::Atof(*PropValue));
			bSet = true;
		}
		else if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Property))
		{
			DoubleProp->SetPropertyValue(ValuePtr, FCString::Atod(*PropValue));
			bSet = true;
		}
		else if (FStrProperty* StrProp = CastField<FStrProperty>(Property))
		{
			StrProp->SetPropertyValue(ValuePtr, PropValue);
			bSet = true;
		}
		else if (FNameProperty* NameProp = CastField<FNameProperty>(Property))
		{
			NameProp->SetPropertyValue(ValuePtr, FName(*PropValue));
			bSet = true;
		}
		else if (FTextProperty* TextProp = CastField<FTextProperty>(Property))
		{
			TextProp->SetPropertyValue(ValuePtr, FText::FromString(PropValue));
			bSet = true;
		}
		else if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Property))
		{
			// Try to load the object from a path
			UObject* Obj = StaticLoadObject(ObjProp->PropertyClass, nullptr, *PropValue);
			if (Obj)
			{
				ObjProp->SetObjectPropertyValue(ValuePtr, Obj);
				bSet = true;
			}
			else
			{
				// Try FindFirstObject for already-loaded objects
				Obj = FindFirstObject<UObject>(*PropValue, EFindFirstObjectOptions::NativeFirst);
				if (Obj && Obj->IsA(ObjProp->PropertyClass))
				{
					ObjProp->SetObjectPropertyValue(ValuePtr, Obj);
					bSet = true;
				}
				else
				{
					OutSkippedProperties.Add(PropName,
						FString::Printf(TEXT("Could not resolve object '%s' for property type '%s'"),
							*PropValue, *ObjProp->PropertyClass->GetName()));
					continue;
				}
			}
		}
		else
		{
			// Generic text import -- handles FKey, enums, structs, etc.
			const TCHAR* ImportResult = Property->ImportText_Direct(*PropValue, ValuePtr, Node, PPF_None);
			bSet = (ImportResult != nullptr);
			if (!bSet)
			{
				OutSkippedProperties.Add(PropName,
					FString::Printf(TEXT("ImportText failed for value '%s'"), *PropValue));
				continue;
			}
		}

		if (bSet)
		{
			OutSetProperties.Add(PropName);
			SetCount++;
			UE_LOG(LogOliveNodeFactory, Verbose,
				TEXT("SetNodePropertiesViaReflection: Set '%s' = '%s'"),
				*PropName, *PropValue);
		}
	}

	return SetCount;
}

UEdGraphNode* FOliveNodeFactory::CreateNodeByClass(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const FString& ClassName,
	const TMap<FString, FString>& Properties)
{
	LastError.Empty();

	// Validate inputs
	if (!Blueprint || !Graph)
	{
		LastError = TEXT("Blueprint or Graph is null");
		return nullptr;
	}

	// Find the K2Node class
	UClass* NodeClass = FindK2NodeClass(ClassName);
	if (!NodeClass)
	{
		LastError = FString::Printf(
			TEXT("Could not find UK2Node subclass '%s'. "
				 "Ensure the class name is correct (e.g., 'K2Node_ComponentBoundEvent', "
				 "'K2Node_Timeline'). The class must be a subclass of UK2Node."),
			*ClassName);
		return nullptr;
	}

	UE_LOG(LogOliveNodeFactory, Log,
		TEXT("CreateNodeByClass: Resolved '%s' -> %s (module: %s)"),
		*ClassName, *NodeClass->GetName(), *NodeClass->GetOuterUPackage()->GetName());

	// Create the node
	UEdGraphNode* NewNode = NewObject<UEdGraphNode>(Graph, NodeClass);
	if (!NewNode)
	{
		LastError = FString::Printf(
			TEXT("NewObject failed for class '%s'"), *NodeClass->GetName());
		return nullptr;
	}

	// Set properties BEFORE AllocateDefaultPins.
	// Many K2Node subclasses generate pins based on property values
	// (e.g., K2Node_ComponentBoundEvent needs DelegatePropertyName set
	// before pin allocation to know which delegate signature to expose).
	TArray<FString> SetProps;
	TMap<FString, FString> SkippedProps;
	const int32 PropsSet = SetNodePropertiesViaReflection(
		NewNode, Properties, SetProps, SkippedProps);

	UE_LOG(LogOliveNodeFactory, Log,
		TEXT("CreateNodeByClass: Set %d/%d properties on %s"),
		PropsSet, Properties.Num(), *NodeClass->GetName());

	// Create GUID for the node
	NewNode->CreateNewGuid();

	// Allocate pins based on current property state
	NewNode->AllocateDefaultPins();

	// Add to graph BEFORE ReconstructNode -- some nodes need graph context
	Graph->AddNode(NewNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

	// Reconstruct to finalize pin layout.
	// This is mandatory defense-in-depth -- many K2Nodes only produce correct
	// pins after reconstruction (which may read properties, resolve references, etc.).
	NewNode->ReconstructNode();

	UE_LOG(LogOliveNodeFactory, Log,
		TEXT("CreateNodeByClass: Successfully created %s with %d pins, "
			 "%d properties set, %d skipped"),
		*NodeClass->GetName(),
		NewNode->Pins.Num(), PropsSet, SkippedProps.Num());

	return NewNode;
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

UFunction* FOliveNodeFactory::FindFunction(const FString& FunctionName, const FString& ClassName, UBlueprint* Blueprint, EOliveFunctionMatchMethod* OutMatchMethod)
{
	// Initialize out-param
	if (OutMatchMethod)
	{
		*OutMatchMethod = EOliveFunctionMatchMethod::None;
	}

	// --- Step 0: Alias lookup ---
	// Before any class search, check if the function name is a known alias.
	// If so, replace with the canonical name for all subsequent searches.
	FString ResolvedName = FunctionName;
	bool bUsedAlias = false;
	{
		const TMap<FString, FString>& Aliases = GetAliasMap();
		FString LowerName = FunctionName.ToLower();
		for (const auto& Pair : Aliases)
		{
			if (Pair.Key.ToLower() == LowerName)
			{
				ResolvedName = Pair.Value;
				bUsedAlias = true;
				UE_LOG(LogOliveNodeFactory, Log, TEXT("FindFunction('%s'): alias resolved -> '%s'"),
					*FunctionName, *ResolvedName);
				break;
			}
		}
	}

	// Lambda: try exact name then K2 prefix variant on a single class
	auto TryClassWithK2 = [&ResolvedName](UClass* Class) -> UFunction*
	{
		if (!Class)
		{
			return nullptr;
		}

		// Exact match
		UFunction* Func = Class->FindFunctionByName(FName(*ResolvedName));
		if (Func)
		{
			return Func;
		}

		// K2 prefix fallback
		if (!ResolvedName.StartsWith(TEXT("K2_")))
		{
			// Try adding K2_ prefix
			Func = Class->FindFunctionByName(FName(*(TEXT("K2_") + ResolvedName)));
		}
		else
		{
			// Try removing K2_ prefix
			Func = Class->FindFunctionByName(FName(*ResolvedName.Mid(3)));
		}
		return Func;
	};

	// Helper: report match method and return
	auto ReportMatch = [&OutMatchMethod, &bUsedAlias](EOliveFunctionMatchMethod Method) {
		if (OutMatchMethod)
		{
			// If the alias map was used, report AliasMap; otherwise report the actual search stage
			*OutMatchMethod = bUsedAlias ? EOliveFunctionMatchMethod::AliasMap : Method;
		}
	};

	// --- Step 1: Search specified ClassName ---
	if (!ClassName.IsEmpty())
	{
		UE_LOG(LogOliveNodeFactory, Verbose, TEXT("FindFunction('%s', class='%s'): searching specified class"), *ResolvedName, *ClassName);
		UClass* Class = FindClass(ClassName);
		if (Class)
		{
			UFunction* Func = TryClassWithK2(Class);
			if (Func)
			{
				UE_LOG(LogOliveNodeFactory, Verbose, TEXT("FindFunction('%s'): found in specified class '%s'"), *ResolvedName, *Class->GetName());
				ReportMatch(EOliveFunctionMatchMethod::ExactName);
				return Func;
			}
		}
	}

	// --- Step 2: Search Blueprint's GeneratedClass ---
	if (Blueprint && Blueprint->GeneratedClass)
	{
		UE_LOG(LogOliveNodeFactory, Verbose, TEXT("FindFunction('%s'): trying Blueprint GeneratedClass '%s'"),
			*ResolvedName, *Blueprint->GeneratedClass->GetName());
		UFunction* Func = TryClassWithK2(Blueprint->GeneratedClass);
		if (Func)
		{
			UE_LOG(LogOliveNodeFactory, Verbose, TEXT("FindFunction('%s'): found in Blueprint GeneratedClass '%s'"),
				*ResolvedName, *Blueprint->GeneratedClass->GetName());
			ReportMatch(EOliveFunctionMatchMethod::GeneratedClass);
			return Func;
		}
	}

	// --- Step 2b: Search Blueprint's own FunctionGraphs (catches uncompiled functions) ---
	// A user-defined function exists as a UEdGraph in FunctionGraphs even before
	// full compilation.  When GeneratedClass doesn't yet contain the UFunction
	// (e.g., after creation but before compile), we fall back to
	// SkeletonGeneratedClass which is updated incrementally by the editor.
	if (Blueprint)
	{
		for (UEdGraph* FuncGraph : Blueprint->FunctionGraphs)
		{
			if (FuncGraph && FuncGraph->GetFName() == FName(*ResolvedName))
			{
				// SkeletonGeneratedClass is updated before a full compile and
				// typically has the UFunction stub for user-defined functions.
				UFunction* FoundFunction = nullptr;
				if (Blueprint->SkeletonGeneratedClass)
				{
					FoundFunction = Blueprint->SkeletonGeneratedClass->FindFunctionByName(
						FName(*ResolvedName));
				}
				if (!FoundFunction && Blueprint->GeneratedClass)
				{
					FoundFunction = Blueprint->GeneratedClass->FindFunctionByName(
						FName(*ResolvedName));
				}
				if (FoundFunction)
				{
					UE_LOG(LogOliveNodeFactory, Log,
						TEXT("FindFunction('%s'): found via Blueprint FunctionGraphs + SkeletonGeneratedClass"),
						*ResolvedName);
					ReportMatch(EOliveFunctionMatchMethod::FunctionGraph);
					return FoundFunction;
				}
				else
				{
					UE_LOG(LogOliveNodeFactory, Log,
						TEXT("FindFunction: Found function graph '%s' but no UFunction. "
							 "Blueprint may need compilation."),
						*ResolvedName);
				}
			}
		}
	}

	// --- Step 3: Search Blueprint parent class hierarchy ---
	if (Blueprint && Blueprint->ParentClass)
	{
		TSet<UClass*> Visited;
		// Skip GeneratedClass if already searched above
		if (Blueprint->GeneratedClass)
		{
			Visited.Add(Blueprint->GeneratedClass);
		}

		UClass* Current = Blueprint->ParentClass;
		while (Current)
		{
			if (!Visited.Contains(Current))
			{
				Visited.Add(Current);
				UE_LOG(LogOliveNodeFactory, Verbose, TEXT("FindFunction('%s'): trying parent class '%s'"),
					*ResolvedName, *Current->GetName());
				UFunction* Func = TryClassWithK2(Current);
				if (Func)
				{
					UE_LOG(LogOliveNodeFactory, Verbose, TEXT("FindFunction('%s'): found in parent class '%s'"),
						*ResolvedName, *Current->GetName());
					ReportMatch(EOliveFunctionMatchMethod::ParentClassSearch);
					return Func;
				}
			}
			Current = Current->GetSuperClass();
		}
	}

	// --- Step 4: Search Blueprint SCS component classes ---
	if (Blueprint && Blueprint->SimpleConstructionScript)
	{
		TArray<USCS_Node*> AllSCSNodes = Blueprint->SimpleConstructionScript->GetAllNodes();
		for (USCS_Node* SCSNode : AllSCSNodes)
		{
			if (SCSNode && SCSNode->ComponentClass)
			{
				UE_LOG(LogOliveNodeFactory, Verbose, TEXT("FindFunction('%s'): trying SCS component class '%s'"),
					*ResolvedName, *SCSNode->ComponentClass->GetName());
				UFunction* Func = TryClassWithK2(SCSNode->ComponentClass);
				if (Func)
				{
					UE_LOG(LogOliveNodeFactory, Verbose, TEXT("FindFunction('%s'): found on SCS component class '%s'"),
						*ResolvedName, *SCSNode->ComponentClass->GetName());
					ReportMatch(EOliveFunctionMatchMethod::ComponentClassSearch);
					return Func;
				}
			}
		}
	}

	// --- Step 4b: Search Blueprint implemented interfaces ---
	// Interface functions live on UInterface subclasses, which are not in the
	// parent class hierarchy or SCS components. When a Blueprint implements an
	// interface, we need to search each interface's function list.
	// Functions matched here require UK2Node_Message (interface message call)
	// instead of UK2Node_CallFunction at node creation time.
	if (Blueprint)
	{
		for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
		{
			if (InterfaceDesc.Interface)
			{
				UE_LOG(LogOliveNodeFactory, Verbose, TEXT("FindFunction('%s'): trying implemented interface '%s'"),
					*ResolvedName, *InterfaceDesc.Interface->GetName());
				UFunction* Func = TryClassWithK2(InterfaceDesc.Interface);
				if (Func)
				{
					UE_LOG(LogOliveNodeFactory, Log, TEXT("FindFunction('%s'): found on implemented interface '%s'"),
						*ResolvedName, *InterfaceDesc.Interface->GetName());
					if (OutMatchMethod)
					{
						// Always report InterfaceSearch for interface matches,
						// even if an alias was used -- the node type depends on this
						*OutMatchMethod = EOliveFunctionMatchMethod::InterfaceSearch;
					}
					return Func;
				}
			}
		}
	}

	// --- Step 5: Search common library classes ---
	TArray<UClass*> LibraryClasses = {
		UKismetSystemLibrary::StaticClass(),
		UKismetMathLibrary::StaticClass(),
		UKismetStringLibrary::StaticClass(),
		UKismetArrayLibrary::StaticClass(),
		UGameplayStatics::StaticClass(),
		UObject::StaticClass(),
		AActor::StaticClass(),
		USceneComponent::StaticClass(),
		UPrimitiveComponent::StaticClass(),
		APawn::StaticClass(),
		ACharacter::StaticClass(),
	};

	for (UClass* LibClass : LibraryClasses)
	{
		UE_LOG(LogOliveNodeFactory, Verbose, TEXT("FindFunction('%s'): trying library class '%s'"), *ResolvedName, *LibClass->GetName());
		UFunction* Func = TryClassWithK2(LibClass);
		if (Func)
		{
			UE_LOG(LogOliveNodeFactory, Verbose, TEXT("FindFunction('%s'): found in library class '%s'"), *ResolvedName, *LibClass->GetName());
			ReportMatch(EOliveFunctionMatchMethod::LibrarySearch);
			return Func;
		}
	}

	UE_LOG(LogOliveNodeFactory, Warning,
		TEXT("FindFunction('%s' [resolved='%s'], class='%s'): FAILED -- searched specified class + Blueprint GeneratedClass + "
			 "Blueprint FunctionGraphs + parent class hierarchy + SCS component classes + "
			 "implemented interfaces + "
			 "library classes [KismetSystemLibrary, KismetMathLibrary, KismetStringLibrary, KismetArrayLibrary, "
			 "GameplayStatics, Object, Actor, SceneComponent, PrimitiveComponent, Pawn, Character]"),
		*FunctionName, *ResolvedName, *ClassName);
	return nullptr;
}

// ============================================================================
// FindFunctionEx
// ============================================================================

FOliveFunctionSearchResult FOliveNodeFactory::FindFunctionEx(
	const FString& FunctionName,
	const FString& ClassName,
	UBlueprint* Blueprint)
{
	FOliveFunctionSearchResult Result;

	// Delegate to existing FindFunction for the actual search
	Result.Function = FindFunction(FunctionName, ClassName, Blueprint, &Result.MatchMethod);

	// If found, populate MatchedClassName and return early -- no search history needed
	if (Result.Function)
	{
		if (UClass* OuterClass = Result.Function->GetOuterUClass())
		{
			Result.MatchedClassName = OuterClass->GetName();
		}
		return Result;
	}

	// --- Build search history for error reporting ---
	// Mirrors the search order of FindFunction above so the AI knows exactly
	// where we looked and can adjust its plan accordingly.

	// Resolve alias name (same as FindFunction Step 0)
	FString ResolvedName = FunctionName;
	{
		const TMap<FString, FString>& Aliases = GetAliasMap();
		FString LowerName = FunctionName.ToLower();
		for (const auto& Pair : Aliases)
		{
			if (Pair.Key.ToLower() == LowerName)
			{
				ResolvedName = Pair.Value;
				break;
			}
		}
	}

	// Step 0: Alias map
	Result.SearchedLocations.Add(FString::Printf(TEXT("alias map (%d entries)"), GetAliasMap().Num()));

	// Step 1: Specified class
	if (!ClassName.IsEmpty())
	{
		Result.SearchedLocations.Add(FString::Printf(TEXT("specified class '%s'"), *ClassName));
	}

	// Steps 2-6: Blueprint-dependent searches
	if (Blueprint)
	{
		// Step 2: GeneratedClass
		if (Blueprint->GeneratedClass)
		{
			Result.SearchedLocations.Add(FString::Printf(TEXT("Blueprint class '%s'"),
				*Blueprint->GeneratedClass->GetName()));
		}

		// Step 2b: FunctionGraphs
		Result.SearchedLocations.Add(FString::Printf(TEXT("function graphs (%d graphs)"),
			Blueprint->FunctionGraphs.Num()));

		// Step 3: Parent hierarchy
		if (Blueprint->ParentClass)
		{
			FString ParentChain;
			UClass* WalkClass = Blueprint->ParentClass;
			while (WalkClass)
			{
				if (!ParentChain.IsEmpty())
				{
					ParentChain += TEXT(" -> ");
				}
				ParentChain += WalkClass->GetName();
				if (WalkClass == UObject::StaticClass())
				{
					break;
				}
				WalkClass = WalkClass->GetSuperClass();
			}
			if (!ParentChain.IsEmpty())
			{
				Result.SearchedLocations.Add(FString::Printf(TEXT("parent hierarchy (%s)"), *ParentChain));
			}
		}

		// Step 4: SCS components
		if (Blueprint->SimpleConstructionScript)
		{
			TArray<USCS_Node*> AllNodes = Blueprint->SimpleConstructionScript->GetAllNodes();
			if (AllNodes.Num() > 0)
			{
				FString CompNames;
				const int32 MaxToShow = 5;
				for (int32 i = 0; i < FMath::Min(AllNodes.Num(), MaxToShow); i++)
				{
					if (AllNodes[i] && AllNodes[i]->ComponentClass)
					{
						if (!CompNames.IsEmpty())
						{
							CompNames += TEXT(", ");
						}
						CompNames += AllNodes[i]->ComponentClass->GetName();
					}
				}
				if (AllNodes.Num() > MaxToShow)
				{
					CompNames += TEXT(", ...");
				}
				Result.SearchedLocations.Add(FString::Printf(TEXT("SCS components (%s)"), *CompNames));
			}
		}

		// Step 4b: Implemented interfaces
		if (Blueprint->ImplementedInterfaces.Num() > 0)
		{
			FString InterfaceNames;
			for (const FBPInterfaceDescription& Desc : Blueprint->ImplementedInterfaces)
			{
				if (Desc.Interface)
				{
					if (!InterfaceNames.IsEmpty())
					{
						InterfaceNames += TEXT(", ");
					}
					InterfaceNames += Desc.Interface->GetName();
				}
			}
			if (!InterfaceNames.IsEmpty())
			{
				Result.SearchedLocations.Add(FString::Printf(TEXT("interfaces (%s)"), *InterfaceNames));
			}
		}
	}

	// Step 5: Library classes (static list, summarize)
	Result.SearchedLocations.Add(
		TEXT("library classes (KismetSystemLibrary, KismetMathLibrary, KismetStringLibrary, ")
		TEXT("KismetArrayLibrary, GameplayStatics, Object, Actor, SceneComponent, ")
		TEXT("PrimitiveComponent, Pawn, Character)"));

	return Result;
}

// ============================================================================
// GetAliasMap
// ============================================================================

const TMap<FString, FString>& FOliveNodeFactory::GetAliasMap()
{
	static const TMap<FString, FString> Aliases = []()
	{
		TMap<FString, FString> Map;

		// ================================================================
		// Transform / Location / Rotation (AActor, K2_ prefixed)
		// ================================================================
		// AActor::K2_GetActorLocation, K2_SetActorLocation, etc.
		Map.Add(TEXT("GetLocation"), TEXT("K2_GetActorLocation"));
		Map.Add(TEXT("SetLocation"), TEXT("K2_SetActorLocation"));
		Map.Add(TEXT("GetActorLocation"), TEXT("K2_GetActorLocation"));
		Map.Add(TEXT("SetActorLocation"), TEXT("K2_SetActorLocation"));
		Map.Add(TEXT("GetRotation"), TEXT("K2_GetActorRotation"));
		Map.Add(TEXT("SetRotation"), TEXT("K2_SetActorRotation"));
		Map.Add(TEXT("GetActorRotation"), TEXT("K2_GetActorRotation"));
		Map.Add(TEXT("SetActorRotation"), TEXT("K2_SetActorRotation"));
		Map.Add(TEXT("SetLocationAndRotation"), TEXT("K2_SetActorLocationAndRotation"));
		Map.Add(TEXT("GetScale"), TEXT("GetActorScale3D"));
		Map.Add(TEXT("SetScale"), TEXT("SetActorScale3D"));
		Map.Add(TEXT("GetTransform"), TEXT("GetActorTransform"));
		Map.Add(TEXT("SetTransform"), TEXT("K2_SetActorTransform"));

		// Direction vectors (AActor -- NOT K2_ prefixed)
		Map.Add(TEXT("GetForwardVector"), TEXT("GetActorForwardVector"));
		Map.Add(TEXT("GetRightVector"), TEXT("GetActorRightVector"));
		Map.Add(TEXT("GetUpVector"), TEXT("GetActorUpVector"));

		// Component location (USceneComponent, K2_ prefixed)
		Map.Add(TEXT("GetWorldLocation"), TEXT("K2_GetComponentLocation"));
		Map.Add(TEXT("GetComponentLocation"), TEXT("K2_GetComponentLocation"));
		Map.Add(TEXT("SetWorldLocation"), TEXT("K2_SetWorldLocation"));
		Map.Add(TEXT("GetWorldRotation"), TEXT("K2_GetComponentRotation"));
		Map.Add(TEXT("GetComponentRotation"), TEXT("K2_GetComponentRotation"));
		Map.Add(TEXT("SetWorldRotation"), TEXT("K2_SetWorldRotation"));
		Map.Add(TEXT("GetComponentTransform"), TEXT("K2_GetComponentToWorld"));
		Map.Add(TEXT("GetWorldTransform"), TEXT("K2_GetComponentToWorld"));
		Map.Add(TEXT("SetRelativeLocation"), TEXT("K2_SetRelativeLocation"));
		Map.Add(TEXT("SetRelativeRotation"), TEXT("K2_SetRelativeRotation"));
		Map.Add(TEXT("AddWorldOffset"), TEXT("K2_AddWorldOffset"));
		Map.Add(TEXT("AddLocalOffset"), TEXT("K2_AddLocalOffset"));
		Map.Add(TEXT("AddWorldRotation"), TEXT("K2_AddWorldRotation"));
		Map.Add(TEXT("AddLocalRotation"), TEXT("K2_AddLocalRotation"));
		Map.Add(TEXT("SetWorldTransform"), TEXT("K2_SetWorldTransform"));
		Map.Add(TEXT("SetRelativeTransform"), TEXT("K2_SetRelativeTransform"));
		Map.Add(TEXT("DetachFromComponent"), TEXT("K2_DetachFromComponent"));

		// ================================================================
		// Actor Operations (AActor, K2_ prefixed or direct)
		// ================================================================
		Map.Add(TEXT("Destroy"), TEXT("K2_DestroyActor"));
		Map.Add(TEXT("DestroyActor"), TEXT("K2_DestroyActor"));
		Map.Add(TEXT("DestroyComponent"), TEXT("K2_DestroyComponent"));
		Map.Add(TEXT("AttachToActor"), TEXT("K2_AttachToActor"));
		Map.Add(TEXT("AttachToComponent"), TEXT("K2_AttachToComponent"));
		Map.Add(TEXT("DetachFromActor"), TEXT("K2_DetachFromActor"));
		Map.Add(TEXT("GetOwner"), TEXT("GetOwner"));
		Map.Add(TEXT("GetDistanceTo"), TEXT("GetDistanceTo"));
		Map.Add(TEXT("Distance"), TEXT("GetDistanceTo"));

		// ================================================================
		// Spawning (UGameplayStatics / K2Node_SpawnActorFromClass)
		// ================================================================
		// Note: SpawnActor uses a dedicated node type (OliveNodeTypes::SpawnActor),
		// but if the AI asks for it as a function call, point to the internal name
		Map.Add(TEXT("SpawnActor"), TEXT("BeginDeferredActorSpawnFromClass"));
		Map.Add(TEXT("SpawnActorFromClass"), TEXT("BeginDeferredActorSpawnFromClass"));

		// ================================================================
		// String Operations (UKismetStringLibrary)
		// ================================================================
		Map.Add(TEXT("Print"), TEXT("PrintString"));
		Map.Add(TEXT("PrintMessage"), TEXT("PrintString"));
		Map.Add(TEXT("Log"), TEXT("PrintString"));
		Map.Add(TEXT("LogMessage"), TEXT("PrintString"));
		Map.Add(TEXT("ToString"), TEXT("Conv_IntToString"));
		Map.Add(TEXT("IntToString"), TEXT("Conv_IntToString"));
		Map.Add(TEXT("FloatToString"), TEXT("Conv_FloatToString"));
		Map.Add(TEXT("BoolToString"), TEXT("Conv_BoolToString"));
		Map.Add(TEXT("Format"), TEXT("Format"));
		Map.Add(TEXT("Concatenate"), TEXT("Concat_StrStr"));
		Map.Add(TEXT("StringConcat"), TEXT("Concat_StrStr"));
		Map.Add(TEXT("StringAppend"), TEXT("Concat_StrStr"));
		Map.Add(TEXT("StringContains"), TEXT("Contains"));
		Map.Add(TEXT("StringLength"), TEXT("Len"));
		Map.Add(TEXT("Substring"), TEXT("GetSubstring"));

		// ================================================================
		// Math Operations (UKismetMathLibrary)
		// ================================================================
		Map.Add(TEXT("Add"), TEXT("Add_VectorVector"));
		Map.Add(TEXT("AddVectors"), TEXT("Add_VectorVector"));
		Map.Add(TEXT("Subtract"), TEXT("Subtract_VectorVector"));
		Map.Add(TEXT("SubtractVectors"), TEXT("Subtract_VectorVector"));
		Map.Add(TEXT("Multiply"), TEXT("Multiply_VectorFloat"));
		Map.Add(TEXT("MultiplyVector"), TEXT("Multiply_VectorFloat"));
		Map.Add(TEXT("Normalize"), TEXT("Normal"));
		Map.Add(TEXT("NormalizeVector"), TEXT("Normal"));
		Map.Add(TEXT("VectorLength"), TEXT("VSize"));
		Map.Add(TEXT("VectorSize"), TEXT("VSize"));
		Map.Add(TEXT("Lerp"), TEXT("Lerp"));
		Map.Add(TEXT("LinearInterpolate"), TEXT("Lerp"));
		Map.Add(TEXT("Clamp"), TEXT("FClamp"));
		Map.Add(TEXT("ClampFloat"), TEXT("FClamp"));
		Map.Add(TEXT("Abs"), TEXT("Abs"));
		Map.Add(TEXT("RandomFloat"), TEXT("RandomFloatInRange"));
		Map.Add(TEXT("RandomInt"), TEXT("RandomIntegerInRange"));
		Map.Add(TEXT("RandomInRange"), TEXT("RandomFloatInRange"));
		Map.Add(TEXT("Min"), TEXT("FMin"));
		Map.Add(TEXT("Max"), TEXT("FMax"));
		Map.Add(TEXT("Floor"), TEXT("FFloor"));
		Map.Add(TEXT("Ceil"), TEXT("FCeil"));
		Map.Add(TEXT("Round"), TEXT("Round"));
		Map.Add(TEXT("Sin"), TEXT("Sin"));
		Map.Add(TEXT("Cos"), TEXT("Cos"));
		Map.Add(TEXT("Tan"), TEXT("Tan"));
		Map.Add(TEXT("Atan2"), TEXT("Atan2"));
		Map.Add(TEXT("Sqrt"), TEXT("Sqrt"));
		Map.Add(TEXT("Power"), TEXT("MultiplyMultiply_FloatFloat"));
		Map.Add(TEXT("DotProduct"), TEXT("Dot_VectorVector"));
		Map.Add(TEXT("CrossProduct"), TEXT("Cross_VectorVector"));

		// ================================================================
		// Object / Validation (UKismetSystemLibrary, UObject)
		// ================================================================
		Map.Add(TEXT("IsValid"), TEXT("IsValid"));
		Map.Add(TEXT("IsValidObject"), TEXT("IsValid"));
		Map.Add(TEXT("GetClass"), TEXT("GetClass"));
		Map.Add(TEXT("GetName"), TEXT("GetObjectName"));
		Map.Add(TEXT("GetDisplayName"), TEXT("GetDisplayName"));

		// ================================================================
		// Component Access (AActor)
		// ================================================================
		Map.Add(TEXT("GetComponentByClass"), TEXT("GetComponentByClass"));
		Map.Add(TEXT("GetComponent"), TEXT("GetComponentByClass"));
		Map.Add(TEXT("AddComponent"), TEXT("AddComponent"));

		// ================================================================
		// Timer Operations (UKismetSystemLibrary, K2_ prefixed)
		// ================================================================
		// By function name (K2_SetTimer takes UObject* + FString FunctionName)
		Map.Add(TEXT("SetTimer"), TEXT("K2_SetTimer"));
		Map.Add(TEXT("SetTimerByFunctionName"), TEXT("K2_SetTimer"));
		Map.Add(TEXT("SetTimerByName"), TEXT("K2_SetTimer"));
		Map.Add(TEXT("ClearTimer"), TEXT("K2_ClearTimer"));
		Map.Add(TEXT("ClearTimerByFunctionName"), TEXT("K2_ClearTimer"));
		Map.Add(TEXT("PauseTimer"), TEXT("K2_PauseTimer"));
		Map.Add(TEXT("PauseTimerByFunctionName"), TEXT("K2_PauseTimer"));
		Map.Add(TEXT("UnPauseTimer"), TEXT("K2_UnPauseTimer"));
		Map.Add(TEXT("UnPauseTimerByFunctionName"), TEXT("K2_UnPauseTimer"));
		Map.Add(TEXT("IsTimerActive"), TEXT("K2_IsTimerActive"));
		Map.Add(TEXT("IsTimerActiveByFunctionName"), TEXT("K2_IsTimerActive"));
		Map.Add(TEXT("TimerExists"), TEXT("K2_TimerExists"));
		Map.Add(TEXT("TimerExistsByFunctionName"), TEXT("K2_TimerExists"));
		Map.Add(TEXT("GetTimerElapsedTime"), TEXT("K2_GetTimerElapsedTime"));
		Map.Add(TEXT("GetTimerElapsedTimeByFunctionName"), TEXT("K2_GetTimerElapsedTime"));
		Map.Add(TEXT("GetTimerRemainingTime"), TEXT("K2_GetTimerRemainingTime"));
		Map.Add(TEXT("GetTimerRemainingTimeByFunctionName"), TEXT("K2_GetTimerRemainingTime"));
		// By event/delegate (K2_SetTimerDelegate takes FTimerDynamicDelegate)
		Map.Add(TEXT("SetTimerByEvent"), TEXT("K2_SetTimerDelegate"));
		Map.Add(TEXT("ClearTimerByEvent"), TEXT("K2_ClearTimerDelegate"));
		Map.Add(TEXT("PauseTimerByEvent"), TEXT("K2_PauseTimerDelegate"));
		Map.Add(TEXT("UnPauseTimerByEvent"), TEXT("K2_UnPauseTimerDelegate"));
		Map.Add(TEXT("IsTimerActiveByEvent"), TEXT("K2_IsTimerActiveDelegate"));
		Map.Add(TEXT("TimerExistsByEvent"), TEXT("K2_TimerExistsDelegate"));

		// ================================================================
		// Physics (UPrimitiveComponent)
		// ================================================================
		Map.Add(TEXT("AddForce"), TEXT("AddForce"));
		Map.Add(TEXT("AddImpulse"), TEXT("AddImpulse"));
		Map.Add(TEXT("AddTorque"), TEXT("AddTorqueInRadians"));
		Map.Add(TEXT("SetSimulatePhysics"), TEXT("SetSimulatePhysics"));
		Map.Add(TEXT("EnablePhysics"), TEXT("SetSimulatePhysics"));
		Map.Add(TEXT("SetPhysicsLinearVelocity"), TEXT("SetPhysicsLinearVelocity"));
		Map.Add(TEXT("SetVelocity"), TEXT("SetPhysicsLinearVelocity"));

		// ================================================================
		// Collision / Trace (UKismetSystemLibrary)
		// ================================================================
		Map.Add(TEXT("LineTrace"), TEXT("LineTraceSingle"));
		Map.Add(TEXT("LineTraceSingle"), TEXT("LineTraceSingle"));
		Map.Add(TEXT("SphereTrace"), TEXT("SphereTraceSingle"));
		Map.Add(TEXT("SphereTraceSingle"), TEXT("SphereTraceSingle"));
		Map.Add(TEXT("SetCollisionEnabled"), TEXT("SetCollisionEnabled"));

		// ================================================================
		// Gameplay Statics (UGameplayStatics)
		// ================================================================
		Map.Add(TEXT("GetPlayerPawn"), TEXT("GetPlayerPawn"));
		Map.Add(TEXT("GetPlayerCharacter"), TEXT("GetPlayerCharacter"));
		Map.Add(TEXT("GetPlayerController"), TEXT("GetPlayerController"));
		Map.Add(TEXT("GetGameMode"), TEXT("GetGameMode"));
		Map.Add(TEXT("GetAllActorsOfClass"), TEXT("GetAllActorsOfClass"));
		Map.Add(TEXT("FindAllActors"), TEXT("GetAllActorsOfClass"));
		Map.Add(TEXT("OpenLevel"), TEXT("OpenLevel"));
		Map.Add(TEXT("LoadLevel"), TEXT("OpenLevel"));
		Map.Add(TEXT("PlaySound"), TEXT("PlaySoundAtLocation"));
		Map.Add(TEXT("PlaySoundAtLocation"), TEXT("PlaySoundAtLocation"));
		Map.Add(TEXT("SpawnEmitter"), TEXT("SpawnEmitterAtLocation"));
		Map.Add(TEXT("SpawnEmitterAtLocation"), TEXT("SpawnEmitterAtLocation"));
		Map.Add(TEXT("SpawnParticle"), TEXT("SpawnEmitterAtLocation"));
		Map.Add(TEXT("ApplyDamage"), TEXT("ApplyDamage"));

		// ================================================================
		// Input (various)
		// ================================================================
		Map.Add(TEXT("GetInputAxisValue"), TEXT("GetInputAxisValue"));
		Map.Add(TEXT("GetInputAxisKeyValue"), TEXT("GetInputAxisKeyValue"));
		Map.Add(TEXT("EnableInput"), TEXT("EnableInput"));
		Map.Add(TEXT("DisableInput"), TEXT("DisableInput"));

		// ================================================================
		// Array Operations (UKismetArrayLibrary)
		// ================================================================
		Map.Add(TEXT("ArrayAdd"), TEXT("Array_Add"));
		Map.Add(TEXT("ArrayRemove"), TEXT("Array_Remove"));
		Map.Add(TEXT("ArrayLength"), TEXT("Array_Length"));
		Map.Add(TEXT("ArrayContains"), TEXT("Array_Contains"));
		Map.Add(TEXT("ArrayGet"), TEXT("Array_Get"));
		Map.Add(TEXT("ArrayClear"), TEXT("Array_Clear"));
		Map.Add(TEXT("ArrayShuffle"), TEXT("Array_Shuffle"));

		// ================================================================
		// Delay / Flow (UKismetSystemLibrary)
		// ================================================================
		Map.Add(TEXT("Wait"), TEXT("Delay"));
		Map.Add(TEXT("Sleep"), TEXT("Delay"));

		// ================================================================
		// Transform Construction (UKismetMathLibrary)
		// ================================================================
		Map.Add(TEXT("MakeTransform"), TEXT("MakeTransform"));
		Map.Add(TEXT("BreakTransform"), TEXT("BreakTransform"));
		Map.Add(TEXT("MakeVector"), TEXT("MakeVector"));
		Map.Add(TEXT("BreakVector"), TEXT("BreakVector"));
		Map.Add(TEXT("MakeRotator"), TEXT("MakeRotator"));
		Map.Add(TEXT("BreakRotator"), TEXT("BreakRotator"));
		Map.Add(TEXT("MakeVector2D"), TEXT("MakeVector2D"));
		Map.Add(TEXT("BreakVector2D"), TEXT("BreakVector2D"));

		// ================================================================
		// Commonly Attempted Names
		// ================================================================
		Map.Add(TEXT("GetActorTransform"), TEXT("GetActorTransform"));
		Map.Add(TEXT("SetMaterial"), TEXT("SetMaterial"));
		Map.Add(TEXT("SetCollisionProfileName"), TEXT("SetCollisionProfileName"));
		Map.Add(TEXT("SetVisibility"), TEXT("SetVisibility"));
		Map.Add(TEXT("SetHiddenInGame"), TEXT("SetHiddenInGame"));

		// ================================================================
		// Movement
		// ================================================================
		Map.Add(TEXT("LaunchCharacter"), TEXT("LaunchCharacter"));
		Map.Add(TEXT("AddMovementInput"), TEXT("AddMovementInput"));
		Map.Add(TEXT("GetVelocity"), TEXT("GetVelocity"));

		// ================================================================
		// UE 5.5+ Float->Double Renames
		// ================================================================
		// In UE 5.5, many float math functions were renamed to use Double.
		// These aliases ensure plans written with old Float names still resolve.
		Map.Add(TEXT("Add_FloatFloat"), TEXT("Add_DoubleDouble"));
		Map.Add(TEXT("Subtract_FloatFloat"), TEXT("Subtract_DoubleDouble"));
		Map.Add(TEXT("Multiply_FloatFloat"), TEXT("Multiply_DoubleDouble"));
		Map.Add(TEXT("Divide_FloatFloat"), TEXT("Divide_DoubleDouble"));
		Map.Add(TEXT("Less_FloatFloat"), TEXT("Less_DoubleDouble"));
		Map.Add(TEXT("Greater_FloatFloat"), TEXT("Greater_DoubleDouble"));
		Map.Add(TEXT("LessEqual_FloatFloat"), TEXT("LessEqual_DoubleDouble"));
		Map.Add(TEXT("GreaterEqual_FloatFloat"), TEXT("GreaterEqual_DoubleDouble"));
		Map.Add(TEXT("EqualEqual_FloatFloat"), TEXT("EqualEqual_DoubleDouble"));
		Map.Add(TEXT("NotEqual_FloatFloat"), TEXT("NotEqual_DoubleDouble"));

		return Map;
	}();

	return Aliases;
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

UK2Node* FOliveNodeFactory::CreateMacroInstanceNode(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const FString& MacroName)
{
	// Load the StandardMacros library
	static const TCHAR* StandardMacrosPath =
		TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros");
	UBlueprint* MacroLib = LoadObject<UBlueprint>(nullptr, StandardMacrosPath);
	if (!MacroLib)
	{
		LastError = FString::Printf(
			TEXT("Failed to load StandardMacros library for macro '%s'"), *MacroName);
		return nullptr;
	}

	// Find the macro graph by name
	UEdGraph* MacroGraph = nullptr;
	for (UEdGraph* MG : MacroLib->MacroGraphs)
	{
		if (MG && MG->GetName() == MacroName)
		{
			MacroGraph = MG;
			break;
		}
	}

	if (!MacroGraph)
	{
		LastError = FString::Printf(
			TEXT("Macro '%s' not found in StandardMacros"), *MacroName);
		return nullptr;
	}

	// Create the macro instance node
	UK2Node_MacroInstance* MacroNode = NewObject<UK2Node_MacroInstance>(Graph);
	MacroNode->SetMacroGraph(MacroGraph);
	MacroNode->AllocateDefaultPins();
	Graph->AddNode(MacroNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
	return MacroNode;
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

	NodeCreators.Add(OliveNodeTypes::WhileLoop, [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
		return CreateMacroInstanceNode(BP, G, TEXT("WhileLoop"));
	});

	NodeCreators.Add(OliveNodeTypes::DoOnce, [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
		return CreateMacroInstanceNode(BP, G, TEXT("DoOnce"));
	});

	NodeCreators.Add(OliveNodeTypes::FlipFlop, [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
		return CreateMacroInstanceNode(BP, G, TEXT("FlipFlop"));
	});

	NodeCreators.Add(OliveNodeTypes::Gate, [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
		return CreateMacroInstanceNode(BP, G, TEXT("Gate"));
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

	NodeCreators.Add(OliveNodeTypes::EnhancedInputAction, [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
		return CreateEnhancedInputActionNode(BP, G, P);
	});

	// Delegate
	NodeCreators.Add(OliveNodeTypes::CallDelegate, [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
		return CreateCallDelegateNode(BP, G, P);
	});

	NodeCreators.Add(OliveNodeTypes::BindDelegate, [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
		return CreateBindDelegateNode(BP, G, P);
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

	// EnhancedInputAction
	TMap<FString, FString> EnhancedInputActionProps;
	EnhancedInputActionProps.Add(TEXT("input_action_name"), TEXT("Name of the UInputAction asset (required, e.g., \"IA_Interact\", \"IA_Jump\")"));
	RequiredPropertiesMap.Add(OliveNodeTypes::EnhancedInputAction, EnhancedInputActionProps);

	// Comment
	TMap<FString, FString> CommentProps;
	CommentProps.Add(TEXT("text"), TEXT("Comment text (optional)"));
	CommentProps.Add(TEXT("width"), TEXT("Comment width in pixels (optional, default 400)"));
	CommentProps.Add(TEXT("height"), TEXT("Comment height in pixels (optional, default 100)"));
	RequiredPropertiesMap.Add(OliveNodeTypes::Comment, CommentProps);

	// CallDelegate
	TMap<FString, FString> CallDelegateProps;
	CallDelegateProps.Add(TEXT("delegate_name"), TEXT("Name of the event dispatcher to broadcast (required)"));
	RequiredPropertiesMap.Add(OliveNodeTypes::CallDelegate, CallDelegateProps);

	// BindDelegate
	TMap<FString, FString> BindDelegateProps;
	BindDelegateProps.Add(TEXT("delegate_name"), TEXT("Name of the event dispatcher to bind to (required)"));
	RequiredPropertiesMap.Add(OliveNodeTypes::BindDelegate, BindDelegateProps);

	// Nodes with no required properties get empty maps
	RequiredPropertiesMap.Add(OliveNodeTypes::Branch, TMap<FString, FString>());
	RequiredPropertiesMap.Add(OliveNodeTypes::ForLoop, TMap<FString, FString>());
	RequiredPropertiesMap.Add(OliveNodeTypes::ForEachLoop, TMap<FString, FString>());
	RequiredPropertiesMap.Add(OliveNodeTypes::Delay, TMap<FString, FString>());
	RequiredPropertiesMap.Add(OliveNodeTypes::IsValid, TMap<FString, FString>());
	RequiredPropertiesMap.Add(OliveNodeTypes::PrintString, TMap<FString, FString>());
	RequiredPropertiesMap.Add(OliveNodeTypes::Reroute, TMap<FString, FString>());
	RequiredPropertiesMap.Add(OliveNodeTypes::WhileLoop, TMap<FString, FString>());
	RequiredPropertiesMap.Add(OliveNodeTypes::DoOnce, TMap<FString, FString>());
	RequiredPropertiesMap.Add(OliveNodeTypes::FlipFlop, TMap<FString, FString>());
	RequiredPropertiesMap.Add(OliveNodeTypes::Gate, TMap<FString, FString>());
}
