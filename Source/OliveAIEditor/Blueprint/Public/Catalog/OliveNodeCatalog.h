// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IR/CommonIR.h"
#include "OliveNodeCatalog.generated.h"

// Forward declarations
class UClass;
class UFunction;
class UK2Node;

DECLARE_LOG_CATEGORY_EXTERN(LogOliveNodeCatalog, Log, All);

/**
 * Information about a Blueprint node type
 *
 * Contains all metadata needed to describe a Blueprint node type,
 * including its identity, pins, behavior flags, and usage documentation.
 * Used by the Node Catalog for AI agent discovery of available nodes.
 */
USTRUCT()
struct OLIVEAIEDITOR_API FOliveNodeTypeInfo
{
	GENERATED_BODY()

	// ============================================================================
	// Identity
	// ============================================================================

	/** Unique identifier (e.g., "K2Node_CallFunction", "Func_KismetMathLibrary_Sin") */
	UPROPERTY()
	FString TypeId;

	/** Human-readable name (e.g., "Sin", "Branch", "Print String") */
	UPROPERTY()
	FString DisplayName;

	/** Primary category (e.g., "Flow Control", "Math", "String") */
	UPROPERTY()
	FString Category;

	/** More specific subcategory (e.g., "Trig" under Math) */
	UPROPERTY()
	FString Subcategory;

	/** What the node does */
	UPROPERTY()
	FString Description;

	/** When and how to use this node */
	UPROPERTY()
	FString Usage;

	/** Example usage patterns */
	UPROPERTY()
	TArray<FString> Examples;

	/** Keywords for search (comma-separated or array) */
	UPROPERTY()
	TArray<FString> Keywords;

	// ============================================================================
	// Template Pins (approximate, actual may vary based on context)
	// ============================================================================

	/** Input pins on this node type */
	UPROPERTY()
	TArray<FOliveIRPin> InputPins;

	/** Output pins on this node type */
	UPROPERTY()
	TArray<FOliveIRPin> OutputPins;

	// ============================================================================
	// Behavior Flags
	// ============================================================================

	/** Whether this is a pure node (no exec pins, no side effects) */
	UPROPERTY()
	bool bIsPure = false;

	/** Whether this node has latent/async behavior (e.g., Delay) */
	UPROPERTY()
	bool bIsLatent = false;

	/** Whether this node requires a target object */
	UPROPERTY()
	bool bRequiresTarget = false;

	/** Whether this is a compact (small) node in the graph */
	UPROPERTY()
	bool bIsCompact = false;

	/** Whether this node is deprecated */
	UPROPERTY()
	bool bIsDeprecated = false;

	/** Whether this is an event node */
	UPROPERTY()
	bool bIsEvent = false;

	// ============================================================================
	// Function-Specific (for CallFunction nodes)
	// ============================================================================

	/** For function nodes: the function name */
	UPROPERTY()
	FString FunctionName;

	/** For function nodes: the owning class */
	UPROPERTY()
	FString FunctionClass;

	/** Class that must have this function (for member calls) */
	UPROPERTY()
	FString RequiredClass;

	// ============================================================================
	// Catalog Metadata
	// ============================================================================

	/** Tool/profile tags for filtering */
	UPROPERTY()
	TArray<FString> Tags;

	/** Source of this node info: "K2Node", "FunctionLibrary", "BuiltIn" */
	UPROPERTY()
	FString Source;

	// ============================================================================
	// Methods
	// ============================================================================

	/**
	 * Convert this node type info to JSON representation
	 * @return JSON object containing all node type information
	 */
	TSharedPtr<FJsonObject> ToJson() const;

	/**
	 * Create from JSON representation
	 * @param JsonObject The JSON to parse
	 * @return Parsed node type info
	 */
	static FOliveNodeTypeInfo FromJson(const TSharedPtr<FJsonObject>& JsonObject);

	/**
	 * Check if this node matches a search query
	 * @param Query Search query string
	 * @return Score (0 = no match, higher = better match)
	 */
	int32 MatchScore(const FString& Query) const;
};

/**
 * A lightweight suggestion returned by fuzzy matching against the node catalog.
 * Not a USTRUCT -- only used transiently and serialized directly to JSON.
 */
struct FOliveNodeSuggestion
{
	/** The catalog type ID or built-in node type name */
	FString TypeId;

	/** Human-readable display name */
	FString DisplayName;

	/** Match score (higher = better match, 0 = no match) */
	int32 Score = 0;
};

/**
 * FOliveNodeCatalog
 *
 * Searchable catalog of all Blueprint node types available in the engine.
 * Built from K2Node classes, Blueprint Function Libraries, and manual entries
 * for commonly used built-in nodes.
 *
 * Features:
 * - Full-text search across node names, descriptions, and keywords
 * - Category-based browsing
 * - Context-aware filtering (nodes available for a specific class)
 * - JSON export for MCP consumption
 * - Profile-based filtering for AI workflows
 *
 * Thread Safety:
 * - All read operations are thread-safe
 * - Initialize/Rebuild should only be called from game thread
 *
 * Usage:
 *   // Initialize on module startup
 *   FOliveNodeCatalog::Get().Initialize();
 *
 *   // Search for nodes
 *   TArray<FOliveNodeTypeInfo> Results = FOliveNodeCatalog::Get().Search("branch");
 *
 *   // Get by category
 *   TArray<FOliveNodeTypeInfo> MathNodes = FOliveNodeCatalog::Get().GetByCategory("Math");
 */
class OLIVEAIEDITOR_API FOliveNodeCatalog
{
public:
	/**
	 * Get the singleton instance
	 * @return Reference to the global node catalog
	 */
	static FOliveNodeCatalog& Get();

	// ============================================================================
	// Lifecycle
	// ============================================================================

	/**
	 * Initialize the catalog by building indexes from all available sources.
	 * Should be called once on module startup.
	 */
	void Initialize();

	/**
	 * Rebuild the catalog.
	 * Call after plugins are loaded or when node availability changes.
	 */
	void Rebuild();

	/**
	 * Shutdown and cleanup resources
	 */
	void Shutdown();

	/**
	 * Check if the catalog has been initialized
	 * @return true if Initialize() has been called successfully
	 */
	bool IsInitialized() const { return bInitialized; }

	// ============================================================================
	// Search & Queries
	// ============================================================================

	/**
	 * Search for nodes matching a query string
	 * @param Query Search query (matched against name, description, keywords)
	 * @param MaxResults Maximum number of results to return
	 * @return Array of matching node types, sorted by relevance
	 */
	TArray<FOliveNodeTypeInfo> Search(const FString& Query, int32 MaxResults = 50) const;

	/**
	 * Get all nodes in a category
	 * @param Category Category name to filter by
	 * @return Array of node types in that category
	 */
	TArray<FOliveNodeTypeInfo> GetByCategory(const FString& Category) const;

	/**
	 * Get nodes available for a specific class context
	 * @param ContextClass The class context (e.g., the Blueprint's parent class)
	 * @return Array of node types available for that class
	 */
	TArray<FOliveNodeTypeInfo> GetForClass(UClass* ContextClass) const;

	/**
	 * Get a specific node type by its ID
	 * @param TypeId The unique type identifier
	 * @return The node type info if found, empty optional otherwise
	 */
	TOptional<FOliveNodeTypeInfo> GetNodeType(const FString& TypeId) const;

	/**
	 * Check if a node type exists in the catalog
	 * @param TypeId The unique type identifier
	 * @return true if the node type exists
	 */
	bool HasNodeType(const FString& TypeId) const;

	/**
	 * Get all categories in the catalog
	 * @return Array of category names, sorted alphabetically
	 */
	TArray<FString> GetCategories() const;

	/**
	 * Get the total number of node types in the catalog
	 * @return Node count
	 */
	int32 GetNodeCount() const;

	// ============================================================================
	// JSON Export
	// ============================================================================

	/**
	 * Export the full catalog as JSON
	 * @return JSON string containing all node types
	 */
	FString ToJson() const;

	/**
	 * Export a filtered catalog for a specific profile
	 * @param ProfileName Focus profile to filter by
	 * @return JSON string containing filtered node types
	 */
	FString ToJsonForProfile(const FString& ProfileName) const;

	/**
	 * Export search results as JSON
	 * @param Query Search query
	 * @param MaxResults Maximum results
	 * @return JSON string containing search results
	 */
	FString SearchToJson(const FString& Query, int32 MaxResults = 50) const;

	/**
	 * Export category listing as JSON
	 * @return JSON string containing categories and node counts
	 */
	FString GetCategoriesJson() const;

	/**
	 * Return the top N closest matches for a query string.
	 * Uses the existing MatchScore infrastructure on catalog entries
	 * and also matches against built-in node type names from the
	 * OliveNodeTypes namespace (Branch, Sequence, CallFunction, etc.).
	 *
	 * Useful for generating "did you mean?" suggestions when the agent
	 * provides an unrecognized node type.
	 *
	 * @param Query  The unrecognized node type string to match against
	 * @param MaxResults  Maximum suggestions to return (default 5)
	 * @return Array of suggestions sorted by score descending
	 */
	TArray<FOliveNodeSuggestion> FuzzyMatch(const FString& Query, int32 MaxResults = 5) const;

private:
	FOliveNodeCatalog() = default;
	~FOliveNodeCatalog() = default;

	// Non-copyable
	FOliveNodeCatalog(const FOliveNodeCatalog&) = delete;
	FOliveNodeCatalog& operator=(const FOliveNodeCatalog&) = delete;

	// ============================================================================
	// Build Methods
	// ============================================================================

	/**
	 * Scan all UK2Node subclasses and add them to the catalog
	 */
	void BuildFromK2NodeClasses();

	/**
	 * Scan all UBlueprintFunctionLibrary subclasses and add their functions
	 */
	void BuildFromFunctionLibraries();

	/**
	 * Add manual entries for built-in flow control nodes
	 */
	void AddBuiltInFlowControlNodes();

	/**
	 * Add manual entries for common math nodes
	 */
	void AddMathNodes();

	/**
	 * Add manual entries for common string nodes
	 */
	void AddStringNodes();

	/**
	 * Add manual entries for common array nodes
	 */
	void AddArrayNodes();

	/**
	 * Add manual entries for common object/utility nodes
	 */
	void AddUtilityNodes();

	/**
	 * Build pin definitions from a UFunction signature
	 * @param Function The function to analyze
	 * @param OutInfo Node type info to populate with pins
	 */
	void BuildPinsFromFunction(const UFunction* Function, FOliveNodeTypeInfo& OutInfo);

	/**
	 * Build index structures for fast lookup
	 */
	void BuildIndexes();

	/**
	 * Add a node type to the catalog
	 * @param Info Node type info to add
	 */
	void AddNodeType(const FOliveNodeTypeInfo& Info);

	// ============================================================================
	// Helper Methods
	// ============================================================================

	/**
	 * Calculate fuzzy match score for search
	 * @param Query Search query
	 * @param Target String to match against
	 * @return Score (0 = no match, higher = better)
	 */
	int32 CalculateMatchScore(const FString& Query, const FString& Target) const;

	/**
	 * Extract category from function metadata
	 * @param Function The function to analyze
	 * @return Category string
	 */
	FString GetFunctionCategory(const UFunction* Function) const;

	/**
	 * Check if a function should be included in the catalog
	 * @param Function The function to check
	 * @return true if function should be cataloged
	 */
	bool ShouldIncludeFunction(const UFunction* Function) const;

	/**
	 * Convert UE type category to IR type category
	 * @param PinType The pin type to convert
	 * @return IR type category
	 */
	EOliveIRTypeCategory ConvertPinTypeCategory(const struct FEdGraphPinType& PinType) const;

	// ============================================================================
	// Data
	// ============================================================================

	/** Main storage: TypeId -> NodeTypeInfo */
	TMap<FString, FOliveNodeTypeInfo> NodeTypes;

	/** Category index: Category -> array of TypeIds */
	TMap<FString, TArray<FString>> CategoryIndex;

	/** Class context index: ClassName -> array of TypeIds for member functions */
	TMap<FString, TArray<FString>> ClassIndex;

	/** Tag index: Tag -> array of TypeIds */
	TMap<FString, TArray<FString>> TagIndex;

	/** Lock for thread-safe access */
	mutable FCriticalSection CatalogLock;

	/** Whether the catalog has been initialized */
	bool bInitialized = false;

	/** Whether a build is in progress */
	bool bIsBuilding = false;
};
