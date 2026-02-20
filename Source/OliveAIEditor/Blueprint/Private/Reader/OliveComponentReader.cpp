// Copyright Bode Software. All Rights Reserved.

#include "OliveComponentReader.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "UObject/PropertyPortFlags.h"
#include "UObject/UnrealType.h"

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

	for (USCS_Node* RootNode : RootNodes)
	{
		if (RootNode)
		{
			BuildComponentTree(RootNode, Components, TEXT(""));
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

	return SCS->GetAllNodes().Num() > 0;
}

// ============================================================================
// Tree Building Methods
// ============================================================================

void FOliveComponentReader::BuildComponentTree(
	const USCS_Node* Node,
	TArray<FOliveIRComponent>& OutComponents,
	const FString& ParentName) const
{
	if (!Node)
	{
		return;
	}

	// Read this node
	FOliveIRComponent Component = ReadComponentNode(Node);

	// Build children recursively
	const TArray<USCS_Node*>& ChildNodes = Node->GetChildNodes();
	for (USCS_Node* ChildNode : ChildNodes)
	{
		if (ChildNode)
		{
			BuildComponentTree(ChildNode, Component.Children, Component.Name);
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
