// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FOliveIRCompileError;
class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class IBlueprintEditor;

/**
 * FOliveBlueprintNavigator
 *
 * Static utility class for navigating to Blueprint assets, graphs,
 * and nodes in the editor. Used by result card navigation actions
 * to open assets and zoom to specific elements.
 */
class OLIVEAIEDITOR_API FOliveBlueprintNavigator
{
public:
	/** Open an asset in the editor */
	static bool OpenAsset(const FString& AssetPath);

	/** Open a specific graph within a Blueprint */
	static bool OpenGraph(const FString& AssetPath, const FString& GraphName);

	/** Select and zoom to specific nodes in a Blueprint */
	static bool SelectAndZoomToNodes(const FString& AssetPath, const TArray<FString>& NodeIds);

	/** Navigate to a compile error location */
	static bool NavigateToCompileError(const FString& AssetPath, const FOliveIRCompileError& Error);

	/** Compile a Blueprint and show results */
	static bool CompileAndShowResults(const FString& AssetPath);

private:
	static UEdGraph* FindGraphByName(UBlueprint* Blueprint, const FString& GraphName);
	static UEdGraphNode* FindNodeById(UBlueprint* Blueprint, const FString& NodeIdStr, UEdGraph** OutGraph = nullptr);
	static void FocusNodesInEditor(UBlueprint* Blueprint, const TArray<UEdGraphNode*>& Nodes);
	static IBlueprintEditor* GetBlueprintEditor(UBlueprint* Blueprint);
	static UBlueprint* LoadBlueprintFromPath(const FString& AssetPath);
};
