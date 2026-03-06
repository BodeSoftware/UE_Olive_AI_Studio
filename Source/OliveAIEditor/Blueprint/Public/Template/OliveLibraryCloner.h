// Copyright Bode Software. All Rights Reserved.

/**
 * OliveLibraryCloner.h
 *
 * Clones a library template into a real Blueprint asset in the user's project.
 * Handles dependency resolution: every type, class, and function name from the
 * source project is resolved against the target project. Unresolvable
 * dependencies are demoted, skipped, or flagged for manual remapping.
 *
 * NOT a singleton -- instantiate fresh per clone operation (like FOlivePlanExecutor).
 * Must be called on the game thread.
 *
 * See plans/library-clone-design.md for full architecture.
 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

// Forward declarations
class UBlueprint;
class UClass;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class UScriptStruct;
struct FOliveLibraryTemplateInfo;

DECLARE_LOG_CATEGORY_EXTERN(LogOliveLibraryCloner, Log, All);

// =============================================================================
// Enums
// =============================================================================

/**
 * Disposition of a library node during clone classification.
 * Determines whether a node from the source template can be recreated.
 */
enum class ECloneNodeDisposition : uint8
{
	Create,      // Node can be fully recreated in the target project
	Skip,        // Node requires an unresolvable dependency; skip in this mode
	Placeholder, // Create a comment node documenting what was here (future)
};

/**
 * Clone mode controlling how much of the template is recreated.
 * Each mode is a strict superset of the previous.
 */
enum class ELibraryCloneMode : uint8
{
	Structure, // Variables, components, dispatchers, function signatures only
	Portable,  // Structure + engine-resolvable graph nodes (skips source-project-specific calls)
	Full,      // Everything; broken references become warnings
};

// =============================================================================
// Per-Node Classification
// =============================================================================

/**
 * Per-node classification result from the pre-classification phase.
 * Determines whether a specific library template node can be recreated
 * and stores the resolved data needed for creation.
 */
struct FCloneNodeClassification
{
	/** Whether to create, skip, or placeholder this node */
	ECloneNodeDisposition Disposition = ECloneNodeDisposition::Skip;

	/** Human-readable reason when Disposition is Skip (for warnings) */
	FString SkipReason;

	/** OliveNodeTypes constant for the resolved node type (when Create) */
	FString ResolvedNodeType;

	/** Properties to pass to FOliveNodeFactory::CreateNode() */
	TMap<FString, FString> Properties;
};

// =============================================================================
// Per-Graph Result
// =============================================================================

/**
 * Detailed result of cloning a single graph (event graph or function).
 * Used for per-graph reporting in the overall clone result.
 */
struct FCloneGraphResult
{
	FString GraphName;
	int32 NodesCreated = 0;
	int32 NodesSkipped = 0;
	int32 ConnectionsSucceeded = 0;
	int32 ConnectionsFailed = 0;
	int32 DefaultsSet = 0;
	int32 ExecGapsBridged = 0;
	int32 ExecGapsUnbridgeable = 0;
	TArray<FString> Warnings;
};

// =============================================================================
// Aggregate Clone Result
// =============================================================================

/**
 * Aggregate result of a library clone operation.
 * Contains structure counts, per-graph results, warnings, remap suggestions,
 * and compile status. Convertible to JSON for tool result reporting.
 */
struct OLIVEAIEDITOR_API FLibraryCloneResult
{
	bool bSuccess = false;
	FString AssetPath;
	FString TemplateId;
	FString ParentClass;
	FString ParentClassNote;
	ELibraryCloneMode Mode = ELibraryCloneMode::Structure;

	// Structure counts
	int32 VariablesCreated = 0;
	int32 VariablesDemoted = 0;
	int32 VariablesSkipped = 0;
	int32 ComponentsCreated = 0;
	int32 ComponentsSkipped = 0;
	int32 InterfacesAdded = 0;
	int32 InterfacesSkipped = 0;
	int32 DispatchersCreated = 0;
	int32 DispatchersSkipped = 0;
	int32 FunctionsCreated = 0;

	// Per-graph results (populated in Phase 2)
	TArray<FCloneGraphResult> GraphResults;

	// Aggregated warnings
	TArray<FString> Warnings;

	/**
	 * Suggestion for remapping an unresolved source-project type.
	 * Includes the source name, where it was used, and a human-readable suggestion.
	 */
	struct FRemapSuggestion
	{
		FString SourceName;
		TArray<FString> UsedIn;
		FString Suggestion;
	};
	TArray<FRemapSuggestion> RemapSuggestions;

	// Compile result
	bool bCompiled = false;
	bool bCompileSuccess = false;
	TArray<FString> CompileErrors;

	/**
	 * Convert the clone result to a JSON object for FOliveToolResult reporting.
	 * Matches the format specified in the design document Section 10.
	 * @return JSON object with all result fields
	 */
	TSharedPtr<FJsonObject> ToJson() const;
};

// =============================================================================
// FOliveLibraryCloner
// =============================================================================

/**
 * FOliveLibraryCloner
 *
 * Clones a library template into a real Blueprint asset.
 * NOT a singleton -- instantiate fresh per clone operation.
 * Must be called on the game thread.
 *
 * Usage:
 *   FOliveLibraryCloner Cloner;
 *   FLibraryCloneResult Result = Cloner.Clone(TemplateId, AssetPath, Mode, RemapMap);
 */
class OLIVEAIEDITOR_API FOliveLibraryCloner
{
public:
	FOliveLibraryCloner() = default;
	~FOliveLibraryCloner() = default;

	/**
	 * Clone a library template into a new Blueprint asset.
	 * This is the only public entry point.
	 *
	 * @param TemplateId          Library template ID (e.g., "combatfs_arrow_component")
	 * @param AssetPath           Target asset path (e.g., "/Game/Blueprints/BP_MyArrow")
	 * @param Mode                Clone depth (Structure/Portable/Full)
	 * @param InRemapMap          Source name -> target name mapping for type resolution
	 * @param GraphWhitelist      Optional: only clone these graphs (empty = all)
	 * @param ParentClassOverride Optional: override parent class instead of template's
	 * @return Clone result with detailed per-graph reporting and remap suggestions
	 */
	FLibraryCloneResult Clone(
		const FString& TemplateId,
		const FString& AssetPath,
		ELibraryCloneMode Mode,
		const TMap<FString, FString>& InRemapMap,
		const TArray<FString>& GraphWhitelist = {},
		const FString& ParentClassOverride = TEXT(""));

private:
	// ================================================================
	// Resolution
	// ================================================================

	/**
	 * Find the deepest resolvable parent class from the inheritance chain.
	 * Implements the "root native ancestor" strategy from design Section 3.3:
	 * walk depends_on chain, try ClassResolver on each ancestor's parent_class,
	 * use the deepest one that resolves.
	 *
	 * @param TemplateInfo The template whose parent class to resolve
	 * @param OutNote      Human-readable note about resolution (e.g., "resolved to native ancestor Actor")
	 * @return Parent class name suitable for CreateBlueprint(), or empty on failure
	 */
	FString ResolveParentClass(
		const FOliveLibraryTemplateInfo& TemplateInfo,
		FString& OutNote);

	/**
	 * Resolve a class name through the remap -> ClassResolver pipeline.
	 * Stage 1: Apply remap map. Stage 2: FOliveClassResolver::Resolve().
	 *
	 * @param SourceName Class name from the source template (may have _C suffix)
	 * @return Resolved UClass*, or nullptr if unresolvable
	 */
	UClass* ResolveClass(const FString& SourceName);

	/**
	 * Resolve a struct name through remap -> FindObject pipeline.
	 * Tries common engine paths: /Script/Engine, /Script/CoreUObject,
	 * /Script/NavigationSystem, /Script/Niagara, /Script/PhysicsCore.
	 *
	 * @param SourceName Struct name from the source template (with or without F prefix)
	 * @return Resolved UScriptStruct*, or nullptr if not found
	 */
	UScriptStruct* ResolveStruct(const FString& SourceName);

	/**
	 * Apply the remap map to a class name.
	 * Strips _C suffix for matching, then re-resolves through ClassResolver.
	 *
	 * @param SourceName Original class name from template
	 * @return Remapped name if found in map, otherwise the original SourceName
	 */
	FString ApplyRemap(const FString& SourceName) const;

	/**
	 * Track an unresolved type for remap suggestions.
	 * Accumulates usage contexts per source name.
	 *
	 * @param SourceName  The unresolved class/struct/enum name
	 * @param UsageContext Where it was used (e.g., "variable: ArrowActor (type)")
	 */
	void TrackUnresolved(const FString& SourceName, const FString& UsageContext);

	// ================================================================
	// Structure Creation
	// ================================================================

	/**
	 * Create variables from template JSON, handling type demotion for
	 * unresolvable object/struct/enum types.
	 *
	 * @param Blueprint    The Blueprint to add variables to
	 * @param AssetPath    Asset path for writer calls
	 * @param TemplateJson Full template JSON (top-level or nested)
	 * @param Result       Clone result to accumulate counts and warnings
	 */
	void CreateVariables(
		UBlueprint* Blueprint,
		const FString& AssetPath,
		const TSharedPtr<FJsonObject>& TemplateJson,
		FLibraryCloneResult& Result);

	/**
	 * Create components from template JSON tree, resolving component classes
	 * and clearing asset reference properties (meshes, materials).
	 *
	 * @param Blueprint    The Blueprint to add components to
	 * @param AssetPath    Asset path for writer calls
	 * @param TemplateJson Full template JSON
	 * @param Result       Clone result to accumulate counts and warnings
	 */
	void CreateComponents(
		UBlueprint* Blueprint,
		const FString& AssetPath,
		const TSharedPtr<FJsonObject>& TemplateJson,
		FLibraryCloneResult& Result);

	/**
	 * Add interfaces from template JSON, skipping unresolvable ones.
	 *
	 * @param Blueprint    The Blueprint to add interfaces to
	 * @param AssetPath    Asset path for writer calls
	 * @param TemplateJson Full template JSON
	 * @param Result       Clone result to accumulate counts and warnings
	 */
	void AddInterfaces(
		UBlueprint* Blueprint,
		const FString& AssetPath,
		const TSharedPtr<FJsonObject>& TemplateJson,
		FLibraryCloneResult& Result);

	/**
	 * Create event dispatchers from template JSON, resolving delegate
	 * parameter types through the pipeline.
	 *
	 * @param Blueprint    The Blueprint to add dispatchers to
	 * @param AssetPath    Asset path for writer calls
	 * @param TemplateJson Full template JSON
	 * @param Result       Clone result to accumulate counts and warnings
	 */
	void CreateDispatchers(
		UBlueprint* Blueprint,
		const FString& AssetPath,
		const TSharedPtr<FJsonObject>& TemplateJson,
		FLibraryCloneResult& Result);

	/**
	 * Create function graphs with signatures (inputs/outputs) but NO node
	 * graph content. Extracts function metadata from the graphs.functions
	 * array in the template JSON. This establishes the function API surface
	 * so that the intermediate compile populates SkeletonGeneratedClass.
	 *
	 * @param Blueprint    The Blueprint to add functions to
	 * @param AssetPath    Asset path for writer calls
	 * @param TemplateJson Full template JSON
	 * @param Result       Clone result to accumulate counts and warnings
	 */
	void CreateFunctionSignatures(
		UBlueprint* Blueprint,
		const FString& AssetPath,
		const TSharedPtr<FJsonObject>& TemplateJson,
		FLibraryCloneResult& Result);

	// ================================================================
	// Graph Cloning (Phase 2)
	// ================================================================

	/**
	 * Clone a single graph's nodes and connections.
	 * Runs the 6-phase pipeline: classify -> create -> wire exec -> wire data
	 * -> set defaults -> auto-layout.
	 *
	 * @param Blueprint The Blueprint being populated
	 * @param Graph     Target UEdGraph to clone into
	 * @param AssetPath Asset path for writer calls
	 * @param GraphJson JSON object for this graph (contains "nodes" array)
	 * @return Per-graph clone result with counts and warnings
	 */
	FCloneGraphResult CloneGraph(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const FString& AssetPath,
		const TSharedPtr<FJsonObject>& GraphJson);

	/**
	 * Phase 1: Pre-classify all nodes in a graph.
	 * Determines which nodes can be recreated (Create) vs skipped (Skip).
	 *
	 * @param Blueprint  The target Blueprint (for variable/function lookups)
	 * @param NodesArray JSON array of node objects from the template
	 * @return Map from library node ID to classification
	 */
	TMap<FString, FCloneNodeClassification> ClassifyNodes(
		UBlueprint* Blueprint,
		const TArray<TSharedPtr<FJsonValue>>& NodesArray);

	/**
	 * Phase 2: Create classified nodes via FOliveNodeFactory.
	 * Populates NodeMap with library node ID -> UEdGraphNode* mappings.
	 *
	 * @param Blueprint       The Blueprint being populated
	 * @param Graph           Target UEdGraph
	 * @param NodesArray      JSON array of node objects
	 * @param Classifications Pre-computed classifications from ClassifyNodes
	 * @param Result          Per-graph result to accumulate counts
	 */
	void CreateNodes(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TArray<TSharedPtr<FJsonValue>>& NodesArray,
		const TMap<FString, FCloneNodeClassification>& Classifications,
		FCloneGraphResult& Result);

	/**
	 * Phase 3: Wire exec connections between created nodes.
	 * Also performs exec gap repair by bridging across skipped nodes.
	 *
	 * @param NodesArray JSON array of node objects (for connection data)
	 * @param Result     Per-graph result (tracks connections and exec gaps)
	 */
	void WireExecConnections(
		const TArray<TSharedPtr<FJsonValue>>& NodesArray,
		FCloneGraphResult& Result);

	/**
	 * Phase 4: Wire data (non-exec) connections between created nodes.
	 * Allows automatic type conversion via FOlivePinConnector.
	 *
	 * @param NodesArray JSON array of node objects (for connection data)
	 * @param Result     Per-graph result (tracks connections)
	 */
	void WireDataConnections(
		const TArray<TSharedPtr<FJsonValue>>& NodesArray,
		FCloneGraphResult& Result);

	/**
	 * Phase 5: Set pin default values on created nodes.
	 * Skips connected pins and asset references.
	 *
	 * @param NodesArray JSON array of node objects (for default data)
	 * @param Result     Per-graph result (tracks defaults set)
	 */
	void SetPinDefaults(
		const TArray<TSharedPtr<FJsonValue>>& NodesArray,
		FCloneGraphResult& Result);

	// ================================================================
	// Pin & Variable Helpers
	// ================================================================

	/**
	 * Find a pin on a node by name and direction.
	 * Tries exact match first, then case-insensitive, then space-stripped fallback.
	 *
	 * @param Node      The node to search
	 * @param PinName   Pin name to find
	 * @param Direction EGPD_Input or EGPD_Output
	 * @return Pin if found, nullptr otherwise
	 */
	UEdGraphPin* FindPinByName(
		UEdGraphNode* Node,
		const FString& PinName,
		EEdGraphPinDirection Direction);

	/**
	 * Check if a pin default value looks like an asset reference.
	 * Returns true for /Game/, /Script/ paths and UE object reference format.
	 *
	 * @param DefaultValue The default value string to check
	 * @return True if value is an asset reference
	 */
	bool IsAssetReference(const FString& DefaultValue) const;

	/**
	 * Check if a variable exists on the Blueprint (NewVariables, FlattenedVariableNames,
	 * or SCS components).
	 *
	 * @param Blueprint The Blueprint to check
	 * @param VarName   Variable name to look up
	 * @return True if the variable exists
	 */
	bool VariableExistsOnBlueprint(const UBlueprint* Blueprint, const FString& VarName) const;

	// ================================================================
	// Internal Helpers
	// ================================================================

	/**
	 * Recursively create components from the tree structure.
	 * @param AssetPath    Asset path for writer calls
	 * @param TreeArray    JSON array of component tree nodes
	 * @param ParentName   Name of the parent component (empty for root attachment)
	 * @param Result       Clone result to accumulate counts and warnings
	 */
	void CreateComponentsFromTree(
		const FString& AssetPath,
		const TArray<TSharedPtr<FJsonValue>>& TreeArray,
		const FString& ParentName,
		FLibraryCloneResult& Result);

	/**
	 * Build remap suggestions from accumulated UnresolvedTypes.
	 * @param Result Clone result to populate RemapSuggestions
	 */
	void BuildRemapSuggestions(FLibraryCloneResult& Result);

	/**
	 * Parse a library template type object into an FOliveIRType.
	 * Handles the library template format: {"category": "object", "class": "Actor"}
	 * as well as simple string types.
	 *
	 * @param TypeJson The type JSON object or value
	 * @param OutClassName Populated with the resolved class name for object types
	 * @return Parsed IR type
	 */
	struct FParsedLibraryType
	{
		FString Category;
		FString ClassName;    // For object/class types
		FString StructName;   // For struct types
		FString EnumName;     // For enum types
		FString ElementType;  // For array types (raw JSON)
	};
	FParsedLibraryType ParseLibraryType(const TSharedPtr<FJsonObject>& TypeJson) const;

	// ================================================================
	// State (per-clone operation, reset each Clone() call)
	// ================================================================

	/** Clone mode for current operation */
	ELibraryCloneMode CurrentMode = ELibraryCloneMode::Portable;

	/** User-provided remap map (source name -> target name) */
	TMap<FString, FString> RemapMap;

	/** Library node ID -> created UEdGraphNode* (for Phase 2) */
	TMap<FString, UEdGraphNode*> NodeMap;

	/** Library node ID -> created Olive node ID (for Phase 2 reporting) */
	TMap<FString, FString> NodeIdMap;

	/** Unresolved source names -> usage contexts (for remap suggestions) */
	TMap<FString, TArray<FString>> UnresolvedTypes;

	/** The Blueprint being populated (set during Clone()) */
	UBlueprint* TargetBlueprint = nullptr;

	/** Variables that were flattened from unresolvable ancestor templates */
	TSet<FString> FlattenedVariableNames;

	/** The original class name of the template being cloned (for self-call detection) */
	FString TemplateClassName;
};
