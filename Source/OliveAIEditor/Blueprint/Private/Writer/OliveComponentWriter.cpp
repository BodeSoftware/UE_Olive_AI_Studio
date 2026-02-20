// Copyright Bode Software. All Rights Reserved.

#include "OliveComponentWriter.h"
#include "OliveBlueprintWriter.h"
#include "OliveBlueprintTypes.h"
#include "Services/OliveTransactionManager.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/PropertyPortFlags.h"
#include "Editor.h"

// ============================================================================
// FOliveComponentWriter Singleton
// ============================================================================

FOliveComponentWriter& FOliveComponentWriter::Get()
{
	static FOliveComponentWriter Instance;
	return Instance;
}

// ============================================================================
// Component Operations
// ============================================================================

FOliveBlueprintWriteResult FOliveComponentWriter::AddComponent(
	const FString& AssetPath,
	const FString& ComponentClass,
	const FString& ComponentName,
	const FString& ParentComponentName)
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

	// Validate that Blueprint supports components
	if (!ValidateCanHaveComponents(Blueprint, Error))
	{
		return FOliveBlueprintWriteResult::Error(Error);
	}

	// Validate the operation using type detector
	EOliveBlueprintType BPType = FOliveBlueprintTypeDetector::DetectType(Blueprint);
	FOliveValidationResult ValidationResult = FOliveBlueprintConstraints::ValidateAddComponent(BPType, ComponentClass);
	if (!ValidationResult.bValid)
	{
		return FOliveBlueprintWriteResult::FromValidation(ValidationResult, AssetPath);
	}

	// Find the component class
	UClass* CompClass = FindComponentClass(ComponentClass);
	if (!CompClass)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Component class '%s' not found"), *ComponentClass));
	}

	// Verify it's an actor component
	if (!CompClass->IsChildOf(UActorComponent::StaticClass()))
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("'%s' is not an ActorComponent class"), *ComponentClass));
	}

	// Get or create the SCS
	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		// Create SCS if needed
		SCS = NewObject<USimpleConstructionScript>(Blueprint);
		Blueprint->SimpleConstructionScript = SCS;
	}

	// Generate unique name if needed
	FString FinalName = GenerateUniqueComponentName(Blueprint, ComponentName);

	// Find parent node if specified
	USCS_Node* ParentNode = nullptr;
	if (!ParentComponentName.IsEmpty())
	{
		ParentNode = FindSCSNode(Blueprint, ParentComponentName);
		if (!ParentNode)
		{
			return FOliveBlueprintWriteResult::Error(
				FString::Printf(TEXT("Parent component '%s' not found"), *ParentComponentName));
		}

		// Verify parent is a scene component (can have children)
		if (!ParentNode->ComponentClass->IsChildOf(USceneComponent::StaticClass()))
		{
			return FOliveBlueprintWriteResult::Error(
				FString::Printf(TEXT("Parent component '%s' is not a SceneComponent and cannot have children"), *ParentComponentName));
		}
	}

	// Use transaction for undo support
	OLIVE_SCOPED_TRANSACTION(FText::Format(
		NSLOCTEXT("OliveCompWriter", "AddComponent", "Add Component '{0}' to '{1}'"),
		FText::FromString(FinalName),
		FText::FromString(Blueprint->GetName())));

	Blueprint->Modify();

	// Create the new SCS node
	USCS_Node* NewNode = SCS->CreateNode(CompClass, FName(*FinalName));

	if (!NewNode)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Failed to create component node '%s'"), *FinalName));
	}

	// Add to parent or as root
	if (ParentNode)
	{
		ParentNode->AddChildNode(NewNode);
	}
	else
	{
		// Check if this is a scene component
		if (CompClass->IsChildOf(USceneComponent::StaticClass()))
		{
			// If there's no root, this becomes the root
			if (SCS->GetRootNodes().Num() == 0)
			{
				SCS->AddNode(NewNode);
			}
			else
			{
				// Attach to the default scene root
				USCS_Node* DefaultRoot = SCS->GetDefaultSceneRootNode();
				if (DefaultRoot)
				{
					DefaultRoot->AddChildNode(NewNode);
				}
				else
				{
					// Add as another root node
					SCS->AddNode(NewNode);
				}
			}
		}
		else
		{
			// Non-scene components are added directly
			SCS->AddNode(NewNode);
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	UE_LOG(LogOliveBPWriter, Log, TEXT("Added component '%s' (Class: %s) to '%s'"),
		*FinalName, *ComponentClass, *AssetPath);

	return FOliveBlueprintWriteResult::Success(AssetPath, FinalName);
}

FOliveBlueprintWriteResult FOliveComponentWriter::RemoveComponent(
	const FString& AssetPath,
	const FString& ComponentName)
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

	// Find the component node
	USCS_Node* NodeToRemove = FindSCSNode(Blueprint, ComponentName);
	if (!NodeToRemove)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Component '%s' not found in Blueprint '%s'"), *ComponentName, *AssetPath));
	}

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		return FOliveBlueprintWriteResult::Error(TEXT("Blueprint has no SimpleConstructionScript"));
	}

	// Use transaction for undo support
	OLIVE_SCOPED_TRANSACTION(FText::Format(
		NSLOCTEXT("OliveCompWriter", "RemoveComponent", "Remove Component '{0}' from '{1}'"),
		FText::FromString(ComponentName),
		FText::FromString(Blueprint->GetName())));

	Blueprint->Modify();

	// Reparent children before removing (move them to the parent of the removed node)
	TArray<USCS_Node*> ChildNodes = NodeToRemove->GetChildNodes();

	// Find parent of the node being removed
	USCS_Node* ParentNode = nullptr;
	for (USCS_Node* RootNode : SCS->GetRootNodes())
	{
		if (RootNode == NodeToRemove)
		{
			// It's a root node, no parent
			break;
		}

		// Search recursively
		TArray<USCS_Node*> NodesToSearch;
		NodesToSearch.Add(RootNode);

		while (NodesToSearch.Num() > 0)
		{
			USCS_Node* CurrentNode = NodesToSearch.Pop();
			for (USCS_Node* Child : CurrentNode->GetChildNodes())
			{
				if (Child == NodeToRemove)
				{
					ParentNode = CurrentNode;
					break;
				}
				NodesToSearch.Add(Child);
			}
			if (ParentNode)
			{
				break;
			}
		}

		if (ParentNode)
		{
			break;
		}
	}

	// Move children to parent (or make them roots)
	for (USCS_Node* ChildNode : ChildNodes)
	{
		NodeToRemove->RemoveChildNode(ChildNode);

		if (ParentNode)
		{
			ParentNode->AddChildNode(ChildNode);
		}
		else
		{
			SCS->AddNode(ChildNode);
		}
	}

	// Remove the node and associated variable
	SCS->RemoveNode(NodeToRemove);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	UE_LOG(LogOliveBPWriter, Log, TEXT("Removed component '%s' from '%s'"), *ComponentName, *AssetPath);

	return FOliveBlueprintWriteResult::Success(AssetPath, ComponentName);
}

FOliveBlueprintWriteResult FOliveComponentWriter::ModifyComponent(
	const FString& AssetPath,
	const FString& ComponentName,
	const TMap<FString, FString>& Properties)
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

	// Find the component node
	USCS_Node* Node = FindSCSNode(Blueprint, ComponentName);
	if (!Node)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Component '%s' not found in Blueprint '%s'"), *ComponentName, *AssetPath));
	}

	if (Properties.Num() == 0)
	{
		FOliveBlueprintWriteResult Result = FOliveBlueprintWriteResult::Success(AssetPath, ComponentName);
		Result.AddWarning(TEXT("No properties specified to modify"));
		return Result;
	}

	// Get the component template
	UActorComponent* ComponentTemplate = Node->ComponentTemplate;
	if (!ComponentTemplate)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Component '%s' has no template"), *ComponentName));
	}

	// Use transaction for undo support
	OLIVE_SCOPED_TRANSACTION(FText::Format(
		NSLOCTEXT("OliveCompWriter", "ModifyComponent", "Modify Component '{0}' in '{1}'"),
		FText::FromString(ComponentName),
		FText::FromString(Blueprint->GetName())));

	Blueprint->Modify();
	ComponentTemplate->Modify();

	FOliveBlueprintWriteResult Result = FOliveBlueprintWriteResult::Success(AssetPath, ComponentName);
	int32 SuccessCount = 0;

	// Apply each property modification
	for (const TPair<FString, FString>& Prop : Properties)
	{
		FString PropError;
		if (SetComponentProperty(ComponentTemplate, Prop.Key, Prop.Value, PropError))
		{
			SuccessCount++;
		}
		else
		{
			Result.AddWarning(FString::Printf(TEXT("Failed to set '%s': %s"), *Prop.Key, *PropError));
		}
	}

	if (SuccessCount == 0)
	{
		Result.bSuccess = false;
		Result.AddError(TEXT("No properties were successfully modified"));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	UE_LOG(LogOliveBPWriter, Log, TEXT("Modified component '%s' in '%s' (%d/%d properties set)"),
		*ComponentName, *AssetPath, SuccessCount, Properties.Num());

	return Result;
}

FOliveBlueprintWriteResult FOliveComponentWriter::ReparentComponent(
	const FString& AssetPath,
	const FString& ComponentName,
	const FString& NewParentName)
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

	// Find the component to reparent
	USCS_Node* NodeToMove = FindSCSNode(Blueprint, ComponentName);
	if (!NodeToMove)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Component '%s' not found"), *ComponentName));
	}

	// Check if the component is a scene component (only scene components can be reparented)
	if (!NodeToMove->ComponentClass->IsChildOf(USceneComponent::StaticClass()))
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Component '%s' is not a SceneComponent and cannot be reparented"), *ComponentName));
	}

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		return FOliveBlueprintWriteResult::Error(TEXT("Blueprint has no SimpleConstructionScript"));
	}

	// Find the new parent (or nullptr for root)
	USCS_Node* NewParentNode = nullptr;
	if (!NewParentName.IsEmpty())
	{
		NewParentNode = FindSCSNode(Blueprint, NewParentName);
		if (!NewParentNode)
		{
			return FOliveBlueprintWriteResult::Error(
				FString::Printf(TEXT("New parent component '%s' not found"), *NewParentName));
		}

		// Verify new parent is a scene component
		if (!NewParentNode->ComponentClass->IsChildOf(USceneComponent::StaticClass()))
		{
			return FOliveBlueprintWriteResult::Error(
				FString::Printf(TEXT("New parent '%s' is not a SceneComponent"), *NewParentName));
		}

		// Check for circular parenting
		USCS_Node* CheckNode = NewParentNode;
		while (CheckNode)
		{
			if (CheckNode == NodeToMove)
			{
				return FOliveBlueprintWriteResult::Error(
					TEXT("Cannot reparent: would create circular hierarchy"));
			}

			// Find parent of CheckNode
			CheckNode = nullptr;
			for (USCS_Node* RootNode : SCS->GetRootNodes())
			{
				TArray<USCS_Node*> ToSearch;
				ToSearch.Add(RootNode);
				while (ToSearch.Num() > 0)
				{
					USCS_Node* Current = ToSearch.Pop();
					for (USCS_Node* Child : Current->GetChildNodes())
					{
						if (Child == NewParentNode)
						{
							CheckNode = Current;
							break;
						}
						ToSearch.Add(Child);
					}
					if (CheckNode)
					{
						break;
					}
				}
				if (CheckNode)
				{
					break;
				}
			}
		}
	}

	// Find current parent
	USCS_Node* CurrentParent = nullptr;
	for (USCS_Node* RootNode : SCS->GetRootNodes())
	{
		if (RootNode == NodeToMove)
		{
			break; // Already a root
		}

		TArray<USCS_Node*> ToSearch;
		ToSearch.Add(RootNode);
		while (ToSearch.Num() > 0)
		{
			USCS_Node* Current = ToSearch.Pop();
			for (USCS_Node* Child : Current->GetChildNodes())
			{
				if (Child == NodeToMove)
				{
					CurrentParent = Current;
					break;
				}
				ToSearch.Add(Child);
			}
			if (CurrentParent)
			{
				break;
			}
		}
		if (CurrentParent)
		{
			break;
		}
	}

	// Use transaction for undo support
	OLIVE_SCOPED_TRANSACTION(FText::Format(
		NSLOCTEXT("OliveCompWriter", "ReparentComponent", "Reparent Component '{0}' in '{1}'"),
		FText::FromString(ComponentName),
		FText::FromString(Blueprint->GetName())));

	Blueprint->Modify();

	// Remove from current parent
	if (CurrentParent)
	{
		CurrentParent->RemoveChildNode(NodeToMove);
	}
	else
	{
		// Was a root node
		SCS->RemoveNode(NodeToMove);
	}

	// Add to new parent
	if (NewParentNode)
	{
		NewParentNode->AddChildNode(NodeToMove);
	}
	else
	{
		// Become a root
		SCS->AddNode(NodeToMove);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	UE_LOG(LogOliveBPWriter, Log, TEXT("Reparented component '%s' to '%s' in '%s'"),
		*ComponentName,
		NewParentName.IsEmpty() ? TEXT("(root)") : *NewParentName,
		*AssetPath);

	return FOliveBlueprintWriteResult::Success(AssetPath, ComponentName);
}

FOliveBlueprintWriteResult FOliveComponentWriter::SetRootComponent(
	const FString& AssetPath,
	const FString& ComponentName)
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

	// Find the component
	USCS_Node* NewRootNode = FindSCSNode(Blueprint, ComponentName);
	if (!NewRootNode)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Component '%s' not found"), *ComponentName));
	}

	// Verify it's a scene component
	if (!NewRootNode->ComponentClass->IsChildOf(USceneComponent::StaticClass()))
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Component '%s' is not a SceneComponent and cannot be root"), *ComponentName));
	}

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		return FOliveBlueprintWriteResult::Error(TEXT("Blueprint has no SimpleConstructionScript"));
	}

	// Use transaction for undo support
	OLIVE_SCOPED_TRANSACTION(FText::Format(
		NSLOCTEXT("OliveCompWriter", "SetRootComponent", "Set Root Component to '{0}' in '{1}'"),
		FText::FromString(ComponentName),
		FText::FromString(Blueprint->GetName())));

	Blueprint->Modify();

	// Make the new node the root
	// This involves removing it from its current location and setting it as the default scene root
	// Find and remove from current parent first
	USCS_Node* CurrentParent = nullptr;
	for (USCS_Node* RootNode : SCS->GetRootNodes())
	{
		if (RootNode == NewRootNode)
		{
			break;
		}

		TArray<USCS_Node*> ToSearch;
		ToSearch.Add(RootNode);
		while (ToSearch.Num() > 0)
		{
			USCS_Node* Current = ToSearch.Pop();
			for (USCS_Node* Child : Current->GetChildNodes())
			{
				if (Child == NewRootNode)
				{
					CurrentParent = Current;
					break;
				}
				ToSearch.Add(Child);
			}
			if (CurrentParent)
			{
				break;
			}
		}
		if (CurrentParent)
		{
			break;
		}
	}

	if (CurrentParent)
	{
		CurrentParent->RemoveChildNode(NewRootNode);
	}
	else
	{
		SCS->RemoveNode(NewRootNode);
	}

	// Get current default root and reparent its children to the new root
	USCS_Node* OldRoot = SCS->GetDefaultSceneRootNode();
	if (OldRoot && OldRoot != NewRootNode)
	{
		// Move all children of old root to new root
		TArray<USCS_Node*> OldRootChildren = OldRoot->GetChildNodes();
		for (USCS_Node* Child : OldRootChildren)
		{
			if (Child != NewRootNode)
			{
				OldRoot->RemoveChildNode(Child);
				NewRootNode->AddChildNode(Child);
			}
		}

		// Remove old root if it was the default scene root
		if (OldRoot == SCS->GetDefaultSceneRootNode())
		{
			// The default scene root is auto-generated, we can remove it
			SCS->RemoveNode(OldRoot);
		}
	}

	// Add new root node to SCS
	SCS->AddNode(NewRootNode);

	// Mark as structurally modified
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	UE_LOG(LogOliveBPWriter, Log, TEXT("Set root component to '%s' in '%s'"), *ComponentName, *AssetPath);

	return FOliveBlueprintWriteResult::Success(AssetPath, ComponentName);
}

FOliveBlueprintWriteResult FOliveComponentWriter::RenameComponent(
	const FString& AssetPath,
	const FString& OldName,
	const FString& NewName)
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

	// Find the component
	USCS_Node* Node = FindSCSNode(Blueprint, OldName);
	if (!Node)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Component '%s' not found"), *OldName));
	}

	// Check if new name is already taken
	if (FindSCSNode(Blueprint, NewName))
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("A component named '%s' already exists"), *NewName));
	}

	// Use transaction for undo support
	OLIVE_SCOPED_TRANSACTION(FText::Format(
		NSLOCTEXT("OliveCompWriter", "RenameComponent", "Rename Component '{0}' to '{1}' in '{2}'"),
		FText::FromString(OldName),
		FText::FromString(NewName),
		FText::FromString(Blueprint->GetName())));

	Blueprint->Modify();

	// Rename the variable name
	FBlueprintEditorUtils::RenameComponentMemberVariable(Blueprint, Node, FName(*NewName));

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	UE_LOG(LogOliveBPWriter, Log, TEXT("Renamed component '%s' to '%s' in '%s'"),
		*OldName, *NewName, *AssetPath);

	return FOliveBlueprintWriteResult::Success(AssetPath, NewName);
}

// ============================================================================
// Private Helper Methods
// ============================================================================

USCS_Node* FOliveComponentWriter::FindSCSNode(const UBlueprint* Blueprint, const FString& ComponentName)
{
	if (!Blueprint || !Blueprint->SimpleConstructionScript)
	{
		return nullptr;
	}

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;

	// Recursive traversal using GetRootNodes() + GetChildNodes() (GetAllNodes() is deprecated in UE 5.5)
	TArray<USCS_Node*> NodesToSearch;
	for (USCS_Node* RootNode : SCS->GetRootNodes())
	{
		NodesToSearch.Add(RootNode);
	}

	while (NodesToSearch.Num() > 0)
	{
		USCS_Node* Current = NodesToSearch.Pop();
		if (!Current)
		{
			continue;
		}

		if (Current->GetVariableName().ToString() == ComponentName)
		{
			return Current;
		}

		for (USCS_Node* Child : Current->GetChildNodes())
		{
			NodesToSearch.Add(Child);
		}
	}

	return nullptr;
}

UClass* FOliveComponentWriter::FindComponentClass(const FString& ClassName)
{
	// Normalize the class name
	FString NormalizedName = ClassName;

	// Try direct lookup first
	UClass* Class = FindFirstObject<UClass>(*NormalizedName, EFindFirstObjectOptions::NativeFirst);
	if (Class && Class->IsChildOf(UActorComponent::StaticClass()))
	{
		return Class;
	}

	// Try with U prefix if not present
	if (!NormalizedName.StartsWith(TEXT("U")))
	{
		FString PrefixedName = TEXT("U") + NormalizedName;
		Class = FindFirstObject<UClass>(*PrefixedName, EFindFirstObjectOptions::NativeFirst);
		if (Class && Class->IsChildOf(UActorComponent::StaticClass()))
		{
			return Class;
		}
	}

	// Try removing U prefix if present
	if (NormalizedName.StartsWith(TEXT("U")))
	{
		FString UnprefixedName = NormalizedName.Mid(1);
		Class = FindFirstObject<UClass>(*UnprefixedName, EFindFirstObjectOptions::NativeFirst);
		if (Class && Class->IsChildOf(UActorComponent::StaticClass()))
		{
			return Class;
		}
	}

	// Try common component class names
	TArray<FString> CommonNames = {
		TEXT("UStaticMeshComponent"),
		TEXT("USkeletalMeshComponent"),
		TEXT("UCapsuleComponent"),
		TEXT("UBoxComponent"),
		TEXT("USphereComponent"),
		TEXT("USceneComponent"),
		TEXT("UCameraComponent"),
		TEXT("USpringArmComponent"),
		TEXT("UPointLightComponent"),
		TEXT("USpotLightComponent"),
		TEXT("UDirectionalLightComponent"),
		TEXT("UAudioComponent"),
		TEXT("UParticleSystemComponent"),
		TEXT("UNiagaraComponent"),
		TEXT("UArrowComponent"),
		TEXT("UBillboardComponent"),
		TEXT("UTextRenderComponent"),
		TEXT("UWidgetComponent"),
		TEXT("UChildActorComponent"),
		TEXT("UProjectileMovementComponent"),
		TEXT("UCharacterMovementComponent"),
		TEXT("UFloatingPawnMovement"),
	};

	// Check if the input matches any common name (case insensitive)
	for (const FString& CommonName : CommonNames)
	{
		FString ShortName = CommonName.Mid(1); // Remove U prefix
		if (NormalizedName.Equals(ShortName, ESearchCase::IgnoreCase) ||
			NormalizedName.Equals(CommonName, ESearchCase::IgnoreCase))
		{
			Class = FindFirstObject<UClass>(*CommonName, EFindFirstObjectOptions::NativeFirst);
			if (Class)
			{
				return Class;
			}
		}
	}

	return nullptr;
}

bool FOliveComponentWriter::ValidateCanHaveComponents(const UBlueprint* Blueprint, FString& OutError)
{
	if (!Blueprint)
	{
		OutError = TEXT("Blueprint is null");
		return false;
	}

	EOliveBlueprintType BPType = FOliveBlueprintTypeDetector::DetectType(Blueprint);
	FOliveBlueprintCapabilities Caps = FOliveBlueprintTypeDetector::GetCapabilities(BPType);

	if (!Caps.bHasComponents)
	{
		OutError = FString::Printf(TEXT("Blueprint type '%s' does not support components"),
			*FOliveBlueprintTypeDetector::TypeToString(BPType));
		return false;
	}

	return true;
}

FString FOliveComponentWriter::GenerateUniqueComponentName(const UBlueprint* Blueprint, const FString& BaseName)
{
	if (!Blueprint || !Blueprint->SimpleConstructionScript)
	{
		return BaseName;
	}

	FString FinalName = BaseName;
	int32 Counter = 1;

	while (FindSCSNode(Blueprint, FinalName))
	{
		FinalName = FString::Printf(TEXT("%s_%d"), *BaseName, Counter);
		Counter++;
	}

	return FinalName;
}

bool FOliveComponentWriter::SetComponentProperty(
	UActorComponent* Component,
	const FString& PropertyName,
	const FString& Value,
	FString& OutError)
{
	if (!Component)
	{
		OutError = TEXT("Component is null");
		return false;
	}

	// Find the property
	FProperty* Property = Component->GetClass()->FindPropertyByName(FName(*PropertyName));
	if (!Property)
	{
		// Try common property aliases
		TMap<FString, FString> Aliases;
		Aliases.Add(TEXT("Location"), TEXT("RelativeLocation"));
		Aliases.Add(TEXT("Rotation"), TEXT("RelativeRotation"));
		Aliases.Add(TEXT("Scale"), TEXT("RelativeScale3D"));
		Aliases.Add(TEXT("Visible"), TEXT("bVisible"));
		Aliases.Add(TEXT("Hidden"), TEXT("bHiddenInGame"));

		if (const FString* ActualName = Aliases.Find(PropertyName))
		{
			Property = Component->GetClass()->FindPropertyByName(FName(**ActualName));
		}
	}

	if (!Property)
	{
		OutError = FString::Printf(TEXT("Property '%s' not found on component class '%s'"),
			*PropertyName, *Component->GetClass()->GetName());
		return false;
	}

	// Check if property is editable
	if (!Property->HasAnyPropertyFlags(CPF_Edit))
	{
		OutError = FString::Printf(TEXT("Property '%s' is not editable"), *PropertyName);
		return false;
	}

	// Get property address
	void* PropertyAddr = Property->ContainerPtrToValuePtr<void>(Component);

	// Import the value from string
	const TCHAR* ImportText = *Value;
	Property->ImportText_Direct(ImportText, PropertyAddr, Component, PPF_None);

	return true;
}

bool FOliveComponentWriter::IsPIEActive() const
{
	return GEditor && GEditor->PlayWorld != nullptr;
}

UBlueprint* FOliveComponentWriter::LoadBlueprintForEditing(const FString& AssetPath, FString& OutError)
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
