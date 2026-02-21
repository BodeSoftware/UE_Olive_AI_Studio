// Copyright Bode Software. All Rights Reserved.

#include "OlivePCGWriter.h"
#include "OlivePCGNodeCatalog.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "PCGSubgraph.h"
#include "PCGComponent.h"
#include "PCGSubsystem.h"
#include "AssetToolsModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Factories/Factory.h"
#include "UObject/SavePackage.h"
#include "Editor.h"
#include "Engine/World.h"

DEFINE_LOG_CATEGORY_STATIC(LogOlivePCGWriter, Log, All);

FOlivePCGWriter& FOlivePCGWriter::Get()
{
	static FOlivePCGWriter Instance;
	return Instance;
}

UPCGGraph* FOlivePCGWriter::CreatePCGGraph(const FString& AssetPath)
{
	// Parse package name and asset name from the path
	FString PackageName = AssetPath;
	FString AssetName;

	int32 LastSlash;
	if (PackageName.FindLastChar('/', LastSlash))
	{
		AssetName = PackageName.Mid(LastSlash + 1);
	}
	else
	{
		AssetName = PackageName;
	}

	// Create the package
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		UE_LOG(LogOlivePCGWriter, Error, TEXT("Failed to create package: %s"), *PackageName);
		return nullptr;
	}

	Package->FullyLoad();

	// Create the PCG graph object
	UPCGGraph* NewGraph = NewObject<UPCGGraph>(Package, *AssetName,
		RF_Public | RF_Standalone | RF_Transactional);

	if (!NewGraph)
	{
		UE_LOG(LogOlivePCGWriter, Error, TEXT("Failed to create PCG graph object"));
		return nullptr;
	}

	// Notify asset registry
	FAssetRegistryModule::AssetCreated(NewGraph);

	// Mark package dirty
	Package->MarkPackageDirty();

	// Save the package
	FString PackageFileName = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	UPackage::SavePackage(Package, NewGraph, *PackageFileName, SaveArgs);

	UE_LOG(LogOlivePCGWriter, Log, TEXT("Created PCG graph: %s"), *AssetPath);
	return NewGraph;
}

FString FOlivePCGWriter::AddNode(UPCGGraph* Graph, const FString& SettingsClassName,
	int32 PosX, int32 PosY)
{
	if (!Graph)
	{
		return FString();
	}

	// Find the settings class via catalog's flexible name resolution
	UClass* SettingsClass = FOlivePCGNodeCatalog::Get().FindSettingsClass(SettingsClassName);
	if (!SettingsClass)
	{
		UE_LOG(LogOlivePCGWriter, Warning, TEXT("Settings class not found: %s"), *SettingsClassName);
		return FString();
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Add PCG Node: %s"), *SettingsClassName)));

	Graph->Modify();

	UPCGSettings* DefaultSettings = nullptr;
	UPCGNode* NewNode = Graph->AddNodeOfType(SettingsClass, DefaultSettings);

	if (!NewNode)
	{
		UE_LOG(LogOlivePCGWriter, Error, TEXT("Failed to add node of type: %s"), *SettingsClassName);
		return FString();
	}

#if WITH_EDITOR
	NewNode->SetNodePosition(PosX, PosY);
#endif

	// Rebuild cache to get the new node's ID
	BuildNodeCache(Graph);

	// Find the new node's ID
	for (const auto& Pair : NodeCache)
	{
		if (Pair.Value == NewNode)
		{
			UE_LOG(LogOlivePCGWriter, Log, TEXT("Added PCG node '%s' as %s"),
				*SettingsClassName, *Pair.Key);
			return Pair.Key;
		}
	}

	// Fallback: assign a new ID
	FString NewId = FString::Printf(TEXT("node_%d"), CacheCounter);
	UE_LOG(LogOlivePCGWriter, Log, TEXT("Added PCG node '%s' as %s"), *SettingsClassName, *NewId);
	return NewId;
}

bool FOlivePCGWriter::RemoveNode(UPCGGraph* Graph, const FString& NodeId)
{
	if (!Graph)
	{
		return false;
	}

	// Protect input/output nodes
	if (NodeId == TEXT("input") || NodeId == TEXT("output"))
	{
		UE_LOG(LogOlivePCGWriter, Warning, TEXT("Cannot remove Input/Output nodes"));
		return false;
	}

	BuildNodeCache(Graph);
	UPCGNode* Node = FindNodeById(NodeId);
	if (!Node)
	{
		UE_LOG(LogOlivePCGWriter, Warning, TEXT("Node not found: %s"), *NodeId);
		return false;
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Remove PCG Node: %s"), *NodeId)));

	Graph->Modify();
	Graph->RemoveNode(Node);

	UE_LOG(LogOlivePCGWriter, Log, TEXT("Removed PCG node: %s"), *NodeId);
	return true;
}

bool FOlivePCGWriter::Connect(UPCGGraph* Graph,
	const FString& SourceNodeId, const FString& SourcePinName,
	const FString& TargetNodeId, const FString& TargetPinName)
{
	if (!Graph)
	{
		return false;
	}

	BuildNodeCache(Graph);

	UPCGNode* SourceNode = FindNodeById(SourceNodeId);
	UPCGNode* TargetNode = FindNodeById(TargetNodeId);

	if (!SourceNode)
	{
		UE_LOG(LogOlivePCGWriter, Warning, TEXT("Source node not found: %s"), *SourceNodeId);
		return false;
	}
	if (!TargetNode)
	{
		UE_LOG(LogOlivePCGWriter, Warning, TEXT("Target node not found: %s"), *TargetNodeId);
		return false;
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Connect PCG: %s.%s -> %s.%s"),
			*SourceNodeId, *SourcePinName, *TargetNodeId, *TargetPinName)));

	Graph->Modify();

	UPCGNode* ResultNode = Graph->AddEdge(SourceNode, FName(*SourcePinName),
		TargetNode, FName(*TargetPinName));

	if (!ResultNode)
	{
		UE_LOG(LogOlivePCGWriter, Warning, TEXT("Failed to add edge"));
		return false;
	}

	UE_LOG(LogOlivePCGWriter, Log, TEXT("Connected %s.%s -> %s.%s"),
		*SourceNodeId, *SourcePinName, *TargetNodeId, *TargetPinName);
	return true;
}

bool FOlivePCGWriter::Disconnect(UPCGGraph* Graph,
	const FString& SourceNodeId, const FString& SourcePinName,
	const FString& TargetNodeId, const FString& TargetPinName)
{
	if (!Graph)
	{
		return false;
	}

	BuildNodeCache(Graph);

	UPCGNode* SourceNode = FindNodeById(SourceNodeId);
	UPCGNode* TargetNode = FindNodeById(TargetNodeId);

	if (!SourceNode || !TargetNode)
	{
		UE_LOG(LogOlivePCGWriter, Warning, TEXT("Source or target node not found"));
		return false;
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Disconnect PCG: %s.%s -> %s.%s"),
			*SourceNodeId, *SourcePinName, *TargetNodeId, *TargetPinName)));

	Graph->Modify();

	bool bRemoved = Graph->RemoveEdge(SourceNode, FName(*SourcePinName),
		TargetNode, FName(*TargetPinName));

	if (!bRemoved)
	{
		UE_LOG(LogOlivePCGWriter, Warning, TEXT("Failed to remove edge"));
		return false;
	}

	UE_LOG(LogOlivePCGWriter, Log, TEXT("Disconnected %s.%s -> %s.%s"),
		*SourceNodeId, *SourcePinName, *TargetNodeId, *TargetPinName);
	return true;
}

bool FOlivePCGWriter::SetSettings(UPCGGraph* Graph, const FString& NodeId,
	const TMap<FString, FString>& Properties)
{
	if (!Graph || Properties.Num() == 0)
	{
		return false;
	}

	BuildNodeCache(Graph);
	UPCGNode* Node = FindNodeById(NodeId);
	if (!Node)
	{
		UE_LOG(LogOlivePCGWriter, Warning, TEXT("Node not found: %s"), *NodeId);
		return false;
	}

	UPCGSettings* Settings = Node->GetSettings();
	if (!Settings)
	{
		UE_LOG(LogOlivePCGWriter, Warning, TEXT("Node %s has no settings"), *NodeId);
		return false;
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Set PCG Settings: %s"), *NodeId)));

	Settings->Modify();

	int32 SetCount = 0;
	for (const auto& Pair : Properties)
	{
		FProperty* Property = Settings->GetClass()->FindPropertyByName(FName(*Pair.Key));
		if (!Property)
		{
			UE_LOG(LogOlivePCGWriter, Warning, TEXT("Property '%s' not found on %s"),
				*Pair.Key, *Settings->GetClass()->GetName());
			continue;
		}

		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Settings);
		if (Property->ImportText_Direct(*Pair.Value, ValuePtr, Settings, PPF_None))
		{
			SetCount++;
		}
		else
		{
			UE_LOG(LogOlivePCGWriter, Warning, TEXT("Failed to set property '%s' = '%s'"),
				*Pair.Key, *Pair.Value);
		}
	}

	// Notify the graph that settings changed
#if WITH_EDITOR
	Settings->PostEditChange();
#endif

	UE_LOG(LogOlivePCGWriter, Log, TEXT("Set %d/%d properties on node %s"),
		SetCount, Properties.Num(), *NodeId);

	return SetCount > 0;
}

FString FOlivePCGWriter::AddSubgraph(UPCGGraph* Graph, const FString& SubgraphPath,
	int32 PosX, int32 PosY)
{
	if (!Graph)
	{
		return FString();
	}

	// Load the subgraph
	UPCGGraph* SubgraphAsset = LoadPCGGraph(SubgraphPath);
	if (!SubgraphAsset)
	{
		UE_LOG(LogOlivePCGWriter, Warning, TEXT("Subgraph not found: %s"), *SubgraphPath);
		return FString();
	}

	// Check for self-reference
	if (SubgraphAsset == Graph)
	{
		UE_LOG(LogOlivePCGWriter, Warning, TEXT("Cannot add graph as its own subgraph"));
		return FString();
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Add PCG Subgraph: %s"), *SubgraphPath)));

	Graph->Modify();

	UPCGSettings* DefaultSettings = nullptr;
	UPCGNode* SubNode = Graph->AddNodeOfType(UPCGSubgraphSettings::StaticClass(), DefaultSettings);

	if (!SubNode)
	{
		UE_LOG(LogOlivePCGWriter, Error, TEXT("Failed to create subgraph node"));
		return FString();
	}

	// Set the subgraph reference
	UPCGBaseSubgraphSettings* SubSettings = Cast<UPCGBaseSubgraphSettings>(SubNode->GetSettings());
	if (SubSettings)
	{
		SubSettings->SetSubgraph(SubgraphAsset);
	}

#if WITH_EDITOR
	SubNode->SetNodePosition(PosX, PosY);
#endif

	// Rebuild cache to get the new node's ID
	BuildNodeCache(Graph);

	for (const auto& Pair : NodeCache)
	{
		if (Pair.Value == SubNode)
		{
			UE_LOG(LogOlivePCGWriter, Log, TEXT("Added subgraph '%s' as %s"),
				*SubgraphPath, *Pair.Key);
			return Pair.Key;
		}
	}

	return FString();
}

FPCGExecuteResult FOlivePCGWriter::Execute(UPCGGraph* Graph, float TimeoutSeconds)
{
	FPCGExecuteResult Result;

	if (!Graph)
	{
		Result.Summary = TEXT("No graph provided");
		return Result;
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		Result.Summary = TEXT("No editor world available");
		return Result;
	}

	UPCGSubsystem* Subsystem = UPCGSubsystem::GetInstance(World);
	if (!Subsystem)
	{
		Result.Summary = TEXT("PCG subsystem not available");
		return Result;
	}

	// Create a temporary actor with a PCG component
	FActorSpawnParameters SpawnParams;
	SpawnParams.Name = FName(*FString::Printf(TEXT("OlivePCG_Temp_%s"), *Graph->GetName()));
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AActor* TempActor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParams);
	if (!TempActor)
	{
		Result.Summary = TEXT("Failed to create temporary actor");
		return Result;
	}

	UPCGComponent* PCGComponent = NewObject<UPCGComponent>(TempActor);
	PCGComponent->RegisterComponent();
	PCGComponent->SetGraph(Graph);

	double StartTime = FPlatformTime::Seconds();

	// Generate
	PCGComponent->GenerateLocal(/*bForce=*/true);

	// Poll for completion (simplified - in editor, generation is typically synchronous)
	double ElapsedTime = FPlatformTime::Seconds() - StartTime;

	Result.bSuccess = true;
	Result.DurationSeconds = (float)ElapsedTime;
	Result.Summary = FString::Printf(TEXT("Graph '%s' executed in %.3fs"),
		*Graph->GetName(), ElapsedTime);

	// Cleanup
	PCGComponent->CleanupLocal(/*bRemoveComponents=*/true);
	World->DestroyActor(TempActor);

	UE_LOG(LogOlivePCGWriter, Log, TEXT("%s"), *Result.Summary);
	return Result;
}

UPCGGraph* FOlivePCGWriter::LoadPCGGraph(const FString& AssetPath) const
{
	return Cast<UPCGGraph>(StaticLoadObject(UPCGGraph::StaticClass(), nullptr, *AssetPath));
}

void FOlivePCGWriter::BuildNodeCache(const UPCGGraph* Graph)
{
	NodeCache.Empty();
	CacheCounter = 0;

	if (!Graph)
	{
		return;
	}

	// Input and Output nodes get fixed IDs
	if (UPCGNode* InputNode = Graph->GetInputNode())
	{
		NodeCache.Add(TEXT("input"), InputNode);
	}
	if (UPCGNode* OutputNode = Graph->GetOutputNode())
	{
		NodeCache.Add(TEXT("output"), OutputNode);
	}

	// Regular nodes
	const TArray<UPCGNode*>& Nodes = Graph->GetNodes();
	for (UPCGNode* Node : Nodes)
	{
		if (Node)
		{
			FString NodeId = FString::Printf(TEXT("node_%d"), CacheCounter++);
			NodeCache.Add(NodeId, Node);
		}
	}
}

UPCGNode* FOlivePCGWriter::FindNodeById(const FString& NodeId) const
{
	UPCGNode* const* Found = NodeCache.Find(NodeId);
	return Found ? *Found : nullptr;
}
