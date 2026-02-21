// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Information about a single PCG settings class (node type)
 */
struct OLIVEAIEDITOR_API FOlivePCGNodeTypeInfo
{
	/** Settings class name (e.g., "PCGSurfaceSamplerSettings") */
	FString ClassName;

	/** Friendly display name */
	FString DisplayName;

	/** Category (e.g., "Sampler", "Filter", "Spawner") */
	FString Category;

	/** Description from class metadata */
	FString Description;

	/** Search keywords */
	TArray<FString> Keywords;

	/** Editable property name -> type string */
	TMap<FString, FString> Properties;

	/** Serialize to JSON */
	TSharedPtr<FJsonObject> ToJson() const;
};

/**
 * FOlivePCGNodeCatalog
 *
 * Enumerates available PCG settings classes (node types) with fuzzy search.
 * Singleton pattern matching FOliveBTNodeCatalog.
 *
 * Usage:
 *   FOlivePCGNodeCatalog::Get().Initialize();
 *   auto Results = FOlivePCGNodeCatalog::Get().Search("sampler");
 */
class OLIVEAIEDITOR_API FOlivePCGNodeCatalog
{
public:
	/** Get the singleton instance */
	static FOlivePCGNodeCatalog& Get();

	/** Scan and cache all PCG settings classes */
	void Initialize();

	/** Clear cached data */
	void Shutdown();

	/**
	 * Fuzzy search for node types
	 * @param Query Search query string
	 * @return Matching node type infos, sorted by relevance
	 */
	TArray<FOlivePCGNodeTypeInfo> Search(const FString& Query) const;

	/**
	 * Get all node types in a category
	 * @param Category Category name
	 * @return Node types in the given category
	 */
	TArray<FOlivePCGNodeTypeInfo> GetByCategory(const FString& Category) const;

	/**
	 * Get a specific node type by class name
	 * @param ClassName Settings class name
	 * @return Pointer to the info, or nullptr if not found
	 */
	const FOlivePCGNodeTypeInfo* GetByClassName(const FString& ClassName) const;

	/**
	 * Find a UClass for a settings class name, trying multiple naming variations
	 * @param Name Name from the AI agent (may be partial)
	 * @return The UClass, or nullptr if not found
	 */
	UClass* FindSettingsClass(const FString& Name) const;

	/** Serialize the entire catalog to JSON */
	TSharedPtr<FJsonObject> ToJson() const;

private:
	FOlivePCGNodeCatalog() = default;
	~FOlivePCGNodeCatalog() = default;
	FOlivePCGNodeCatalog(const FOlivePCGNodeCatalog&) = delete;
	FOlivePCGNodeCatalog& operator=(const FOlivePCGNodeCatalog&) = delete;

	/** Extract editable properties from a class */
	void ExtractPropertiesFromClass(const UClass* Class, TMap<FString, FString>& OutProperties) const;

	/** Compute fuzzy search score */
	int32 ComputeSearchScore(const FOlivePCGNodeTypeInfo& Info, const FString& Query) const;

	/** All discovered node types */
	TArray<FOlivePCGNodeTypeInfo> NodeTypes;

	/** Class name -> index in NodeTypes */
	TMap<FString, int32> ClassNameIndex;

	/** Class name -> UClass pointer */
	TMap<FString, UClass*> ClassMap;

	bool bInitialized = false;
};
