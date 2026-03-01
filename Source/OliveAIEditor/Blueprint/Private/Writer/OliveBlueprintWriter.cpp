// Copyright Bode Software. All Rights Reserved.

#include "OliveBlueprintWriter.h"
#include "OliveBlueprintTypes.h"
#include "OliveClassResolver.h"
#include "Services/OliveTransactionManager.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "Editor.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY(LogOliveBPWriter);

// ============================================================================
// FOliveBlueprintWriteResult Implementation
// ============================================================================

FOliveBlueprintWriteResult FOliveBlueprintWriteResult::Success(const FString& InAssetPath, const FString& InCreatedItemName)
{
	FOliveBlueprintWriteResult Result;
	Result.bSuccess = true;
	Result.AssetPath = InAssetPath;
	Result.CreatedItemName = InCreatedItemName;
	return Result;
}

FOliveBlueprintWriteResult FOliveBlueprintWriteResult::SuccessWithNode(const FString& InAssetPath, const FString& InNodeId)
{
	FOliveBlueprintWriteResult Result;
	Result.bSuccess = true;
	Result.AssetPath = InAssetPath;
	Result.CreatedNodeId = InNodeId;
	return Result;
}

FOliveBlueprintWriteResult FOliveBlueprintWriteResult::Error(const FString& ErrorMessage, const FString& InAssetPath)
{
	FOliveBlueprintWriteResult Result;
	Result.bSuccess = false;
	Result.AssetPath = InAssetPath;
	Result.Errors.Add(ErrorMessage);
	return Result;
}

FOliveBlueprintWriteResult FOliveBlueprintWriteResult::FromValidation(const FOliveValidationResult& ValidationResult, const FString& InAssetPath)
{
	FOliveBlueprintWriteResult Result;
	Result.bSuccess = ValidationResult.bValid;
	Result.AssetPath = InAssetPath;

	for (const FOliveIRMessage& Message : ValidationResult.Messages)
	{
		if (Message.Severity == EOliveIRSeverity::Error)
		{
			Result.Errors.Add(Message.Message);
		}
		else if (Message.Severity == EOliveIRSeverity::Warning)
		{
			Result.Warnings.Add(Message.Message);
		}
	}

	return Result;
}

void FOliveBlueprintWriteResult::AddWarning(const FString& Warning)
{
	Warnings.Add(Warning);
}

void FOliveBlueprintWriteResult::AddError(const FString& ErrorMessage)
{
	Errors.Add(ErrorMessage);
	bSuccess = false;
}

TSharedPtr<FJsonObject> FOliveBlueprintWriteResult::ToJson() const
{
	TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject());

	JsonObject->SetBoolField(TEXT("success"), bSuccess);
	JsonObject->SetStringField(TEXT("assetPath"), AssetPath);

	if (!CreatedItemName.IsEmpty())
	{
		JsonObject->SetStringField(TEXT("createdItemName"), CreatedItemName);
	}

	if (!CreatedNodeId.IsEmpty())
	{
		JsonObject->SetStringField(TEXT("createdNodeId"), CreatedNodeId);
	}

	if (CreatedNodeIds.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> NodeIdsArray;
		for (const FString& NodeId : CreatedNodeIds)
		{
			NodeIdsArray.Add(MakeShareable(new FJsonValueString(NodeId)));
		}
		JsonObject->SetArrayField(TEXT("createdNodeIds"), NodeIdsArray);
	}

	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarningsArray;
		for (const FString& Warning : Warnings)
		{
			WarningsArray.Add(MakeShareable(new FJsonValueString(Warning)));
		}
		JsonObject->SetArrayField(TEXT("warnings"), WarningsArray);
	}

	if (Errors.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ErrorsArray;
		for (const FString& Error : Errors)
		{
			ErrorsArray.Add(MakeShareable(new FJsonValueString(Error)));
		}
		JsonObject->SetArrayField(TEXT("errors"), ErrorsArray);
	}

	JsonObject->SetBoolField(TEXT("compileSuccess"), bCompileSuccess);

	if (CompileErrors.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> CompileErrorsArray;
		for (const FString& CompileError : CompileErrors)
		{
			CompileErrorsArray.Add(MakeShareable(new FJsonValueString(CompileError)));
		}
		JsonObject->SetArrayField(TEXT("compileErrors"), CompileErrorsArray);
	}

	return JsonObject;
}

// ============================================================================
// FOliveBlueprintWriter Singleton
// ============================================================================

FOliveBlueprintWriter& FOliveBlueprintWriter::Get()
{
	static FOliveBlueprintWriter Instance;
	return Instance;
}

// ============================================================================
// Asset-Level Operations
// ============================================================================

FOliveBlueprintWriteResult FOliveBlueprintWriter::CreateBlueprint(
	const FString& AssetPath,
	const FString& ParentClass,
	EOliveBlueprintType Type)
{
	if (IsPIEActive())
	{
		return FOliveBlueprintWriteResult::Error(TEXT("Cannot create Blueprints while Play-In-Editor is active"));
	}

	// Find the parent class
	UClass* ParentUClass = FindParentClass(ParentClass);
	if (!ParentUClass)
	{
		UE_LOG(LogOliveBPWriter, Error, TEXT("CreateBlueprint: Parent class '%s' could not be resolved"), *ParentClass);
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Parent class '%s' not found"), *ParentClass));
	}

	// Validate long package path format up front for actionable errors.
	if (!FPackageName::IsValidLongPackageName(AssetPath))
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Invalid Blueprint asset path '%s'. Expected '/Game/.../BP_Name'"), *AssetPath));
	}

	// Parse the asset path
	FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);
	FString AssetName = FPackageName::GetShortName(AssetPath);

	// Validate asset name
	if (AssetName.IsEmpty())
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(
				TEXT("Path '%s' is a folder, not an asset path. "
					 "Append the Blueprint name, e.g. '%s/BP_MyBlueprint'. "
					 "The path must end with the asset name like '/Game/Blueprints/BP_Gun'."),
				*AssetPath, *AssetPath));
	}

	// Check if asset already exists (UObject path = Package.ObjectName)
	FString FullObjectPath = AssetPath + TEXT(".") + AssetName;
	if (FindObject<UBlueprint>(nullptr, *FullObjectPath))
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Blueprint already exists at path: %s"), *AssetPath));
	}

	// Create the package
	UPackage* Package = CreatePackage(*AssetPath);
	if (!Package)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Failed to create package for: %s"), *AssetPath));
	}

	// Use transaction for undo support
	OLIVE_SCOPED_TRANSACTION(FText::Format(
		NSLOCTEXT("OliveBPWriter", "CreateBlueprint", "Create Blueprint '{0}'"),
		FText::FromString(AssetName)));

	// Get the UE Blueprint type
	EBlueprintType UEBPType = GetUEBlueprintType(Type);

	// Create the Blueprint
	UBlueprint* NewBlueprint = FKismetEditorUtilities::CreateBlueprint(
		ParentUClass,
		Package,
		*AssetName,
		UEBPType,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass()
	);

	if (!NewBlueprint)
	{
		UE_LOG(LogOliveBPWriter, Error,
			TEXT("CreateBlueprint failed: path='%s' parent='%s' type=%d package='%s' asset='%s'"),
			*AssetPath, *ParentClass, static_cast<int32>(Type), *PackagePath, *AssetName);
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Failed to create Blueprint: %s"), *AssetPath));
	}

	// Mark the package as dirty
	Package->MarkPackageDirty();

	// Notify asset registry
	FAssetRegistryModule::AssetCreated(NewBlueprint);

	UE_LOG(LogOliveBPWriter, Log, TEXT("Created Blueprint: %s (Parent: %s, Type: %d)"),
		*AssetPath, *ParentClass, static_cast<int32>(Type));

	return FOliveBlueprintWriteResult::Success(AssetPath, AssetName);
}

FOliveBlueprintWriteResult FOliveBlueprintWriter::DeleteBlueprint(const FString& AssetPath)
{
	if (IsPIEActive())
	{
		return FOliveBlueprintWriteResult::Error(TEXT("Cannot delete Blueprints while Play-In-Editor is active"));
	}

	// Load the Blueprint to verify it exists
	FString Error;
	UBlueprint* Blueprint = LoadBlueprintForEditing(AssetPath, Error);
	if (!Blueprint)
	{
		return FOliveBlueprintWriteResult::Error(Error);
	}

	// Use asset tools to delete with proper reference handling
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

	TArray<UObject*> AssetsToDelete;
	AssetsToDelete.Add(Blueprint);

	// This will handle reference cleanup
	int32 DeletedCount = ObjectTools::DeleteObjects(AssetsToDelete, /*bShowConfirmation=*/false);

	if (DeletedCount == 0)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Failed to delete Blueprint: %s (may have references)"), *AssetPath));
	}

	UE_LOG(LogOliveBPWriter, Log, TEXT("Deleted Blueprint: %s"), *AssetPath);

	return FOliveBlueprintWriteResult::Success(AssetPath);
}

FOliveBlueprintWriteResult FOliveBlueprintWriter::DuplicateBlueprint(
	const FString& SourcePath,
	const FString& DestPath)
{
	if (IsPIEActive())
	{
		return FOliveBlueprintWriteResult::Error(TEXT("Cannot duplicate Blueprints while Play-In-Editor is active"));
	}

	// Load the source Blueprint
	FString Error;
	UBlueprint* SourceBlueprint = LoadBlueprintForEditing(SourcePath, Error);
	if (!SourceBlueprint)
	{
		return FOliveBlueprintWriteResult::Error(Error);
	}

	// Parse destination path
	FString DestPackagePath = FPackageName::GetLongPackagePath(DestPath);
	FString DestAssetName = FPackageName::GetShortName(DestPath);

	// Check if destination already exists (UObject path = Package.ObjectName)
	FString FullDestObjectPath = DestPath + TEXT(".") + DestAssetName;
	if (FindObject<UBlueprint>(nullptr, *FullDestObjectPath))
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Blueprint already exists at destination: %s"), *DestPath));
	}

	// Use asset tools to duplicate
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

	UObject* DuplicatedAsset = AssetTools.DuplicateAsset(DestAssetName, DestPackagePath, SourceBlueprint);

	if (!DuplicatedAsset)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Failed to duplicate Blueprint from '%s' to '%s'"), *SourcePath, *DestPath));
	}

	UE_LOG(LogOliveBPWriter, Log, TEXT("Duplicated Blueprint: %s -> %s"), *SourcePath, *DestPath);

	return FOliveBlueprintWriteResult::Success(DestPath, DestAssetName);
}

FOliveBlueprintWriteResult FOliveBlueprintWriter::SetParentClass(
	const FString& AssetPath,
	const FString& NewParentClass)
{
	if (IsPIEActive())
	{
		return FOliveBlueprintWriteResult::Error(TEXT("Cannot modify Blueprints while Play-In-Editor is active"));
	}

	FString Error;
	UBlueprint* Blueprint = LoadBlueprintForEditing(AssetPath, Error);
	if (!Blueprint)
	{
		return FOliveBlueprintWriteResult::Error(Error);
	}

	// Find the new parent class
	UClass* NewParentUClass = FindParentClass(NewParentClass);
	if (!NewParentUClass)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Parent class '%s' not found"), *NewParentClass));
	}

	// Check if current parent is the same
	if (Blueprint->ParentClass == NewParentUClass)
	{
		FOliveBlueprintWriteResult Result = FOliveBlueprintWriteResult::Success(AssetPath);
		Result.AddWarning(TEXT("Blueprint already has this parent class"));
		return Result;
	}

	// Use transaction for undo support
	OLIVE_SCOPED_TRANSACTION(FText::Format(
		NSLOCTEXT("OliveBPWriter", "SetParentClass", "Set Parent Class of '{0}' to '{1}'"),
		FText::FromString(Blueprint->GetName()),
		FText::FromString(NewParentClass)));

	Blueprint->Modify();

	// Change the parent class directly (ReparentBlueprint was removed in UE 5.5)
	Blueprint->ParentClass = NewParentUClass;
	FBlueprintEditorUtils::RefreshAllNodes(Blueprint);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	UE_LOG(LogOliveBPWriter, Log, TEXT("Set parent class of '%s' to '%s'"), *AssetPath, *NewParentClass);

	return FOliveBlueprintWriteResult::Success(AssetPath);
}

FOliveBlueprintWriteResult FOliveBlueprintWriter::AddInterface(
	const FString& AssetPath,
	const FString& InterfacePath)
{
	if (IsPIEActive())
	{
		return FOliveBlueprintWriteResult::Error(TEXT("Cannot modify Blueprints while Play-In-Editor is active"));
	}

	FString Error;
	UBlueprint* Blueprint = LoadBlueprintForEditing(AssetPath, Error);
	if (!Blueprint)
	{
		return FOliveBlueprintWriteResult::Error(Error);
	}

	// Find the interface class
	UClass* InterfaceClass = FindInterfaceClass(InterfacePath);
	if (!InterfaceClass)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Interface '%s' not found. Searched: LoadObject, FindFirstObject, "
				"I/U prefix variants, asset registry. Suggestion: Use the full path "
				"(e.g., '/Game/Interfaces/%s') or search with project.search(query='%s')."),
				*InterfacePath, *InterfacePath, *InterfacePath));
	}

	// Check if it's actually an interface
	if (!InterfaceClass->HasAnyClassFlags(CLASS_Interface))
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("'%s' is not an interface class"), *InterfacePath));
	}

	// Check if interface is already implemented
	for (const FBPInterfaceDescription& ExistingInterface : Blueprint->ImplementedInterfaces)
	{
		if (ExistingInterface.Interface == InterfaceClass)
		{
			FOliveBlueprintWriteResult Result = FOliveBlueprintWriteResult::Success(AssetPath);
			Result.AddWarning(FString::Printf(TEXT("Interface '%s' is already implemented"), *InterfacePath));
			return Result;
		}
	}

	// Pre-flight check: detect function graph name collisions BEFORE adding the interface.
	// ImplementNewInterface auto-creates function graph stubs for each interface function
	// that has FUNC_BlueprintEvent. If a graph with the same name already exists, UE produces
	// a permanent compile error ("Graph named 'X' already exists").
	{
		TArray<FString> ConflictingNames;
		for (TFieldIterator<UFunction> FuncIt(InterfaceClass, EFieldIteratorFlags::IncludeSuper); FuncIt; ++FuncIt)
		{
			UFunction* InterfaceFunc = *FuncIt;
			if (!InterfaceFunc || !InterfaceFunc->HasAnyFunctionFlags(FUNC_BlueprintEvent))
			{
				continue;
			}

			const FName FuncName = InterfaceFunc->GetFName();

			// Check existing function graphs
			bool bConflictFound = false;
			for (UEdGraph* Graph : Blueprint->FunctionGraphs)
			{
				if (Graph && Graph->GetFName() == FuncName)
				{
					ConflictingNames.Add(FuncName.ToString());
					bConflictFound = true;
					break;
				}
			}

			// Also check UbergraphPages (event graphs can have sub-graphs with matching names)
			if (!bConflictFound)
			{
				for (UEdGraph* Graph : Blueprint->UbergraphPages)
				{
					if (Graph && Graph->GetFName() == FuncName)
					{
						ConflictingNames.Add(FuncName.ToString());
						break;
					}
				}
			}
		}

		if (ConflictingNames.Num() > 0)
		{
			return FOliveBlueprintWriteResult::Error(
				FString::Printf(
					TEXT("[INTERFACE_GRAPH_CONFLICT] Cannot add interface '%s': function graph(s) "
						"named '%s' already exist on this Blueprint. Adding the interface would "
						"create duplicate graphs causing permanent compile errors. Remove or rename "
						"the conflicting function(s) first using blueprint.remove_function, then "
						"add the interface."),
					*InterfacePath,
					*FString::Join(ConflictingNames, TEXT("', '"))),
				AssetPath);
		}
	}

	// Use transaction for undo support
	OLIVE_SCOPED_TRANSACTION(FText::Format(
		NSLOCTEXT("OliveBPWriter", "AddInterface", "Add Interface '{0}' to '{1}'"),
		FText::FromString(InterfacePath),
		FText::FromString(Blueprint->GetName())));

	Blueprint->Modify();

	// Add the interface (UE 5.5 requires FTopLevelAssetPath)
	FTopLevelAssetPath InterfaceAssetPath(InterfaceClass->GetPackage()->GetFName(), InterfaceClass->GetFName());
	FBlueprintEditorUtils::ImplementNewInterface(Blueprint, InterfaceAssetPath);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	UE_LOG(LogOliveBPWriter, Log, TEXT("Added interface '%s' to '%s'"), *InterfacePath, *AssetPath);

	return FOliveBlueprintWriteResult::Success(AssetPath, InterfaceClass->GetName());
}

FOliveBlueprintWriteResult FOliveBlueprintWriter::RemoveInterface(
	const FString& AssetPath,
	const FString& InterfacePath)
{
	if (IsPIEActive())
	{
		return FOliveBlueprintWriteResult::Error(TEXT("Cannot modify Blueprints while Play-In-Editor is active"));
	}

	FString Error;
	UBlueprint* Blueprint = LoadBlueprintForEditing(AssetPath, Error);
	if (!Blueprint)
	{
		return FOliveBlueprintWriteResult::Error(Error);
	}

	// Find the interface class
	UClass* InterfaceClass = FindInterfaceClass(InterfacePath);
	if (!InterfaceClass)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Interface '%s' not found. Searched: LoadObject, FindFirstObject, "
				"I/U prefix variants, asset registry. Suggestion: Use the full path "
				"(e.g., '/Game/Interfaces/%s') or search with project.search(query='%s')."),
				*InterfacePath, *InterfacePath, *InterfacePath));
	}

	// Check if interface is implemented
	bool bFound = false;
	for (const FBPInterfaceDescription& ExistingInterface : Blueprint->ImplementedInterfaces)
	{
		if (ExistingInterface.Interface == InterfaceClass)
		{
			bFound = true;
			break;
		}
	}

	if (!bFound)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Interface '%s' is not implemented by '%s'"), *InterfacePath, *AssetPath));
	}

	// Use transaction for undo support
	OLIVE_SCOPED_TRANSACTION(FText::Format(
		NSLOCTEXT("OliveBPWriter", "RemoveInterface", "Remove Interface '{0}' from '{1}'"),
		FText::FromString(InterfacePath),
		FText::FromString(Blueprint->GetName())));

	Blueprint->Modify();

	// Remove the interface (UE 5.5 requires FTopLevelAssetPath)
	FTopLevelAssetPath InterfaceAssetPath(InterfaceClass->GetPackage()->GetFName(), InterfaceClass->GetFName());
	FBlueprintEditorUtils::RemoveInterface(Blueprint, InterfaceAssetPath);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	UE_LOG(LogOliveBPWriter, Log, TEXT("Removed interface '%s' from '%s'"), *InterfacePath, *AssetPath);

	return FOliveBlueprintWriteResult::Success(AssetPath);
}

// ============================================================================
// Blueprint Interface Creation
// ============================================================================

FOliveBlueprintWriteResult FOliveBlueprintWriter::CreateBlueprintInterface(
	const FString& AssetPath,
	const TArray<FOliveIRFunctionSignature>& Functions)
{
	if (IsPIEActive())
	{
		return FOliveBlueprintWriteResult::Error(
			TEXT("Cannot create Blueprints while Play-In-Editor is active"));
	}

	// Validate path format
	if (!FPackageName::IsValidLongPackageName(AssetPath))
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Invalid Blueprint asset path '%s'. "
				"Expected '/Game/.../BPI_Name'"), *AssetPath));
	}

	FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);
	FString AssetName = FPackageName::GetShortName(AssetPath);

	if (AssetName.IsEmpty())
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Path '%s' is a folder, not an asset path. "
				"Append the interface name, e.g. '%s/BPI_Interactable'."),
				*AssetPath, *AssetPath));
	}

	// Check if asset already exists
	FString FullObjectPath = AssetPath + TEXT(".") + AssetName;
	if (FindObject<UBlueprint>(nullptr, *FullObjectPath))
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Blueprint already exists at path: %s"),
				*AssetPath));
	}

	// Must have at least one function
	if (Functions.Num() == 0)
	{
		return FOliveBlueprintWriteResult::Error(
			TEXT("Blueprint Interface must have at least one function. "
				 "Provide a 'functions' array with function definitions."));
	}

	// Create the package
	UPackage* Package = CreatePackage(*AssetPath);
	if (!Package)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Failed to create package for: %s"),
				*AssetPath));
	}

	// Transaction for undo support
	OLIVE_SCOPED_TRANSACTION(FText::Format(
		NSLOCTEXT("OliveBPWriter", "CreateBlueprintInterface",
			"Create Blueprint Interface '{0}'"),
		FText::FromString(AssetName)));

	// Create the Blueprint Interface
	// UInterface::StaticClass() is the parent for all Blueprint Interfaces
	UBlueprint* NewBPI = FKismetEditorUtilities::CreateBlueprint(
		UInterface::StaticClass(),
		Package,
		*AssetName,
		BPTYPE_Interface,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass()
	);

	if (!NewBPI)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Failed to create Blueprint Interface: %s"),
				*AssetPath));
	}

	// Add each function signature to the interface
	TArray<FString> CreatedFunctions;
	TArray<FString> Warnings;

	for (const FOliveIRFunctionSignature& Sig : Functions)
	{
		if (Sig.Name.IsEmpty())
		{
			Warnings.Add(TEXT("Skipped function with empty name"));
			continue;
		}

		// Create a new function graph
		UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
			NewBPI,
			FName(*Sig.Name),
			UEdGraph::StaticClass(),
			UEdGraphSchema_K2::StaticClass()
		);

		if (!FuncGraph)
		{
			Warnings.Add(FString::Printf(
				TEXT("Failed to create function graph for '%s'"),
				*Sig.Name));
			continue;
		}

		// Add to Blueprint's function graphs
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(
			NewBPI, FuncGraph, /*bIsUserCreated=*/true,
			/*SignatureFromObject=*/nullptr);

		// Find entry node to add input parameters
		UK2Node_FunctionEntry* EntryNode = nullptr;
		for (UEdGraphNode* Node : FuncGraph->Nodes)
		{
			EntryNode = Cast<UK2Node_FunctionEntry>(Node);
			if (EntryNode) break;
		}

		if (EntryNode)
		{
			// Add input parameters to the entry node
			for (const FOliveIRFunctionParam& Param : Sig.Inputs)
			{
				FEdGraphPinType PinType = CreatePinTypeFromParam(Param);

				TSharedPtr<FUserPinInfo> NewPinInfo =
					MakeShareable(new FUserPinInfo());
				NewPinInfo->PinName = FName(*Param.Name);
				NewPinInfo->PinType = PinType;
				// Entry node outputs are function inputs
				NewPinInfo->DesiredPinDirection = EGPD_Output;

				EntryNode->UserDefinedPins.Add(NewPinInfo);
			}

			EntryNode->ReconstructNode();
		}

		// Add output parameters via result node
		if (Sig.Outputs.Num() > 0)
		{
			UK2Node_FunctionResult* ResultNode = nullptr;
			for (UEdGraphNode* Node : FuncGraph->Nodes)
			{
				ResultNode = Cast<UK2Node_FunctionResult>(Node);
				if (ResultNode) break;
			}

			// Create result node if not found
			if (!ResultNode)
			{
				FGraphNodeCreator<UK2Node_FunctionResult> Creator(*FuncGraph);
				ResultNode = Creator.CreateNode();
				ResultNode->NodePosX = EntryNode ?
					EntryNode->NodePosX + 400 : 400;
				ResultNode->NodePosY = EntryNode ?
					EntryNode->NodePosY : 0;
				Creator.Finalize();
			}

			for (const FOliveIRFunctionParam& Param : Sig.Outputs)
			{
				FEdGraphPinType PinType = CreatePinTypeFromParam(Param);

				TSharedPtr<FUserPinInfo> NewPinInfo =
					MakeShareable(new FUserPinInfo());
				NewPinInfo->PinName = FName(*Param.Name);
				NewPinInfo->PinType = PinType;
				// Result node inputs are function outputs
				NewPinInfo->DesiredPinDirection = EGPD_Input;

				ResultNode->UserDefinedPins.Add(NewPinInfo);
			}

			ResultNode->ReconstructNode();
		}

		CreatedFunctions.Add(Sig.Name);
	}

	// Mark as structurally modified and notify asset registry
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(NewBPI);
	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewBPI);

	UE_LOG(LogOliveBPWriter, Log,
		TEXT("Created Blueprint Interface: %s with %d functions"),
		*AssetPath, CreatedFunctions.Num());

	FOliveBlueprintWriteResult Result =
		FOliveBlueprintWriteResult::Success(AssetPath, AssetName);
	for (const FString& W : Warnings)
	{
		Result.AddWarning(W);
	}
	return Result;
}

// ============================================================================
// Variable Operations
// ============================================================================

FOliveBlueprintWriteResult FOliveBlueprintWriter::AddVariable(
	const FString& AssetPath,
	const FOliveIRVariable& Variable)
{
	if (IsPIEActive())
	{
		return FOliveBlueprintWriteResult::Error(TEXT("Cannot modify Blueprints while Play-In-Editor is active"));
	}

	FString Error;
	UBlueprint* Blueprint = LoadBlueprintForEditing(AssetPath, Error);
	if (!Blueprint)
	{
		return FOliveBlueprintWriteResult::Error(Error);
	}

	// Validate the operation
	EOliveBlueprintType BPType = FOliveBlueprintTypeDetector::DetectType(Blueprint);
	FOliveValidationResult ValidationResult = FOliveBlueprintConstraints::ValidateAddVariable(BPType, Variable.Name);
	if (!ValidationResult.bValid)
	{
		return FOliveBlueprintWriteResult::FromValidation(ValidationResult, AssetPath);
	}

	// Check for name conflicts
	int32 ExistingIndex = FindVariableIndex(Blueprint, Variable.Name);
	if (ExistingIndex != INDEX_NONE)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Variable '%s' already exists in Blueprint '%s'"), *Variable.Name, *AssetPath));
	}

	// Run deterministic correction rules before type conversion.
	FOliveIRVariable CorrectedVariable = Variable;
	FOliveVariableCorrectionDecision Correction = ApplyVariableCorrectionRules(CorrectedVariable);
	if (Correction.Action == EOliveVariableCorrectionAction::RouteToDispatcher)
	{
		FOliveBlueprintWriteResult DispatcherResult = AddEventDispatcher(AssetPath, CorrectedVariable.Name);
		if (DispatcherResult.bSuccess && !Correction.Message.IsEmpty())
		{
			DispatcherResult.AddWarning(FString::Printf(TEXT("[%s] %s"), *Correction.RuleId, *Correction.Message));
		}
		return DispatcherResult;
	}
	if (Correction.Action == EOliveVariableCorrectionAction::Reject)
	{
		const FString Reason = !Correction.Message.IsEmpty()
			? Correction.Message
			: TEXT("Variable request rejected by correction policy.");
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("[%s] %s"), *Correction.RuleId, *Reason),
			AssetPath);
	}

	// Convert IR type to UE pin type
	FEdGraphPinType PinType = ConvertIRType(CorrectedVariable.Type);
	FString TypeError;
	if (!ValidateVariableTypeForCreation(CorrectedVariable, PinType, TypeError))
	{
		return FOliveBlueprintWriteResult::Error(TypeError);
	}

	// Use transaction for undo support
	OLIVE_SCOPED_TRANSACTION(FText::Format(
		NSLOCTEXT("OliveBPWriter", "AddVariable", "Add Variable '{0}' to '{1}'"),
		FText::FromString(CorrectedVariable.Name),
		FText::FromString(Blueprint->GetName())));

	Blueprint->Modify();

	// Create the variable description
	FBPVariableDescription NewVar;
	NewVar.VarName = FName(*CorrectedVariable.Name);
	NewVar.VarType = PinType;
	NewVar.VarGuid = FGuid::NewGuid();
	NewVar.FriendlyName = CorrectedVariable.Name;
	NewVar.Category = FText::FromString(CorrectedVariable.Category);
	NewVar.DefaultValue = CorrectedVariable.DefaultValue;

	// Set property flags
	EPropertyFlags Flags = CPF_Edit | CPF_BlueprintVisible;

	if (CorrectedVariable.bBlueprintReadWrite)
	{
		// Default is read/write, nothing extra needed
	}
	else
	{
		Flags |= CPF_BlueprintReadOnly;
	}

	if (CorrectedVariable.bExposeOnSpawn)
	{
		Flags |= CPF_ExposeOnSpawn;
	}

	if (CorrectedVariable.bReplicated)
	{
		Flags |= CPF_Net;
	}

	if (CorrectedVariable.bSaveGame)
	{
		Flags |= CPF_SaveGame;
	}

	NewVar.PropertyFlags = Flags;

	// Set metadata
	if (!CorrectedVariable.Description.IsEmpty())
	{
		NewVar.SetMetaData(FBlueprintMetadata::MD_Tooltip, CorrectedVariable.Description);
	}

	// Add to Blueprint
	Blueprint->NewVariables.Add(NewVar);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	UE_LOG(LogOliveBPWriter, Log, TEXT("Added variable '%s' (Type: %s) to '%s'"),
		*CorrectedVariable.Name,
		*CorrectedVariable.Type.GetDisplayName(),
		*AssetPath);

	FOliveBlueprintWriteResult FinalResult = FOliveBlueprintWriteResult::Success(AssetPath, CorrectedVariable.Name);
	if (!Correction.Message.IsEmpty() && !Correction.RuleId.IsEmpty())
	{
		FinalResult.AddWarning(FString::Printf(TEXT("[%s] %s"), *Correction.RuleId, *Correction.Message));
	}
	return FinalResult;
}

FOliveBlueprintWriteResult FOliveBlueprintWriter::RemoveVariable(
	const FString& AssetPath,
	const FString& VariableName)
{
	if (IsPIEActive())
	{
		return FOliveBlueprintWriteResult::Error(TEXT("Cannot modify Blueprints while Play-In-Editor is active"));
	}

	FString Error;
	UBlueprint* Blueprint = LoadBlueprintForEditing(AssetPath, Error);
	if (!Blueprint)
	{
		return FOliveBlueprintWriteResult::Error(Error);
	}

	// Find the variable
	int32 VarIndex = FindVariableIndex(Blueprint, VariableName);
	if (VarIndex == INDEX_NONE)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Variable '%s' not found in Blueprint '%s'"), *VariableName, *AssetPath));
	}

	// Use transaction for undo support
	OLIVE_SCOPED_TRANSACTION(FText::Format(
		NSLOCTEXT("OliveBPWriter", "RemoveVariable", "Remove Variable '{0}' from '{1}'"),
		FText::FromString(VariableName),
		FText::FromString(Blueprint->GetName())));

	Blueprint->Modify();

	// Remove all references to this variable in graphs
	FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, FName(*VariableName));

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	UE_LOG(LogOliveBPWriter, Log, TEXT("Removed variable '%s' from '%s'"), *VariableName, *AssetPath);

	return FOliveBlueprintWriteResult::Success(AssetPath, VariableName);
}

FOliveBlueprintWriteResult FOliveBlueprintWriter::ModifyVariable(
	const FString& AssetPath,
	const FString& VariableName,
	const TMap<FString, FString>& Modifications)
{
	if (IsPIEActive())
	{
		return FOliveBlueprintWriteResult::Error(TEXT("Cannot modify Blueprints while Play-In-Editor is active"));
	}

	FString Error;
	UBlueprint* Blueprint = LoadBlueprintForEditing(AssetPath, Error);
	if (!Blueprint)
	{
		return FOliveBlueprintWriteResult::Error(Error);
	}

	// Find the variable
	int32 VarIndex = FindVariableIndex(Blueprint, VariableName);
	if (VarIndex == INDEX_NONE)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Variable '%s' not found in Blueprint '%s'"), *VariableName, *AssetPath));
	}

	if (Modifications.Num() == 0)
	{
		FOliveBlueprintWriteResult Result = FOliveBlueprintWriteResult::Success(AssetPath, VariableName);
		Result.AddWarning(TEXT("No modifications specified"));
		return Result;
	}

	// Use transaction for undo support
	OLIVE_SCOPED_TRANSACTION(FText::Format(
		NSLOCTEXT("OliveBPWriter", "ModifyVariable", "Modify Variable '{0}' in '{1}'"),
		FText::FromString(VariableName),
		FText::FromString(Blueprint->GetName())));

	Blueprint->Modify();

	FBPVariableDescription& VarDesc = Blueprint->NewVariables[VarIndex];
	FOliveBlueprintWriteResult Result = FOliveBlueprintWriteResult::Success(AssetPath, VariableName);

	// Apply modifications
	for (const TPair<FString, FString>& Mod : Modifications)
	{
		if (Mod.Key == TEXT("Category"))
		{
			VarDesc.Category = FText::FromString(Mod.Value);
		}
		else if (Mod.Key == TEXT("DefaultValue"))
		{
			VarDesc.DefaultValue = Mod.Value;
		}
		else if (Mod.Key == TEXT("Description"))
		{
			VarDesc.SetMetaData(FBlueprintMetadata::MD_Tooltip, Mod.Value);
		}
		else if (Mod.Key == TEXT("bBlueprintReadWrite"))
		{
			bool bReadWrite = Mod.Value.ToBool();
			if (bReadWrite)
			{
				VarDesc.PropertyFlags &= ~CPF_BlueprintReadOnly;
			}
			else
			{
				VarDesc.PropertyFlags |= CPF_BlueprintReadOnly;
			}
		}
		else if (Mod.Key == TEXT("bExposeOnSpawn"))
		{
			if (Mod.Value.ToBool())
			{
				VarDesc.PropertyFlags |= CPF_ExposeOnSpawn;
			}
			else
			{
				VarDesc.PropertyFlags &= ~CPF_ExposeOnSpawn;
			}
		}
		else if (Mod.Key == TEXT("bReplicated"))
		{
			if (Mod.Value.ToBool())
			{
				VarDesc.PropertyFlags |= CPF_Net;
			}
			else
			{
				VarDesc.PropertyFlags &= ~CPF_Net;
			}
		}
		else
		{
			Result.AddWarning(FString::Printf(TEXT("Unknown modification key: '%s'"), *Mod.Key));
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	UE_LOG(LogOliveBPWriter, Log, TEXT("Modified variable '%s' in '%s' (%d changes)"),
		*VariableName, *AssetPath, Modifications.Num());

	return Result;
}

// ============================================================================
// Function Operations
// ============================================================================

FOliveBlueprintWriteResult FOliveBlueprintWriter::AddFunction(
	const FString& AssetPath,
	const FOliveIRFunctionSignature& Signature)
{
	if (IsPIEActive())
	{
		return FOliveBlueprintWriteResult::Error(TEXT("Cannot modify Blueprints while Play-In-Editor is active"));
	}

	FString Error;
	UBlueprint* Blueprint = LoadBlueprintForEditing(AssetPath, Error);
	if (!Blueprint)
	{
		return FOliveBlueprintWriteResult::Error(Error);
	}

	// Validate the operation
	EOliveBlueprintType BPType = FOliveBlueprintTypeDetector::DetectType(Blueprint);
	FOliveValidationResult ValidationResult = FOliveBlueprintConstraints::ValidateAddFunction(BPType, Signature.bIsStatic, Signature.bIsPublic);
	if (!ValidationResult.bValid)
	{
		return FOliveBlueprintWriteResult::FromValidation(ValidationResult, AssetPath);
	}

	// Check for name conflicts
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == Signature.Name)
		{
			return FOliveBlueprintWriteResult::Error(
				FString::Printf(TEXT("Function '%s' already exists in Blueprint '%s'"), *Signature.Name, *AssetPath));
		}
	}

	// Use transaction for undo support
	OLIVE_SCOPED_TRANSACTION(FText::Format(
		NSLOCTEXT("OliveBPWriter", "AddFunction", "Add Function '{0}' to '{1}'"),
		FText::FromString(Signature.Name),
		FText::FromString(Blueprint->GetName())));

	Blueprint->Modify();

	// Create the function graph
	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint,
		FName(*Signature.Name),
		UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass()
	);

	if (!NewGraph)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Failed to create function graph for '%s'"), *Signature.Name));
	}

	// Add the graph to the Blueprint
	FBlueprintEditorUtils::AddFunctionGraph(Blueprint, NewGraph, /*bIsUserCreated=*/true, /*SignatureFromObject=*/(UClass*)nullptr);

	// Find the function entry node to set up parameters
	UK2Node_FunctionEntry* EntryNode = nullptr;
	for (UEdGraphNode* Node : NewGraph->Nodes)
	{
		EntryNode = Cast<UK2Node_FunctionEntry>(Node);
		if (EntryNode)
		{
			break;
		}
	}

	if (EntryNode)
	{
		// Set function flags via metadata
		if (Signature.bIsPure)
		{
			EntryNode->AddExtraFlags(FUNC_BlueprintPure);
		}

		if (Signature.bIsStatic)
		{
			EntryNode->AddExtraFlags(FUNC_Static);
		}

		if (Signature.bIsConst)
		{
			EntryNode->AddExtraFlags(FUNC_Const);
		}

		if (Signature.bCallInEditor)
		{
			EntryNode->MetaData.bCallInEditor = true;
		}

		// Add input parameters
		for (const FOliveIRFunctionParam& Param : Signature.Inputs)
		{
			FEdGraphPinType PinType = CreatePinTypeFromParam(Param);

			TSharedPtr<FUserPinInfo> NewPinInfo = MakeShareable(new FUserPinInfo());
			NewPinInfo->PinName = FName(*Param.Name);
			NewPinInfo->PinType = PinType;
			NewPinInfo->DesiredPinDirection = EGPD_Output; // Entry node outputs are function inputs

			EntryNode->UserDefinedPins.Add(NewPinInfo);
		}

		EntryNode->ReconstructNode();
	}

	// Add output parameters (create result node if needed)
	if (Signature.Outputs.Num() > 0)
	{
		// Find or create result node
		UK2Node_FunctionResult* ResultNode = nullptr;
		for (UEdGraphNode* Node : NewGraph->Nodes)
		{
			ResultNode = Cast<UK2Node_FunctionResult>(Node);
			if (ResultNode)
			{
				break;
			}
		}

		if (!ResultNode)
		{
			// Create result node
			FGraphNodeCreator<UK2Node_FunctionResult> ResultCreator(*NewGraph);
			ResultNode = ResultCreator.CreateNode();
			ResultNode->NodePosX = 400;
			ResultNode->NodePosY = 0;
			ResultCreator.Finalize();
		}

		if (ResultNode)
		{
			for (const FOliveIRFunctionParam& Param : Signature.Outputs)
			{
				FEdGraphPinType PinType = CreatePinTypeFromParam(Param);

				TSharedPtr<FUserPinInfo> NewPinInfo = MakeShareable(new FUserPinInfo());
				NewPinInfo->PinName = FName(*Param.Name);
				NewPinInfo->PinType = PinType;
				NewPinInfo->DesiredPinDirection = EGPD_Input; // Result node inputs are function outputs

				ResultNode->UserDefinedPins.Add(NewPinInfo);
			}

			ResultNode->ReconstructNode();
		}
	}

	// Set category and description
	if (!Signature.Category.IsEmpty() && EntryNode)
	{
		EntryNode->MetaData.Category = FText::FromString(Signature.Category);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	UE_LOG(LogOliveBPWriter, Log, TEXT("Added function '%s' to '%s' (Inputs: %d, Outputs: %d)"),
		*Signature.Name, *AssetPath, Signature.Inputs.Num(), Signature.Outputs.Num());

	return FOliveBlueprintWriteResult::Success(AssetPath, Signature.Name);
}

FOliveBlueprintWriteResult FOliveBlueprintWriter::RemoveFunction(
	const FString& AssetPath,
	const FString& FunctionName)
{
	if (IsPIEActive())
	{
		return FOliveBlueprintWriteResult::Error(TEXT("Cannot modify Blueprints while Play-In-Editor is active"));
	}

	FString Error;
	UBlueprint* Blueprint = LoadBlueprintForEditing(AssetPath, Error);
	if (!Blueprint)
	{
		return FOliveBlueprintWriteResult::Error(Error);
	}

	// Find the function graph
	UEdGraph* FunctionGraph = nullptr;
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == FunctionName)
		{
			FunctionGraph = Graph;
			break;
		}
	}

	if (!FunctionGraph)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Function '%s' not found in Blueprint '%s'"), *FunctionName, *AssetPath));
	}

	// Use transaction for undo support
	OLIVE_SCOPED_TRANSACTION(FText::Format(
		NSLOCTEXT("OliveBPWriter", "RemoveFunction", "Remove Function '{0}' from '{1}'"),
		FText::FromString(FunctionName),
		FText::FromString(Blueprint->GetName())));

	Blueprint->Modify();

	// Remove the function graph
	FBlueprintEditorUtils::RemoveGraph(Blueprint, FunctionGraph);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	UE_LOG(LogOliveBPWriter, Log, TEXT("Removed function '%s' from '%s'"), *FunctionName, *AssetPath);

	return FOliveBlueprintWriteResult::Success(AssetPath, FunctionName);
}

FOliveBlueprintWriteResult FOliveBlueprintWriter::OverrideFunction(
	const FString& AssetPath,
	const FString& FunctionName)
{
	if (IsPIEActive())
	{
		return FOliveBlueprintWriteResult::Error(TEXT("Cannot modify Blueprints while Play-In-Editor is active"));
	}

	FString Error;
	UBlueprint* Blueprint = LoadBlueprintForEditing(AssetPath, Error);
	if (!Blueprint)
	{
		return FOliveBlueprintWriteResult::Error(Error);
	}

	if (!Blueprint->ParentClass)
	{
		return FOliveBlueprintWriteResult::Error(TEXT("Blueprint has no parent class"));
	}

	// Find the function in the parent class
	UFunction* ParentFunction = Blueprint->ParentClass->FindFunctionByName(FName(*FunctionName));

	// If not in parent class, check implemented interfaces
	if (!ParentFunction)
	{
		for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
		{
			if (InterfaceDesc.Interface)
			{
				ParentFunction = InterfaceDesc.Interface->FindFunctionByName(FName(*FunctionName));
				if (ParentFunction)
				{
					break;
				}
			}
		}
	}

	if (!ParentFunction)
	{
		// Build a helpful error message listing where we searched
		FString SearchedLocations = FString::Printf(
			TEXT("Function '%s' not found. Searched: parent class '%s'"),
			*FunctionName, *Blueprint->ParentClass->GetName());

		for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
		{
			if (InterfaceDesc.Interface)
			{
				SearchedLocations += FString::Printf(TEXT(", interface '%s'"),
					*InterfaceDesc.Interface->GetName());
			}
		}

		return FOliveBlueprintWriteResult::Error(SearchedLocations);
	}

	// Check if already overridden
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == FunctionName)
		{
			FOliveBlueprintWriteResult Result = FOliveBlueprintWriteResult::Success(AssetPath, FunctionName);
			Result.AddWarning(FString::Printf(TEXT("Function '%s' is already overridden"), *FunctionName));
			return Result;
		}
	}

	// Use transaction for undo support
	OLIVE_SCOPED_TRANSACTION(FText::Format(
		NSLOCTEXT("OliveBPWriter", "OverrideFunction", "Override Function '{0}' in '{1}'"),
		FText::FromString(FunctionName),
		FText::FromString(Blueprint->GetName())));

	Blueprint->Modify();

	// Create override graph - this handles the event vs function distinction
	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint,
		FName(*FunctionName),
		UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass()
	);

	if (!NewGraph)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Failed to create override graph for '%s'"), *FunctionName));
	}

	// Add as override (bIsUserCreated = false indicates override)
	FBlueprintEditorUtils::AddFunctionGraph(Blueprint, NewGraph, /*bIsUserCreated=*/false, /*SignatureFromObject=*/(UClass*)nullptr);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	UE_LOG(LogOliveBPWriter, Log, TEXT("Created override for function '%s' in '%s'"), *FunctionName, *AssetPath);

	return FOliveBlueprintWriteResult::Success(AssetPath, FunctionName);
}

FOliveBlueprintWriteResult FOliveBlueprintWriter::AddCustomEvent(
	const FString& AssetPath,
	const FString& EventName,
	const TArray<FOliveIRFunctionParam>& Params)
{
	if (IsPIEActive())
	{
		return FOliveBlueprintWriteResult::Error(TEXT("Cannot modify Blueprints while Play-In-Editor is active"));
	}

	FString Error;
	UBlueprint* Blueprint = LoadBlueprintForEditing(AssetPath, Error);
	if (!Blueprint)
	{
		return FOliveBlueprintWriteResult::Error(Error);
	}

	// Validate the operation
	EOliveBlueprintType BPType = FOliveBlueprintTypeDetector::DetectType(Blueprint);
	FOliveValidationResult ValidationResult = FOliveBlueprintConstraints::ValidateAddEventGraph(BPType);
	if (!ValidationResult.bValid)
	{
		return FOliveBlueprintWriteResult::FromValidation(ValidationResult, AssetPath);
	}

	// Get or create the event graph
	UEdGraph* EventGraph = nullptr;
	if (Blueprint->UbergraphPages.Num() > 0)
	{
		EventGraph = Blueprint->UbergraphPages[0];
	}

	if (!EventGraph)
	{
		return FOliveBlueprintWriteResult::Error(TEXT("Blueprint has no event graph"));
	}

	// Check for existing event with same name
	for (UEdGraphNode* Node : EventGraph->Nodes)
	{
		UK2Node_CustomEvent* ExistingEvent = Cast<UK2Node_CustomEvent>(Node);
		if (ExistingEvent && ExistingEvent->CustomFunctionName.ToString() == EventName)
		{
			return FOliveBlueprintWriteResult::Error(
				FString::Printf(TEXT("Custom event '%s' already exists"), *EventName));
		}
	}

	// Use transaction for undo support
	OLIVE_SCOPED_TRANSACTION(FText::Format(
		NSLOCTEXT("OliveBPWriter", "AddCustomEvent", "Add Custom Event '{0}' to '{1}'"),
		FText::FromString(EventName),
		FText::FromString(Blueprint->GetName())));

	Blueprint->Modify();

	// Create the custom event node
	FGraphNodeCreator<UK2Node_CustomEvent> EventCreator(*EventGraph);
	UK2Node_CustomEvent* EventNode = EventCreator.CreateNode();
	EventNode->CustomFunctionName = FName(*EventName);
	EventNode->NodePosX = 0;
	EventNode->NodePosY = 0;

	// Add parameters
	for (const FOliveIRFunctionParam& Param : Params)
	{
		FEdGraphPinType PinType = CreatePinTypeFromParam(Param);

		TSharedPtr<FUserPinInfo> NewPinInfo = MakeShareable(new FUserPinInfo());
		NewPinInfo->PinName = FName(*Param.Name);
		NewPinInfo->PinType = PinType;
		NewPinInfo->DesiredPinDirection = EGPD_Output;

		EventNode->UserDefinedPins.Add(NewPinInfo);
	}

	EventCreator.Finalize();

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	UE_LOG(LogOliveBPWriter, Log, TEXT("Added custom event '%s' to '%s'"), *EventName, *AssetPath);

	return FOliveBlueprintWriteResult::Success(AssetPath, EventName);
}

FOliveBlueprintWriteResult FOliveBlueprintWriter::AddEventDispatcher(
	const FString& AssetPath,
	const FString& DispatcherName,
	const TArray<FOliveIRFunctionParam>& Params)
{
	if (IsPIEActive())
	{
		return FOliveBlueprintWriteResult::Error(TEXT("Cannot modify Blueprints while Play-In-Editor is active"));
	}

	FString Error;
	UBlueprint* Blueprint = LoadBlueprintForEditing(AssetPath, Error);
	if (!Blueprint)
	{
		return FOliveBlueprintWriteResult::Error(Error);
	}

	// Check for existing dispatcher with same name
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (Var.VarName.ToString() == DispatcherName &&
			Var.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
		{
			return FOliveBlueprintWriteResult::Error(
				FString::Printf(TEXT("Event dispatcher '%s' already exists"), *DispatcherName));
		}
	}

	// Use transaction for undo support
	OLIVE_SCOPED_TRANSACTION(FText::Format(
		NSLOCTEXT("OliveBPWriter", "AddEventDispatcher", "Add Event Dispatcher '{0}' to '{1}'"),
		FText::FromString(DispatcherName),
		FText::FromString(Blueprint->GetName())));

	Blueprint->Modify();

	// ====================================================================
	// Step 1: Create the delegate signature graph via AddFunctionGraph.
	// AddFunctionGraph → CreateDefaultNodesForGraph creates a properly
	// initialized UK2Node_FunctionEntry (with FunctionReference set).
	// We then move the graph from FunctionGraphs to DelegateSignatureGraphs.
	// Direct DelegateSignatureGraphs.Add + CreateDefaultNodesForGraph
	// does NOT work — the schema only creates entry nodes for graphs it
	// finds in FunctionGraphs.
	// ====================================================================
	UEdGraph* SigGraph = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint,
		FName(*DispatcherName),
		UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass()
	);

	if (!SigGraph)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Failed to create delegate signature graph for '%s'"), *DispatcherName));
	}

	// AddFunctionGraph creates default nodes (entry + result) with proper init
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, SigGraph, /*bIsUserCreated=*/true, /*SignatureFromObject=*/nullptr);

	// Move from FunctionGraphs to DelegateSignatureGraphs
	Blueprint->FunctionGraphs.Remove(SigGraph);
	Blueprint->DelegateSignatureGraphs.Add(SigGraph);
	SigGraph->bAllowDeletion = false;
	SigGraph->bAllowRenaming = true;

	// ====================================================================
	// Step 2: Find the entry node, add params, remove the result node.
	// Delegate signatures are fire-and-forget — no return value.
	// ====================================================================
	UK2Node_FunctionEntry* EntryNode = nullptr;
	for (UEdGraphNode* Node : SigGraph->Nodes)
	{
		EntryNode = Cast<UK2Node_FunctionEntry>(Node);
		if (EntryNode)
		{
			break;
		}
	}

	// Remove FunctionResult node — delegates don't return values
	for (int32 i = SigGraph->Nodes.Num() - 1; i >= 0; --i)
	{
		if (Cast<UK2Node_FunctionResult>(SigGraph->Nodes[i]))
		{
			SigGraph->RemoveNode(SigGraph->Nodes[i]);
			break;
		}
	}

	if (EntryNode)
	{
		// Add parameters to the delegate signature
		for (const FOliveIRFunctionParam& Param : Params)
		{
			FEdGraphPinType PinType = CreatePinTypeFromParam(Param);

			TSharedPtr<FUserPinInfo> NewPinInfo = MakeShareable(new FUserPinInfo());
			NewPinInfo->PinName = FName(*Param.Name);
			NewPinInfo->PinType = PinType;
			NewPinInfo->DesiredPinDirection = EGPD_Output; // Entry outputs = delegate params

			EntryNode->UserDefinedPins.Add(NewPinInfo);
		}

		EntryNode->ReconstructNode();
	}

	// ====================================================================
	// Step 3: Create the PC_MCDelegate variable.
	// This links the delegate property to the signature graph by name:
	//   Variable "OnBulletHit" -> graph "OnBulletHit" -> compiled function
	//   "OnBulletHit__DelegateSignature"
	// ====================================================================
	FEdGraphPinType DelegatePinType;
	DelegatePinType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;

	FBlueprintEditorUtils::AddMemberVariable(
		Blueprint, FName(*DispatcherName), DelegatePinType);

	// ====================================================================
	// Step 4: Structural modification triggers recompile
	// ====================================================================
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	UE_LOG(LogOliveBPWriter, Log,
		TEXT("Added event dispatcher '%s' to '%s' with %d params"),
		*DispatcherName, *AssetPath, Params.Num());

	return FOliveBlueprintWriteResult::Success(AssetPath, DispatcherName);
}

// ============================================================================
// Compilation and Saving
// ============================================================================

FOliveBlueprintWriteResult FOliveBlueprintWriter::Compile(const FString& AssetPath)
{
	if (IsPIEActive())
	{
		return FOliveBlueprintWriteResult::Error(TEXT("Cannot compile Blueprints while Play-In-Editor is active"));
	}

	FString Error;
	UBlueprint* Blueprint = LoadBlueprintForEditing(AssetPath, Error);
	if (!Blueprint)
	{
		return FOliveBlueprintWriteResult::Error(Error);
	}

	// Compile the Blueprint
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	FOliveBlueprintWriteResult Result = FOliveBlueprintWriteResult::Success(AssetPath);

	// Check compile status
	if (Blueprint->Status == BS_Error)
	{
		Result.bCompileSuccess = false;

		// Extract compile errors from the message log
		// Note: Full error extraction requires hooking into the compiler messages
		// For now, we indicate failure and recommend checking the message log
		Result.CompileErrors.Add(TEXT("Blueprint compilation failed. Check the Output Log for details."));
	}
	else if (Blueprint->Status == BS_UpToDateWithWarnings)
	{
		Result.bCompileSuccess = true;
		Result.AddWarning(TEXT("Blueprint compiled with warnings"));
	}
	else
	{
		Result.bCompileSuccess = true;
	}

	UE_LOG(LogOliveBPWriter, Log, TEXT("Compiled Blueprint '%s' (Status: %d)"),
		*AssetPath, static_cast<int32>(Blueprint->Status));

	return Result;
}

FOliveBlueprintWriteResult FOliveBlueprintWriter::CompileAndSave(const FString& AssetPath)
{
	// First compile
	FOliveBlueprintWriteResult CompileResult = Compile(AssetPath);
	if (!CompileResult.bSuccess)
	{
		return CompileResult;
	}

	// Then save
	FOliveBlueprintWriteResult SaveResult = Save(AssetPath);

	// Merge results
	SaveResult.bCompileSuccess = CompileResult.bCompileSuccess;
	SaveResult.CompileErrors = CompileResult.CompileErrors;
	SaveResult.Warnings.Append(CompileResult.Warnings);

	return SaveResult;
}

FOliveBlueprintWriteResult FOliveBlueprintWriter::Save(const FString& AssetPath)
{
	if (IsPIEActive())
	{
		return FOliveBlueprintWriteResult::Error(TEXT("Cannot save Blueprints while Play-In-Editor is active"));
	}

	FString Error;
	UBlueprint* Blueprint = LoadBlueprintForEditing(AssetPath, Error);
	if (!Blueprint)
	{
		return FOliveBlueprintWriteResult::Error(Error);
	}

	UPackage* Package = Blueprint->GetPackage();
	if (!Package)
	{
		return FOliveBlueprintWriteResult::Error(TEXT("Blueprint has no package"));
	}

	// Get the package filename
	FString PackageFilename;
	if (!FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), PackageFilename, FPackageName::GetAssetPackageExtension()))
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Failed to determine filename for package: %s"), *Package->GetName()));
	}

	// Save the package
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;

	FSavePackageResultStruct SaveResult = UPackage::Save(Package, Blueprint, *PackageFilename, SaveArgs);

	if (SaveResult.Result != ESavePackageResult::Success)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Failed to save Blueprint: %s"), *AssetPath));
	}

	UE_LOG(LogOliveBPWriter, Log, TEXT("Saved Blueprint: %s"), *AssetPath);

	return FOliveBlueprintWriteResult::Success(AssetPath);
}

// ============================================================================
// Private Helper Methods
// ============================================================================

UBlueprint* FOliveBlueprintWriter::LoadBlueprintForEditing(const FString& AssetPath, FString& OutError)
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

void FOliveBlueprintWriter::MarkDirty(UBlueprint* Blueprint)
{
	if (Blueprint)
	{
		Blueprint->MarkPackageDirty();
	}
}

FOliveValidationResult FOliveBlueprintWriter::ValidateOperation(
	const FString& Operation,
	const UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Params)
{
	FOliveValidationResult Result;

	if (!Blueprint)
	{
		Result.AddError(TEXT("INVALID_BLUEPRINT"), TEXT("Blueprint is null"));
		return Result;
	}

	// Use the validation engine for standard checks
	return FOliveValidationEngine::Get().ValidateOperation(Operation, Params, const_cast<UBlueprint*>(Blueprint));
}

bool FOliveBlueprintWriter::ParseNestedIRType(const FString& JsonString, FOliveIRType& OutType) const
{
	OutType = FOliveIRType();
	if (JsonString.IsEmpty())
	{
		return false;
	}

	TSharedPtr<FJsonObject> TypeJson;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (FJsonSerializer::Deserialize(Reader, TypeJson) && TypeJson.IsValid())
	{
		OutType = FOliveIRType::FromJson(TypeJson);
		return OutType.Category != EOliveIRTypeCategory::Unknown;
	}

	const FString Lower = JsonString.ToLower().TrimStartAndEnd();
	if (Lower == TEXT("bool")) OutType.Category = EOliveIRTypeCategory::Bool;
	else if (Lower == TEXT("byte")) OutType.Category = EOliveIRTypeCategory::Byte;
	else if (Lower == TEXT("int")) OutType.Category = EOliveIRTypeCategory::Int;
	else if (Lower == TEXT("int64")) OutType.Category = EOliveIRTypeCategory::Int64;
	else if (Lower == TEXT("float")) OutType.Category = EOliveIRTypeCategory::Float;
	else if (Lower == TEXT("double")) OutType.Category = EOliveIRTypeCategory::Double;
	else if (Lower == TEXT("string")) OutType.Category = EOliveIRTypeCategory::String;
	else if (Lower == TEXT("name")) OutType.Category = EOliveIRTypeCategory::Name;
	else if (Lower == TEXT("text")) OutType.Category = EOliveIRTypeCategory::Text;
	else if (Lower == TEXT("vector")) OutType.Category = EOliveIRTypeCategory::Vector;
	else if (Lower == TEXT("vector2d")) OutType.Category = EOliveIRTypeCategory::Vector2D;
	else if (Lower == TEXT("rotator")) OutType.Category = EOliveIRTypeCategory::Rotator;
	else if (Lower == TEXT("transform")) OutType.Category = EOliveIRTypeCategory::Transform;
	else if (Lower == TEXT("color")) OutType.Category = EOliveIRTypeCategory::Color;
	else if (Lower == TEXT("linear_color") || Lower == TEXT("linearcolor")) OutType.Category = EOliveIRTypeCategory::LinearColor;
	else return false;

	return true;
}

bool FOliveBlueprintWriter::ValidateVariableTypeForCreation(
	const FOliveIRVariable& Variable,
	const FEdGraphPinType& PinType,
	FString& OutError) const
{
	OutError.Reset();

	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard
		|| PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
	{
		OutError = FString::Printf(
			TEXT("Unsupported or unresolved variable type '%s' for variable '%s'"),
			*Variable.Type.GetDisplayName(),
			*Variable.Name);
		return false;
	}

	const bool bNeedsSubType =
		Variable.Type.Category == EOliveIRTypeCategory::Object
		|| Variable.Type.Category == EOliveIRTypeCategory::Class
		|| Variable.Type.Category == EOliveIRTypeCategory::Interface
		|| Variable.Type.Category == EOliveIRTypeCategory::Struct
		|| Variable.Type.Category == EOliveIRTypeCategory::Enum;

	if (bNeedsSubType && PinType.PinSubCategoryObject == nullptr)
	{
		FString NameHint;
		if (!Variable.Type.ClassName.IsEmpty()) NameHint = Variable.Type.ClassName;
		else if (!Variable.Type.StructName.IsEmpty()) NameHint = Variable.Type.StructName;
		else if (!Variable.Type.EnumName.IsEmpty()) NameHint = Variable.Type.EnumName;
		else NameHint = TEXT("(empty)");

		OutError = FString::Printf(
			TEXT("Type resolution failed for variable '%s': could not find '%s'. "
				 "Use UE class names without prefix: 'Actor' not 'AActor', "
				 "'StaticMeshComponent' not 'UStaticMeshComponent'. "
				 "For TSubclassOf<Actor>, use category:'class' + class_name:'Actor'."),
			*Variable.Name,
			*NameHint);
		return false;
	}

	if (PinType.ContainerType == EPinContainerType::Map
		&& PinType.PinValueType.TerminalCategory == NAME_None)
	{
		OutError = FString::Printf(
			TEXT("Map value type resolution failed for variable '%s'."),
			*Variable.Name);
		return false;
	}

	return true;
}

FOliveBlueprintWriter::FOliveVariableCorrectionDecision FOliveBlueprintWriter::ApplyVariableCorrectionRules(
	FOliveIRVariable& Variable) const
{
	// Rule: multicast delegate member variables must be created as dispatchers.
	if (Variable.Type.Category == EOliveIRTypeCategory::MulticastDelegate)
	{
		FOliveVariableCorrectionDecision Decision;
		Decision.Action = EOliveVariableCorrectionAction::RouteToDispatcher;
		Decision.RuleId = TEXT("RULE_DISPATCHER_ROUTE");
		Decision.Message = TEXT("Multicast delegate variable routed to event dispatcher creation to guarantee signature integrity.");
		return Decision;
	}

	// Rule: single-cast delegate variables are not supported by current writer path.
	if (Variable.Type.Category == EOliveIRTypeCategory::Delegate)
	{
		FOliveVariableCorrectionDecision Decision;
		Decision.Action = EOliveVariableCorrectionAction::RouteToDispatcher;
		Decision.RuleId = TEXT("RULE_DELEGATE_COERCE_TO_DISPATCHER");
		Decision.Message = TEXT("Single-cast delegate request coerced to multicast event dispatcher for Blueprint safety.");
		return Decision;
	}

	// Rule: unknown/wildcard/exec variable types are coerced to string.
	if (Variable.Type.Category == EOliveIRTypeCategory::Unknown
		|| Variable.Type.Category == EOliveIRTypeCategory::Wildcard
		|| Variable.Type.Category == EOliveIRTypeCategory::Exec)
	{
		Variable.Type.Category = EOliveIRTypeCategory::String;
		FOliveVariableCorrectionDecision Decision;
		Decision.Action = EOliveVariableCorrectionAction::Continue;
		Decision.RuleId = TEXT("RULE_COERCE_TO_STRING");
		Decision.Message = TEXT("Unsupported variable type coerced to string.");
		return Decision;
	}

	return FOliveVariableCorrectionDecision();
}

UClass* FOliveBlueprintWriter::ResolveClassByName(const FString& ClassName)
{
	// Delegate to the shared class resolver which handles native classes,
	// Blueprint generated classes (BP_Gun, BP_Gun_C), full asset paths
	// (/Game/Blueprints/BP_Gun), and asset registry search — with LRU cache.
	FOliveClassResolveResult Result = FOliveClassResolver::Resolve(ClassName);
	return Result.Class;
}

FEdGraphPinType FOliveBlueprintWriter::ConvertIRType(const FOliveIRType& IRType)
{
	FEdGraphPinType PinType;

	// Map IR category to UE pin category
	switch (IRType.Category)
	{
	case EOliveIRTypeCategory::Bool:
		PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		break;
	case EOliveIRTypeCategory::Byte:
		PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
		break;
	case EOliveIRTypeCategory::Int:
		PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
		break;
	case EOliveIRTypeCategory::Int64:
		PinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
		break;
	case EOliveIRTypeCategory::Float:
		// UE 5.5+: Float/Double use PC_Real as PinCategory with PC_Float/PC_Double as PinSubCategory.
		// Using PC_Float directly as PinCategory causes "Can't parse default value" warnings
		// because the engine cannot resolve a FProperty from a bare PC_Float category.
		PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		PinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
		break;
	case EOliveIRTypeCategory::Double:
		PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		PinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		break;
	case EOliveIRTypeCategory::String:
		PinType.PinCategory = UEdGraphSchema_K2::PC_String;
		break;
	case EOliveIRTypeCategory::Name:
		PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
		break;
	case EOliveIRTypeCategory::Text:
		PinType.PinCategory = UEdGraphSchema_K2::PC_Text;
		break;
	case EOliveIRTypeCategory::Vector:
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
		break;
	case EOliveIRTypeCategory::Vector2D:
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FVector2D>::Get();
		break;
	case EOliveIRTypeCategory::Rotator:
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
		break;
	case EOliveIRTypeCategory::Transform:
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
		break;
	case EOliveIRTypeCategory::Color:
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FColor>::Get();
		break;
	case EOliveIRTypeCategory::LinearColor:
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FLinearColor>::Get();
		break;
	case EOliveIRTypeCategory::Object:
		PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
		if (!IRType.ClassName.IsEmpty())
		{
			UClass* Class = ResolveClassByName(IRType.ClassName);
			if (Class)
			{
				PinType.PinSubCategoryObject = Class;
			}
		}
		break;
	case EOliveIRTypeCategory::Class:
		PinType.PinCategory = UEdGraphSchema_K2::PC_Class;
		if (!IRType.ClassName.IsEmpty())
		{
			UClass* Class = ResolveClassByName(IRType.ClassName);
			if (Class)
			{
				PinType.PinSubCategoryObject = Class;
			}
		}
		break;
	case EOliveIRTypeCategory::Interface:
		PinType.PinCategory = UEdGraphSchema_K2::PC_Interface;
		if (!IRType.ClassName.IsEmpty())
		{
			UClass* Class = ResolveClassByName(IRType.ClassName);
			if (Class)
			{
				PinType.PinSubCategoryObject = Class;
			}
		}
		break;
	case EOliveIRTypeCategory::Struct:
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		if (!IRType.StructName.IsEmpty())
		{
			UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, *IRType.StructName);
			if (!Struct)
			{
				Struct = FindFirstObject<UScriptStruct>( *IRType.StructName);
			}
			if (Struct)
			{
				PinType.PinSubCategoryObject = Struct;
			}
		}
		break;
	case EOliveIRTypeCategory::Enum:
		PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
		if (!IRType.EnumName.IsEmpty())
		{
			UEnum* Enum = FindObject<UEnum>(nullptr, *IRType.EnumName);
			if (!Enum)
			{
				Enum = FindFirstObject<UEnum>( *IRType.EnumName);
			}
			if (Enum)
			{
				PinType.PinSubCategoryObject = Enum;
			}
		}
		break;
	case EOliveIRTypeCategory::Delegate:
		PinType.PinCategory = UEdGraphSchema_K2::PC_Delegate;
		break;
	case EOliveIRTypeCategory::MulticastDelegate:
		PinType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
		break;
	case EOliveIRTypeCategory::Array:
	{
		FOliveIRType ElementType;
		if (!ParseNestedIRType(IRType.ElementTypeJson, ElementType))
		{
			ElementType.Category = EOliveIRTypeCategory::String;
		}
		FEdGraphPinType ElementPinType = ConvertIRType(ElementType);
		PinType = ElementPinType;
		PinType.ContainerType = EPinContainerType::Array;
		PinType.PinValueType.TerminalCategory = NAME_None;
		PinType.PinValueType.TerminalSubCategory = NAME_None;
		PinType.PinValueType.TerminalSubCategoryObject = nullptr;
		break;
	}
	case EOliveIRTypeCategory::Set:
	{
		FOliveIRType ElementType;
		if (!ParseNestedIRType(IRType.ElementTypeJson, ElementType))
		{
			ElementType.Category = EOliveIRTypeCategory::String;
		}
		FEdGraphPinType ElementPinType = ConvertIRType(ElementType);
		PinType = ElementPinType;
		PinType.ContainerType = EPinContainerType::Set;
		PinType.PinValueType.TerminalCategory = NAME_None;
		PinType.PinValueType.TerminalSubCategory = NAME_None;
		PinType.PinValueType.TerminalSubCategoryObject = nullptr;
		break;
	}
	case EOliveIRTypeCategory::Map:
	{
		FOliveIRType KeyType;
		FOliveIRType ValueType;
		if (!ParseNestedIRType(IRType.KeyTypeJson, KeyType))
		{
			KeyType.Category = EOliveIRTypeCategory::String;
		}
		if (!ParseNestedIRType(IRType.ValueTypeJson, ValueType))
		{
			ValueType.Category = EOliveIRTypeCategory::String;
		}

		FEdGraphPinType KeyPinType = ConvertIRType(KeyType);
		FEdGraphPinType ValuePinType = ConvertIRType(ValueType);

		PinType = KeyPinType;
		PinType.ContainerType = EPinContainerType::Map;
		PinType.PinValueType.TerminalCategory = ValuePinType.PinCategory;
		PinType.PinValueType.TerminalSubCategory = ValuePinType.PinSubCategory;
		PinType.PinValueType.TerminalSubCategoryObject = ValuePinType.PinSubCategoryObject;
		break;
	}
	case EOliveIRTypeCategory::Exec:
		PinType.PinCategory = UEdGraphSchema_K2::PC_Exec;
		break;
	default:
		// Default to wildcard for unknown types
		PinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
		break;
	}

	PinType.bIsReference = IRType.bIsReference;
	PinType.bIsConst = IRType.bIsConst;

	return PinType;
}

UClass* FOliveBlueprintWriter::FindParentClass(const FString& ClassName)
{
	const FString Normalized = ClassName.TrimStartAndEnd();
	if (Normalized.IsEmpty())
	{
		return nullptr;
	}

	FOliveClassResolveResult Result = FOliveClassResolver::Resolve(Normalized);
	if (Result.IsValid())
	{
		return Result.Class;
	}

	return nullptr;
}

UClass* FOliveBlueprintWriter::FindInterfaceClass(const FString& InterfacePath)
{
	// Try as Blueprint interface first
	if (InterfacePath.Contains(TEXT("/")))
	{
		UBlueprint* InterfaceBP = LoadObject<UBlueprint>(nullptr, *InterfacePath);
		if (InterfaceBP && InterfaceBP->GeneratedClass)
		{
			return InterfaceBP->GeneratedClass;
		}
	}

	// Try as native interface
	UClass* Class = FindFirstObject<UClass>( *InterfacePath);
	if (Class)
	{
		return Class;
	}

	// Try with I prefix
	FString PrefixedName = TEXT("I") + InterfacePath;
	Class = FindFirstObject<UClass>( *PrefixedName);
	if (Class)
	{
		return Class;
	}

	// Try with U prefix (some interfaces are UInterface-derived)
	PrefixedName = TEXT("U") + InterfacePath;
	Class = FindFirstObject<UClass>( *PrefixedName);
	if (Class)
	{
		return Class;
	}

	// Asset registry fallback: search for Blueprint interface by short name
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		TArray<FAssetData> Assets;
		AssetRegistry.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), Assets);

		for (const FAssetData& Asset : Assets)
		{
			if (Asset.AssetName.ToString() == InterfacePath ||
				Asset.AssetName.ToString() == (TEXT("BPI_") + InterfacePath))
			{
				UBlueprint* InterfaceBP = Cast<UBlueprint>(Asset.GetAsset());
				if (InterfaceBP && InterfaceBP->GeneratedClass &&
					InterfaceBP->GeneratedClass->HasAnyClassFlags(CLASS_Interface))
				{
					Class = InterfaceBP->GeneratedClass;
					UE_LOG(LogOliveBPWriter, Log,
						TEXT("FindInterfaceClass: Resolved short name '%s' to '%s' via asset registry"),
						*InterfacePath, *Asset.GetObjectPathString());
					break;
				}
			}
		}
	}

	return Class;
}

int32 FOliveBlueprintWriter::FindVariableIndex(const UBlueprint* Blueprint, const FString& VariableName)
{
	if (!Blueprint)
	{
		return INDEX_NONE;
	}

	for (int32 i = 0; i < Blueprint->NewVariables.Num(); ++i)
	{
		if (Blueprint->NewVariables[i].VarName.ToString() == VariableName)
		{
			return i;
		}
	}

	return INDEX_NONE;
}

FEdGraphPinType FOliveBlueprintWriter::CreatePinTypeFromParam(const FOliveIRFunctionParam& Param)
{
	FEdGraphPinType PinType = ConvertIRType(Param.Type);
	PinType.bIsReference = Param.bIsReference;
	return PinType;
}

bool FOliveBlueprintWriter::IsPIEActive() const
{
	return GEditor && GEditor->PlayWorld != nullptr;
}

EBlueprintType FOliveBlueprintWriter::GetUEBlueprintType(EOliveBlueprintType BPType) const
{
	switch (BPType)
	{
	case EOliveBlueprintType::Normal:
		return BPTYPE_Normal;
	case EOliveBlueprintType::Interface:
		return BPTYPE_Interface;
	case EOliveBlueprintType::FunctionLibrary:
		return BPTYPE_FunctionLibrary;
	case EOliveBlueprintType::MacroLibrary:
		return BPTYPE_MacroLibrary;
	default:
		return BPTYPE_Normal;
	}
}
