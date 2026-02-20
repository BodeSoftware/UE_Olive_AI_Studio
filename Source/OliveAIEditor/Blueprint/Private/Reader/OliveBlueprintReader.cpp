// Copyright Bode Software. All Rights Reserved.

#include "OliveBlueprintReader.h"
#include "OliveComponentReader.h"
#include "OlivePinSerializer.h"
#include "OliveNodeSerializer.h"
#include "OliveGraphReader.h"
#include "OliveBlueprintTypes.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/UObjectGlobals.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Editor.h"

DEFINE_LOG_CATEGORY(LogOliveBPReader);

// ============================================================================
// Singleton
// ============================================================================

FOliveBlueprintReader& FOliveBlueprintReader::Get()
{
	static FOliveBlueprintReader Instance;
	return Instance;
}

FOliveBlueprintReader::FOliveBlueprintReader()
{
	ComponentReader = MakeShared<FOliveComponentReader>();
	PinSerializer = MakeShared<FOlivePinSerializer>();
	NodeSerializer = MakeShared<FOliveNodeSerializer>();
	GraphReader = MakeShared<FOliveGraphReader>();
}

// ============================================================================
// Summary Reading
// ============================================================================

TOptional<FOliveIRBlueprint> FOliveBlueprintReader::ReadBlueprintSummary(const FString& AssetPath)
{
	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		UE_LOG(LogOliveBPReader, Warning, TEXT("Failed to load Blueprint at path: %s"), *AssetPath);
		return TOptional<FOliveIRBlueprint>();
	}

	return ReadBlueprintSummary(Blueprint);
}

TOptional<FOliveIRBlueprint> FOliveBlueprintReader::ReadBlueprintSummary(const UBlueprint* Blueprint)
{
	if (!Blueprint)
	{
		UE_LOG(LogOliveBPReader, Warning, TEXT("ReadBlueprintSummary called with null Blueprint"));
		return TOptional<FOliveIRBlueprint>();
	}

	FOliveIRBlueprint IR;

	// Basic metadata
	IR.Name = Blueprint->GetName();
	IR.Path = Blueprint->GetPathName();

	// Detect Blueprint type
	EOliveBlueprintType EditorType = FOliveBlueprintTypeDetector::DetectType(Blueprint);
	IR.Type = FOliveBlueprintTypeDetector::ToIRType(EditorType);

	// Read parent class info
	ReadParentClassInfo(Blueprint, IR);

	// Read capabilities
	ReadCapabilities(Blueprint, IR);

	// Read interfaces
	ReadInterfacesInfo(Blueprint, IR);

	// Read compilation status
	ReadCompilationStatus(Blueprint, IR);

	// Read variables
	IR.Variables = ReadVariables(Blueprint);

	// Read components
	IR.Components = ReadComponents(Blueprint);
	IR.RootComponentName = ComponentReader->GetRootComponentName(Blueprint);

	// Read graph names (not full graph data)
	ReadGraphNames(Blueprint, IR);

	// Read event dispatchers
	ReadEventDispatchers(Blueprint, IR);

	// Check dirty and edit states
	IR.bIsDirty = Blueprint->GetOutermost()->IsDirty();

	// Check if being edited (UE 5.5: FindEditorForAsset requires non-const pointer)
	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	if (AssetEditorSubsystem)
	{
		IR.bIsBeingEdited = AssetEditorSubsystem->FindEditorForAsset(const_cast<UBlueprint*>(Blueprint), false) != nullptr;
	}

	UE_LOG(LogOliveBPReader, Log, TEXT("Read Blueprint summary: %s (Type: %s, Variables: %d, Components: %d)"),
		*IR.Name,
		*FOliveBlueprintTypeDetector::TypeToString(EditorType),
		IR.Variables.Num(),
		IR.Components.Num());

	return TOptional<FOliveIRBlueprint>(MoveTemp(IR));
}

TArray<FOliveIRVariable> FOliveBlueprintReader::ReadVariables(const UBlueprint* Blueprint)
{
	TArray<FOliveIRVariable> Variables;

	if (!Blueprint)
	{
		return Variables;
	}

	// Read variables defined in this Blueprint
	for (const FBPVariableDescription& VarDesc : Blueprint->NewVariables)
	{
		FOliveIRVariable Var = ConvertVariableToIR(VarDesc, TEXT("self"));
		Variables.Add(MoveTemp(Var));
	}

	// Optionally include inherited variables from parent Blueprint
	// Note: We only show variables from the immediate parent Blueprint, not all ancestors
	if (Blueprint->ParentClass)
	{
		UBlueprint* ParentBP = Cast<UBlueprint>(Blueprint->ParentClass->ClassGeneratedBy);
		if (ParentBP)
		{
			for (const FBPVariableDescription& VarDesc : ParentBP->NewVariables)
			{
				FOliveIRVariable Var = ConvertVariableToIR(VarDesc, ParentBP->GetName());
				Variables.Add(MoveTemp(Var));
			}
		}
	}

	return Variables;
}

TArray<FOliveIRVariable> FOliveBlueprintReader::ReadVariables(const FString& AssetPath)
{
	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		return TArray<FOliveIRVariable>();
	}
	return ReadVariables(Blueprint);
}

TArray<FOliveIRComponent> FOliveBlueprintReader::ReadComponents(const UBlueprint* Blueprint)
{
	if (!Blueprint || !ComponentReader.IsValid())
	{
		return TArray<FOliveIRComponent>();
	}
	return ComponentReader->ReadComponents(Blueprint);
}

TArray<FOliveIRComponent> FOliveBlueprintReader::ReadComponents(const FString& AssetPath)
{
	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		return TArray<FOliveIRComponent>();
	}
	return ReadComponents(Blueprint);
}

TArray<FOliveIRFunctionSignature> FOliveBlueprintReader::ReadFunctionSignatures(const UBlueprint* Blueprint)
{
	TArray<FOliveIRFunctionSignature> Signatures;

	if (!Blueprint)
	{
		return Signatures;
	}

	// Read user-defined functions from function graphs
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (!Graph)
		{
			continue;
		}

		// Find the function entry node
		UK2Node_FunctionEntry* EntryNode = nullptr;
		UK2Node_FunctionResult* ResultNode = nullptr;

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
			{
				EntryNode = Entry;
			}
			else if (UK2Node_FunctionResult* Result = Cast<UK2Node_FunctionResult>(Node))
			{
				ResultNode = Result;
			}
		}

		if (EntryNode)
		{
			FOliveIRFunctionSignature Sig = ExtractFunctionSignature(EntryNode, ResultNode);
			Sig.DefinedIn = TEXT("self");
			Signatures.Add(MoveTemp(Sig));
		}
	}

	// Read event signatures from event graph
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (!Graph)
		{
			continue;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
			{
				FOliveIRFunctionSignature Sig;
				Sig.Name = EventNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
				Sig.bIsEvent = true;
				Sig.DefinedIn = TEXT("self");

				// Events don't have regular inputs/outputs in the same way
				// They have output pins that become available after the event fires
				Signatures.Add(MoveTemp(Sig));
			}
			else if (UK2Node_CustomEvent* CustomEventNode = Cast<UK2Node_CustomEvent>(Node))
			{
				FOliveIRFunctionSignature Sig;
				Sig.Name = CustomEventNode->CustomFunctionName.ToString();
				Sig.bIsEvent = true;
				Sig.DefinedIn = TEXT("self");
				Signatures.Add(MoveTemp(Sig));
			}
		}
	}

	return Signatures;
}

TArray<FOliveIRFunctionSignature> FOliveBlueprintReader::ReadFunctionSignatures(const FString& AssetPath)
{
	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		return TArray<FOliveIRFunctionSignature>();
	}
	return ReadFunctionSignatures(Blueprint);
}

TArray<FString> FOliveBlueprintReader::ReadHierarchy(const UBlueprint* Blueprint)
{
	TArray<FString> Hierarchy;

	if (!Blueprint)
	{
		return Hierarchy;
	}

	// Start with the Blueprint's generated class
	UClass* CurrentClass = Blueprint->GeneratedClass;
	if (!CurrentClass && Blueprint->ParentClass)
	{
		CurrentClass = Blueprint->ParentClass;
	}

	// Walk up the inheritance chain
	while (CurrentClass && CurrentClass != UObject::StaticClass())
	{
		// Check if this is a Blueprint-generated class
		UBlueprint* BP = Cast<UBlueprint>(CurrentClass->ClassGeneratedBy);
		if (BP)
		{
			// Blueprint class - use asset name
			Hierarchy.Add(BP->GetName());
		}
		else
		{
			// Native class - use class name
			Hierarchy.Add(CurrentClass->GetName());
		}

		CurrentClass = CurrentClass->GetSuperClass();
	}

	// Add UObject at the end
	Hierarchy.Add(TEXT("Object"));

	return Hierarchy;
}

TArray<FString> FOliveBlueprintReader::ReadHierarchy(const FString& AssetPath)
{
	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		return TArray<FString>();
	}
	return ReadHierarchy(Blueprint);
}

// ============================================================================
// Full Reading (Batch 4 - Implemented)
// ============================================================================

TOptional<FOliveIRBlueprint> FOliveBlueprintReader::ReadBlueprintFull(const FString& AssetPath)
{
	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		UE_LOG(LogOliveBPReader, Warning, TEXT("ReadBlueprintFull: Failed to load Blueprint at path: %s"), *AssetPath);
		return TOptional<FOliveIRBlueprint>();
	}

	return ReadBlueprintFull(Blueprint);
}

TOptional<FOliveIRBlueprint> FOliveBlueprintReader::ReadBlueprintFull(const UBlueprint* Blueprint)
{
	if (!Blueprint)
	{
		UE_LOG(LogOliveBPReader, Warning, TEXT("ReadBlueprintFull called with null Blueprint"));
		return TOptional<FOliveIRBlueprint>();
	}

	// Start with the summary
	TOptional<FOliveIRBlueprint> OptionalIR = ReadBlueprintSummary(Blueprint);
	if (!OptionalIR.IsSet())
	{
		return OptionalIR;
	}

	FOliveIRBlueprint IR = MoveTemp(OptionalIR.GetValue());

	// Read all graphs with full node data
	ReadAllGraphs(Blueprint, IR);

	UE_LOG(LogOliveBPReader, Log, TEXT("Read Blueprint full: %s (Graphs: %d)"),
		*IR.Name,
		IR.Graphs.Num());

	return TOptional<FOliveIRBlueprint>(MoveTemp(IR));
}

TOptional<FOliveIRGraph> FOliveBlueprintReader::ReadFunctionGraph(const UBlueprint* Blueprint, const FString& FunctionName)
{
	if (!Blueprint || !GraphReader.IsValid())
	{
		UE_LOG(LogOliveBPReader, Warning, TEXT("ReadFunctionGraph: Invalid Blueprint or GraphReader"));
		return TOptional<FOliveIRGraph>();
	}

	// Find the function graph by name
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == FunctionName)
		{
			FOliveIRGraph GraphIR = GraphReader->ReadGraph(Graph, Blueprint);
			return TOptional<FOliveIRGraph>(MoveTemp(GraphIR));
		}
	}

	UE_LOG(LogOliveBPReader, Warning, TEXT("ReadFunctionGraph: Function '%s' not found in Blueprint '%s'"),
		*FunctionName,
		*Blueprint->GetName());
	return TOptional<FOliveIRGraph>();
}

TOptional<FOliveIRGraph> FOliveBlueprintReader::ReadFunctionGraph(const FString& BlueprintPath, const FString& FunctionName)
{
	UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
	if (!Blueprint)
	{
		return TOptional<FOliveIRGraph>();
	}
	return ReadFunctionGraph(Blueprint, FunctionName);
}

TOptional<FOliveIRGraph> FOliveBlueprintReader::ReadEventGraph(const UBlueprint* Blueprint, const FString& GraphName)
{
	if (!Blueprint || !GraphReader.IsValid())
	{
		UE_LOG(LogOliveBPReader, Warning, TEXT("ReadEventGraph: Invalid Blueprint or GraphReader"));
		return TOptional<FOliveIRGraph>();
	}

	// Find the event graph by name
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph && Graph->GetName() == GraphName)
		{
			FOliveIRGraph GraphIR = GraphReader->ReadGraph(Graph, Blueprint);
			return TOptional<FOliveIRGraph>(MoveTemp(GraphIR));
		}
	}

	// If no specific name was found and the default was requested, try the first Ubergraph
	if (GraphName == TEXT("EventGraph") && Blueprint->UbergraphPages.Num() > 0)
	{
		UEdGraph* FirstGraph = Blueprint->UbergraphPages[0];
		if (FirstGraph)
		{
			FOliveIRGraph GraphIR = GraphReader->ReadGraph(FirstGraph, Blueprint);
			return TOptional<FOliveIRGraph>(MoveTemp(GraphIR));
		}
	}

	UE_LOG(LogOliveBPReader, Warning, TEXT("ReadEventGraph: Event graph '%s' not found in Blueprint '%s'"),
		*GraphName,
		*Blueprint->GetName());
	return TOptional<FOliveIRGraph>();
}

TOptional<FOliveIRGraph> FOliveBlueprintReader::ReadEventGraph(const FString& BlueprintPath, const FString& EventGraphName)
{
	UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
	if (!Blueprint)
	{
		return TOptional<FOliveIRGraph>();
	}
	return ReadEventGraph(Blueprint, EventGraphName);
}

TOptional<FOliveIRGraph> FOliveBlueprintReader::ReadMacroGraph(const UBlueprint* Blueprint, const FString& MacroName)
{
	if (!Blueprint || !GraphReader.IsValid())
	{
		UE_LOG(LogOliveBPReader, Warning, TEXT("ReadMacroGraph: Invalid Blueprint or GraphReader"));
		return TOptional<FOliveIRGraph>();
	}

	// Find the macro graph by name
	for (UEdGraph* Graph : Blueprint->MacroGraphs)
	{
		if (Graph && Graph->GetName() == MacroName)
		{
			FOliveIRGraph GraphIR = GraphReader->ReadGraph(Graph, Blueprint);
			return TOptional<FOliveIRGraph>(MoveTemp(GraphIR));
		}
	}

	UE_LOG(LogOliveBPReader, Warning, TEXT("ReadMacroGraph: Macro '%s' not found in Blueprint '%s'"),
		*MacroName,
		*Blueprint->GetName());
	return TOptional<FOliveIRGraph>();
}

TOptional<FOliveIRGraph> FOliveBlueprintReader::ReadMacroGraph(const FString& BlueprintPath, const FString& MacroName)
{
	UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
	if (!Blueprint)
	{
		return TOptional<FOliveIRGraph>();
	}
	return ReadMacroGraph(Blueprint, MacroName);
}

TArray<FOliveIRFunctionSignature> FOliveBlueprintReader::ReadOverridableFunctions(const UBlueprint* Blueprint)
{
	TArray<FOliveIRFunctionSignature> Signatures;

	if (!Blueprint || !Blueprint->ParentClass)
	{
		return Signatures;
	}

	// Get all overridable function names from implemented interfaces (UE 5.5: manual iteration)
	TSet<FName> OverridableFunctionNames;
	for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
	{
		if (UClass* InterfaceClass = InterfaceDesc.Interface)
		{
			for (TFieldIterator<UFunction> FuncIt(InterfaceClass, EFieldIteratorFlags::IncludeSuper); FuncIt; ++FuncIt)
			{
				UFunction* Function = *FuncIt;
				if (Function && !Function->HasAnyFunctionFlags(FUNC_Private))
				{
					OverridableFunctionNames.Add(Function->GetFName());
				}
			}
		}
	}

	// Also get overridable events from the parent class
	for (TFieldIterator<UFunction> FuncIt(Blueprint->ParentClass, EFieldIteratorFlags::IncludeSuper); FuncIt; ++FuncIt)
	{
		UFunction* Function = *FuncIt;
		if (!Function)
		{
			continue;
		}

		// Check if this function can be overridden
		if (Function->HasAnyFunctionFlags(FUNC_BlueprintEvent) &&
			!Function->HasAnyFunctionFlags(FUNC_Final))
		{
			// Check if already overridden in this Blueprint
			bool bAlreadyOverridden = false;
			for (UEdGraph* Graph : Blueprint->FunctionGraphs)
			{
				if (Graph && Graph->GetName() == Function->GetName())
				{
					bAlreadyOverridden = true;
					break;
				}
			}

			// Also check event graphs for native events
			if (!bAlreadyOverridden)
			{
				for (UEdGraph* Graph : Blueprint->UbergraphPages)
				{
					if (Graph)
					{
						for (UEdGraphNode* Node : Graph->Nodes)
						{
							if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
							{
								if (EventNode->GetFunctionName() == Function->GetFName())
								{
									bAlreadyOverridden = true;
									break;
								}
							}
						}
					}
					if (bAlreadyOverridden)
					{
						break;
					}
				}
			}

			if (!bAlreadyOverridden)
			{
				FOliveIRFunctionSignature Sig;
				Sig.Name = Function->GetName();
				Sig.bIsEvent = Function->HasAnyFunctionFlags(FUNC_BlueprintEvent);
				Sig.bIsPure = Function->HasAnyFunctionFlags(FUNC_BlueprintPure);
				Sig.bIsConst = Function->HasAnyFunctionFlags(FUNC_Const);

				// Get the class that defines this function
				if (UClass* OwnerClass = Function->GetOwnerClass())
				{
					Sig.DefinedIn = OwnerClass->GetName();
				}

				// Extract parameters
				for (TFieldIterator<FProperty> PropIt(Function); PropIt; ++PropIt)
				{
					FProperty* Prop = *PropIt;
					if (!Prop)
					{
						continue;
					}

					FOliveIRFunctionParam Param;
					Param.Name = Prop->GetName();

					// Check if it's a return parameter
					if (Prop->HasAnyPropertyFlags(CPF_ReturnParm))
					{
						Param.bIsOutParam = true;
						Sig.Outputs.Add(MoveTemp(Param));
					}
					else if (Prop->HasAnyPropertyFlags(CPF_OutParm))
					{
						Param.bIsOutParam = true;
						Param.bIsReference = true;
						Sig.Outputs.Add(MoveTemp(Param));
					}
					else
					{
						Param.bIsReference = Prop->HasAnyPropertyFlags(CPF_ReferenceParm);
						Sig.Inputs.Add(MoveTemp(Param));
					}
				}

				// Get metadata
				if (Function->HasMetaData(FBlueprintMetadata::MD_FunctionCategory))
				{
					Sig.Category = Function->GetMetaData(FBlueprintMetadata::MD_FunctionCategory);
				}
				if (Function->HasMetaData(FBlueprintMetadata::MD_Tooltip))
				{
					Sig.Description = Function->GetMetaData(FBlueprintMetadata::MD_Tooltip);
				}

				Signatures.Add(MoveTemp(Sig));
			}
		}
	}

	UE_LOG(LogOliveBPReader, Log, TEXT("Found %d overridable functions for Blueprint '%s'"),
		Signatures.Num(),
		*Blueprint->GetName());

	return Signatures;
}

TArray<FOliveIRFunctionSignature> FOliveBlueprintReader::ReadOverridableFunctions(const FString& BlueprintPath)
{
	UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
	if (!Blueprint)
	{
		return TArray<FOliveIRFunctionSignature>();
	}
	return ReadOverridableFunctions(Blueprint);
}

TArray<FOliveIRFunctionSignature> FOliveBlueprintReader::ReadOverriddenFunctions(const UBlueprint* Blueprint)
{
	TArray<FOliveIRFunctionSignature> Signatures;

	if (!Blueprint || !Blueprint->ParentClass)
	{
		return Signatures;
	}

	// Check function graphs for overridden functions
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (!Graph)
		{
			continue;
		}

		// Find the function entry node
		UK2Node_FunctionEntry* EntryNode = nullptr;
		UK2Node_FunctionResult* ResultNode = nullptr;

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
			{
				EntryNode = Entry;
			}
			else if (UK2Node_FunctionResult* Result = Cast<UK2Node_FunctionResult>(Node))
			{
				ResultNode = Result;
			}
		}

		if (EntryNode)
		{
			// Check if this is an override by looking for the function in parent
			UFunction* ParentFunction = Blueprint->ParentClass->FindFunctionByName(FName(*Graph->GetName()));
			if (ParentFunction)
			{
				FOliveIRFunctionSignature Sig = ExtractFunctionSignature(EntryNode, ResultNode);
				Sig.bIsOverride = true;
				Sig.DefinedIn = TEXT("self");
				Signatures.Add(MoveTemp(Sig));
			}
		}
	}

	// Check event graphs for overridden events
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (!Graph)
		{
			continue;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
			{
				// Check if this event is defined in a parent class
				if (UFunction* ParentFunction = Blueprint->ParentClass->FindFunctionByName(EventNode->GetFunctionName()))
				{
					FOliveIRFunctionSignature Sig;
					Sig.Name = EventNode->GetFunctionName().ToString();
					Sig.bIsEvent = true;
					Sig.bIsOverride = true;
					Sig.DefinedIn = TEXT("self");

					// Get the parent class that defines this event
					if (UClass* OwnerClass = ParentFunction->GetOwnerClass())
					{
						Sig.Category = OwnerClass->GetName();
					}

					Signatures.Add(MoveTemp(Sig));
				}
			}
		}
	}

	UE_LOG(LogOliveBPReader, Log, TEXT("Found %d overridden functions for Blueprint '%s'"),
		Signatures.Num(),
		*Blueprint->GetName());

	return Signatures;
}

// ============================================================================
// Private Helper Methods
// ============================================================================

UBlueprint* FOliveBlueprintReader::LoadBlueprint(const FString& AssetPath) const
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
		UE_LOG(LogOliveBPReader, Warning, TEXT("Failed to load Blueprint: %s"), *AssetPath);
	}

	return Blueprint;
}

void FOliveBlueprintReader::ReadInterfacesInfo(const UBlueprint* Blueprint, FOliveIRBlueprint& OutIR) const
{
	if (!Blueprint)
	{
		return;
	}

	for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
	{
		if (!InterfaceDesc.Interface)
		{
			continue;
		}

		FOliveIRInterfaceRef InterfaceRef;
		InterfaceRef.Name = InterfaceDesc.Interface->GetName();

		// Get path if it's a Blueprint interface
		UBlueprint* InterfaceBP = Cast<UBlueprint>(InterfaceDesc.Interface->ClassGeneratedBy);
		if (InterfaceBP)
		{
			InterfaceRef.Path = InterfaceBP->GetPathName();
		}

		// Get required functions from the interface
		for (TFieldIterator<UFunction> FuncIt(InterfaceDesc.Interface); FuncIt; ++FuncIt)
		{
			UFunction* Func = *FuncIt;
			if (Func && Func->HasAnyFunctionFlags(FUNC_BlueprintEvent | FUNC_BlueprintCallable))
			{
				InterfaceRef.RequiredFunctions.Add(Func->GetName());
			}
		}

		OutIR.Interfaces.Add(MoveTemp(InterfaceRef));
	}
}

void FOliveBlueprintReader::ReadCompilationStatus(const UBlueprint* Blueprint, FOliveIRBlueprint& OutIR) const
{
	if (!Blueprint)
	{
		return;
	}

	// Map UE compile status to IR status
	switch (Blueprint->Status)
	{
	case BS_Unknown:
		OutIR.CompileStatus = EOliveIRCompileStatus::Unknown;
		break;
	case BS_Dirty:
		OutIR.CompileStatus = EOliveIRCompileStatus::Dirty;
		break;
	case BS_Error:
		OutIR.CompileStatus = EOliveIRCompileStatus::Error;
		break;
	case BS_UpToDate:
		OutIR.CompileStatus = EOliveIRCompileStatus::UpToDate;
		break;
	case BS_UpToDateWithWarnings:
		OutIR.CompileStatus = EOliveIRCompileStatus::Warning;
		break;
	default:
		OutIR.CompileStatus = EOliveIRCompileStatus::Unknown;
		break;
	}

	// Provide basic compile status messages based on Blueprint status.
	// PHASE2_DEFERRED: Full compile message extraction requires hooking into
	// FKismetCompilerContext or capturing the message log during compilation.
	// For now, provide summary messages based on the compile status enum.
	if (Blueprint->Status == BS_Error)
	{
		FOliveIRMessage ErrorMsg;
		ErrorMsg.Severity = EOliveIRSeverity::Error;
		ErrorMsg.Code = TEXT("COMPILE_ERROR");
		ErrorMsg.Message = TEXT("Blueprint has compile errors. Open in Blueprint editor to see details.");
		ErrorMsg.Details.Add(TEXT("phase2_deferred"), TEXT("true"));
		OutIR.CompileMessages.Add(ErrorMsg);
	}
	else if (Blueprint->Status == BS_UpToDateWithWarnings)
	{
		FOliveIRMessage WarnMsg;
		WarnMsg.Severity = EOliveIRSeverity::Warning;
		WarnMsg.Code = TEXT("COMPILE_WARNING");
		WarnMsg.Message = TEXT("Blueprint compiled with warnings. Open in Blueprint editor to see details.");
		WarnMsg.Details.Add(TEXT("phase2_deferred"), TEXT("true"));
		OutIR.CompileMessages.Add(WarnMsg);
	}
}

void FOliveBlueprintReader::ReadParentClassInfo(const UBlueprint* Blueprint, FOliveIRBlueprint& OutIR) const
{
	if (!Blueprint || !Blueprint->ParentClass)
	{
		return;
	}

	UClass* ParentClass = Blueprint->ParentClass;
	OutIR.ParentClass.Name = ParentClass->GetName();

	// Check if parent is a Blueprint or native class
	UBlueprint* ParentBP = Cast<UBlueprint>(ParentClass->ClassGeneratedBy);
	if (ParentBP)
	{
		OutIR.ParentClass.Source = TEXT("blueprint");
		OutIR.ParentClass.Path = ParentBP->GetPathName();
	}
	else
	{
		OutIR.ParentClass.Source = TEXT("cpp");
	}
}

void FOliveBlueprintReader::ReadEventDispatchers(const UBlueprint* Blueprint, FOliveIRBlueprint& OutIR) const
{
	if (!Blueprint)
	{
		return;
	}

	// Event dispatchers are multicast delegates defined in the Blueprint
	for (const FBPVariableDescription& VarDesc : Blueprint->NewVariables)
	{
		// Check if this is a multicast delegate
		if (VarDesc.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
		{
			FOliveIREventDispatcher Dispatcher;
			Dispatcher.Name = VarDesc.VarName.ToString();
			Dispatcher.Category = VarDesc.Category.ToString();
			// Tooltip/description from metadata
			if (VarDesc.HasMetaData(FBlueprintMetadata::MD_Tooltip))
			{
				Dispatcher.Description = VarDesc.GetMetaData(FBlueprintMetadata::MD_Tooltip);
			}

			// PHASE2_DEFERRED: Delegate signature parameter extraction requires finding
			// the delegate's signature function and reading its parameter list.
			// For now, the dispatcher is recorded without parameter info.

			OutIR.EventDispatchers.Add(MoveTemp(Dispatcher));
		}
	}
}

void FOliveBlueprintReader::ReadGraphNames(const UBlueprint* Blueprint, FOliveIRBlueprint& OutIR) const
{
	if (!Blueprint)
	{
		return;
	}

	// Event graphs (Ubergraph pages)
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph)
		{
			OutIR.EventGraphNames.Add(Graph->GetName());
		}
	}

	// Function graphs
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph)
		{
			OutIR.FunctionNames.Add(Graph->GetName());
		}
	}

	// Macro graphs
	for (UEdGraph* Graph : Blueprint->MacroGraphs)
	{
		if (Graph)
		{
			OutIR.MacroNames.Add(Graph->GetName());
		}
	}
}

void FOliveBlueprintReader::ReadCapabilities(const UBlueprint* Blueprint, FOliveIRBlueprint& OutIR) const
{
	if (!Blueprint)
	{
		return;
	}

	EOliveBlueprintType EditorType = FOliveBlueprintTypeDetector::DetectType(Blueprint);
	FOliveBlueprintCapabilities Caps = FOliveBlueprintTypeDetector::GetCapabilities(EditorType);

	OutIR.Capabilities.bHasEventGraph = Caps.bHasEventGraph;
	OutIR.Capabilities.bHasFunctions = Caps.bHasFunctions;
	OutIR.Capabilities.bHasVariables = Caps.bHasVariables;
	OutIR.Capabilities.bHasComponents = Caps.bHasComponents;
	OutIR.Capabilities.bHasMacros = Caps.bHasMacros;
	OutIR.Capabilities.bHasAnimGraph = Caps.bHasAnimGraph;
	OutIR.Capabilities.bHasWidgetTree = Caps.bHasWidgetTree;
	OutIR.Capabilities.bHasStateMachine = Caps.bHasStateMachines;
}

void FOliveBlueprintReader::ReadAllGraphs(const UBlueprint* Blueprint, FOliveIRBlueprint& OutIR)
{
	if (!Blueprint || !GraphReader.IsValid())
	{
		return;
	}

	// Read event graphs (Ubergraph pages)
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph)
		{
			GraphReader->ClearCache();
			FOliveIRGraph GraphIR = GraphReader->ReadGraph(Graph, Blueprint);
			OutIR.Graphs.Add(MoveTemp(GraphIR));
		}
	}

	// Read function graphs
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph)
		{
			GraphReader->ClearCache();
			FOliveIRGraph GraphIR = GraphReader->ReadGraph(Graph, Blueprint);
			OutIR.Graphs.Add(MoveTemp(GraphIR));
		}
	}

	// Read macro graphs
	for (UEdGraph* Graph : Blueprint->MacroGraphs)
	{
		if (Graph)
		{
			GraphReader->ClearCache();
			FOliveIRGraph GraphIR = GraphReader->ReadGraph(Graph, Blueprint);
			OutIR.Graphs.Add(MoveTemp(GraphIR));
		}
	}

	UE_LOG(LogOliveBPReader, Log, TEXT("Read %d graphs from Blueprint '%s' (Event: %d, Function: %d, Macro: %d)"),
		OutIR.Graphs.Num(),
		*Blueprint->GetName(),
		Blueprint->UbergraphPages.Num(),
		Blueprint->FunctionGraphs.Num(),
		Blueprint->MacroGraphs.Num());
}

FOliveIRVariable FOliveBlueprintReader::ConvertVariableToIR(const FBPVariableDescription& VarDesc, const FString& DefinedIn) const
{
	FOliveIRVariable Var;

	Var.Name = VarDesc.VarName.ToString();
	Var.DefinedIn = DefinedIn;
	Var.Category = VarDesc.Category.ToString();

	// Convert pin type to IR type using pin serializer
	if (PinSerializer.IsValid())
	{
		Var.Type = PinSerializer->SerializePinType(VarDesc.VarType);
	}

	// Default value
	Var.DefaultValue = VarDesc.DefaultValue;

	// Read property flags
	Var.bBlueprintReadWrite = (VarDesc.PropertyFlags & CPF_BlueprintReadOnly) == 0;
	Var.bExposeOnSpawn = (VarDesc.PropertyFlags & CPF_ExposeOnSpawn) != 0;
	Var.bReplicated = (VarDesc.PropertyFlags & CPF_Net) != 0;
	Var.bSaveGame = (VarDesc.PropertyFlags & CPF_SaveGame) != 0;
	Var.bEditAnywhere = (VarDesc.PropertyFlags & CPF_Edit) != 0;
	Var.bBlueprintVisible = (VarDesc.PropertyFlags & CPF_BlueprintVisible) != 0;

	// Replication condition
	if (Var.bReplicated)
	{
		switch (VarDesc.ReplicationCondition)
		{
		case COND_None:
			Var.ReplicationCondition = TEXT("None");
			break;
		case COND_InitialOnly:
			Var.ReplicationCondition = TEXT("InitialOnly");
			break;
		case COND_OwnerOnly:
			Var.ReplicationCondition = TEXT("OwnerOnly");
			break;
		case COND_SkipOwner:
			Var.ReplicationCondition = TEXT("SkipOwner");
			break;
		case COND_SimulatedOnly:
			Var.ReplicationCondition = TEXT("SimulatedOnly");
			break;
		case COND_AutonomousOnly:
			Var.ReplicationCondition = TEXT("AutonomousOnly");
			break;
		case COND_SimulatedOrPhysics:
			Var.ReplicationCondition = TEXT("SimulatedOrPhysics");
			break;
		case COND_InitialOrOwner:
			Var.ReplicationCondition = TEXT("InitialOrOwner");
			break;
		case COND_Custom:
			Var.ReplicationCondition = TEXT("Custom");
			break;
		case COND_ReplayOrOwner:
			Var.ReplicationCondition = TEXT("ReplayOrOwner");
			break;
		case COND_ReplayOnly:
			Var.ReplicationCondition = TEXT("ReplayOnly");
			break;
		case COND_SimulatedOnlyNoReplay:
			Var.ReplicationCondition = TEXT("SimulatedOnlyNoReplay");
			break;
		case COND_SimulatedOrPhysicsNoReplay:
			Var.ReplicationCondition = TEXT("SimulatedOrPhysicsNoReplay");
			break;
		case COND_SkipReplay:
			Var.ReplicationCondition = TEXT("SkipReplay");
			break;
		default:
			Var.ReplicationCondition = TEXT("Unknown");
			break;
		}
	}

	// Description from metadata
	if (VarDesc.HasMetaData(FBlueprintMetadata::MD_Tooltip))
	{
		Var.Description = VarDesc.GetMetaData(FBlueprintMetadata::MD_Tooltip);
	}

	return Var;
}

FOliveIRFunctionSignature FOliveBlueprintReader::ExtractFunctionSignature(
	const UK2Node_FunctionEntry* EntryNode,
	const UK2Node_FunctionResult* ResultNode) const
{
	FOliveIRFunctionSignature Sig;

	if (!EntryNode)
	{
		return Sig;
	}

	// Get function name from the graph
	if (UEdGraph* Graph = EntryNode->GetGraph())
	{
		Sig.Name = Graph->GetName();
	}

	// Get function flags from entry node
	const UFunction* SignatureFunction = EntryNode->FindSignatureFunction();
	if (SignatureFunction)
	{
		Sig.bIsStatic = SignatureFunction->HasAnyFunctionFlags(FUNC_Static);
		Sig.bIsPure = SignatureFunction->HasAnyFunctionFlags(FUNC_BlueprintPure);
		Sig.bIsConst = SignatureFunction->HasAnyFunctionFlags(FUNC_Const);
		Sig.bIsPublic = SignatureFunction->HasAnyFunctionFlags(FUNC_BlueprintCallable);
		Sig.bCallInEditor = SignatureFunction->HasMetaData(FBlueprintMetadata::MD_CallInEditor);

		// Get category and keywords
		if (SignatureFunction->HasMetaData(FBlueprintMetadata::MD_FunctionCategory))
		{
			Sig.Category = SignatureFunction->GetMetaData(FBlueprintMetadata::MD_FunctionCategory);
		}
		if (SignatureFunction->HasMetaData(FBlueprintMetadata::MD_FunctionKeywords))
		{
			Sig.Keywords = SignatureFunction->GetMetaData(FBlueprintMetadata::MD_FunctionKeywords);
		}
		if (SignatureFunction->HasMetaData(FBlueprintMetadata::MD_Tooltip))
		{
			Sig.Description = SignatureFunction->GetMetaData(FBlueprintMetadata::MD_Tooltip);
		}
	}

	// Extract inputs from the entry node's output pins
	// (Entry node's outputs are the function's inputs)
	for (UEdGraphPin* Pin : EntryNode->Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Output)
		{
			continue;
		}

		// Skip exec pins and self pins
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec ||
			Pin->PinName == UEdGraphSchema_K2::PN_Self)
		{
			continue;
		}

		FOliveIRFunctionParam Param;
		Param.Name = Pin->GetName();
		if (PinSerializer.IsValid())
		{
			Param.Type = PinSerializer->SerializePinType(Pin->PinType);
		}
		Param.DefaultValue = Pin->DefaultValue;
		Param.bIsReference = Pin->PinType.bIsReference;

		Sig.Inputs.Add(MoveTemp(Param));
	}

	// Extract outputs from the result node's input pins
	// (Result node's inputs are the function's outputs)
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

			FOliveIRFunctionParam Param;
			Param.Name = Pin->GetName();
			if (PinSerializer.IsValid())
			{
				Param.Type = PinSerializer->SerializePinType(Pin->PinType);
			}
			Param.bIsOutParam = true;

			Sig.Outputs.Add(MoveTemp(Param));
		}
	}

	return Sig;
}
