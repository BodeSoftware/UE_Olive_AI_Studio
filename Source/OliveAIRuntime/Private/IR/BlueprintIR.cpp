// Copyright Bode Software. All Rights Reserved.

#include "IR/BlueprintIR.h"
#include "IR/OliveIRSchema.h"
#include "Serialization/JsonSerializer.h"

// FOliveIRBlueprintCapabilities

TSharedPtr<FJsonObject> FOliveIRBlueprintCapabilities::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetBoolField(TEXT("has_event_graph"), bHasEventGraph);
	Json->SetBoolField(TEXT("has_functions"), bHasFunctions);
	Json->SetBoolField(TEXT("has_variables"), bHasVariables);
	Json->SetBoolField(TEXT("has_components"), bHasComponents);
	Json->SetBoolField(TEXT("has_macros"), bHasMacros);
	Json->SetBoolField(TEXT("has_anim_graph"), bHasAnimGraph);
	Json->SetBoolField(TEXT("has_widget_tree"), bHasWidgetTree);
	Json->SetBoolField(TEXT("has_state_machine"), bHasStateMachine);
	return Json;
}

FOliveIRBlueprintCapabilities FOliveIRBlueprintCapabilities::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRBlueprintCapabilities Caps;
	if (JsonObject.IsValid())
	{
		Caps.bHasEventGraph = JsonObject->GetBoolField(TEXT("has_event_graph"));
		Caps.bHasFunctions = JsonObject->GetBoolField(TEXT("has_functions"));
		Caps.bHasVariables = JsonObject->GetBoolField(TEXT("has_variables"));
		Caps.bHasComponents = JsonObject->GetBoolField(TEXT("has_components"));
		Caps.bHasMacros = JsonObject->GetBoolField(TEXT("has_macros"));
		Caps.bHasAnimGraph = JsonObject->GetBoolField(TEXT("has_anim_graph"));
		Caps.bHasWidgetTree = JsonObject->GetBoolField(TEXT("has_widget_tree"));
		Caps.bHasStateMachine = JsonObject->GetBoolField(TEXT("has_state_machine"));
	}
	return Caps;
}

// FOliveIRInterfaceRef

TSharedPtr<FJsonObject> FOliveIRInterfaceRef::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Name);
	Json->SetStringField(TEXT("path"), Path);

	if (RequiredFunctions.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> FuncsArray;
		for (const FString& Func : RequiredFunctions)
		{
			FuncsArray.Add(MakeShared<FJsonValueString>(Func));
		}
		Json->SetArrayField(TEXT("required_functions"), FuncsArray);
	}

	return Json;
}

FOliveIRInterfaceRef FOliveIRInterfaceRef::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRInterfaceRef Ref;
	if (JsonObject.IsValid())
	{
		Ref.Name = JsonObject->GetStringField(TEXT("name"));
		Ref.Path = JsonObject->GetStringField(TEXT("path"));

		const TArray<TSharedPtr<FJsonValue>>* FuncsArray;
		if (JsonObject->TryGetArrayField(TEXT("required_functions"), FuncsArray))
		{
			for (const auto& Value : *FuncsArray)
			{
				Ref.RequiredFunctions.Add(Value->AsString());
			}
		}
	}
	return Ref;
}

// FOliveIRClassRef

TSharedPtr<FJsonObject> FOliveIRClassRef::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Name);
	Json->SetStringField(TEXT("source"), Source);
	if (!Path.IsEmpty())
	{
		Json->SetStringField(TEXT("path"), Path);
	}
	return Json;
}

FOliveIRClassRef FOliveIRClassRef::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRClassRef Ref;
	if (JsonObject.IsValid())
	{
		Ref.Name = JsonObject->GetStringField(TEXT("name"));
		Ref.Source = JsonObject->GetStringField(TEXT("source"));
		Ref.Path = JsonObject->GetStringField(TEXT("path"));
	}
	return Ref;
}

// FOliveIREventDispatcher

TSharedPtr<FJsonObject> FOliveIREventDispatcher::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Name);

	if (Parameters.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ParamsArray;
		for (const FOliveIRPin& Param : Parameters)
		{
			ParamsArray.Add(MakeShared<FJsonValueObject>(Param.ToJson()));
		}
		Json->SetArrayField(TEXT("parameters"), ParamsArray);
	}

	if (!Description.IsEmpty())
	{
		Json->SetStringField(TEXT("description"), Description);
	}
	if (!Category.IsEmpty())
	{
		Json->SetStringField(TEXT("category"), Category);
	}

	return Json;
}

FOliveIREventDispatcher FOliveIREventDispatcher::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIREventDispatcher Dispatcher;
	if (JsonObject.IsValid())
	{
		Dispatcher.Name = JsonObject->GetStringField(TEXT("name"));
		Dispatcher.Description = JsonObject->GetStringField(TEXT("description"));
		Dispatcher.Category = JsonObject->GetStringField(TEXT("category"));

		const TArray<TSharedPtr<FJsonValue>>* ParamsArray;
		if (JsonObject->TryGetArrayField(TEXT("parameters"), ParamsArray))
		{
			for (const auto& Value : *ParamsArray)
			{
				Dispatcher.Parameters.Add(FOliveIRPin::FromJson(Value->AsObject()));
			}
		}
	}
	return Dispatcher;
}

// FOliveIRBlueprint

TSharedPtr<FJsonObject> FOliveIRBlueprint::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("schema_version"), OliveIR::SchemaVersion);
	Json->SetStringField(TEXT("name"), Name);
	Json->SetStringField(TEXT("path"), Path);

	// Blueprint type
	FString TypeStr;
	switch (Type)
	{
		case EOliveIRBlueprintType::Normal: TypeStr = TEXT("Normal"); break;
		case EOliveIRBlueprintType::Interface: TypeStr = TEXT("Interface"); break;
		case EOliveIRBlueprintType::FunctionLibrary: TypeStr = TEXT("FunctionLibrary"); break;
		case EOliveIRBlueprintType::MacroLibrary: TypeStr = TEXT("MacroLibrary"); break;
		case EOliveIRBlueprintType::LevelScript: TypeStr = TEXT("LevelScript"); break;
		default: TypeStr = TEXT("Unknown"); break;
	}
	Json->SetStringField(TEXT("type"), TypeStr);

	Json->SetObjectField(TEXT("parent_class"), ParentClass.ToJson());
	Json->SetObjectField(TEXT("capabilities"), Capabilities.ToJson());

	// Compile status
	FString StatusStr;
	switch (CompileStatus)
	{
		case EOliveIRCompileStatus::UpToDate: StatusStr = TEXT("success"); break;
		case EOliveIRCompileStatus::Dirty: StatusStr = TEXT("dirty"); break;
		case EOliveIRCompileStatus::Error: StatusStr = TEXT("error"); break;
		case EOliveIRCompileStatus::Warning: StatusStr = TEXT("warning"); break;
		default: StatusStr = TEXT("unknown"); break;
	}
	Json->SetStringField(TEXT("compile_status"), StatusStr);

	// Interfaces
	if (Interfaces.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> InterfacesArray;
		for (const FOliveIRInterfaceRef& Interface : Interfaces)
		{
			InterfacesArray.Add(MakeShared<FJsonValueObject>(Interface.ToJson()));
		}
		Json->SetArrayField(TEXT("interfaces"), InterfacesArray);
	}

	// Compile messages
	if (CompileMessages.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> MessagesArray;
		for (const FOliveIRMessage& Msg : CompileMessages)
		{
			MessagesArray.Add(MakeShared<FJsonValueObject>(Msg.ToJson()));
		}
		Json->SetArrayField(TEXT("compile_messages"), MessagesArray);
	}

	// Variables
	if (Variables.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> VarsArray;
		for (const FOliveIRVariable& Var : Variables)
		{
			VarsArray.Add(MakeShared<FJsonValueObject>(Var.ToJson()));
		}
		Json->SetArrayField(TEXT("variables"), VarsArray);
	}

	// Components
	if (Components.Num() > 0)
	{
		TSharedPtr<FJsonObject> ComponentsJson = MakeShared<FJsonObject>();
		ComponentsJson->SetStringField(TEXT("root"), RootComponentName);

		TArray<TSharedPtr<FJsonValue>> TreeArray;
		for (const FOliveIRComponent& Comp : Components)
		{
			TreeArray.Add(MakeShared<FJsonValueObject>(Comp.ToJson()));
		}
		ComponentsJson->SetArrayField(TEXT("tree"), TreeArray);
		Json->SetObjectField(TEXT("components"), ComponentsJson);
	}

	// Graph names and summaries
	TSharedPtr<FJsonObject> GraphsJson = MakeShared<FJsonObject>();
	{
		// Event graphs: use summary objects (name + node_count) when available, fall back to name-only
		TArray<TSharedPtr<FJsonValue>> EventGraphsArray;
		if (EventGraphSummaries.Num() > 0)
		{
			for (const FOliveIRGraphSummary& Summary : EventGraphSummaries)
			{
				TSharedPtr<FJsonObject> SummaryObj = MakeShared<FJsonObject>();
				SummaryObj->SetStringField(TEXT("name"), Summary.Name);
				SummaryObj->SetNumberField(TEXT("node_count"), Summary.NodeCount);
				EventGraphsArray.Add(MakeShared<FJsonValueObject>(SummaryObj));
			}
		}
		else
		{
			for (const FString& GraphName : EventGraphNames)
			{
				EventGraphsArray.Add(MakeShared<FJsonValueString>(GraphName));
			}
		}
		GraphsJson->SetArrayField(TEXT("event_graphs"), EventGraphsArray);

		// Functions: use summary objects (name + node_count) when available, fall back to name-only
		TArray<TSharedPtr<FJsonValue>> FunctionsArray;
		if (FunctionSummaries.Num() > 0)
		{
			for (const FOliveIRGraphSummary& Summary : FunctionSummaries)
			{
				TSharedPtr<FJsonObject> SummaryObj = MakeShared<FJsonObject>();
				SummaryObj->SetStringField(TEXT("name"), Summary.Name);
				SummaryObj->SetNumberField(TEXT("node_count"), Summary.NodeCount);
				FunctionsArray.Add(MakeShared<FJsonValueObject>(SummaryObj));
			}
		}
		else
		{
			for (const FString& FuncName : FunctionNames)
			{
				FunctionsArray.Add(MakeShared<FJsonValueString>(FuncName));
			}
		}
		GraphsJson->SetArrayField(TEXT("functions"), FunctionsArray);

		TArray<TSharedPtr<FJsonValue>> MacrosArray;
		for (const FString& MacroName : MacroNames)
		{
			MacrosArray.Add(MakeShared<FJsonValueString>(MacroName));
		}
		GraphsJson->SetArrayField(TEXT("macros"), MacrosArray);
	}
	Json->SetObjectField(TEXT("graphs"), GraphsJson);

	// Event dispatchers
	if (EventDispatchers.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> DispatchersArray;
		for (const FOliveIREventDispatcher& Dispatcher : EventDispatchers)
		{
			DispatchersArray.Add(MakeShared<FJsonValueObject>(Dispatcher.ToJson()));
		}
		Json->SetArrayField(TEXT("event_dispatchers"), DispatchersArray);
	}

	// State
	if (bIsDirty)
	{
		Json->SetBoolField(TEXT("unsaved_changes"), true);
	}
	if (bIsBeingEdited)
	{
		Json->SetBoolField(TEXT("being_edited"), true);
	}

	return Json;
}

FOliveIRBlueprint FOliveIRBlueprint::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRBlueprint BP;
	if (!JsonObject.IsValid())
	{
		return BP;
	}

	// Parse schema version if present
	if (JsonObject->HasField(TEXT("schema_version")))
	{
		BP.SchemaVersion = JsonObject->GetStringField(TEXT("schema_version"));
	}

	BP.Name = JsonObject->GetStringField(TEXT("name"));
	BP.Path = JsonObject->GetStringField(TEXT("path"));

	FString TypeStr = JsonObject->GetStringField(TEXT("type"));
	if (TypeStr == TEXT("Normal")) BP.Type = EOliveIRBlueprintType::Normal;
	else if (TypeStr == TEXT("Interface")) BP.Type = EOliveIRBlueprintType::Interface;
	else if (TypeStr == TEXT("FunctionLibrary")) BP.Type = EOliveIRBlueprintType::FunctionLibrary;
	else if (TypeStr == TEXT("MacroLibrary")) BP.Type = EOliveIRBlueprintType::MacroLibrary;
	else if (TypeStr == TEXT("LevelScript")) BP.Type = EOliveIRBlueprintType::LevelScript;
	else BP.Type = EOliveIRBlueprintType::Unknown;

	const TSharedPtr<FJsonObject>* ParentJson;
	if (JsonObject->TryGetObjectField(TEXT("parent_class"), ParentJson))
	{
		BP.ParentClass = FOliveIRClassRef::FromJson(*ParentJson);
	}

	const TSharedPtr<FJsonObject>* CapsJson;
	if (JsonObject->TryGetObjectField(TEXT("capabilities"), CapsJson))
	{
		BP.Capabilities = FOliveIRBlueprintCapabilities::FromJson(*CapsJson);
	}

	FString StatusStr = JsonObject->GetStringField(TEXT("compile_status"));
	if (StatusStr == TEXT("success")) BP.CompileStatus = EOliveIRCompileStatus::UpToDate;
	else if (StatusStr == TEXT("dirty")) BP.CompileStatus = EOliveIRCompileStatus::Dirty;
	else if (StatusStr == TEXT("error")) BP.CompileStatus = EOliveIRCompileStatus::Error;
	else if (StatusStr == TEXT("warning")) BP.CompileStatus = EOliveIRCompileStatus::Warning;
	else BP.CompileStatus = EOliveIRCompileStatus::Unknown;

	BP.bIsDirty = JsonObject->GetBoolField(TEXT("unsaved_changes"));
	BP.bIsBeingEdited = JsonObject->GetBoolField(TEXT("being_edited"));

	// Arrays would be parsed similarly...

	return BP;
}

// FOliveIRWidgetNode

TSharedPtr<FJsonObject> FOliveIRWidgetNode::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Name);
	Json->SetStringField(TEXT("widget_class"), WidgetClass);

	if (!SlotType.IsEmpty())
	{
		Json->SetStringField(TEXT("slot_type"), SlotType);
	}

	if (Children.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ChildrenArray;
		for (const FOliveIRWidgetNode& Child : Children)
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

FOliveIRWidgetNode FOliveIRWidgetNode::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRWidgetNode Node;
	if (!JsonObject.IsValid())
	{
		return Node;
	}

	Node.Name = JsonObject->GetStringField(TEXT("name"));
	Node.WidgetClass = JsonObject->GetStringField(TEXT("widget_class"));
	Node.SlotType = JsonObject->GetStringField(TEXT("slot_type"));

	const TArray<TSharedPtr<FJsonValue>>* ChildrenArray;
	if (JsonObject->TryGetArrayField(TEXT("children"), ChildrenArray))
	{
		for (const auto& ChildValue : *ChildrenArray)
		{
			Node.Children.Add(FOliveIRWidgetNode::FromJson(ChildValue->AsObject()));
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

// FOliveIRAnimState

TSharedPtr<FJsonObject> FOliveIRAnimState::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Name);

	if (bIsConduit)
	{
		Json->SetBoolField(TEXT("is_conduit"), true);
	}

	if (!AnimationAsset.IsEmpty())
	{
		Json->SetStringField(TEXT("animation_asset"), AnimationAsset);
	}

	if (TransitionsIn.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		for (const FString& Trans : TransitionsIn)
		{
			Array.Add(MakeShared<FJsonValueString>(Trans));
		}
		Json->SetArrayField(TEXT("transitions_in"), Array);
	}

	if (TransitionsOut.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		for (const FString& Trans : TransitionsOut)
		{
			Array.Add(MakeShared<FJsonValueString>(Trans));
		}
		Json->SetArrayField(TEXT("transitions_out"), Array);
	}

	return Json;
}

FOliveIRAnimState FOliveIRAnimState::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRAnimState State;
	if (!JsonObject.IsValid())
	{
		return State;
	}

	State.Name = JsonObject->GetStringField(TEXT("name"));
	State.bIsConduit = JsonObject->GetBoolField(TEXT("is_conduit"));
	State.AnimationAsset = JsonObject->GetStringField(TEXT("animation_asset"));

	const TArray<TSharedPtr<FJsonValue>>* Array;
	if (JsonObject->TryGetArrayField(TEXT("transitions_in"), Array))
	{
		for (const auto& Value : *Array)
		{
			State.TransitionsIn.Add(Value->AsString());
		}
	}
	if (JsonObject->TryGetArrayField(TEXT("transitions_out"), Array))
	{
		for (const auto& Value : *Array)
		{
			State.TransitionsOut.Add(Value->AsString());
		}
	}

	return State;
}

// FOliveIRAnimStateMachine

TSharedPtr<FJsonObject> FOliveIRAnimStateMachine::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Name);
	Json->SetStringField(TEXT("entry_state"), EntryState);

	if (States.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> StatesArray;
		for (const FOliveIRAnimState& State : States)
		{
			StatesArray.Add(MakeShared<FJsonValueObject>(State.ToJson()));
		}
		Json->SetArrayField(TEXT("states"), StatesArray);
	}

	return Json;
}

FOliveIRAnimStateMachine FOliveIRAnimStateMachine::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRAnimStateMachine SM;
	if (!JsonObject.IsValid())
	{
		return SM;
	}

	SM.Name = JsonObject->GetStringField(TEXT("name"));
	SM.EntryState = JsonObject->GetStringField(TEXT("entry_state"));

	const TArray<TSharedPtr<FJsonValue>>* StatesArray;
	if (JsonObject->TryGetArrayField(TEXT("states"), StatesArray))
	{
		for (const auto& StateValue : *StatesArray)
		{
			SM.States.Add(FOliveIRAnimState::FromJson(StateValue->AsObject()));
		}
	}

	return SM;
}
