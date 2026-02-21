// Copyright Bode Software. All Rights Reserved.

#include "OliveBlueprintNavigator.h"
#include "IR/OliveCompileIR.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "BlueprintEditor.h"
#include "Subsystems/AssetEditorSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogOliveBPNavigator, Log, All);

bool FOliveBlueprintNavigator::OpenAsset(const FString& AssetPath)
{
	if (AssetPath.IsEmpty())
	{
		UE_LOG(LogOliveBPNavigator, Warning, TEXT("OpenAsset: Empty asset path"));
		return false;
	}

	UBlueprint* Blueprint = LoadBlueprintFromPath(AssetPath);
	if (!Blueprint)
	{
		UE_LOG(LogOliveBPNavigator, Warning, TEXT("OpenAsset: Failed to load '%s'"), *AssetPath);
		return false;
	}

	UAssetEditorSubsystem* AssetEditor = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!AssetEditor)
	{
		return false;
	}

	AssetEditor->OpenEditorForAsset(Blueprint);
	return true;
}

bool FOliveBlueprintNavigator::OpenGraph(const FString& AssetPath, const FString& GraphName)
{
	if (!OpenAsset(AssetPath))
	{
		return false;
	}

	UBlueprint* Blueprint = LoadBlueprintFromPath(AssetPath);
	if (!Blueprint)
	{
		return false;
	}

	UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
	if (!Graph)
	{
		UE_LOG(LogOliveBPNavigator, Warning, TEXT("Graph '%s' not found in '%s'"), *GraphName, *AssetPath);
		return false;
	}

	FKismetEditorUtilities::BringKismetToFocusAttentionOnObject(Graph);
	return true;
}

bool FOliveBlueprintNavigator::SelectAndZoomToNodes(const FString& AssetPath, const TArray<FString>& NodeIds)
{
	if (NodeIds.Num() == 0)
	{
		return false;
	}

	if (!OpenAsset(AssetPath))
	{
		return false;
	}

	UBlueprint* Blueprint = LoadBlueprintFromPath(AssetPath);
	if (!Blueprint)
	{
		return false;
	}

	TArray<UEdGraphNode*> FoundNodes;
	for (const FString& NodeIdStr : NodeIds)
	{
		UEdGraphNode* Node = FindNodeById(Blueprint, NodeIdStr);
		if (Node)
		{
			FoundNodes.Add(Node);
		}
	}

	if (FoundNodes.Num() == 0)
	{
		UE_LOG(LogOliveBPNavigator, Warning, TEXT("No nodes found from %d IDs in '%s'"), NodeIds.Num(), *AssetPath);
		return false;
	}

	FocusNodesInEditor(Blueprint, FoundNodes);
	return true;
}

bool FOliveBlueprintNavigator::NavigateToCompileError(const FString& AssetPath, const FOliveIRCompileError& Error)
{
	if (!Error.NodeId.IsEmpty())
	{
		return SelectAndZoomToNodes(AssetPath, { Error.NodeId });
	}
	else if (!Error.GraphName.IsEmpty())
	{
		return OpenGraph(AssetPath, Error.GraphName);
	}
	return OpenAsset(AssetPath);
}

bool FOliveBlueprintNavigator::CompileAndShowResults(const FString& AssetPath)
{
	UBlueprint* Blueprint = LoadBlueprintFromPath(AssetPath);
	if (!Blueprint)
	{
		return false;
	}

	OpenAsset(AssetPath);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	return true;
}

UBlueprint* FOliveBlueprintNavigator::LoadBlueprintFromPath(const FString& AssetPath)
{
	return Cast<UBlueprint>(StaticLoadObject(UBlueprint::StaticClass(), nullptr, *AssetPath));
}

UEdGraph* FOliveBlueprintNavigator::FindGraphByName(UBlueprint* Blueprint, const FString& GraphName)
{
	if (!Blueprint)
	{
		return nullptr;
	}

	auto SearchArray = [&GraphName](const TArray<UEdGraph*>& Graphs) -> UEdGraph*
	{
		for (UEdGraph* Graph : Graphs)
		{
			if (Graph && Graph->GetName() == GraphName)
			{
				return Graph;
			}
		}
		return nullptr;
	};

	if (UEdGraph* Found = SearchArray(Blueprint->UbergraphPages)) return Found;
	if (UEdGraph* Found = SearchArray(Blueprint->FunctionGraphs)) return Found;
	if (UEdGraph* Found = SearchArray(Blueprint->MacroGraphs)) return Found;
	if (UEdGraph* Found = SearchArray(Blueprint->DelegateSignatureGraphs)) return Found;

	return nullptr;
}

UEdGraphNode* FOliveBlueprintNavigator::FindNodeById(UBlueprint* Blueprint, const FString& NodeIdStr, UEdGraph** OutGraph)
{
	if (!Blueprint || NodeIdStr.IsEmpty())
	{
		return nullptr;
	}

	TArray<UEdGraph*> AllGraphs;
	AllGraphs.Append(Blueprint->UbergraphPages);
	AllGraphs.Append(Blueprint->FunctionGraphs);
	AllGraphs.Append(Blueprint->MacroGraphs);
	AllGraphs.Append(Blueprint->DelegateSignatureGraphs);

	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;

			FString GuidHyphens = Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
			FString GuidDigits = Node->NodeGuid.ToString(EGuidFormats::Digits);

			if (NodeIdStr == GuidHyphens || NodeIdStr == GuidDigits)
			{
				if (OutGraph) *OutGraph = Graph;
				return Node;
			}
		}
	}

	return nullptr;
}

void FOliveBlueprintNavigator::FocusNodesInEditor(UBlueprint* Blueprint, const TArray<UEdGraphNode*>& Nodes)
{
	if (Nodes.Num() == 0 || !Blueprint)
	{
		return;
	}

	FKismetEditorUtilities::BringKismetToFocusAttentionOnObject(Nodes[0]);

	IBlueprintEditor* BPEditor = GetBlueprintEditor(Blueprint);
	if (BPEditor)
	{
		FBlueprintEditor* Editor = static_cast<FBlueprintEditor*>(BPEditor);
		if (Editor)
		{
			Editor->JumpToNode(Nodes[0]);
		}
	}
}

IBlueprintEditor* FOliveBlueprintNavigator::GetBlueprintEditor(UBlueprint* Blueprint)
{
	if (!Blueprint || !GEditor)
	{
		return nullptr;
	}

	UAssetEditorSubsystem* AssetEditor = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!AssetEditor)
	{
		return nullptr;
	}

	IAssetEditorInstance* EditorInstance = AssetEditor->FindEditorForAsset(Blueprint, false);
	return EditorInstance ? static_cast<IBlueprintEditor*>(EditorInstance) : nullptr;
}
