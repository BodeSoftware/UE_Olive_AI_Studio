// Copyright Bode Software. All Rights Reserved.

#include "OliveComponentReader.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Animation/Skeleton.h"
#include "UObject/PropertyPortFlags.h"
#include "UObject/UnrealType.h"
#include "GameFramework/Actor.h"

DEFINE_LOG_CATEGORY_STATIC(LogOliveComponentReader, Log, All);

// ============================================================================
// Main Public Methods
// ============================================================================

TArray<FOliveIRComponent> FOliveComponentReader::ReadComponents(const UBlueprint* Blueprint) const
{
	TArray<FOliveIRComponent> Components;

	if (!Blueprint)
	{
		UE_LOG(LogOliveComponentReader, Warning, TEXT("ReadComponents called with null Blueprint"));
		return Components;
	}

	// Get the Simple Construction Script
	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		// Not all Blueprint types have an SCS (e.g., function libraries)
		UE_LOG(LogOliveComponentReader, Verbose, TEXT("Blueprint '%s' has no SimpleConstructionScript"), *Blueprint->GetName());
		return Components;
	}

	// Get the root nodes (top-level components)
	const TArray<USCS_Node*>& RootNodes = SCS->GetRootNodes();

	// Visited set is shared across all root-node traversals so cross-tree cycles
	// are also caught. Each root starts at depth 0.
	TSet<const USCS_Node*> Visited;
	for (USCS_Node* RootNode : RootNodes)
	{
		if (RootNode)
		{
			BuildComponentTree(RootNode, Components, TEXT(""), Visited, 0);
		}
	}

	// Mark the default scene root component
	if (Components.Num() > 0)
	{
		USCS_Node* DefaultRoot = SCS->GetDefaultSceneRootNode();
		if (DefaultRoot)
		{
			FString DefaultRootName = DefaultRoot->GetVariableName().ToString();
			for (FOliveIRComponent& Component : Components)
			{
				if (Component.Name == DefaultRootName)
				{
					Component.bIsRoot = true;
					break;
				}
			}
		}
		else if (Components.Num() == 1)
		{
			// If there's only one root node, mark it as root
			Components[0].bIsRoot = true;
		}
	}

	// Read native C++ components from parent class CDO.
	// These are components defined in C++ constructors (e.g., ACharacter's CapsuleComponent,
	// Mesh, CharacterMovement) that don't appear in the Blueprint's SCS.
	if (Blueprint->ParentClass && Blueprint->ParentClass->IsChildOf(AActor::StaticClass()))
	{
		AActor* CDO = Cast<AActor>(Blueprint->ParentClass->GetDefaultObject());
		if (CDO)
		{
			TInlineComponentArray<UActorComponent*> NativeComps;
			CDO->GetComponents(NativeComps);

			// Build a set of SCS component names to avoid duplicates
			TSet<FString> SCSNames;
			for (const FOliveIRComponent& Existing : Components)
			{
				SCSNames.Add(Existing.Name);
			}

			for (UActorComponent* Comp : NativeComps)
			{
				if (!Comp)
				{
					continue;
				}

				FString CompName = Comp->GetName();
				if (SCSNames.Contains(CompName))
				{
					continue; // SCS overrides native
				}

				FOliveIRComponent IRComp;
				IRComp.Name = CompName;
				IRComp.ComponentClass = GetCleanClassName(Comp->GetClass());
				IRComp.bIsRoot = (Comp == CDO->GetRootComponent());
				// Mark as inherited via Properties map (avoids IR struct change)
				IRComp.Properties.Add(TEXT("inherited"), TEXT("true"));

				// Capture attachment parent for hierarchy visibility
				if (USceneComponent* Scene = Cast<USceneComponent>(Comp))
				{
					if (Scene->GetAttachParent())
					{
						IRComp.Properties.Add(TEXT("attach_parent"),
							Scene->GetAttachParent()->GetName());
					}
				}

				Components.Add(MoveTemp(IRComp));
			}
		}
	}

	UE_LOG(LogOliveComponentReader, Log, TEXT("Read %d components from Blueprint '%s'"),
		Components.Num(), *Blueprint->GetName());

	return Components;
}

FString FOliveComponentReader::GetRootComponentName(const UBlueprint* Blueprint) const
{
	if (!Blueprint)
	{
		return FString();
	}

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		return FString();
	}

	// Get the default scene root node
	USCS_Node* DefaultRoot = SCS->GetDefaultSceneRootNode();
	if (DefaultRoot)
	{
		return DefaultRoot->GetVariableName().ToString();
	}

	// Fallback to the first root node
	const TArray<USCS_Node*>& RootNodes = SCS->GetRootNodes();
	if (RootNodes.Num() > 0 && RootNodes[0])
	{
		return RootNodes[0]->GetVariableName().ToString();
	}

	return FString();
}

FOliveIRComponent FOliveComponentReader::ReadComponentNode(const USCS_Node* Node) const
{
	FOliveIRComponent Component;

	if (!Node)
	{
		return Component;
	}

	// Get the variable name
	Component.Name = Node->GetVariableName().ToString();

	// Get the component class
	if (Node->ComponentTemplate)
	{
		UClass* ComponentClass = Node->ComponentTemplate->GetClass();
		if (ComponentClass)
		{
			Component.ComponentClass = GetCleanClassName(ComponentClass);
		}
	}
	else if (Node->ComponentClass)
	{
		Component.ComponentClass = GetCleanClassName(Node->ComponentClass);
	}

	// Read modified properties
	Component.Properties = ReadComponentProperties(Node);

	// Extract skeleton data for SkeletalMeshComponents (sockets and filtered bones)
	if (Node->ComponentClass && Node->ComponentClass->IsChildOf(USkeletalMeshComponent::StaticClass()))
	{
		if (USkeletalMeshComponent* SkelMeshComp = Cast<USkeletalMeshComponent>(Node->ComponentTemplate))
		{
			USkeletalMesh* SkelMesh = SkelMeshComp->GetSkeletalMeshAsset();
			if (SkelMesh)
			{
				// --- Sockets ---
				TArray<USkeletalMeshSocket*> AllSockets = SkelMesh->GetActiveSocketList();
				for (const USkeletalMeshSocket* Socket : AllSockets)
				{
					if (Socket)
					{
						Component.Sockets.Add(Socket->SocketName.ToString());
					}
				}

				// Also get sockets from the skeleton itself for full coverage
				if (USkeleton* Skeleton = SkelMesh->GetSkeleton())
				{
					for (const USkeletalMeshSocket* Socket : Skeleton->Sockets)
					{
						if (Socket)
						{
							const FString SocketName = Socket->SocketName.ToString();
							Component.Sockets.AddUnique(SocketName);
						}
					}
				}

				// --- Bones (filtered to useful attachment points) ---
				const FReferenceSkeleton& RefSkeleton = SkelMesh->GetRefSkeleton();
				const int32 NumBones = RefSkeleton.GetNum();

				for (int32 i = 0; i < NumBones; ++i)
				{
					const FString BoneName = RefSkeleton.GetBoneName(i).ToString();

					// Filter out noise bones that are rarely useful for attachment.
					// Keep: major skeletal landmarks (hands, spine, head, limbs, feet, pelvis)
					// Skip: twist bones, IK helpers, virtual bones, correction/adjustment bones
					const FString BoneLower = BoneName.ToLower();

					bool bSkip = false;
					if (BoneLower.Contains(TEXT("twist")))       bSkip = true;
					if (BoneLower.Contains(TEXT("ik_")))          bSkip = true;
					if (BoneLower.StartsWith(TEXT("vb ")))        bSkip = true;  // Virtual Bones prefix (space)
					if (BoneLower.StartsWith(TEXT("vb_")))        bSkip = true;  // Virtual Bones prefix (underscore)
					if (BoneLower.Contains(TEXT("_adjust")))      bSkip = true;
					if (BoneLower.Contains(TEXT("_corrective")))  bSkip = true;
					if (BoneLower.Contains(TEXT("_helper")))      bSkip = true;

					if (!bSkip)
					{
						Component.Bones.Add(BoneName);
					}
				}
			}
		}
	}

	return Component;
}

bool FOliveComponentReader::HasComponents(const UBlueprint* Blueprint) const
{
	if (!Blueprint)
	{
		return false;
	}

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		return false;
	}

	return SCS->GetRootNodes().Num() > 0;
}

// ============================================================================
// Tree Building Methods
// ============================================================================

void FOliveComponentReader::BuildComponentTree(
	const USCS_Node* Node,
	TArray<FOliveIRComponent>& OutComponents,
	const FString& ParentName,
	TSet<const USCS_Node*>& Visited,
	int32 Depth) const
{
	static constexpr int32 MaxComponentTreeDepth = 20;

	if (!Node)
	{
		return;
	}

	// Guard against circular SCS node references
	if (Visited.Contains(Node))
	{
		UE_LOG(LogOliveComponentReader, Warning,
			TEXT("BuildComponentTree: Circular USCS_Node reference detected at component '%s' — stopping traversal"),
			*Node->GetVariableName().ToString());
		return;
	}

	// Guard against unreasonably deep component hierarchies
	if (Depth >= MaxComponentTreeDepth)
	{
		UE_LOG(LogOliveComponentReader, Warning,
			TEXT("BuildComponentTree: Depth cap (%d) reached at component '%s' — stopping traversal"),
			MaxComponentTreeDepth, *Node->GetVariableName().ToString());
		return;
	}

	Visited.Add(Node);

	// Read this node
	FOliveIRComponent Component = ReadComponentNode(Node);

	// Build children recursively
	const TArray<USCS_Node*>& ChildNodes = Node->GetChildNodes();
	for (USCS_Node* ChildNode : ChildNodes)
	{
		if (ChildNode)
		{
			BuildComponentTree(ChildNode, Component.Children, Component.Name, Visited, Depth + 1);
		}
	}

	// Add to output array
	OutComponents.Add(MoveTemp(Component));
}

TMap<FString, FString> FOliveComponentReader::ReadComponentProperties(const USCS_Node* Node) const
{
	TMap<FString, FString> Properties;

	if (!Node || !Node->ComponentTemplate)
	{
		return Properties;
	}

	UActorComponent* ComponentTemplate = Node->ComponentTemplate;
	UClass* ComponentClass = ComponentTemplate->GetClass();
	if (!ComponentClass)
	{
		return Properties;
	}

	// Get the Class Default Object for comparison
	UObject* CDO = ComponentClass->GetDefaultObject();
	if (!CDO)
	{
		return Properties;
	}

	// Iterate through all properties
	for (TFieldIterator<FProperty> PropIt(ComponentClass); PropIt; ++PropIt)
	{
		FProperty* Property = *PropIt;
		if (!Property)
		{
			continue;
		}

		// Skip properties that shouldn't be exposed
		if (!Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
		{
			continue;
		}

		// Skip deprecated properties
		if (Property->HasAnyPropertyFlags(CPF_Deprecated))
		{
			continue;
		}

		// Check if the property value differs from the default
		if (IsPropertyModified(ComponentTemplate, Property))
		{
			const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(ComponentTemplate);
			FString ValueStr = SerializePropertyValue(Property, ValuePtr);

			if (!ValueStr.IsEmpty())
			{
				Properties.Add(Property->GetName(), ValueStr);
			}
		}
	}

	return Properties;
}

FString FOliveComponentReader::GetCleanClassName(const UClass* Class) const
{
	if (!Class)
	{
		return TEXT("Unknown");
	}

	FString ClassName = Class->GetName();

	// Remove common prefixes for cleaner display
	// Note: We only remove U prefix, as A prefix is for actors which are different
	if (ClassName.StartsWith(TEXT("U"), ESearchCase::CaseSensitive))
	{
		ClassName.RemoveAt(0, 1);
	}

	return ClassName;
}

FString FOliveComponentReader::SerializePropertyValue(const FProperty* Property, const void* ValuePtr) const
{
	if (!Property || !ValuePtr)
	{
		return FString();
	}

	// Use ExportTextItem for conversion to string
	FString ValueStr;
	Property->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, nullptr, PPF_None);

	return ValueStr;
}

bool FOliveComponentReader::IsPropertyModified(const UActorComponent* Component, const FProperty* Property) const
{
	if (!Component || !Property)
	{
		return false;
	}

	UClass* ComponentClass = Component->GetClass();
	if (!ComponentClass)
	{
		return false;
	}

	UObject* CDO = ComponentClass->GetDefaultObject();
	if (!CDO)
	{
		return false;
	}

	// Compare property values between instance and CDO
	const void* InstanceValue = Property->ContainerPtrToValuePtr<void>(Component);
	const void* DefaultValue = Property->ContainerPtrToValuePtr<void>(CDO);

	if (!InstanceValue || !DefaultValue)
	{
		return false;
	}

	// Use Identical for comparison
	return !Property->Identical(InstanceValue, DefaultValue);
}
