// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/OliveToolRegistry.h"

DECLARE_LOG_CATEGORY_EXTERN(LogOliveMultiAsset, Log, All);

/**
 * FOliveMultiAssetOperations
 *
 * Provides cross-system operations that work with multiple assets
 * of different types. Orchestrates existing domain-specific tools.
 */
class OLIVEAIEDITOR_API FOliveMultiAssetOperations
{
public:
	/** Get singleton instance */
	static FOliveMultiAssetOperations& Get();

	/**
	 * Read multiple assets in one call (max 20)
	 * Dispatches to appropriate read tools based on asset type
	 * @param Paths Array of asset paths to read
	 * @param ReadMode Read mode for Blueprint assets (e.g., "summary", "full")
	 * @return Combined result with per-asset success/failure details
	 */
	FOliveToolResult BulkRead(const TArray<FString>& Paths, const FString& ReadMode = TEXT("summary"));

	/**
	 * Add an interface to multiple Blueprint assets
	 * Wraps blueprint.add_interface for each target
	 * @param Paths Array of Blueprint asset paths
	 * @param InterfaceName Name of the interface to implement
	 * @return Combined result with per-asset success/failure details
	 */
	FOliveToolResult ImplementInterface(const TArray<FString>& Paths, const FString& InterfaceName);

	/**
	 * Rename an asset with dependency awareness
	 * Uses FAssetToolsModule for rename + reference update
	 * @param AssetPath Current asset path
	 * @param NewName New name for the asset
	 * @param bUpdateReferences Whether to update all references to this asset
	 * @return Result with old/new path information
	 */
	FOliveToolResult RefactorRename(const FString& AssetPath, const FString& NewName, bool bUpdateReferences = true);

	/**
	 * Create a complete AI character setup: Blueprint + BehaviorTree + Blackboard
	 * Orchestrates existing creation tools in one transaction
	 * @param Name Character name (used as suffix for all assets)
	 * @param Path Content directory path for created assets
	 * @param ParentClass Parent class for the Blueprint (default: ACharacter)
	 * @param BlackboardKeys Key-value pairs of name->type for Blackboard keys
	 * @param BehaviorTreeRoot Root composite type (default: Selector)
	 * @return Result listing all created assets
	 */
	FOliveToolResult CreateAICharacter(
		const FString& Name,
		const FString& Path,
		const FString& ParentClass = TEXT("ACharacter"),
		const TArray<TPair<FString, FString>>& BlackboardKeys = {},
		const FString& BehaviorTreeRoot = TEXT("Selector")
	);

	/**
	 * Analyze a Blueprint and scaffold C++ migration artifacts.
	 * Non-destructive: leaves the source Blueprint intact.
	 */
	FOliveToolResult MoveToCpp(
		const FString& AssetPath,
		const FString& ModuleName,
		const FString& TargetClassName,
		const FString& ParentClass = TEXT(""),
		bool bCreateWrapperBlueprint = true,
		bool bCompileAfter = false
	);

private:
	FOliveMultiAssetOperations() = default;

	/** Determine the read tool for an asset based on its class */
	FString GetReadToolForAsset(const FString& AssetPath) const;

	/** Resolve and delete an asset by path for compensation flows */
	bool DeleteAssetByPath(const FString& AssetPath);
};
