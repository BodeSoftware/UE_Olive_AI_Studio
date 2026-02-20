// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Information about a Behavior Tree node type
 */
struct OLIVEAIEDITOR_API FOliveBTNodeTypeInfo
{
	/** Full class name (e.g., "BTTask_MoveTo") */
	FString ClassName;

	/** Display name for UI */
	FString DisplayName;

	/** Category: "Task", "Decorator", or "Service" */
	FString Category;

	/** Description from class metadata */
	FString Description;

	/** Search keywords */
	TArray<FString> Keywords;

	/** Editable property names and types */
	TMap<FString, FString> DefaultProperties;

	/** Properties that are FBlackboardKeySelector */
	TArray<FString> BlackboardKeyProperties;

	/** Convert to JSON for MCP responses */
	TSharedPtr<FJsonObject> ToJson() const;
};

/**
 * FOliveBTNodeCatalog
 *
 * Scans all UBTTaskNode, UBTDecorator, and UBTService subclasses
 * and indexes them for search and discovery. Used by AI agents to
 * find available node types when building behavior trees.
 *
 * Usage:
 *   FOliveBTNodeCatalog& Catalog = FOliveBTNodeCatalog::Get();
 *   Catalog.Initialize();
 *   TArray<FOliveBTNodeTypeInfo> Results = Catalog.Search("move");
 */
class OLIVEAIEDITOR_API FOliveBTNodeCatalog
{
public:
	/** Get the singleton instance */
	static FOliveBTNodeCatalog& Get();

	/** Initialize by scanning all BT node classes */
	void Initialize();

	/** Shutdown and clear cached data */
	void Shutdown();

	/**
	 * Fuzzy search for node types
	 * @param Query Search query
	 * @return Matching node types sorted by relevance
	 */
	TArray<FOliveBTNodeTypeInfo> Search(const FString& Query) const;

	/**
	 * Get all nodes in a category
	 * @param Category "Task", "Decorator", or "Service"
	 * @return All node types in that category
	 */
	TArray<FOliveBTNodeTypeInfo> GetByCategory(const FString& Category) const;

	/**
	 * Get a node type by class name
	 * @param ClassName The class name
	 * @return Node type info, or nullptr if not found
	 */
	const FOliveBTNodeTypeInfo* GetByClassName(const FString& ClassName) const;

	/**
	 * Get all catalog entries as JSON (for MCP resource)
	 * @return JSON object with categories and node types
	 */
	TSharedPtr<FJsonObject> ToJson() const;

	/** Get total number of cataloged node types */
	int32 GetNodeCount() const { return AllNodes.Num(); }

	/** Whether the catalog has been initialized */
	bool IsInitialized() const { return bInitialized; }

private:
	FOliveBTNodeCatalog() = default;
	~FOliveBTNodeCatalog() = default;

	FOliveBTNodeCatalog(const FOliveBTNodeCatalog&) = delete;
	FOliveBTNodeCatalog& operator=(const FOliveBTNodeCatalog&) = delete;

	/** Extract editable properties from a class */
	void ExtractPropertiesFromClass(UClass* NodeClass, FOliveBTNodeTypeInfo& OutInfo);

	/** All cataloged node types */
	TArray<FOliveBTNodeTypeInfo> AllNodes;

	/** Lookup by class name */
	TMap<FString, int32> ClassNameIndex;

	/** Whether catalog has been initialized */
	bool bInitialized = false;
};
