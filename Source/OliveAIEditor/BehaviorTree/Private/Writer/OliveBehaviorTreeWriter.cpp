// Copyright Bode Software. All Rights Reserved.

#include "OliveBehaviorTreeWriter.h"
#include "OliveBTNodeFactory.h"
#include "OliveBlackboardReader.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "AssetToolsModule.h"
#include "Factories/DataAssetFactory.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"

FOliveBehaviorTreeWriter& FOliveBehaviorTreeWriter::Get()
{
	static FOliveBehaviorTreeWriter Instance;
	return Instance;
}

UBehaviorTree* FOliveBehaviorTreeWriter::LoadBehaviorTree(const FString& AssetPath) const
{
	return Cast<UBehaviorTree>(
		StaticLoadObject(UBehaviorTree::StaticClass(), nullptr, *AssetPath));
}

UBehaviorTree* FOliveBehaviorTreeWriter::CreateBehaviorTree(const FString& AssetPath)
{
	FString PackagePath;
	FString AssetName;

	int32 LastSlash;
	if (AssetPath.FindLastChar('/', LastSlash))
	{
		PackagePath = AssetPath.Left(LastSlash);
		AssetName = AssetPath.RightChop(LastSlash + 1);
	}
	else
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("CreateBehaviorTree: Invalid path '%s'"), *AssetPath);
		return nullptr;
	}

	if (AssetName.Contains(TEXT(".")))
	{
		AssetName = FPaths::GetBaseFilename(AssetName);
	}

	const FScopedTransaction Transaction(FText::Format(
		NSLOCTEXT("OliveAI", "CreateBT", "Olive AI: Create Behavior Tree '{0}'"),
		FText::FromString(AssetName)));

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

	UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath,
		UBehaviorTree::StaticClass(), nullptr);

	UBehaviorTree* NewBT = Cast<UBehaviorTree>(NewAsset);
	if (!NewBT)
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("CreateBehaviorTree: Failed to create asset at '%s'"), *AssetPath);
		return nullptr;
	}

	// Create default root Selector
	UBTCompositeNode* RootNode = FOliveBTNodeFactory::Get().CreateComposite(NewBT, TEXT("Selector"));
	if (RootNode)
	{
		NewBT->RootNode = RootNode;
	}

	UE_LOG(LogOliveBTWriter, Log, TEXT("Created behavior tree: %s"), *AssetPath);
	return NewBT;
}

bool FOliveBehaviorTreeWriter::SetBlackboard(UBehaviorTree* BehaviorTree, const FString& BlackboardPath)
{
	if (!BehaviorTree)
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("SetBlackboard: BehaviorTree is null"));
		return false;
	}

	UBlackboardData* BB = Cast<UBlackboardData>(
		StaticLoadObject(UBlackboardData::StaticClass(), nullptr, *BlackboardPath));
	if (!BB)
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("SetBlackboard: Blackboard not found: %s"), *BlackboardPath);
		return false;
	}

	const FScopedTransaction Transaction(FText::Format(
		NSLOCTEXT("OliveAI", "SetBTBlackboard", "Olive AI: Set Blackboard to '{0}'"),
		FText::FromString(BB->GetName())));

	BehaviorTree->Modify();
	BehaviorTree->BlackboardAsset = BB;

	UE_LOG(LogOliveBTWriter, Log, TEXT("Set blackboard of '%s' to '%s'"),
		*BehaviorTree->GetName(), *BB->GetName());
	return true;
}

FString FOliveBehaviorTreeWriter::AddComposite(UBehaviorTree* BehaviorTree,
	const FString& ParentNodeId, const FString& CompositeType, int32 ChildIndex)
{
	if (!BehaviorTree)
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("AddComposite: BehaviorTree is null"));
		return FString();
	}

	BuildNodeCache(BehaviorTree);

	UBTNode* ParentNode = FindNodeById(ParentNodeId);
	UBTCompositeNode* ParentComposite = Cast<UBTCompositeNode>(ParentNode);
	if (!ParentComposite)
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("AddComposite: Parent node '%s' not found or not a composite"),
			*ParentNodeId);
		return FString();
	}

	const FScopedTransaction Transaction(FText::Format(
		NSLOCTEXT("OliveAI", "AddBTComposite", "Olive AI: Add {0} Composite"),
		FText::FromString(CompositeType)));

	BehaviorTree->Modify();

	UBTCompositeNode* NewComposite = FOliveBTNodeFactory::Get().CreateComposite(BehaviorTree, CompositeType);
	if (!NewComposite)
	{
		return FString();
	}

	// Add as child
	FBTCompositeChild NewChild;
	NewChild.ChildComposite = NewComposite;

	if (ChildIndex >= 0 && ChildIndex < ParentComposite->Children.Num())
	{
		ParentComposite->Children.Insert(NewChild, ChildIndex);
	}
	else
	{
		ParentComposite->Children.Add(NewChild);
	}

	// Rebuild cache to get the new node's ID
	BuildNodeCache(BehaviorTree);

	// Find the new node's ID
	for (const auto& Pair : NodeCache)
	{
		if (Pair.Value == NewComposite)
		{
			UE_LOG(LogOliveBTWriter, Log, TEXT("Added composite '%s' as '%s' under '%s'"),
				*CompositeType, *Pair.Key, *ParentNodeId);
			return Pair.Key;
		}
	}

	return FString();
}

FString FOliveBehaviorTreeWriter::AddTask(UBehaviorTree* BehaviorTree,
	const FString& ParentNodeId, const FString& TaskClass,
	int32 ChildIndex, const TMap<FString, FString>& Properties)
{
	if (!BehaviorTree)
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("AddTask: BehaviorTree is null"));
		return FString();
	}

	BuildNodeCache(BehaviorTree);

	UBTCompositeNode* ParentComposite = Cast<UBTCompositeNode>(FindNodeById(ParentNodeId));
	if (!ParentComposite)
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("AddTask: Parent node '%s' not found or not a composite"),
			*ParentNodeId);
		return FString();
	}

	const FScopedTransaction Transaction(FText::Format(
		NSLOCTEXT("OliveAI", "AddBTTask", "Olive AI: Add Task '{0}'"),
		FText::FromString(TaskClass)));

	BehaviorTree->Modify();

	UBTTaskNode* NewTask = FOliveBTNodeFactory::Get().CreateTask(BehaviorTree, TaskClass);
	if (!NewTask)
	{
		return FString();
	}

	// Apply initial properties
	if (Properties.Num() > 0)
	{
		ApplyProperties(NewTask, Properties);
	}

	// Add as child
	FBTCompositeChild NewChild;
	NewChild.ChildTask = NewTask;

	if (ChildIndex >= 0 && ChildIndex < ParentComposite->Children.Num())
	{
		ParentComposite->Children.Insert(NewChild, ChildIndex);
	}
	else
	{
		ParentComposite->Children.Add(NewChild);
	}

	BuildNodeCache(BehaviorTree);

	for (const auto& Pair : NodeCache)
	{
		if (Pair.Value == NewTask)
		{
			UE_LOG(LogOliveBTWriter, Log, TEXT("Added task '%s' as '%s' under '%s'"),
				*TaskClass, *Pair.Key, *ParentNodeId);
			return Pair.Key;
		}
	}

	return FString();
}

FString FOliveBehaviorTreeWriter::AddDecorator(UBehaviorTree* BehaviorTree,
	const FString& NodeId, const FString& DecoratorClass,
	const TMap<FString, FString>& Properties)
{
	if (!BehaviorTree)
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("AddDecorator: BehaviorTree is null"));
		return FString();
	}

	BuildNodeCache(BehaviorTree);

	UBTNode* TargetNode = FindNodeById(NodeId);
	if (!TargetNode)
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("AddDecorator: Node '%s' not found"), *NodeId);
		return FString();
	}

	const FScopedTransaction Transaction(FText::Format(
		NSLOCTEXT("OliveAI", "AddBTDecorator", "Olive AI: Add Decorator '{0}'"),
		FText::FromString(DecoratorClass)));

	BehaviorTree->Modify();

	UBTDecorator* NewDecorator = FOliveBTNodeFactory::Get().CreateDecorator(BehaviorTree, DecoratorClass);
	if (!NewDecorator)
	{
		return FString();
	}

	if (Properties.Num() > 0)
	{
		ApplyProperties(NewDecorator, Properties);
	}

	// Find the parent composite and the child index for this node
	// Decorators are stored on the FBTCompositeChild entry in the parent
	if (!BehaviorTree->RootNode)
	{
		return FString();
	}

	// If the target is the root composite itself, we need special handling
	// Decorators on root can be added to root's own decorator array
	UBTCompositeNode* AsComposite = Cast<UBTCompositeNode>(TargetNode);
	if (AsComposite)
	{
		// For composites, find the parent's FBTCompositeChild entry
		int32 ChildIdx;
		UBTCompositeNode* Parent = FindParentComposite(BehaviorTree, TargetNode, ChildIdx);
		if (Parent && Parent->Children.IsValidIndex(ChildIdx))
		{
			Parent->Children[ChildIdx].Decorators.Add(NewDecorator);
		}
		else if (AsComposite == BehaviorTree->RootNode)
		{
			// Root node - decorators go on root's first presentation
			// In UE, root decorators are typically not supported directly,
			// so we log a warning
			UE_LOG(LogOliveBTWriter, Warning, TEXT("Cannot add decorators directly to the root node"));
			return FString();
		}
		else
		{
			return FString();
		}
	}
	else
	{
		// For tasks, find the parent's FBTCompositeChild entry
		int32 ChildIdx;
		UBTCompositeNode* Parent = FindParentComposite(BehaviorTree, TargetNode, ChildIdx);
		if (Parent && Parent->Children.IsValidIndex(ChildIdx))
		{
			Parent->Children[ChildIdx].Decorators.Add(NewDecorator);
		}
		else
		{
			UE_LOG(LogOliveBTWriter, Error, TEXT("AddDecorator: Could not find parent for node '%s'"), *NodeId);
			return FString();
		}
	}

	BuildNodeCache(BehaviorTree);

	for (const auto& Pair : NodeCache)
	{
		if (Pair.Value == NewDecorator)
		{
			UE_LOG(LogOliveBTWriter, Log, TEXT("Added decorator '%s' as '%s' on '%s'"),
				*DecoratorClass, *Pair.Key, *NodeId);
			return Pair.Key;
		}
	}

	return FString();
}

FString FOliveBehaviorTreeWriter::AddService(UBehaviorTree* BehaviorTree,
	const FString& NodeId, const FString& ServiceClass,
	const TMap<FString, FString>& Properties)
{
	if (!BehaviorTree)
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("AddService: BehaviorTree is null"));
		return FString();
	}

	BuildNodeCache(BehaviorTree);

	UBTNode* TargetNode = FindNodeById(NodeId);
	if (!TargetNode)
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("AddService: Node '%s' not found"), *NodeId);
		return FString();
	}

	const FScopedTransaction Transaction(FText::Format(
		NSLOCTEXT("OliveAI", "AddBTService", "Olive AI: Add Service '{0}'"),
		FText::FromString(ServiceClass)));

	BehaviorTree->Modify();

	UBTService* NewService = FOliveBTNodeFactory::Get().CreateService(BehaviorTree, ServiceClass);
	if (!NewService)
	{
		return FString();
	}

	if (Properties.Num() > 0)
	{
		ApplyProperties(NewService, Properties);
	}

	// Services attach to composites or tasks
	UBTCompositeNode* AsComposite = Cast<UBTCompositeNode>(TargetNode);
	UBTTaskNode* AsTask = Cast<UBTTaskNode>(TargetNode);

	if (AsComposite)
	{
		AsComposite->Services.Add(NewService);
	}
	else if (AsTask)
	{
		AsTask->Services.Add(NewService);
	}
	else
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("AddService: Node '%s' is not a composite or task"), *NodeId);
		return FString();
	}

	BuildNodeCache(BehaviorTree);

	for (const auto& Pair : NodeCache)
	{
		if (Pair.Value == NewService)
		{
			UE_LOG(LogOliveBTWriter, Log, TEXT("Added service '%s' as '%s' on '%s'"),
				*ServiceClass, *Pair.Key, *NodeId);
			return Pair.Key;
		}
	}

	return FString();
}

bool FOliveBehaviorTreeWriter::RemoveNode(UBehaviorTree* BehaviorTree, const FString& NodeId)
{
	if (!BehaviorTree || !BehaviorTree->RootNode)
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("RemoveNode: BehaviorTree is null or has no root"));
		return false;
	}

	BuildNodeCache(BehaviorTree);

	UBTNode* TargetNode = FindNodeById(NodeId);
	if (!TargetNode)
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("RemoveNode: Node '%s' not found"), *NodeId);
		return false;
	}

	// Don't remove root
	if (TargetNode == BehaviorTree->RootNode)
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("RemoveNode: Cannot remove root node"));
		return false;
	}

	const FScopedTransaction Transaction(FText::Format(
		NSLOCTEXT("OliveAI", "RemoveBTNode", "Olive AI: Remove Node '{0}'"),
		FText::FromString(NodeId)));

	BehaviorTree->Modify();

	// Check if it's a decorator
	UBTDecorator* AsDecorator = Cast<UBTDecorator>(TargetNode);
	if (AsDecorator)
	{
		// Search all composite children for this decorator
		TFunction<bool(UBTCompositeNode*)> RemoveDecoratorFromTree = [&](UBTCompositeNode* Composite) -> bool
		{
			for (FBTCompositeChild& Child : Composite->Children)
			{
				if (Child.Decorators.Remove(AsDecorator) > 0)
				{
					return true;
				}
				if (Child.ChildComposite)
				{
					if (RemoveDecoratorFromTree(Child.ChildComposite))
					{
						return true;
					}
				}
			}
			return false;
		};

		if (RemoveDecoratorFromTree(BehaviorTree->RootNode))
		{
			UE_LOG(LogOliveBTWriter, Log, TEXT("Removed decorator '%s'"), *NodeId);
			return true;
		}
		return false;
	}

	// Check if it's a service
	UBTService* AsService = Cast<UBTService>(TargetNode);
	if (AsService)
	{
		// Search composites and tasks for this service
		TFunction<bool(UBTCompositeNode*)> RemoveServiceFromTree = [&](UBTCompositeNode* Composite) -> bool
		{
			if (Composite->Services.Remove(AsService) > 0)
			{
				return true;
			}
			for (FBTCompositeChild& Child : Composite->Children)
			{
				if (Child.ChildTask && Child.ChildTask->Services.Remove(AsService) > 0)
				{
					return true;
				}
				if (Child.ChildComposite)
				{
					if (RemoveServiceFromTree(Child.ChildComposite))
					{
						return true;
					}
				}
			}
			return false;
		};

		if (RemoveServiceFromTree(BehaviorTree->RootNode))
		{
			UE_LOG(LogOliveBTWriter, Log, TEXT("Removed service '%s'"), *NodeId);
			return true;
		}
		return false;
	}

	// It's a composite or task - find and remove from parent
	int32 ChildIdx;
	UBTCompositeNode* Parent = FindParentComposite(BehaviorTree, TargetNode, ChildIdx);
	if (Parent && Parent->Children.IsValidIndex(ChildIdx))
	{
		Parent->Children.RemoveAt(ChildIdx);
		UE_LOG(LogOliveBTWriter, Log, TEXT("Removed node '%s'"), *NodeId);
		return true;
	}

	UE_LOG(LogOliveBTWriter, Error, TEXT("RemoveNode: Could not find parent for node '%s'"), *NodeId);
	return false;
}

bool FOliveBehaviorTreeWriter::MoveNode(UBehaviorTree* BehaviorTree,
	const FString& NodeId, const FString& NewParentId, int32 ChildIndex)
{
	if (!BehaviorTree || !BehaviorTree->RootNode)
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("MoveNode: BehaviorTree is null"));
		return false;
	}

	BuildNodeCache(BehaviorTree);

	UBTNode* TargetNode = FindNodeById(NodeId);
	if (!TargetNode)
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("MoveNode: Node '%s' not found"), *NodeId);
		return false;
	}

	UBTCompositeNode* NewParent = Cast<UBTCompositeNode>(FindNodeById(NewParentId));
	if (!NewParent)
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("MoveNode: New parent '%s' not found or not composite"), *NewParentId);
		return false;
	}

	if (TargetNode == BehaviorTree->RootNode)
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("MoveNode: Cannot move root node"));
		return false;
	}

	const FScopedTransaction Transaction(FText::Format(
		NSLOCTEXT("OliveAI", "MoveBTNode", "Olive AI: Move Node '{0}' to '{1}'"),
		FText::FromString(NodeId), FText::FromString(NewParentId)));

	BehaviorTree->Modify();

	// Find and detach from old parent
	int32 OldChildIdx;
	UBTCompositeNode* OldParent = FindParentComposite(BehaviorTree, TargetNode, OldChildIdx);
	if (!OldParent || !OldParent->Children.IsValidIndex(OldChildIdx))
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("MoveNode: Could not find current parent for '%s'"), *NodeId);
		return false;
	}

	// Save the child entry (preserves decorators)
	FBTCompositeChild ChildEntry = OldParent->Children[OldChildIdx];
	OldParent->Children.RemoveAt(OldChildIdx);

	// Attach to new parent
	if (ChildIndex >= 0 && ChildIndex <= NewParent->Children.Num())
	{
		NewParent->Children.Insert(ChildEntry, ChildIndex);
	}
	else
	{
		NewParent->Children.Add(ChildEntry);
	}

	UE_LOG(LogOliveBTWriter, Log, TEXT("Moved node '%s' to parent '%s'"), *NodeId, *NewParentId);
	return true;
}

bool FOliveBehaviorTreeWriter::SetNodeProperty(UBehaviorTree* BehaviorTree,
	const FString& NodeId, const FString& PropertyName, const FString& Value)
{
	if (!BehaviorTree)
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("SetNodeProperty: BehaviorTree is null"));
		return false;
	}

	BuildNodeCache(BehaviorTree);

	UBTNode* TargetNode = FindNodeById(NodeId);
	if (!TargetNode)
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("SetNodeProperty: Node '%s' not found"), *NodeId);
		return false;
	}

	const FScopedTransaction Transaction(FText::Format(
		NSLOCTEXT("OliveAI", "SetBTProperty", "Olive AI: Set Property '{0}' on Node '{1}'"),
		FText::FromString(PropertyName), FText::FromString(NodeId)));

	BehaviorTree->Modify();
	TargetNode->Modify();

	FProperty* Prop = TargetNode->GetClass()->FindPropertyByName(FName(*PropertyName));
	if (!Prop)
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("SetNodeProperty: Property '%s' not found on node '%s'"),
			*PropertyName, *NodeId);
		return false;
	}

	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(TargetNode);
	if (!Prop->ImportText_Direct(*Value, ValuePtr, TargetNode, PPF_None))
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("SetNodeProperty: Failed to import value '%s' for property '%s'"),
			*Value, *PropertyName);
		return false;
	}

	UE_LOG(LogOliveBTWriter, Log, TEXT("Set property '%s' = '%s' on node '%s'"),
		*PropertyName, *Value, *NodeId);
	return true;
}

void FOliveBehaviorTreeWriter::BuildNodeCache(const UBehaviorTree* BehaviorTree)
{
	NodeCache.Empty();
	CacheCounter = 0;

	if (!BehaviorTree || !BehaviorTree->RootNode)
	{
		return;
	}

	// node_0 is the root composite (matches serializer ordering)
	NodeCache.Add(FString::Printf(TEXT("node_%d"), CacheCounter++), BehaviorTree->RootNode);

	WalkTreeForCache(BehaviorTree->RootNode);
}

void FOliveBehaviorTreeWriter::WalkTreeForCache(const UBTCompositeNode* Composite)
{
	if (!Composite)
	{
		return;
	}

	// Cache services on this composite
	for (UBTService* Service : Composite->Services)
	{
		if (Service)
		{
			NodeCache.Add(FString::Printf(TEXT("node_%d"), CacheCounter++), Service);
		}
	}

	// Cache children
	for (const FBTCompositeChild& Child : Composite->Children)
	{
		UBTNode* ChildNode = Child.ChildComposite ? (UBTNode*)Child.ChildComposite : (UBTNode*)Child.ChildTask;
		if (!ChildNode)
		{
			continue;
		}

		NodeCache.Add(FString::Printf(TEXT("node_%d"), CacheCounter++), ChildNode);

		// Cache decorators on this child
		for (UBTDecorator* Decorator : Child.Decorators)
		{
			if (Decorator)
			{
				NodeCache.Add(FString::Printf(TEXT("node_%d"), CacheCounter++), Decorator);
			}
		}

		// Cache services on tasks
		UBTTaskNode* TaskNode = Cast<UBTTaskNode>(ChildNode);
		if (TaskNode)
		{
			for (UBTService* Service : TaskNode->Services)
			{
				if (Service)
				{
					NodeCache.Add(FString::Printf(TEXT("node_%d"), CacheCounter++), Service);
				}
			}
		}

		// Recurse into child composites
		if (Child.ChildComposite)
		{
			WalkTreeForCache(Child.ChildComposite);
		}
	}
}

UBTNode* FOliveBehaviorTreeWriter::FindNodeById(const FString& NodeId) const
{
	UBTNode* const* Found = NodeCache.Find(NodeId);
	return Found ? *Found : nullptr;
}

UBTCompositeNode* FOliveBehaviorTreeWriter::FindParentComposite(
	const UBehaviorTree* BehaviorTree, const UBTNode* TargetNode, int32& OutChildIndex) const
{
	if (!BehaviorTree || !BehaviorTree->RootNode || !TargetNode)
	{
		OutChildIndex = INDEX_NONE;
		return nullptr;
	}

	TFunction<UBTCompositeNode*(UBTCompositeNode*)> FindParent =
		[&](UBTCompositeNode* Composite) -> UBTCompositeNode*
	{
		for (int32 i = 0; i < Composite->Children.Num(); ++i)
		{
			const FBTCompositeChild& Child = Composite->Children[i];

			if (Child.ChildComposite == TargetNode || Child.ChildTask == TargetNode)
			{
				OutChildIndex = i;
				return Composite;
			}

			if (Child.ChildComposite)
			{
				UBTCompositeNode* Result = FindParent(Child.ChildComposite);
				if (Result)
				{
					return Result;
				}
			}
		}

		return nullptr;
	};

	return FindParent(BehaviorTree->RootNode);
}

void FOliveBehaviorTreeWriter::ApplyProperties(UBTNode* Node, const TMap<FString, FString>& Properties)
{
	if (!Node)
	{
		return;
	}

	for (const auto& Pair : Properties)
	{
		FProperty* Prop = Node->GetClass()->FindPropertyByName(FName(*Pair.Key));
		if (!Prop)
		{
			UE_LOG(LogOliveBTWriter, Warning, TEXT("Property '%s' not found on %s"),
				*Pair.Key, *Node->GetClass()->GetName());
			continue;
		}

		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Node);
		if (!Prop->ImportText_Direct(*Pair.Value, ValuePtr, Node, PPF_None))
		{
			UE_LOG(LogOliveBTWriter, Warning, TEXT("Failed to set property '%s' to '%s' on %s"),
				*Pair.Key, *Pair.Value, *Node->GetClass()->GetName());
		}
	}
}
