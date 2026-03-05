// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "MCP/OliveToolRegistry.h"  // FOliveToolResult

DECLARE_LOG_CATEGORY_EXTERN(LogOliveTemplates, Log, All);

/**
 * Metadata for a loaded template (both factory and reference).
 * Populated during Initialize() from JSON files on disk.
 */
struct OLIVEAIEDITOR_API FOliveTemplateInfo
{
    /** Unique template identifier (filename without extension) */
    FString TemplateId;

    /** "factory" or "reference" */
    FString TemplateType;

    /** Human-readable display name */
    FString DisplayName;

    /** Structural description for AI reasoning (mandatory) */
    FString CatalogDescription;

    /** Comma-separated domain examples for fast matching (optional) */
    FString CatalogExamples;

    /** Space-separated tags for reference templates */
    FString Tags;

    /** Absolute path to the JSON file on disk */
    FString FilePath;

    /** Full parsed JSON content (cached for ApplyTemplate and GetTemplateContent) */
    TSharedPtr<FJsonObject> FullJson;
};

/**
 * Lightweight function summary for the library index.
 * No node data -- loaded on demand from disk.
 */
struct OLIVEAIEDITOR_API FOliveLibraryFunctionSummary
{
    FString Name;
    FString GraphType;  // "Function" or "EventGraph"
    int32 NodeCount = 0;
    FString Description;
    FString Tags;
};

/**
 * Library template metadata. Lightweight -- no full JSON in memory at startup.
 */
struct OLIVEAIEDITOR_API FOliveLibraryTemplateInfo
{
    FString TemplateId;
    FString DisplayName;
    FString CatalogDescription;
    FString Tags;
    FString InheritedTags;
    FString FilePath;
    FString SourceProject;

    FString BlueprintType;       // "Normal", "ActorComponent", etc.
    FString ParentClassName;
    FString ParentClassSource;   // "native" or "blueprint"
    FString DependsOn;           // Template ID of parent (empty if root)

    bool bHasParameters = false;

    TArray<FString> InterfaceNames;
    TArray<FString> VariableNames;
    TArray<FString> ComponentNames;
    TArray<FString> DispatcherNames;
    TArray<FOliveLibraryFunctionSummary> Functions;
};

/**
 * FOliveLibraryIndex
 *
 * Scalable index for all templates. Stores lightweight metadata at startup.
 * Full JSON loaded lazily on demand with LRU cache.
 * Supports text search across template names, tags, function names, and keywords.
 */
class OLIVEAIEDITOR_API FOliveLibraryIndex
{
public:
    /** Scan template directories recursively, index metadata, discard full JSON. */
    void Initialize(const FString& TemplatesRootDir);

    /** Clear all data. */
    void Shutdown();

    /** Search across all templates and functions. Returns JSON results array. */
    TArray<TSharedPtr<FJsonObject>> Search(const FString& Query, int32 MaxResults = 20) const;

    /** Find a template by ID. Returns nullptr if not found. */
    const FOliveLibraryTemplateInfo* FindTemplate(const FString& TemplateId) const;

    /** Get all indexed templates. */
    const TMap<FString, FOliveLibraryTemplateInfo>& GetAllTemplates() const { return Templates; }

    /** Lazy-load full JSON for a template from disk. Uses LRU cache. */
    TSharedPtr<FJsonObject> LoadFullJson(const FString& TemplateId) const;

    /** Get a specific function's graph data as formatted string. */
    FString GetFunctionContent(const FString& TemplateId, const FString& FunctionName) const;

    /** Get template structure overview (no node data). */
    FString GetTemplateOverview(const FString& TemplateId) const;

    /** Walk depends_on chain from child to root. Returns ordered array root->leaf. */
    TArray<const FOliveLibraryTemplateInfo*> ResolveInheritanceChain(const FString& TemplateId) const;

    /** Build compact catalog text for prompt injection. */
    FString BuildCatalog() const;

    /** Sentinel prefix returned by GetFunctionContent when a function is not found.
     *  Callers check for this to distinguish not-found from real content. */
    static const FString& GetFuncNotFoundSentinel()
    {
        static const FString S(TEXT("@@FUNC_NOT_FOUND@@"));
        return S;
    }

    /** Tokenize a string into lowercase words (splits on spaces/underscores/hyphens, filters stop words). */
    static TArray<FString> Tokenize(const FString& Input);

    /** Total indexed templates. */
    int32 Num() const { return Templates.Num(); }

    /** Whether Initialize has been called. */
    bool IsInitialized() const { return bInitialized; }

private:
    /** Parse one JSON file for metadata only. */
    bool IndexTemplateFile(const FString& FilePath);

    /** Build search tokens for a template and add to inverted index. */
    void BuildSearchTokens(const FOliveLibraryTemplateInfo& Info);

    /** All indexed templates keyed by TemplateId. */
    TMap<FString, FOliveLibraryTemplateInfo> Templates;

    /** Inverted search index: token -> set of template IDs. */
    TMap<FString, TSet<FString>> SearchIndex;

    /** LRU cache for recently loaded full JSONs. */
    mutable TMap<FString, TSharedPtr<FJsonObject>> JsonCache;
    mutable TArray<FString> CacheOrder;
    static constexpr int32 MAX_CACHE_SIZE = 10;

    bool bInitialized = false;
};

/**
 * FOliveTemplateSystem
 *
 * Loads template JSON files from Content/Templates/, builds an auto-generated
 * catalog block for prompt injection, and provides ApplyTemplate() for factory
 * template execution.
 *
 * Singleton. Game thread only.
 *
 * File discovery pattern matches OlivePromptAssembler::LoadPromptTemplates()
 * and FOliveCrossSystemToolHandlers::LoadRecipeLibrary() -- uses IPlatformFile
 * to scan disk directories under the plugin's Content/ folder.
 */
class OLIVEAIEDITOR_API FOliveTemplateSystem
{
public:
    static FOliveTemplateSystem& Get();

    /**
     * Scan Content/Templates/ recursively and load all template metadata.
     * Call after FOliveNodeCatalog::Initialize() and before prompt assembler
     * needs the catalog block.
     */
    void Initialize();

    /** Hot-reload: re-scan and rebuild everything. */
    void Reload();

    /** Tear down cached data. Called during module shutdown. */
    void Shutdown();

    // ============================================================
    // Query
    // ============================================================

    /** Get the cached catalog block string for prompt injection. */
    const FString& GetCatalogBlock() const { return CachedCatalog; }

    /** Find a template by ID. Returns nullptr if not found. */
    const FOliveTemplateInfo* FindTemplate(const FString& TemplateId) const;

    /** Get all loaded template infos. */
    const TMap<FString, FOliveTemplateInfo>& GetAllTemplates() const { return Templates; }

    /** Get templates filtered by type ("factory" or "reference"). */
    TArray<const FOliveTemplateInfo*> GetTemplatesByType(const FString& Type) const;

    /** Return true if at least one template is loaded. */
    bool HasTemplates() const { return Templates.Num() > 0; }

    /** Search across all indexed templates (library + factory). */
    TArray<TSharedPtr<FJsonObject>> SearchTemplates(const FString& Query, int32 MaxResults = 20) const;

    /** Access the library index directly. */
    const FOliveLibraryIndex& GetLibraryIndex() const { return LibraryIndex; }

    // ============================================================
    // Execution
    // ============================================================

    /**
     * Apply a factory template: create Blueprint, add variables, add dispatchers,
     * create functions, execute plan JSON for each function, compile.
     *
     * @param TemplateId   Factory template ID
     * @param UserParams   Parameter overrides from the AI
     * @param PresetName   Optional preset name (applied before UserParams)
     * @param AssetPath    Where to create the Blueprint (/Game/...)
     * @return Tool result with created asset info or error
     */
    FOliveToolResult ApplyTemplate(
        const FString& TemplateId,
        const TMap<FString, FString>& UserParams,
        const FString& PresetName,
        const FString& AssetPath);

    /**
     * Get a template's content as a formatted string for AI reference reading.
     * For factory templates: parameter schema + presets + function plan outlines.
     * For reference templates: all patterns (or a specific one).
     *
     * @param TemplateId   Template ID
     * @param PatternName  For reference templates, optional specific pattern
     * @return Formatted content string, or empty + error via out param
     */
    FString GetTemplateContent(
        const FString& TemplateId,
        const FString& PatternName = TEXT("")) const;

private:
    FOliveTemplateSystem() = default;

    // Non-copyable
    FOliveTemplateSystem(const FOliveTemplateSystem&) = delete;
    FOliveTemplateSystem& operator=(const FOliveTemplateSystem&) = delete;

    // ============================================================
    // Loading
    // ============================================================

    /** Get the plugin's Content/Templates/ directory path. */
    FString GetTemplatesDirectory() const;

    /** Recursively scan a directory for .json files and load them. */
    void ScanDirectory(const FString& Directory);

    /** Parse a single JSON file and register it as a template. */
    bool LoadTemplateFile(const FString& FilePath);

    // ============================================================
    // Catalog
    // ============================================================

    /** Rebuild CachedCatalog from all loaded Templates. */
    void RebuildCatalog();

    // ============================================================
    // Parameter Substitution
    // ============================================================

    /**
     * Merge default parameters from the template schema with user overrides.
     * If PresetName is non-empty, preset values are applied between defaults
     * and user overrides.
     */
    TMap<FString, FString> MergeParameters(
        const FOliveTemplateInfo& Info,
        const TMap<FString, FString>& UserParams,
        const FString& PresetName) const;

    /**
     * Replace ${param} tokens in a JSON string with values from MergedParams.
     * Logs warnings for any unsubstituted tokens.
     */
    FString SubstituteParameters(
        const FString& Input,
        const TMap<FString, FString>& MergedParams) const;

    /**
     * Evaluate simple conditional expressions like "${start_full} ? ${max_value} : 0".
     * Only supports bool ternary for defaults. Returns the substituted string
     * with conditionals resolved.
     */
    FString EvaluateConditionals(
        const FString& Input,
        const TMap<FString, FString>& MergedParams) const;

    // ============================================================
    // State
    // ============================================================

    /** All loaded templates keyed by TemplateId */
    TMap<FString, FOliveTemplateInfo> Templates;

    /** Cached catalog block text (rebuilt on Initialize/Reload) */
    FString CachedCatalog;

    /** Library index for scalable template search and lazy loading */
    FOliveLibraryIndex LibraryIndex;

    /** Whether Initialize() has been called */
    bool bInitialized = false;
};
