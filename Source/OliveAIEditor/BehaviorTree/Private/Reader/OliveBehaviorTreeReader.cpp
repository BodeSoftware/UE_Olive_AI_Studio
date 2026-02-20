// Copyright Bode Software. All Rights Reserved.

#include "OliveBehaviorTreeReader.h"
#include "OliveBTNodeSerializer.h"
#include "OliveBlackboardReader.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"

// FBTReadResult

TSharedPtr<FJsonObject> FBTReadResult::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetObjectField(TEXT("behavior_tree"), BehaviorTree.ToJson());

	if (Blackboard.IsSet())
	{
		Json->SetObjectField(TEXT("blackboard"), Blackboard.GetValue().ToJson());
	}

	return Json;
}

// FOliveBehaviorTreeReader

FOliveBehaviorTreeReader& FOliveBehaviorTreeReader::Get()
{
	static FOliveBehaviorTreeReader Instance;
	return Instance;
}

UBehaviorTree* FOliveBehaviorTreeReader::LoadBehaviorTree(const FString& AssetPath) const
{
	UBehaviorTree* BT = Cast<UBehaviorTree>(
		StaticLoadObject(UBehaviorTree::StaticClass(), nullptr, *AssetPath));

	if (!BT)
	{
		UE_LOG(LogOliveBTReader, Warning, TEXT("Failed to load behavior tree: %s"), *AssetPath);
	}

	return BT;
}

TOptional<FBTReadResult> FOliveBehaviorTreeReader::ReadBehaviorTree(const FString& AssetPath, bool bIncludeBlackboard)
{
	UBehaviorTree* BT = LoadBehaviorTree(AssetPath);
	if (!BT)
	{
		return {};
	}

	return ReadBehaviorTree(BT, bIncludeBlackboard);
}

TOptional<FBTReadResult> FOliveBehaviorTreeReader::ReadBehaviorTree(const UBehaviorTree* BehaviorTree, bool bIncludeBlackboard)
{
	if (!BehaviorTree)
	{
		return {};
	}

	FBTReadResult Result;

	// Build BT IR
	Result.BehaviorTree.Name = BehaviorTree->GetName();
	Result.BehaviorTree.Path = BehaviorTree->GetPathName();

	// Set blackboard path
	if (BehaviorTree->BlackboardAsset)
	{
		Result.BehaviorTree.BlackboardPath = BehaviorTree->BlackboardAsset->GetPathName();
	}

	// Serialize node tree
	Result.BehaviorTree.Root = FOliveBTNodeSerializer::Get().SerializeTree(BehaviorTree);

	// Collect used classes
	CollectUsedClasses(Result.BehaviorTree.Root,
		Result.BehaviorTree.UsedTaskClasses,
		Result.BehaviorTree.UsedDecoratorClasses,
		Result.BehaviorTree.UsedServiceClasses);

	// Read associated blackboard if requested
	if (bIncludeBlackboard && BehaviorTree->BlackboardAsset)
	{
		TOptional<FOliveIRBlackboard> BBIR =
			FOliveBlackboardReader::Get().ReadBlackboard(BehaviorTree->BlackboardAsset);
		if (BBIR.IsSet())
		{
			Result.Blackboard = BBIR;
		}
	}

	UE_LOG(LogOliveBTReader, Verbose, TEXT("Read behavior tree '%s' with %d task classes, %d decorator classes, %d service classes"),
		*Result.BehaviorTree.Name,
		Result.BehaviorTree.UsedTaskClasses.Num(),
		Result.BehaviorTree.UsedDecoratorClasses.Num(),
		Result.BehaviorTree.UsedServiceClasses.Num());

	return Result;
}

void FOliveBehaviorTreeReader::CollectUsedClasses(const FOliveIRBTNode& Node,
	TArray<FString>& OutTasks, TArray<FString>& OutDecorators, TArray<FString>& OutServices)
{
	// Collect this node's class
	switch (Node.NodeType)
	{
		case EOliveIRBTNodeType::Task:
			OutTasks.AddUnique(Node.NodeClass);
			break;
		case EOliveIRBTNodeType::Decorator:
			OutDecorators.AddUnique(Node.NodeClass);
			break;
		case EOliveIRBTNodeType::Service:
			OutServices.AddUnique(Node.NodeClass);
			break;
		default:
			break;
	}

	// Recurse into decorators
	for (const FOliveIRBTNode& Decorator : Node.Decorators)
	{
		CollectUsedClasses(Decorator, OutTasks, OutDecorators, OutServices);
	}

	// Recurse into services
	for (const FOliveIRBTNode& Service : Node.Services)
	{
		CollectUsedClasses(Service, OutTasks, OutDecorators, OutServices);
	}

	// Recurse into children
	for (const FOliveIRBTNode& Child : Node.Children)
	{
		CollectUsedClasses(Child, OutTasks, OutDecorators, OutServices);
	}
}
