// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Tickable.h"
#include "AssetRegistry/AssetData.h"
#include "OliveProjectIndex.generated.h"

/**
 * Information about an indexed asset
 */
USTRUCT()
struct OLIVEAIEDITOR_API FOliveAssetInfo
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString Path;

	UPROPERTY()
	FName AssetClass;

	UPROPERTY()
	FName ParentClass;  // For Blueprints

	UPROPERTY()
	TArray<FString> Interfaces;  // For Blueprints

	UPROPERTY()
	TArray<FString> Dependencies;

	UPROPERTY()
	TArray<FString> Referencers;

	UPROPERTY()
	FDateTime LastModified;

	// Quick access flags
	UPROPERTY()
	bool bIsBlueprint = false;

	UPROPERTY()
	bool bIsBehaviorTree = false;

	UPROPERTY()
	bool bIsBlackboard = false;

	/** For BehaviorTree assets, associated Blackboard asset path if available */
	UPROPERTY()
	FString AssociatedBlackboardPath;

	UPROPERTY()
	bool bIsPCG = false;

	UPROPERTY()
	bool bIsMaterial = false;

	UPROPERTY()
	bool bIsWidget = false;

	/** Convert to JSON */
	TSharedPtr<FJsonObject> ToJson() const;
};

/**
 * Class hierarchy node
 */
USTRUCT()
struct OLIVEAIEDITOR_API FOliveClassHierarchyNode
{
	GENERATED_BODY()

	UPROPERTY()
	FName ClassName;

	UPROPERTY()
	FName ParentClassName;

	UPROPERTY()
	TArray<FName> ChildClasses;

	UPROPERTY()
	bool bIsBlueprintClass = false;

	UPROPERTY()
	FString BlueprintPath;

	TSharedPtr<FJsonObject> ToJson() const;
};

/**
 * Project configuration
 */
USTRUCT()
struct OLIVEAIEDITOR_API FOliveProjectConfig
{
	GENERATED_BODY()

	UPROPERTY()
	FString ProjectName;

	UPROPERTY()
	FString EngineVersion;

	UPROPERTY()
	TArray<FString> EnabledPlugins;

	UPROPERTY()
	TArray<FString> PrimaryAssetTypes;

	TSharedPtr<FJsonObject> ToJson() const;
};

/**
 * Project Index
 *
 * Fast queryable index of all project assets with metadata.
 * Built on startup, incrementally updated via asset registry delegates.
 */
class OLIVEAIEDITOR_API FOliveProjectIndex : public FTickableEditorObject
{
public:
	FOliveProjectIndex();
	virtual ~FOliveProjectIndex();

	/**
	 * Get the singleton instance.
	 * Note: This requires the OliveAIEditor module to be loaded.
	 */
	static FOliveProjectIndex& Get();

	/** Initialize the index */
	void Initialize();

	/** Shutdown and cleanup */
	void Shutdown();

	/** Check if index is ready */
	bool IsReady() const { return bIsReady; }

	// ==========================================
	// Asset Queries
	// ==========================================

	/**
	 * Search assets by name (fuzzy)
	 * @param Query Search query
	 * @param MaxResults Maximum results to return
	 * @return Matching assets sorted by relevance
	 */
	TArray<FOliveAssetInfo> SearchAssets(const FString& Query, int32 MaxResults = 50) const;

	/**
	 * Get assets by class
	 * @param ClassName Class to filter by
	 * @return All assets of that class
	 */
	TArray<FOliveAssetInfo> GetAssetsByClass(FName ClassName) const;

	/**
	 * Get asset by exact path
	 * @param Path Asset path
	 * @return Asset info if found
	 */
	TOptional<FOliveAssetInfo> GetAssetByPath(const FString& Path) const;

	/**
	 * Get all Blueprints
	 */
	TArray<FOliveAssetInfo> GetAllBlueprints() const;

	/**
	 * Get all Behavior Trees
	 */
	TArray<FOliveAssetInfo> GetAllBehaviorTrees() const;

	// ==========================================
	// Class Hierarchy Queries
	// ==========================================

	/**
	 * Get child classes of a parent
	 */
	TArray<FName> GetChildClasses(FName ParentClass) const;

	/**
	 * Get inheritance chain for a class
	 */
	TArray<FName> GetParentChain(FName ChildClass) const;

	/**
	 * Check if one class inherits from another
	 */
	bool IsChildOf(FName ChildClass, FName ParentClass) const;

	// ==========================================
	// Dependency Queries
	// ==========================================

	/**
	 * Get assets this asset depends on
	 */
	TArray<FString> GetDependencies(const FString& AssetPath) const;

	/**
	 * Get assets that reference this asset
	 */
	TArray<FString> GetReferencers(const FString& AssetPath) const;

	// ==========================================
	// Project Info
	// ==========================================

	/**
	 * Get project configuration
	 */
	const FOliveProjectConfig& GetProjectConfig() const { return ProjectConfig; }

	/**
	 * Get total asset count
	 */
	int32 GetAssetCount() const;

	// ==========================================
	// JSON Export (for MCP Resources)
	// ==========================================

	FString GetSearchResultsJson(const FString& Query, int32 MaxResults = 50) const;
	FString GetAssetInfoJson(const FString& Path) const;
	FString GetClassHierarchyJson(FName RootClass = NAME_None) const;
	FString GetProjectConfigJson() const;

	// ==========================================
	// FTickableEditorObject
	// ==========================================

	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override { return bIsReady; }

private:
	void RebuildIndex();
	void BuildProjectConfig();
	void BuildClassHierarchy();

	// Asset registry callbacks
	void OnAssetAdded(const FAssetData& AssetData);
	void OnAssetRemoved(const FAssetData& AssetData);
	void OnAssetRenamed(const FAssetData& AssetData, const FString& OldPath);
	void OnAssetUpdated(const FAssetData& AssetData);
	void OnFilesLoaded();

	// Helper methods
	FOliveAssetInfo AssetDataToInfo(const FAssetData& AssetData) const;
	int32 CalculateFuzzyScore(const FString& Query, const FString& Target) const;
	void ProcessPendingUpdates();

	// Index data
	TMap<FString, FOliveAssetInfo> AssetIndex;  // Path -> Info
	TMap<FName, FOliveClassHierarchyNode> ClassHierarchy;
	FOliveProjectConfig ProjectConfig;

	// State
	bool bIsReady = false;
	bool bIsBuilding = false;

	// Delegate handles
	FDelegateHandle AssetAddedHandle;
	FDelegateHandle AssetRemovedHandle;
	FDelegateHandle AssetRenamedHandle;
	FDelegateHandle AssetUpdatedHandle;
	FDelegateHandle FilesLoadedHandle;

	// Incremental update queue
	TQueue<TPair<FString, bool>> PendingUpdates;  // Path, bIsRemoval
	float TimeSinceLastProcess = 0.0f;
	static constexpr float ProcessInterval = 0.5f;

	mutable FCriticalSection IndexLock;
};
