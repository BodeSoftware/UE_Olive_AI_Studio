// Copyright Bode Software. All Rights Reserved.

#include "OliveBTNodeSerializer.h"
#include "OliveBlackboardReader.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Composites/BTComposite_SimpleParallel.h"
#include "BehaviorTree/BlackboardData.h"
#include "UObject/UnrealType.h"

FOliveBTNodeSerializer& FOliveBTNodeSerializer::Get()
{
	static FOliveBTNodeSerializer Instance;
	return Instance;
}

FOliveIRBTNode FOliveBTNodeSerializer::SerializeTree(const UBehaviorTree* BehaviorTree)
{
	NodeCounter = 0;

	if (!BehaviorTree || !BehaviorTree->RootNode)
	{
		FOliveIRBTNode EmptyRoot;
		EmptyRoot.Id = NextNodeId();
		EmptyRoot.NodeType = EOliveIRBTNodeType::Root;
		EmptyRoot.Title = TEXT("Root");
		EmptyRoot.NodeClass = TEXT("Root");
		UE_LOG(LogOliveBTReader, Warning, TEXT("BehaviorTree has no root node"));
		return EmptyRoot;
	}

	// Treat the actual root composite as node_0 so reader IDs match writer IDs.
	return SerializeComposite(BehaviorTree->RootNode);
}

FOliveIRBTNode FOliveBTNodeSerializer::SerializeComposite(const UBTCompositeNode* CompositeNode)
{
	FOliveIRBTNode NodeIR;
	NodeIR.Id = NextNodeId();
	NodeIR.NodeType = EOliveIRBTNodeType::Composite;
	NodeIR.CompositeType = MapCompositeType(CompositeNode);
	NodeIR.NodeClass = CompositeNode->GetClass()->GetName();
	NodeIR.Title = CompositeNode->GetNodeName();
	NodeIR.Properties = ReadNodeProperties(CompositeNode);
	NodeIR.ReferencedBlackboardKeys = ExtractBlackboardKeyReferences(CompositeNode);

	// Serialize services on this composite
	for (const UBTService* Service : CompositeNode->Services)
	{
		if (Service)
		{
			NodeIR.Services.Add(SerializeService(Service));
		}
	}

	// Serialize children
	for (const FBTCompositeChild& Child : CompositeNode->Children)
	{
		FOliveIRBTNode ChildIR;

		if (Child.ChildComposite)
		{
			ChildIR = SerializeComposite(Child.ChildComposite);
		}
		else if (Child.ChildTask)
		{
			ChildIR = SerializeTask(Child.ChildTask);
		}
		else
		{
			continue;
		}

		// Add decorators from the FBTCompositeChild (these decorate this child connection)
		for (const UBTDecorator* Decorator : Child.Decorators)
		{
			if (Decorator)
			{
				ChildIR.Decorators.Add(SerializeDecorator(Decorator));
			}
		}

		NodeIR.Children.Add(MoveTemp(ChildIR));
	}

	return NodeIR;
}

FOliveIRBTNode FOliveBTNodeSerializer::SerializeTask(const UBTTaskNode* TaskNode)
{
	FOliveIRBTNode NodeIR;
	NodeIR.Id = NextNodeId();
	NodeIR.NodeType = EOliveIRBTNodeType::Task;
	NodeIR.NodeClass = TaskNode->GetClass()->GetName();
	NodeIR.Title = TaskNode->GetNodeName();
	NodeIR.Properties = ReadNodeProperties(TaskNode);
	NodeIR.ReferencedBlackboardKeys = ExtractBlackboardKeyReferences(TaskNode);

	// Tasks can have services
	for (const UBTService* Service : TaskNode->Services)
	{
		if (Service)
		{
			NodeIR.Services.Add(SerializeService(Service));
		}
	}

	return NodeIR;
}

FOliveIRBTNode FOliveBTNodeSerializer::SerializeDecorator(const UBTDecorator* DecoratorNode)
{
	FOliveIRBTNode NodeIR;
	NodeIR.Id = NextNodeId();
	NodeIR.NodeType = EOliveIRBTNodeType::Decorator;
	NodeIR.NodeClass = DecoratorNode->GetClass()->GetName();
	NodeIR.Title = DecoratorNode->GetNodeName();
	NodeIR.Properties = ReadNodeProperties(DecoratorNode);
	NodeIR.ReferencedBlackboardKeys = ExtractBlackboardKeyReferences(DecoratorNode);

	return NodeIR;
}

FOliveIRBTNode FOliveBTNodeSerializer::SerializeService(const UBTService* ServiceNode)
{
	FOliveIRBTNode NodeIR;
	NodeIR.Id = NextNodeId();
	NodeIR.NodeType = EOliveIRBTNodeType::Service;
	NodeIR.NodeClass = ServiceNode->GetClass()->GetName();
	NodeIR.Title = ServiceNode->GetNodeName();
	NodeIR.Properties = ReadNodeProperties(ServiceNode);
	NodeIR.ReferencedBlackboardKeys = ExtractBlackboardKeyReferences(ServiceNode);

	return NodeIR;
}

TMap<FString, FString> FOliveBTNodeSerializer::ReadNodeProperties(const UBTNode* Node)
{
	TMap<FString, FString> Props;

	if (!Node)
	{
		return Props;
	}

	UClass* NodeClass = Node->GetClass();

	for (TFieldIterator<FProperty> PropIt(NodeClass, EFieldIteratorFlags::IncludeSuper); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;

		// Skip properties defined on the base UBTNode class itself
		if (Prop->GetOwnerClass() == UBTNode::StaticClass())
		{
			continue;
		}

		// Only include editable properties
		if (!Prop->HasAnyPropertyFlags(CPF_Edit))
		{
			continue;
		}

		// Skip FBlackboardKeySelector properties (handled separately)
		if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			if (StructProp->Struct && StructProp->Struct->GetFName() == FName(TEXT("BlackboardKeySelector")))
			{
				continue;
			}
		}

		FString Value;
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Node);
		Prop->ExportText_Direct(Value, ValuePtr, ValuePtr, nullptr, PPF_None);

		if (!Value.IsEmpty())
		{
			Props.Add(Prop->GetName(), Value);
		}
	}

	return Props;
}

TArray<FString> FOliveBTNodeSerializer::ExtractBlackboardKeyReferences(const UBTNode* Node)
{
	TArray<FString> Keys;

	if (!Node)
	{
		return Keys;
	}

	for (TFieldIterator<FStructProperty> PropIt(Node->GetClass()); PropIt; ++PropIt)
	{
		if (PropIt->Struct && PropIt->Struct->GetFName() == FName(TEXT("BlackboardKeySelector")))
		{
			// Read the SelectedKeyName from the struct
			const void* StructPtr = PropIt->ContainerPtrToValuePtr<void>(Node);

			// Find the SelectedKeyName property within BlackboardKeySelector
			const FNameProperty* KeyNameProp = CastField<FNameProperty>(
				PropIt->Struct->FindPropertyByName(FName(TEXT("SelectedKeyName"))));

			if (KeyNameProp)
			{
				FName KeyName = KeyNameProp->GetPropertyValue_InContainer(StructPtr);
				if (!KeyName.IsNone())
				{
					Keys.AddUnique(KeyName.ToString());
				}
			}
		}
	}

	return Keys;
}

EOliveIRBTCompositeType FOliveBTNodeSerializer::MapCompositeType(const UBTCompositeNode* CompositeNode)
{
	if (!CompositeNode)
	{
		return EOliveIRBTCompositeType::Unknown;
	}

	if (CompositeNode->IsA<UBTComposite_Selector>())
	{
		return EOliveIRBTCompositeType::Selector;
	}
	if (CompositeNode->IsA<UBTComposite_Sequence>())
	{
		return EOliveIRBTCompositeType::Sequence;
	}
	if (CompositeNode->IsA<UBTComposite_SimpleParallel>())
	{
		return EOliveIRBTCompositeType::SimpleParallel;
	}

	return EOliveIRBTCompositeType::Unknown;
}

FString FOliveBTNodeSerializer::NextNodeId()
{
	return FString::Printf(TEXT("node_%d"), NodeCounter++);
}
