# Phase 0 — Detailed Implementation Plan

> **Goal:** Plugin loads, MCP server runs, chat UI opens, agent can connect (internal or external), and project index is queryable.
> **Current State:** Basic single-module plugin shell exists. Needs complete restructure.

---

## Table of Contents

1. [Architecture Changes Required](#1-architecture-changes-required)
2. [Task Breakdown](#2-task-breakdown)
3. [Module 1: Plugin Restructure](#3-module-1-plugin-restructure)
4. [Module 2: Shared Services Layer](#4-module-2-shared-services-layer)
5. [Module 3: Project Index](#5-module-3-project-index)
6. [Module 4: Tool Registry & MCP Server](#6-module-4-tool-registry--mcp-server)
7. [Module 5: Provider Abstraction & Conversation Manager](#7-module-5-provider-abstraction--conversation-manager)
8. [Module 6: Chat UI (Slate Panel)](#8-module-6-chat-ui-slate-panel)
9. [Module 7: Focus Profiles & System Prompts](#9-module-7-focus-profiles--system-prompts)
10. [Module 8: Configuration System](#10-module-8-configuration-system)
11. [Edge Cases & Failure Modes](#11-edge-cases--failure-modes)
12. [Testing Checklist](#12-testing-checklist)
13. [File Structure](#13-file-structure)
14. [Implementation Order](#14-implementation-order)

---

## 1. Architecture Changes Required

### Current State
```
Source/
└── UE_Olive_AI_Studio/          # Single Runtime module
    ├── UE_Olive_AI_Studio.Build.cs
    ├── Public/UE_Olive_AI_Studio.h
    └── Private/UE_Olive_AI_Studio.cpp
```

### Target State
```
Source/
├── OliveAIRuntime/              # Minimal runtime module (IR definitions only)
│   ├── OliveAIRuntime.Build.cs
│   ├── Public/
│   │   └── IR/                  # Intermediate Representation structs
│   │       ├── OliveIRTypes.h
│   │       ├── BlueprintIR.h
│   │       ├── BehaviorTreeIR.h
│   │       ├── PCGIR.h
│   │       └── CommonIR.h
│   └── Private/
│       └── OliveAIRuntimeModule.cpp
│
└── OliveAIEditor/               # Editor-only module (everything else)
    ├── OliveAIEditor.Build.cs
    ├── Public/
    │   ├── OliveAIEditorModule.h
    │   ├── UI/                  # Chat panel, operation feed, context bar
    │   ├── Chat/                # Conversation manager, streaming
    │   ├── Providers/           # API provider clients
    │   ├── MCP/                 # MCP server, protocol, transport
    │   ├── Services/            # Shared services (transactions, validation)
    │   ├── Index/               # Project index
    │   ├── Context/             # Context assembly
    │   ├── Profiles/            # Focus profiles
    │   └── Settings/            # Configuration
    └── Private/
        └── (mirrors Public)
```

### Key Module Dependencies

**OliveAIRuntime:**
- `Core`
- `CoreUObject`
- `Json`
- `JsonUtilities`

**OliveAIEditor:**
- `Core`, `CoreUObject`, `Engine`
- `UnrealEd` (editor APIs)
- `Slate`, `SlateCore`, `EditorStyle`, `InputCore`
- `HTTP` (API calls to providers)
- `HttpServer` (MCP server)
- `Json`, `JsonUtilities`
- `AssetRegistry` (project index)
- `BlueprintGraph`, `Kismet` (future phases, but register now)
- `Projects` (engine version detection)
- `DeveloperSettings` (configuration)
- `OliveAIRuntime` (our runtime module)

---

## 2. Task Breakdown

### High-Level Task List

| # | Task | Complexity | Dependencies |
|---|------|------------|--------------|
| 1 | Plugin restructure (2 modules) | Medium | None |
| 2 | IR struct definitions | Low | Task 1 |
| 3 | Shared Services Layer | Medium | Task 1 |
| 4 | Project Index | Medium | Task 3 |
| 5 | Tool Registry | Medium | Task 3 |
| 6 | MCP Server Core | High | Task 5 |
| 7 | Provider Abstraction | Medium | Task 3 |
| 8 | OpenRouter Client | High | Task 7 |
| 9 | Anthropic Client | Medium | Task 7 |
| 10 | Conversation Manager | High | Task 7, 8 |
| 11 | Chat UI - Base Panel | High | Task 1 |
| 12 | Chat UI - Message List | Medium | Task 11 |
| 13 | Chat UI - Operation Feed | Medium | Task 11 |
| 14 | Chat UI - Context Bar | Medium | Task 11 |
| 15 | Chat UI - Input Field + @Mentions | High | Task 11, 4 |
| 16 | Focus Profiles System | Medium | Task 5 |
| 17 | System Prompt Assembly | Medium | Task 16 |
| 18 | Configuration UI | Medium | Task 1 |
| 19 | Integration & Testing | High | All |

---

## 3. Module 1: Plugin Restructure

### 3.1 Update .uplugin

**File:** `UE_Olive_AI_Studio.uplugin`

```json
{
    "FileVersion": 3,
    "Version": 1,
    "VersionName": "0.1.0-alpha",
    "FriendlyName": "Olive AI Studio",
    "Description": "AI-powered development assistant for Unreal Engine with built-in chat and MCP server support",
    "Category": "Editor",
    "CreatedBy": "Bode Software",
    "CreatedByURL": "https://bodesoftware.com/",
    "DocsURL": "",
    "MarketplaceURL": "",
    "SupportURL": "",
    "CanContainContent": true,
    "IsBetaVersion": true,
    "IsExperimentalVersion": false,
    "Installed": false,
    "Modules": [
        {
            "Name": "OliveAIRuntime",
            "Type": "Runtime",
            "LoadingPhase": "Default"
        },
        {
            "Name": "OliveAIEditor",
            "Type": "Editor",
            "LoadingPhase": "PostEngineInit"
        }
    ],
    "Plugins": [
        {
            "Name": "EditorScriptingUtilities",
            "Enabled": true
        }
    ]
}
```

### 3.2 Runtime Module Build.cs

**File:** `Source/OliveAIRuntime/OliveAIRuntime.Build.cs`

```csharp
using UnrealBuildTool;

public class OliveAIRuntime : ModuleRules
{
    public OliveAIRuntime(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Json",
            "JsonUtilities"
        });
    }
}
```

### 3.3 Editor Module Build.cs

**File:** `Source/OliveAIEditor/OliveAIEditor.Build.cs`

```csharp
using UnrealBuildTool;

public class OliveAIEditor : ModuleRules
{
    public OliveAIEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "OliveAIRuntime"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            // Editor
            "UnrealEd",
            "EditorFramework",
            "EditorStyle",
            "EditorSubsystem",
            "ToolMenus",
            "LevelEditor",
            "MainFrame",
            "WorkspaceMenuStructure",

            // UI
            "Slate",
            "SlateCore",
            "InputCore",
            "PropertyEditor",

            // Networking
            "HTTP",
            "HttpServer",
            "Sockets",
            "Networking",

            // Data
            "Json",
            "JsonUtilities",

            // Asset Management
            "AssetRegistry",
            "AssetTools",
            "ContentBrowser",

            // Blueprint (for future, register dependencies now)
            "BlueprintGraph",
            "Kismet",
            "KismetWidgets",
            "GraphEditor",

            // Configuration
            "Projects",
            "DeveloperSettings",
            "Settings",
            "SettingsEditor"
        });
    }
}
```

### 3.4 Edge Cases for Module Setup

| Edge Case | Handling |
|-----------|----------|
| Module load order issues | Editor module explicitly depends on Runtime; LoadingPhase PostEngineInit ensures editor subsystems available |
| Missing dependency at startup | Build.cs lists all required modules; compilation fails early if missing |
| Hot reload during development | Module implements proper Shutdown to clean up MCP server, timers |
| PIE (Play in Editor) session | Editor module doesn't interfere; tools reject writes during PIE |

---

## 4. Module 2: Shared Services Layer

### 4.1 Transaction Manager

**Purpose:** Wraps all write operations in `FScopedTransaction` for undo support.

**File:** `OliveAIEditor/Public/Services/OliveTransactionManager.h`

```cpp
DECLARE_LOG_CATEGORY_EXTERN(LogOliveTransaction, Log, All);

class OLIVEAIEDITOR_API FOliveTransactionManager
{
public:
    static FOliveTransactionManager& Get();

    // Begin a named transaction, returns transaction ID
    int32 BeginTransaction(const FText& Description);

    // End transaction by ID
    void EndTransaction(int32 TransactionId);

    // Cancel/rollback transaction
    void CancelTransaction(int32 TransactionId);

    // Scoped helper
    class FScopedOliveTransaction
    {
    public:
        FScopedOliveTransaction(const FText& Description);
        ~FScopedOliveTransaction();
        void Cancel();
    private:
        TUniquePtr<FScopedTransaction> Transaction;
        bool bCancelled = false;
    };

private:
    TMap<int32, TUniquePtr<FScopedTransaction>> ActiveTransactions;
    int32 NextTransactionId = 1;
    FCriticalSection TransactionLock;
};
```

**Edge Cases:**
| Edge Case | Handling |
|-----------|----------|
| Nested transactions | UE supports nested FScopedTransaction; maintain correct nesting |
| Transaction abandoned (crash) | FScopedTransaction destructor handles cleanup |
| Very long transaction (>100 operations) | Periodically call `GEditor->EndTransaction()` to batch |
| Transaction during PIE | Reject before starting transaction |

### 4.2 Asset Resolver

**Purpose:** Load assets by path/name/class with comprehensive error handling.

**File:** `OliveAIEditor/Public/Services/OliveAssetResolver.h`

```cpp
UENUM()
enum class EOliveAssetResolveResult : uint8
{
    Success,
    NotFound,
    WrongType,
    Redirected,
    LoadFailed,
    CurrentlyEdited,
    PathInvalid
};

USTRUCT()
struct FOliveAssetResolveInfo
{
    GENERATED_BODY()

    EOliveAssetResolveResult Result = EOliveAssetResolveResult::NotFound;
    FString ResolvedPath;
    FString OriginalPath;
    FString RedirectedFrom;  // If followed a redirector
    bool bIsBeingEdited = false;
    FString ErrorMessage;

    UObject* Asset = nullptr;
};

class OLIVEAIEDITOR_API FOliveAssetResolver
{
public:
    static FOliveAssetResolver& Get();

    // Resolve by exact path
    FOliveAssetResolveInfo ResolveByPath(const FString& AssetPath, UClass* ExpectedClass = nullptr);

    // Resolve by name with optional class filter
    TArray<FOliveAssetResolveInfo> ResolveByName(const FString& AssetName, UClass* ClassFilter = nullptr);

    // Check if asset is currently being edited
    bool IsAssetBeingEdited(const FString& AssetPath) const;

    // Get the editor for an asset if open
    IAssetEditorInstance* GetAssetEditor(const FString& AssetPath) const;

    // Follow redirectors
    FString FollowRedirectors(const FString& AssetPath) const;

private:
    UAssetEditorSubsystem* GetAssetEditorSubsystem() const;
};
```

**Edge Cases:**
| Edge Case | Handling |
|-----------|----------|
| Asset path with spaces | Handle URL encoding; paths like `/Game/My Assets/BP_Test` work |
| Redirector chain | Follow up to 10 redirectors, then error |
| Asset loaded but not saved | `bIsDirty` flag in resolve info |
| Asset in external package | Handle mounted pak files correctly |
| Case sensitivity | FName comparison is case-insensitive; normalize |
| Plugin content paths | `/PluginName/` paths resolve correctly |

### 4.3 Validation Engine

**Purpose:** Rule-based validation for all operations before execution.

**File:** `OliveAIEditor/Public/Services/OliveValidationEngine.h`

```cpp
UENUM()
enum class EOliveValidationSeverity : uint8
{
    Error,      // Operation blocked
    Warning,    // Operation proceeds with warning
    Info        // Informational only
};

USTRUCT()
struct FOliveValidationResult
{
    GENERATED_BODY()

    bool bValid = true;
    TArray<FOliveValidationMessage> Messages;

    void AddError(const FString& Code, const FString& Message, const FString& Suggestion = TEXT(""));
    void AddWarning(const FString& Code, const FString& Message, const FString& Suggestion = TEXT(""));
    bool HasErrors() const;
    FString ToJson() const;
};

USTRUCT()
struct FOliveValidationMessage
{
    GENERATED_BODY()

    EOliveValidationSeverity Severity;
    FString Code;           // e.g., "TYPE_CONSTRAINT_VIOLATION"
    FString Message;        // Human-readable
    FString Suggestion;     // What the AI should do instead
    TMap<FString, FString> Details;
};

// Rule interface
class IOliveValidationRule
{
public:
    virtual ~IOliveValidationRule() = default;
    virtual FName GetRuleName() const = 0;
    virtual FOliveValidationResult Validate(const TSharedPtr<FJsonObject>& ToolParams, UObject* TargetAsset) = 0;
};

class OLIVEAIEDITOR_API FOliveValidationEngine
{
public:
    static FOliveValidationEngine& Get();

    // Register a validation rule
    void RegisterRule(TSharedPtr<IOliveValidationRule> Rule);

    // Run all applicable rules for an operation
    FOliveValidationResult ValidateOperation(
        const FString& ToolName,
        const TSharedPtr<FJsonObject>& Params,
        UObject* TargetAsset = nullptr
    );

    // Built-in rule sets
    void RegisterBlueprintRules();
    void RegisterBehaviorTreeRules();
    void RegisterPCGRules();

private:
    TArray<TSharedPtr<IOliveValidationRule>> Rules;
};
```

**Built-in Rules for Phase 0:**
1. `SchemaValidationRule` — JSON params match expected schema
2. `AssetExistsRule` — Target asset exists when required
3. `PIEProtectionRule` — Block writes during Play in Editor
4. `AssetTypeRule` — Operation valid for target asset type

**Edge Cases:**
| Edge Case | Handling |
|-----------|----------|
| Rule throws exception | Catch, log, treat as error |
| Conflicting rules | First error wins; collect all warnings |
| Async validation needed | Return pending result, callback when complete |
| Rule needs network access | Not supported in Phase 0; rules must be synchronous |

### 4.4 Error Response Builder

**Purpose:** Standardized JSON error format for all tool responses.

**File:** `OliveAIEditor/Public/Services/OliveErrorBuilder.h`

```cpp
class OLIVEAIEDITOR_API FOliveErrorBuilder
{
public:
    static TSharedPtr<FJsonObject> Success(const TSharedPtr<FJsonObject>& Data = nullptr);

    static TSharedPtr<FJsonObject> Error(
        const FString& Code,
        const FString& Message,
        const FString& Suggestion = TEXT(""),
        const TMap<FString, FString>& Details = {}
    );

    static TSharedPtr<FJsonObject> FromValidationResult(const FOliveValidationResult& Result);

    // Common error codes
    static const FString ERR_NOT_FOUND;           // "ASSET_NOT_FOUND"
    static const FString ERR_TYPE_CONSTRAINT;     // "TYPE_CONSTRAINT_VIOLATION"
    static const FString ERR_INVALID_PARAMS;      // "INVALID_PARAMETERS"
    static const FString ERR_PIE_ACTIVE;          // "PIE_ACTIVE"
    static const FString ERR_COMPILE_FAILED;      // "COMPILATION_FAILED"
    static const FString ERR_INTERNAL;            // "INTERNAL_ERROR"
};
```

**Standard Error Response Format:**
```json
{
    "success": false,
    "error": {
        "code": "TYPE_CONSTRAINT_VIOLATION",
        "message": "Blueprint Interfaces cannot contain variables.",
        "details": {
            "blueprint": "/Game/Interfaces/BPI_Damageable",
            "blueprint_type": "Interface",
            "attempted_operation": "add_variable"
        },
        "suggestion": "Add the variable to the implementing Blueprint instead.",
        "severity": "error"
    }
}
```

---

## 5. Module 3: Project Index

### 5.1 Core Index Structure

**Purpose:** Fast queryable index of all project assets with metadata.

**File:** `OliveAIEditor/Public/Index/OliveProjectIndex.h`

```cpp
USTRUCT()
struct FOliveAssetInfo
{
    GENERATED_BODY()

    FString Name;
    FString Path;
    FName AssetClass;
    FName ParentClass;          // For Blueprints
    TArray<FString> Interfaces; // For Blueprints
    TArray<FString> Dependencies;
    TArray<FString> Referencers;
    FDateTime LastModified;

    // Quick access flags
    bool bIsBlueprint = false;
    bool bIsBehaviorTree = false;
    bool bIsBlackboard = false;
    bool bIsPCG = false;
};

USTRUCT()
struct FOliveClassHierarchyNode
{
    GENERATED_BODY()

    FName ClassName;
    FName ParentClassName;
    TArray<FName> ChildClasses;
    bool bIsBlueprintClass = false;
    FString BlueprintPath;  // If this is a Blueprint-generated class
};

USTRUCT()
struct FOliveProjectConfig
{
    GENERATED_BODY()

    FString ProjectName;
    FString EngineVersion;
    TArray<FString> EnabledPlugins;
    TArray<FString> PrimaryAssetTypes;
};

class OLIVEAIEDITOR_API FOliveProjectIndex : public FTickableEditorObject
{
public:
    static FOliveProjectIndex& Get();

    // Initialization
    void Initialize();
    void Shutdown();

    // Queries
    TArray<FOliveAssetInfo> SearchAssets(const FString& Query, int32 MaxResults = 50);
    TArray<FOliveAssetInfo> GetAssetsByClass(FName ClassName);
    TOptional<FOliveAssetInfo> GetAssetByPath(const FString& Path);

    // Class hierarchy
    TArray<FName> GetChildClasses(FName ParentClass);
    TArray<FName> GetParentChain(FName ChildClass);
    bool IsChildOf(FName ChildClass, FName ParentClass);

    // Dependencies
    TArray<FString> GetDependencies(const FString& AssetPath);
    TArray<FString> GetReferencers(const FString& AssetPath);

    // Project info
    const FOliveProjectConfig& GetProjectConfig() const;

    // Exposed as MCP Resources
    FString GetSearchResultsJson(const FString& Query);
    FString GetAssetInfoJson(const FString& Path);
    FString GetClassHierarchyJson(FName RootClass);

    // FTickableEditorObject
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;

private:
    void RebuildIndex();
    void OnAssetAdded(const FAssetData& AssetData);
    void OnAssetRemoved(const FAssetData& AssetData);
    void OnAssetRenamed(const FAssetData& AssetData, const FString& OldPath);
    void OnAssetUpdated(const FAssetData& AssetData);

    TMap<FString, FOliveAssetInfo> AssetIndex;  // Path -> Info
    TMap<FName, FOliveClassHierarchyNode> ClassHierarchy;
    FOliveProjectConfig ProjectConfig;

    FDelegateHandle AssetAddedHandle;
    FDelegateHandle AssetRemovedHandle;
    FDelegateHandle AssetRenamedHandle;

    // Incremental update queue
    TQueue<TPair<FString, bool>> PendingUpdates;  // Path, bIsRemoval
    float TimeSinceLastProcess = 0.0f;
    static constexpr float ProcessInterval = 0.5f;

    FCriticalSection IndexLock;
};
```

### 5.2 Search Implementation

**Fuzzy Search Algorithm:**
```cpp
int32 CalculateFuzzyScore(const FString& Query, const FString& Target)
{
    // 1. Exact match = 1000
    // 2. Starts with = 500 + (length match bonus)
    // 3. Contains = 200 + position penalty
    // 4. Acronym match (BP_PlayerCharacter matches "bppc") = 300
    // 5. Levenshtein distance for typo tolerance = 100 - distance
}

TArray<FOliveAssetInfo> FOliveProjectIndex::SearchAssets(const FString& Query, int32 MaxResults)
{
    TArray<TPair<int32, FOliveAssetInfo>> ScoredResults;

    FScopeLock Lock(&IndexLock);
    for (const auto& Pair : AssetIndex)
    {
        int32 Score = CalculateFuzzyScore(Query, Pair.Value.Name);
        if (Score > 50)  // Minimum threshold
        {
            ScoredResults.Emplace(Score, Pair.Value);
        }
    }

    // Sort by score descending
    ScoredResults.Sort([](const auto& A, const auto& B) { return A.Key > B.Key; });

    // Return top results
    TArray<FOliveAssetInfo> Results;
    for (int32 i = 0; i < FMath::Min(MaxResults, ScoredResults.Num()); ++i)
    {
        Results.Add(ScoredResults[i].Value);
    }
    return Results;
}
```

### 5.3 Edge Cases for Project Index

| Edge Case | Handling |
|-----------|----------|
| Very large project (100K+ assets) | Paginated results; async initial build; progress notification |
| Asset registry not ready at startup | Defer initialization until `OnFilesLoaded` delegate fires |
| Rapid asset changes (batch import) | Queue updates, process in batches every 0.5s |
| External tool modifies assets | Asset registry delegates catch most changes; periodic full refresh option |
| Circular references | Track visited during dependency walk; max depth 50 |
| Memory pressure | Store minimal info in index; load full details on demand |
| Plugin assets | Include `/Plugins/` content; filter option in queries |
| Engine content | Exclude `/Engine/` by default; option to include |

---

## 6. Module 4: Tool Registry & MCP Server

### 6.1 Tool Registry

**File:** `OliveAIEditor/Public/MCP/OliveToolRegistry.h`

```cpp
USTRUCT()
struct FOliveToolDefinition
{
    GENERATED_BODY()

    FString Name;               // e.g., "blueprint.read"
    FString Description;        // Human-readable description
    TSharedPtr<FJsonObject> InputSchema;  // JSON Schema for parameters
    TArray<FString> Tags;       // For filtering by Focus Profile
    FString Category;           // "blueprint", "project", "behaviortree", etc.
};

USTRUCT()
struct FOliveToolResult
{
    GENERATED_BODY()

    bool bSuccess = false;
    TSharedPtr<FJsonObject> Data;
    TArray<FOliveValidationMessage> Messages;
    double ExecutionTimeMs = 0.0;

    FString ToJsonString() const;
};

// Tool handler function signature
DECLARE_DELEGATE_RetVal_OneParam(FOliveToolResult, FOliveToolHandler, const TSharedPtr<FJsonObject>&);

class OLIVEAIEDITOR_API FOliveToolRegistry
{
public:
    static FOliveToolRegistry& Get();

    // Register a tool
    void RegisterTool(
        const FString& Name,
        const FString& Description,
        const TSharedPtr<FJsonObject>& InputSchema,
        FOliveToolHandler Handler,
        const TArray<FString>& Tags = {}
    );

    // Unregister (for hot reload)
    void UnregisterTool(const FString& Name);

    // Query tools
    TArray<FOliveToolDefinition> GetAllTools() const;
    TArray<FOliveToolDefinition> GetToolsForProfile(const FString& ProfileName) const;
    TArray<FOliveToolDefinition> GetToolsByCategory(const FString& Category) const;
    bool HasTool(const FString& Name) const;

    // Execute
    FOliveToolResult ExecuteTool(const FString& Name, const TSharedPtr<FJsonObject>& Params);

    // For MCP: Get tools in MCP format
    TSharedPtr<FJsonObject> GetToolsListMCP() const;

private:
    struct FToolEntry
    {
        FOliveToolDefinition Definition;
        FOliveToolHandler Handler;
    };

    TMap<FString, FToolEntry> Tools;
    mutable FRWLock ToolsLock;
};
```

### 6.2 Initial Tool Set (Phase 0 Stubs)

Tools that exist in Phase 0 (stubs that return placeholder data or "not yet implemented"):

**Project Tools (Functional):**
- `project.search` — Search assets by name (uses Project Index)
- `project.get_asset_info` — Get asset metadata
- `project.get_class_hierarchy` — Get class inheritance tree
- `project.get_dependencies` — Get asset dependencies
- `project.get_project_info` — Engine version, plugins, etc.

**Blueprint Tools (Stubs):**
- `blueprint.read` — Returns "Phase 1 required"
- `blueprint.create` — Returns "Phase 1 required"

### 6.3 MCP Server Core

**File:** `OliveAIEditor/Public/MCP/OliveMCPServer.h`

```cpp
UENUM()
enum class EOliveMCPServerState : uint8
{
    Stopped,
    Starting,
    Running,
    Stopping,
    Error
};

DECLARE_MULTICAST_DELEGATE_OneParam(FOnMCPClientConnected, const FString& /* ClientId */);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnMCPClientDisconnected, const FString& /* ClientId */);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnMCPToolCalled, const FString& /* ToolName */, const FString& /* ClientId */);

class OLIVEAIEDITOR_API FOliveMCPServer
{
public:
    static FOliveMCPServer& Get();

    // Lifecycle
    bool Start(int32 Port = 3000);
    void Stop();
    EOliveMCPServerState GetState() const;
    int32 GetActualPort() const;  // May differ if requested port was in use

    // Events
    FOnMCPClientConnected OnClientConnected;
    FOnMCPClientDisconnected OnClientDisconnected;
    FOnMCPToolCalled OnToolCalled;

    // Active connections
    TArray<FString> GetConnectedClients() const;

    // Send notification to specific client or broadcast
    void SendNotification(const FString& Method, const TSharedPtr<FJsonObject>& Params, const FString& ClientId = TEXT(""));

private:
    void HandleRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    void ProcessJsonRpcRequest(const TSharedPtr<FJsonObject>& Request, const FHttpResultCallback& OnComplete);

    // JSON-RPC method handlers
    TSharedPtr<FJsonObject> HandleInitialize(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleToolsList(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleToolsCall(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleResourcesList(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleResourcesRead(const TSharedPtr<FJsonObject>& Params);

    // State
    EOliveMCPServerState State = EOliveMCPServerState::Stopped;
    int32 ActualPort = 0;
    FHttpServerModule* HttpServerModule = nullptr;
    TSharedPtr<IHttpRouter> HttpRouter;

    // Connected clients
    TMap<FString, FDateTime> ConnectedClients;  // ClientId -> LastActivity
    FCriticalSection ClientsLock;

    // MCP protocol state per client
    struct FMCPClientState
    {
        bool bInitialized = false;
        FString ClientName;
        FString ClientVersion;
    };
    TMap<FString, FMCPClientState> ClientStates;
};
```

### 6.4 JSON-RPC 2.0 Implementation

**File:** `OliveAIEditor/Public/MCP/OliveJsonRpc.h`

```cpp
namespace OliveJsonRpc
{
    // Error codes (JSON-RPC 2.0 standard)
    constexpr int32 PARSE_ERROR = -32700;
    constexpr int32 INVALID_REQUEST = -32600;
    constexpr int32 METHOD_NOT_FOUND = -32601;
    constexpr int32 INVALID_PARAMS = -32602;
    constexpr int32 INTERNAL_ERROR = -32603;

    // MCP-specific error codes
    constexpr int32 TOOL_NOT_FOUND = -32000;
    constexpr int32 TOOL_EXECUTION_ERROR = -32001;
    constexpr int32 RESOURCE_NOT_FOUND = -32002;

    TSharedPtr<FJsonObject> CreateResponse(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Result);
    TSharedPtr<FJsonObject> CreateErrorResponse(const TSharedPtr<FJsonValue>& Id, int32 Code, const FString& Message, const TSharedPtr<FJsonObject>& Data = nullptr);
    TSharedPtr<FJsonObject> ParseRequest(const FString& JsonString, FString& OutError);
    bool ValidateRequest(const TSharedPtr<FJsonObject>& Request, FString& OutError);
}
```

### 6.5 MCP Protocol Messages

**Initialize Request:**
```json
{
    "jsonrpc": "2.0",
    "id": 1,
    "method": "initialize",
    "params": {
        "protocolVersion": "2024-11-05",
        "capabilities": {
            "tools": {}
        },
        "clientInfo": {
            "name": "claude-code",
            "version": "1.0.0"
        }
    }
}
```

**Initialize Response:**
```json
{
    "jsonrpc": "2.0",
    "id": 1,
    "result": {
        "protocolVersion": "2024-11-05",
        "capabilities": {
            "tools": { "listChanged": true },
            "resources": { "listChanged": true }
        },
        "serverInfo": {
            "name": "olive-ai-studio",
            "version": "0.1.0"
        }
    }
}
```

**Tools/Call Request:**
```json
{
    "jsonrpc": "2.0",
    "id": 2,
    "method": "tools/call",
    "params": {
        "name": "project.search",
        "arguments": {
            "query": "BP_Player"
        }
    }
}
```

### 6.6 Edge Cases for MCP Server

| Edge Case | Handling |
|-----------|----------|
| Port already in use | Try ports 3000-3010; return actual port in logs and UI |
| Multiple simultaneous connections | Support up to 10 concurrent clients |
| Client disconnects mid-operation | Operation continues; result is discarded |
| Very large response (>1MB) | Chunked transfer encoding for HTTP |
| Client sends invalid JSON | Return PARSE_ERROR with helpful message |
| Unknown method | Return METHOD_NOT_FOUND with list of available methods |
| Tool execution takes >30s | Return timeout error; make configurable |
| Rapid reconnections (DoS) | Rate limit: max 10 connections per second |
| Client never sends `initialized` | Reject tool calls; return "not initialized" error |

---

## 7. Module 5: Provider Abstraction & Conversation Manager

### 7.1 Provider Interface

**File:** `OliveAIEditor/Public/Providers/IOliveAIProvider.h`

```cpp
USTRUCT()
struct FOliveProviderConfig
{
    GENERATED_BODY()

    FString ProviderName;    // "openrouter", "anthropic", "openai", "google", "ollama"
    FString ApiKey;          // Encrypted at rest
    FString ModelId;
    FString BaseUrl;         // For self-hosted or proxy
    float Temperature = 0.0f;
    int32 MaxTokens = 4096;
    int32 TimeoutSeconds = 120;
};

USTRUCT()
struct FOliveStreamChunk
{
    GENERATED_BODY()

    FString Text;
    bool bIsToolCall = false;
    FString ToolCallId;
    FString ToolName;
    TSharedPtr<FJsonObject> ToolArguments;
};

USTRUCT()
struct FOliveProviderUsage
{
    GENERATED_BODY()

    int32 PromptTokens = 0;
    int32 CompletionTokens = 0;
    double EstimatedCostUSD = 0.0;
};

DECLARE_DELEGATE_OneParam(FOnOliveStreamChunk, const FOliveStreamChunk&);
DECLARE_DELEGATE_OneParam(FOnOliveToolCall, const FOliveStreamChunk&);
DECLARE_DELEGATE_TwoParams(FOnOliveComplete, const FString& /* FullResponse */, const FOliveProviderUsage&);
DECLARE_DELEGATE_OneParam(FOnOliveError, const FString& /* ErrorMessage */);

class IOliveAIProvider
{
public:
    virtual ~IOliveAIProvider() = default;

    // Provider identity
    virtual FString GetProviderName() const = 0;
    virtual TArray<FString> GetAvailableModels() const = 0;
    virtual FString GetRecommendedModel() const = 0;

    // Configuration
    virtual void Configure(const FOliveProviderConfig& Config) = 0;
    virtual bool ValidateConfig(FString& OutError) const = 0;

    // Request
    virtual void SendMessage(
        const TArray<TSharedPtr<FJsonObject>>& Messages,  // Conversation history
        const TArray<FOliveToolDefinition>& Tools,
        FOnOliveStreamChunk OnChunk,
        FOnOliveToolCall OnToolCall,
        FOnOliveComplete OnComplete,
        FOnOliveError OnError
    ) = 0;

    // Cancel in-flight request
    virtual void CancelRequest() = 0;

    // Status
    virtual bool IsBusy() const = 0;
};
```

### 7.2 OpenRouter Client

**File:** `OliveAIEditor/Public/Providers/OliveOpenRouterProvider.h`

```cpp
class OLIVEAIEDITOR_API FOliveOpenRouterProvider : public IOliveAIProvider
{
public:
    FOliveOpenRouterProvider();
    virtual ~FOliveOpenRouterProvider();

    // IOliveAIProvider
    virtual FString GetProviderName() const override { return TEXT("OpenRouter"); }
    virtual TArray<FString> GetAvailableModels() const override;
    virtual FString GetRecommendedModel() const override { return TEXT("anthropic/claude-sonnet-4"); }
    virtual void Configure(const FOliveProviderConfig& Config) override;
    virtual bool ValidateConfig(FString& OutError) const override;
    virtual void SendMessage(...) override;
    virtual void CancelRequest() override;
    virtual bool IsBusy() const override;

private:
    void ProcessSSELine(const FString& Line);
    void OnResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess);

    TSharedPtr<FJsonObject> BuildRequestBody(
        const TArray<TSharedPtr<FJsonObject>>& Messages,
        const TArray<FOliveToolDefinition>& Tools
    );

    FOliveProviderConfig Config;
    TSharedPtr<IHttpRequest> CurrentRequest;
    FString AccumulatedResponse;

    // SSE parsing state
    FString SSEBuffer;

    // Callbacks
    FOnOliveStreamChunk OnChunkCallback;
    FOnOliveToolCall OnToolCallCallback;
    FOnOliveComplete OnCompleteCallback;
    FOnOliveError OnErrorCallback;

    static const FString OpenRouterApiUrl;  // "https://openrouter.ai/api/v1/chat/completions"
};
```

### 7.3 OpenRouter Request Format

```json
{
    "model": "anthropic/claude-sonnet-4",
    "messages": [
        {
            "role": "system",
            "content": "You are an AI assistant for Unreal Engine development..."
        },
        {
            "role": "user",
            "content": "Create a health pickup Blueprint"
        }
    ],
    "tools": [
        {
            "type": "function",
            "function": {
                "name": "project.search",
                "description": "Search for assets in the project",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "query": {
                            "type": "string",
                            "description": "Search query"
                        }
                    },
                    "required": ["query"]
                }
            }
        }
    ],
    "stream": true,
    "temperature": 0,
    "max_tokens": 4096
}
```

### 7.4 SSE (Server-Sent Events) Parsing

```cpp
void FOliveOpenRouterProvider::ProcessSSELine(const FString& Line)
{
    if (Line.StartsWith(TEXT("data: ")))
    {
        FString JsonData = Line.RightChop(6);  // Remove "data: "

        if (JsonData == TEXT("[DONE]"))
        {
            // Stream complete
            OnCompleteCallback.ExecuteIfBound(AccumulatedResponse, CalculateUsage());
            return;
        }

        TSharedPtr<FJsonObject> Chunk;
        if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JsonData), Chunk))
        {
            // Extract delta content
            const TArray<TSharedPtr<FJsonValue>>* Choices;
            if (Chunk->TryGetArrayField(TEXT("choices"), Choices) && Choices->Num() > 0)
            {
                TSharedPtr<FJsonObject> Delta = (*Choices)[0]->AsObject()->GetObjectField(TEXT("delta"));

                // Text content
                if (Delta->HasField(TEXT("content")))
                {
                    FString Content = Delta->GetStringField(TEXT("content"));
                    AccumulatedResponse += Content;

                    FOliveStreamChunk StreamChunk;
                    StreamChunk.Text = Content;
                    OnChunkCallback.ExecuteIfBound(StreamChunk);
                }

                // Tool calls
                if (Delta->HasField(TEXT("tool_calls")))
                {
                    // Parse tool call delta...
                }
            }
        }
    }
}
```

### 7.5 Anthropic Direct Client

**File:** `OliveAIEditor/Public/Providers/OliveAnthropicProvider.h`

Similar to OpenRouter but uses Anthropic's native API format with `anthropic-version` header.

**Key Differences:**
- Different auth header: `x-api-key` instead of `Authorization: Bearer`
- Different tool format (Anthropic native vs OpenAI function calling)
- Different streaming format

### 7.6 Conversation Manager

**File:** `OliveAIEditor/Public/Chat/OliveConversationManager.h`

```cpp
USTRUCT()
struct FOliveChatMessage
{
    GENERATED_BODY()

    FString Role;           // "system", "user", "assistant", "tool"
    FString Content;
    FDateTime Timestamp;

    // For tool responses
    FString ToolCallId;
    FString ToolName;

    // For assistant messages with tool calls
    TArray<FOliveToolCallInfo> ToolCalls;
};

USTRUCT()
struct FOliveToolCallInfo
{
    GENERATED_BODY()

    FString Id;
    FString Name;
    TSharedPtr<FJsonObject> Arguments;
    TSharedPtr<FJsonObject> Result;
    double ExecutionTimeMs = 0.0;
};

DECLARE_MULTICAST_DELEGATE_OneParam(FOnOliveChatMessageReceived, const FOliveChatMessage&);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnOliveChatStreamChunk, const FString&);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnOliveToolCallStarted, const FString& /* ToolName */, const FString& /* ToolCallId */);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnOliveToolCallCompleted, const FString& /* ToolName */, const FString& /* ToolCallId */, bool /* bSuccess */);

class OLIVEAIEDITOR_API FOliveConversationManager
{
public:
    FOliveConversationManager();
    ~FOliveConversationManager();

    // Session management
    void StartNewSession();
    void ClearHistory();
    FGuid GetSessionId() const;

    // Message handling
    void SendUserMessage(const FString& Message);
    void CancelCurrentRequest();
    bool IsProcessing() const;

    // History access
    const TArray<FOliveChatMessage>& GetMessageHistory() const;

    // Context management
    void SetActiveContext(const TArray<FString>& AssetPaths);
    void SetFocusProfile(const FString& ProfileName);

    // Provider management
    void SetProvider(TSharedPtr<IOliveAIProvider> Provider);

    // Events
    FOnOliveChatMessageReceived OnMessageReceived;
    FOnOliveChatStreamChunk OnStreamChunk;
    FOnOliveToolCallStarted OnToolCallStarted;
    FOnOliveToolCallCompleted OnToolCallCompleted;

private:
    void ProcessToolCalls(const TArray<FOliveToolCallInfo>& ToolCalls);
    void SendToolResults(const TArray<FOliveChatMessage>& ToolResultMessages);
    TSharedPtr<FJsonObject> AssembleSystemPrompt();
    TArray<TSharedPtr<FJsonObject>> ConvertMessagesToJson();

    TArray<FOliveChatMessage> MessageHistory;
    FGuid SessionId;
    FString ActiveFocusProfile;
    TArray<FString> ActiveContextPaths;
    TSharedPtr<IOliveAIProvider> Provider;
    bool bIsProcessing = false;

    // System prompt components
    FString BaseSystemPrompt;
    TMap<FString, FString> FocusProfilePrompts;
};
```

### 7.7 Edge Cases for Providers & Conversation

| Edge Case | Handling |
|-----------|----------|
| API key invalid | Clear error: "Invalid API key. Please check your OpenRouter API key in settings." |
| Rate limited | Parse `Retry-After` header; show countdown; auto-retry |
| Model unavailable | Suggest alternative model; list available models |
| Network timeout | Configurable timeout (default 120s); retry with backoff |
| Partial response (stream interrupted) | Mark message as incomplete; offer retry |
| Tool call with invalid arguments | Return error to model; let it self-correct |
| Multiple tool calls in one response | Execute sequentially; collect all results; send back together |
| Tool execution throws exception | Catch; return structured error to model |
| Very long conversation (>100 messages) | Truncate oldest messages; keep system prompt; warn user |
| Context window exceeded | Calculate tokens; truncate intelligently; inform model of truncation |
| User sends message during processing | Queue message; process after current completes |
| Provider switch mid-conversation | Allow; convert message format; may lose some context |

---

## 8. Module 6: Chat UI (Slate Panel)

### 8.1 Panel Registration

**File:** `OliveAIEditor/Private/UI/OliveAIEditorCommands.h`

```cpp
class FOliveAIEditorCommands : public TCommands<FOliveAIEditorCommands>
{
public:
    FOliveAIEditorCommands();
    virtual void RegisterCommands() override;

    TSharedPtr<FUICommandInfo> OpenChatPanel;
    TSharedPtr<FUICommandInfo> ToggleMCPServer;
};
```

**File:** `OliveAIEditor/Private/OliveAIEditorModule.cpp`

```cpp
void FOliveAIEditorModule::StartupModule()
{
    // Register commands
    FOliveAIEditorCommands::Register();

    // Register tab spawner
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
        FOliveAIChatPanel::TabId,
        FOnSpawnTab::CreateStatic(&FOliveAIChatPanel::SpawnTab)
    )
    .SetDisplayName(LOCTEXT("ChatPanelTitle", "Olive AI Chat"))
    .SetMenuType(ETabSpawnerMenuType::Hidden)
    .SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"));

    // Add menu entry
    UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools", ...);

    // Add toolbar button (optional)
    ...
}
```

### 8.2 Main Chat Panel

**File:** `OliveAIEditor/Public/UI/SOliveAIChatPanel.h`

```cpp
class OLIVEAIEDITOR_API SOliveAIChatPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SOliveAIChatPanel) {}
    SLATE_END_ARGS()

    static const FName TabId;
    static TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs& Args);

    void Construct(const FArguments& InArgs);

private:
    // Widget construction
    TSharedRef<SWidget> BuildContextBar();
    TSharedRef<SWidget> BuildMessageArea();
    TSharedRef<SWidget> BuildQuickActions();
    TSharedRef<SWidget> BuildInputArea();
    TSharedRef<SWidget> BuildFocusDropdown();

    // Event handlers
    void OnSendMessage();
    void OnFocusProfileChanged(TSharedPtr<FString> NewProfile, ESelectInfo::Type SelectInfo);
    void OnContextAssetRemoved(const FString& AssetPath);
    void OnSettingsClicked();

    // Conversation manager callbacks
    void HandleStreamChunk(const FString& Chunk);
    void HandleMessageReceived(const FOliveChatMessage& Message);
    void HandleToolCallStarted(const FString& ToolName, const FString& ToolCallId);
    void HandleToolCallCompleted(const FString& ToolName, const FString& ToolCallId, bool bSuccess);

    // Child widgets
    TSharedPtr<SOliveAIContextBar> ContextBar;
    TSharedPtr<SOliveAIMessageList> MessageList;
    TSharedPtr<SOliveAIInputField> InputField;
    TSharedPtr<SComboBox<TSharedPtr<FString>>> FocusProfileDropdown;

    // State
    TSharedPtr<FOliveConversationManager> ConversationManager;
    TArray<TSharedPtr<FString>> FocusProfiles;
    TSharedPtr<FString> CurrentFocusProfile;
};
```

### 8.3 Message List Widget

**File:** `OliveAIEditor/Public/UI/SOliveAIMessageList.h`

```cpp
class OLIVEAIEDITOR_API SOliveAIMessageList : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SOliveAIMessageList) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    // Add messages
    void AddUserMessage(const FString& Message);
    void AddAssistantMessage(const FString& Message);
    void AddSystemMessage(const FString& Message);
    void AppendToLastMessage(const FString& Chunk);  // For streaming

    // Operation feed
    void AddOperationFeed(TSharedRef<SOliveAIOperationFeed> Feed);

    // Clear
    void ClearMessages();

    // Scroll
    void ScrollToBottom();

private:
    TSharedRef<ITableRow> GenerateMessageRow(TSharedPtr<FOliveUIMessage> Message, const TSharedRef<STableViewBase>& OwnerTable);

    TSharedPtr<SScrollBox> ScrollBox;
    TSharedPtr<SVerticalBox> MessagesContainer;
    TArray<TSharedPtr<FOliveUIMessage>> Messages;

    // Currently streaming message widget (for appending)
    TSharedPtr<SOliveAIMessageWidget> StreamingMessageWidget;
};

// Individual message widget
class SOliveAIMessageWidget : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SOliveAIMessageWidget) {}
        SLATE_ARGUMENT(FString, Role)
        SLATE_ARGUMENT(FString, Content)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    void AppendContent(const FString& AdditionalContent);
    void SetOperationFeed(TSharedRef<SOliveAIOperationFeed> Feed);

private:
    TSharedPtr<SRichTextBlock> ContentBlock;
    TSharedPtr<SBox> OperationFeedContainer;
    FString FullContent;
};
```

### 8.4 Operation Feed Widget

**File:** `OliveAIEditor/Public/UI/SOliveAIOperationFeed.h`

```cpp
UENUM()
enum class EOliveOperationStatus : uint8
{
    Pending,     // ⏳
    InProgress,  // 🔄
    Success,     // ✅
    Warning,     // ⚠️
    Error        // ❌
};

USTRUCT()
struct FOliveOperationEntry
{
    GENERATED_BODY()

    FString ToolName;
    FString Description;
    EOliveOperationStatus Status = EOliveOperationStatus::Pending;
    double DurationMs = 0.0;
    TSharedPtr<FJsonObject> Details;
    TArray<FOliveOperationEntry> SubOperations;
};

class OLIVEAIEDITOR_API SOliveAIOperationFeed : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SOliveAIOperationFeed) {}
        SLATE_ARGUMENT(bool, IsCollapsed)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    // Operations
    int32 AddOperation(const FString& ToolName, const FString& Description);
    void UpdateOperationStatus(int32 OperationId, EOliveOperationStatus Status);
    void AddSubOperation(int32 ParentId, const FString& Description, EOliveOperationStatus Status);
    void SetOperationDuration(int32 OperationId, double DurationMs);

    // Collapse/expand
    void SetCollapsed(bool bCollapsed);
    void ToggleCollapsed();

private:
    TSharedRef<ITableRow> GenerateOperationRow(TSharedPtr<FOliveOperationEntry> Operation, const TSharedRef<STableViewBase>& OwnerTable);
    FSlateColor GetStatusColor(EOliveOperationStatus Status) const;
    const FSlateBrush* GetStatusIcon(EOliveOperationStatus Status) const;

    TSharedPtr<SExpandableArea> ExpandableArea;
    TSharedPtr<SListView<TSharedPtr<FOliveOperationEntry>>> OperationList;
    TArray<TSharedPtr<FOliveOperationEntry>> Operations;
    TMap<int32, TSharedPtr<FOliveOperationEntry>> OperationMap;
    int32 NextOperationId = 1;
};
```

### 8.5 Context Bar Widget

**File:** `OliveAIEditor/Public/UI/SOliveAIContextBar.h`

```cpp
class OLIVEAIEDITOR_API SOliveAIContextBar : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SOliveAIContextBar) {}
    SLATE_END_ARGS()

    DECLARE_DELEGATE_OneParam(FOnContextAssetRemoved, const FString& /* AssetPath */);
    FOnContextAssetRemoved OnAssetRemoved;

    void Construct(const FArguments& InArgs);

    // Context management
    void AddContextAsset(const FString& AssetPath, const FString& DisplayName);
    void RemoveContextAsset(const FString& AssetPath);
    void SetAutoContext(const FString& AssetPath, const FString& DisplayName);
    void ClearAllContext();
    TArray<FString> GetContextAssetPaths() const;

private:
    TSharedRef<SWidget> BuildAssetTag(const FString& AssetPath, const FString& DisplayName, bool bIsAutoContext);
    void OnRemoveClicked(const FString& AssetPath);
    void OnAssetClicked(const FString& AssetPath);

    TSharedPtr<SWrapBox> TagContainer;
    TMap<FString, TSharedPtr<SWidget>> AssetTags;
    FString AutoContextPath;
};
```

### 8.6 Input Field with @Mentions

**File:** `OliveAIEditor/Public/UI/SOliveAIInputField.h`

```cpp
class OLIVEAIEDITOR_API SOliveAIInputField : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SOliveAIInputField) {}
    SLATE_END_ARGS()

    DECLARE_DELEGATE_OneParam(FOnMessageSubmit, const FString& /* Message */);
    DECLARE_DELEGATE_OneParam(FOnAssetMentioned, const FString& /* AssetPath */);
    FOnMessageSubmit OnMessageSubmit;
    FOnAssetMentioned OnAssetMentioned;

    void Construct(const FArguments& InArgs);

    void SetEnabled(bool bEnabled);
    void Clear();
    void Focus();

private:
    void OnTextChanged(const FText& NewText);
    void OnTextCommitted(const FText& NewText, ETextCommit::Type CommitType);
    FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& KeyEvent);

    // @Mention handling
    void CheckForMentionTrigger(const FString& Text, int32 CursorPosition);
    void ShowMentionPopup(const FString& SearchQuery);
    void HideMentionPopup();
    void OnMentionSelected(const FString& AssetPath, const FString& AssetName);

    TSharedPtr<SMultiLineEditableTextBox> TextBox;
    TSharedPtr<SOliveAIMentionPopup> MentionPopup;
    bool bMentionPopupVisible = false;
    int32 MentionStartPosition = -1;
};
```

### 8.7 Mention Popup (Asset Search Autocomplete)

**File:** `OliveAIEditor/Public/UI/SOliveAIMentionPopup.h`

```cpp
class OLIVEAIEDITOR_API SOliveAIMentionPopup : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SOliveAIMentionPopup) {}
    SLATE_END_ARGS()

    DECLARE_DELEGATE_TwoParams(FOnMentionSelected, const FString& /* AssetPath */, const FString& /* AssetName */);
    FOnMentionSelected OnMentionSelected;

    void Construct(const FArguments& InArgs);

    void Search(const FString& Query);
    void SelectNext();
    void SelectPrevious();
    void ConfirmSelection();
    void Hide();

private:
    TSharedRef<ITableRow> GenerateSearchResultRow(TSharedPtr<FOliveAssetInfo> AssetInfo, const TSharedRef<STableViewBase>& OwnerTable);
    void OnSearchResultSelected(TSharedPtr<FOliveAssetInfo> AssetInfo, ESelectInfo::Type SelectInfo);
    void OnSearchResultDoubleClicked(TSharedPtr<FOliveAssetInfo> AssetInfo);

    TSharedPtr<SListView<TSharedPtr<FOliveAssetInfo>>> ResultsList;
    TArray<TSharedPtr<FOliveAssetInfo>> SearchResults;
    int32 SelectedIndex = 0;

    // Async search
    TFuture<TArray<FOliveAssetInfo>> PendingSearch;
};
```

### 8.8 Edge Cases for Chat UI

| Edge Case | Handling |
|-----------|----------|
| Very long message (>10K chars) | Truncate display with "Show more" expander |
| Rapid streaming (1000 chunks/sec) | Batch UI updates; update every 50ms max |
| Markdown rendering issues | Use conservative subset: bold, code, lists; escape edge cases |
| Code blocks with syntax highlighting | Use `SyntaxHighlighterTextLayoutMarshaller` or simple mono font |
| Panel closed during streaming | Continue operation; show notification on complete; reopen shows result |
| @mention with 100K assets | Async search; debounce input; show first 50 results |
| Asset path with special characters | URL encode for display; preserve original for operations |
| User resizes panel very small | Responsive layout; scroll for content; minimum size |
| Theme switching (light/dark) | Use `FAppStyle` colors; subscribe to theme change delegate |
| Multiple panels open | Share ConversationManager; sync state across panels |

---

## 9. Module 7: Focus Profiles & System Prompts

### 9.1 Focus Profile Definition

**File:** `OliveAIEditor/Public/Profiles/OliveFocusProfile.h`

```cpp
USTRUCT()
struct FOliveFocusProfile
{
    GENERATED_BODY()

    FString Name;                    // "Auto", "Blueprint", "AI & Behavior", etc.
    FString DisplayName;             // Localized display name
    FString Description;             // Tooltip description
    TArray<FString> ToolCategories;  // Categories to include (empty = all)
    TArray<FString> ExcludedTools;   // Specific tools to exclude
    FString SystemPromptAddition;    // Added to base system prompt
    FString IconName;                // Slate icon name
    int32 SortOrder = 0;             // Display order in dropdown
};

class OLIVEAIEDITOR_API FOliveFocusProfileManager
{
public:
    static FOliveFocusProfileManager& Get();

    void Initialize();

    // Profiles
    TArray<FOliveFocusProfile> GetAllProfiles() const;
    FOliveFocusProfile GetProfile(const FString& Name) const;
    bool HasProfile(const FString& Name) const;

    // Custom profiles
    void AddCustomProfile(const FOliveFocusProfile& Profile);
    void RemoveCustomProfile(const FString& Name);
    void SaveCustomProfiles();
    void LoadCustomProfiles();

    // Tool filtering
    TArray<FString> GetToolsForProfile(const FString& ProfileName) const;

private:
    void RegisterDefaultProfiles();

    TMap<FString, FOliveFocusProfile> Profiles;
    TArray<FString> CustomProfileNames;
};
```

### 9.2 Default Focus Profiles

```cpp
void FOliveFocusProfileManager::RegisterDefaultProfiles()
{
    // Auto (default)
    {
        FOliveFocusProfile Profile;
        Profile.Name = TEXT("Auto");
        Profile.DisplayName = LOCTEXT("ProfileAuto", "Auto");
        Profile.Description = LOCTEXT("ProfileAutoDesc", "All tools available. AI determines which to use.");
        Profile.ToolCategories = {};  // Empty = all
        Profile.SortOrder = 0;
        Profiles.Add(Profile.Name, Profile);
    }

    // Blueprint
    {
        FOliveFocusProfile Profile;
        Profile.Name = TEXT("Blueprint");
        Profile.DisplayName = LOCTEXT("ProfileBlueprint", "Blueprint");
        Profile.Description = LOCTEXT("ProfileBlueprintDesc", "Focus on Blueprint development.");
        Profile.ToolCategories = { TEXT("blueprint"), TEXT("project") };
        Profile.SystemPromptAddition = TEXT("You are focused on Blueprint development. Prioritize Blueprint-based solutions.");
        Profile.SortOrder = 1;
        Profiles.Add(Profile.Name, Profile);
    }

    // AI & Behavior
    {
        FOliveFocusProfile Profile;
        Profile.Name = TEXT("AIBehavior");
        Profile.DisplayName = LOCTEXT("ProfileAIBehavior", "AI & Behavior");
        Profile.Description = LOCTEXT("ProfileAIBehaviorDesc", "Focus on AI systems: Behavior Trees, Blackboards, Blueprints.");
        Profile.ToolCategories = { TEXT("blueprint"), TEXT("behaviortree"), TEXT("blackboard"), TEXT("project") };
        Profile.SystemPromptAddition = TEXT("You are focused on AI system development. Consider Behavior Trees, Blackboards, and AI-related Blueprints.");
        Profile.SortOrder = 2;
        Profiles.Add(Profile.Name, Profile);
    }

    // Level & PCG
    {
        FOliveFocusProfile Profile;
        Profile.Name = TEXT("LevelPCG");
        Profile.DisplayName = LOCTEXT("ProfileLevelPCG", "Level & PCG");
        Profile.Description = LOCTEXT("ProfileLevelPCGDesc", "Focus on procedural content and level design.");
        Profile.ToolCategories = { TEXT("blueprint"), TEXT("pcg"), TEXT("project") };
        Profile.SystemPromptAddition = TEXT("You are focused on level design and procedural content generation.");
        Profile.SortOrder = 3;
        Profiles.Add(Profile.Name, Profile);
    }

    // C++ & Blueprint
    {
        FOliveFocusProfile Profile;
        Profile.Name = TEXT("CppBlueprint");
        Profile.DisplayName = LOCTEXT("ProfileCppBP", "C++ & Blueprint");
        Profile.Description = LOCTEXT("ProfileCppBPDesc", "Mixed C++ and Blueprint workflows.");
        Profile.ToolCategories = { TEXT("blueprint"), TEXT("cpp"), TEXT("project") };
        Profile.SystemPromptAddition = TEXT("You are working with both C++ and Blueprints. Consider exposing C++ to Blueprint and vice versa.");
        Profile.SortOrder = 4;
        Profiles.Add(Profile.Name, Profile);
    }
}
```

### 9.3 System Prompt Structure

**File:** `Content/SystemPrompts/BaseSystemPrompt.txt`

```
You are Olive AI, an expert AI assistant for Unreal Engine development integrated directly into the editor.

## Your Capabilities
- Read and understand any Blueprint, Behavior Tree, PCG graph, or C++ class in the project
- Create new Blueprints with components, variables, functions, and event graph logic
- Modify existing Blueprints surgically (add nodes, connect pins, edit properties)
- Search the project index to find assets by name or type
- Understand class hierarchies and dependencies

## How to Use Tools
1. **Always read before writing.** Before modifying an asset, use the read tool to understand its current state.
2. **One operation at a time.** Chain operations logically but don't skip steps.
3. **Compile after writes.** After making changes to a Blueprint, compile it to verify success.
4. **Self-correct on errors.** If compilation fails, read the errors and attempt fixes (up to 3 times).

## Response Guidelines
- Be concise. Show your work in the operation feed, not in prose.
- For complex tasks, briefly outline your plan before executing.
- If uncertain, ask clarifying questions rather than guessing.
- Report results clearly: what was created/modified, any issues encountered.

## Safety Rules
- Never modify assets not mentioned in the user's request.
- Always use transactions so operations can be undone.
- Warn before destructive operations (deletion, reparenting).
- Respect confirmation tiers - don't bypass the approval flow.

## Project Context
Engine Version: {ENGINE_VERSION}
Project Name: {PROJECT_NAME}
Enabled Plugins: {ENABLED_PLUGINS}

## Current Context
{ACTIVE_CONTEXT}
```

### 9.4 Prompt Assembly

**File:** `OliveAIEditor/Public/Chat/OlivePromptAssembler.h`

```cpp
class OLIVEAIEDITOR_API FOlivePromptAssembler
{
public:
    static FOlivePromptAssembler& Get();

    // Assemble full system prompt
    FString AssembleSystemPrompt(
        const FString& FocusProfileName,
        const TArray<FString>& ContextAssetPaths,
        int32 MaxTokens = 4000
    );

    // Components
    FString GetBasePrompt() const;
    FString GetProfilePromptAddition(const FString& ProfileName) const;
    FString GetProjectContext() const;
    FString GetActiveContext(const TArray<FString>& AssetPaths, int32 MaxTokens) const;

    // Token estimation
    int32 EstimateTokenCount(const FString& Text) const;

private:
    void LoadPromptTemplates();
    FString SubstituteVariables(const FString& Template) const;

    FString BasePromptTemplate;
    TMap<FString, FString> ProfilePrompts;

    // Rough estimate: 1 token ≈ 4 characters for English
    static constexpr float CharsPerToken = 4.0f;
};
```

---

## 10. Module 8: Configuration System

### 10.1 Settings Classes

**File:** `OliveAIEditor/Public/Settings/OliveAISettings.h`

```cpp
UENUM()
enum class EOliveAIProvider : uint8
{
    OpenRouter UMETA(DisplayName = "OpenRouter (Recommended)"),
    Anthropic UMETA(DisplayName = "Anthropic (Direct)"),
    OpenAI UMETA(DisplayName = "OpenAI (Direct)"),
    Google UMETA(DisplayName = "Google AI (Direct)"),
    Ollama UMETA(DisplayName = "Ollama (Local)")
};

UCLASS(Config=Editor, DefaultConfig, meta=(DisplayName="Olive AI Studio"))
class OLIVEAIEDITOR_API UOliveAISettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UOliveAISettings();

    //~ Begin UDeveloperSettings
    virtual FName GetContainerName() const override { return TEXT("Editor"); }
    virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
    virtual FName GetSectionName() const override { return TEXT("Olive AI Studio"); }
    //~ End UDeveloperSettings

    // Provider settings
    UPROPERTY(Config, EditAnywhere, Category="AI Provider")
    EOliveAIProvider Provider = EOliveAIProvider::OpenRouter;

    UPROPERTY(Config, EditAnywhere, Category="AI Provider", meta=(PasswordField=true))
    FString OpenRouterApiKey;

    UPROPERTY(Config, EditAnywhere, Category="AI Provider", meta=(PasswordField=true))
    FString AnthropicApiKey;

    UPROPERTY(Config, EditAnywhere, Category="AI Provider")
    FString SelectedModel = TEXT("anthropic/claude-sonnet-4");

    UPROPERTY(Config, EditAnywhere, Category="AI Provider", meta=(ClampMin=0, ClampMax=2))
    float Temperature = 0.0f;

    UPROPERTY(Config, EditAnywhere, Category="AI Provider", meta=(ClampMin=256, ClampMax=128000))
    int32 MaxTokens = 4096;

    // MCP Server settings
    UPROPERTY(Config, EditAnywhere, Category="MCP Server")
    bool bAutoStartMCPServer = true;

    UPROPERTY(Config, EditAnywhere, Category="MCP Server", meta=(ClampMin=1024, ClampMax=65535))
    int32 MCPServerPort = 3000;

    // UI settings
    UPROPERTY(Config, EditAnywhere, Category="User Interface")
    bool bShowOperationFeed = true;

    UPROPERTY(Config, EditAnywhere, Category="User Interface")
    bool bShowQuickActions = true;

    UPROPERTY(Config, EditAnywhere, Category="User Interface")
    bool bNotifyOnCompletion = true;

    UPROPERTY(Config, EditAnywhere, Category="User Interface")
    bool bPlaySoundOnCompletion = false;

    // Get singleton
    static UOliveAISettings* Get();
};
```

### 10.2 Secure API Key Storage

**Note:** For Phase 0, API keys are stored in the config file. Future improvement: use platform keychain (Windows Credential Manager, macOS Keychain).

```cpp
// Future implementation for secure storage
class FOliveSecureStorage
{
public:
    static bool StoreApiKey(const FString& KeyName, const FString& KeyValue);
    static bool RetrieveApiKey(const FString& KeyName, FString& OutKeyValue);
    static bool DeleteApiKey(const FString& KeyName);

private:
#if PLATFORM_WINDOWS
    static bool StoreToCredentialManager(const FString& KeyName, const FString& KeyValue);
    static bool RetrieveFromCredentialManager(const FString& KeyName, FString& OutKeyValue);
#elif PLATFORM_MAC
    static bool StoreToKeychain(const FString& KeyName, const FString& KeyValue);
    static bool RetrieveFromKeychain(const FString& KeyName, FString& OutKeyValue);
#endif
};
```

---

## 11. Edge Cases & Failure Modes

### 11.1 Comprehensive Edge Case Matrix

| Category | Edge Case | Detection | Handling | Recovery |
|----------|-----------|-----------|----------|----------|
| **Startup** | Asset registry not ready | `FAssetRegistryModule::Get().IsLoadingAssets()` | Defer Project Index build | Subscribe to `OnFilesLoaded` |
| **Startup** | HTTP module not available | Module load check | Disable MCP server; warn | Retry on next editor restart |
| **Network** | DNS resolution failure | HTTP error callback | Retry with exponential backoff | Manual retry button |
| **Network** | SSL certificate error | HTTP error code | Clear error message | Link to troubleshooting |
| **Network** | Proxy configuration needed | Timeout with specific patterns | Detect from system settings | Settings UI for proxy |
| **API** | Invalid API key | 401 response | Clear message in chat | Link to settings |
| **API** | Rate limited | 429 + Retry-After | Auto-retry with countdown | Show countdown in UI |
| **API** | Model deprecated | Model-specific error | Suggest alternative | Update to recommended |
| **API** | Insufficient credits | Provider-specific error | Clear message | Link to provider dashboard |
| **Streaming** | Connection dropped mid-stream | No more chunks + no [DONE] | Mark partial; offer retry | "Continue" button |
| **Streaming** | Malformed SSE data | JSON parse error | Skip chunk; continue | Log for debugging |
| **Tools** | Tool execution timeout (>30s) | Timer | Cancel + error response | Configurable timeout |
| **Tools** | Tool throws unhandled exception | Try-catch wrapper | Structured error response | Log stack trace |
| **Context** | Context exceeds token limit | Token estimation | Truncate oldest/lowest priority | Warn user; log details |
| **MCP** | Port conflict | Bind error | Try next port (3001-3010) | Show actual port in UI |
| **MCP** | Firewall blocking | Connection timeout | Clear error | Instructions for firewall |
| **UI** | Panel crash during render | Slate exception | Catch; show error state | Reopen panel |
| **UI** | Input focus issues | Focus not responding | Check `HasKeyboardFocus` | Force focus on interaction |
| **Memory** | Very long conversation | Check message count | Warn at 100+; truncate at 200 | "Clear history" option |
| **Memory** | Large search results | Count before loading | Paginate results | Load more on scroll |
| **Undo** | Transaction spans multiple assets | Multi-object transaction | Single Ctrl+Z undoes all | Verify with test |
| **Concurrency** | Tool called during tool execution | State check | Queue or reject | Sequential execution |
| **PIE** | Write attempted during PIE | `GEditor->bIsSimulatingInEditor` or `GEditor->PlayWorld` | Reject with clear error | Auto-retry when PIE ends (optional) |

### 11.2 Graceful Degradation Strategy

| Component Failure | Fallback Behavior |
|-------------------|-------------------|
| MCP Server fails to start | Chat UI works; log MCP error; manual retry button |
| OpenRouter unreachable | Try direct Anthropic API if key present; else clear error |
| Project Index build fails | Search returns empty with error; manual rebuild button |
| Focus Profile config corrupt | Fall back to "Auto" profile |
| System prompt file missing | Use hardcoded minimal prompt |
| Context assembly fails | Send without context; warn in UI |

---

## 12. Testing Checklist

### 12.1 Unit Tests

- [ ] **IR Serialization:** Round-trip test for all IR types
- [ ] **Transaction Manager:** Begin/end, nested, cancel
- [ ] **Asset Resolver:** Valid path, missing, redirector, wrong type
- [ ] **Validation Engine:** Each rule with valid/invalid inputs
- [ ] **Error Builder:** JSON format matches spec
- [ ] **Project Index:** Search accuracy, fuzzy match, class hierarchy
- [ ] **Tool Registry:** Register, execute, list, filter by profile
- [ ] **JSON-RPC:** Parse, validate, error responses
- [ ] **SSE Parser:** Valid chunks, malformed data, [DONE] signal
- [ ] **Prompt Assembly:** Token estimation, truncation, variable substitution

### 12.2 Integration Tests

- [ ] **MCP Handshake:** Initialize → initialized → tools/list
- [ ] **MCP Tool Call:** Call project.search, verify response format
- [ ] **Provider Request:** Send message, receive stream, parse chunks
- [ ] **Tool Execution Flow:** Chat sends message → tools called → results shown
- [ ] **Context Update:** Open Blueprint → context bar updates
- [ ] **@Mention Flow:** Type @ → search → select → add to context

### 12.3 UI Tests

- [ ] **Panel Spawn:** Menu item works, panel opens
- [ ] **Panel Docking:** Dock right, bottom, float, restore on restart
- [ ] **Message Rendering:** User, assistant, system messages styled correctly
- [ ] **Streaming:** Characters appear smoothly, no flicker
- [ ] **Operation Feed:** Operations appear, status updates, collapse/expand
- [ ] **Context Bar:** Add, remove, click to open asset
- [ ] **Input Field:** Multi-line, Enter to send, Shift+Enter newline
- [ ] **@Mention Popup:** Appears, keyboard navigation, selection
- [ ] **Focus Dropdown:** Shows profiles, selection persists
- [ ] **Settings Link:** Opens settings page

### 12.4 End-to-End Tests

- [ ] **Full Chat Flow:** User types → AI responds → tool called → result shown
- [ ] **MCP Client Connection:** Claude Code connects, calls tools, receives responses
- [ ] **Project Search:** Search finds existing assets accurately
- [ ] **Error Handling:** Invalid input shows appropriate error in chat
- [ ] **Long Conversation:** 50+ messages, performance acceptable
- [ ] **Focus Profile Switch:** Tools filtered correctly

---

## 13. File Structure

```
Source/
├── OliveAIRuntime/
│   ├── OliveAIRuntime.Build.cs
│   ├── Public/
│   │   ├── OliveAIRuntimeModule.h
│   │   └── IR/
│   │       ├── OliveIRTypes.h
│   │       ├── BlueprintIR.h
│   │       ├── BehaviorTreeIR.h
│   │       ├── PCGIR.h
│   │       └── CommonIR.h
│   └── Private/
│       └── OliveAIRuntimeModule.cpp
│
└── OliveAIEditor/
    ├── OliveAIEditor.Build.cs
    ├── Public/
    │   ├── OliveAIEditorModule.h
    │   │
    │   ├── UI/
    │   │   ├── SOliveAIChatPanel.h
    │   │   ├── SOliveAIMessageList.h
    │   │   ├── SOliveAIMessageWidget.h
    │   │   ├── SOliveAIOperationFeed.h
    │   │   ├── SOliveAIContextBar.h
    │   │   ├── SOliveAIInputField.h
    │   │   └── SOliveAIMentionPopup.h
    │   │
    │   ├── Chat/
    │   │   ├── OliveConversationManager.h
    │   │   └── OlivePromptAssembler.h
    │   │
    │   ├── Providers/
    │   │   ├── IOliveAIProvider.h
    │   │   ├── OliveOpenRouterProvider.h
    │   │   └── OliveAnthropicProvider.h
    │   │
    │   ├── MCP/
    │   │   ├── OliveMCPServer.h
    │   │   ├── OliveToolRegistry.h
    │   │   └── OliveJsonRpc.h
    │   │
    │   ├── Services/
    │   │   ├── OliveTransactionManager.h
    │   │   ├── OliveAssetResolver.h
    │   │   ├── OliveValidationEngine.h
    │   │   └── OliveErrorBuilder.h
    │   │
    │   ├── Index/
    │   │   └── OliveProjectIndex.h
    │   │
    │   ├── Profiles/
    │   │   └── OliveFocusProfileManager.h
    │   │
    │   └── Settings/
    │       └── OliveAISettings.h
    │
    └── Private/
        ├── OliveAIEditorModule.cpp
        ├── OliveAIEditorCommands.cpp
        │
        ├── UI/
        │   ├── SOliveAIChatPanel.cpp
        │   ├── SOliveAIMessageList.cpp
        │   ├── SOliveAIMessageWidget.cpp
        │   ├── SOliveAIOperationFeed.cpp
        │   ├── SOliveAIContextBar.cpp
        │   ├── SOliveAIInputField.cpp
        │   └── SOliveAIMentionPopup.cpp
        │
        ├── Chat/
        │   ├── OliveConversationManager.cpp
        │   └── OlivePromptAssembler.cpp
        │
        ├── Providers/
        │   ├── OliveOpenRouterProvider.cpp
        │   └── OliveAnthropicProvider.cpp
        │
        ├── MCP/
        │   ├── OliveMCPServer.cpp
        │   ├── OliveToolRegistry.cpp
        │   └── OliveJsonRpc.cpp
        │
        ├── Services/
        │   ├── OliveTransactionManager.cpp
        │   ├── OliveAssetResolver.cpp
        │   ├── OliveValidationEngine.cpp
        │   └── OliveErrorBuilder.cpp
        │
        ├── Index/
        │   └── OliveProjectIndex.cpp
        │
        ├── Profiles/
        │   └── OliveFocusProfileManager.cpp
        │
        └── Settings/
            └── OliveAISettings.cpp

Content/
└── SystemPrompts/
    ├── BaseSystemPrompt.txt
    ├── ProfileBlueprint.txt
    ├── ProfileAIBehavior.txt
    ├── ProfileLevelPCG.txt
    └── ProfileCppBlueprint.txt

Config/
└── DefaultOliveAI.ini
```

---

## 14. Implementation Order

### Week 1-2: Foundation
1. Plugin restructure (2 modules)
2. Build.cs files with all dependencies
3. Module startup/shutdown
4. IR struct definitions (headers only, minimal impl)
5. Settings class and configuration UI

### Week 2-3: Shared Services
6. Transaction Manager
7. Asset Resolver
8. Validation Engine (base + 2-3 rules)
9. Error Builder

### Week 3-4: Project Index & Tool Registry
10. Project Index (build, search, class hierarchy)
11. Tool Registry
12. Initial tools: project.search, project.get_info

### Week 4-5: MCP Server
13. HTTP server setup
14. JSON-RPC implementation
15. MCP protocol handlers
16. Tool execution through registry

### Week 5-6: Provider Layer
17. Provider interface
18. OpenRouter client (streaming)
19. SSE parsing
20. Anthropic client (if time permits)

### Week 6-7: Conversation Manager
21. Message history
22. Tool call handling
23. Prompt assembly
24. Token management

### Week 7-8: Chat UI
25. Panel registration and spawning
26. Message list with streaming
27. Operation feed
28. Context bar
29. Input field with @mentions

### Week 8-9: Focus Profiles & Polish
30. Focus profile manager
31. System prompt files
32. Integration testing
33. Bug fixes

### Week 9-10: Testing & Hardening
34. Full test suite
35. Edge case handling
36. Performance optimization
37. Documentation

---

## Completion Criteria

Phase 0 is complete when:

- [ ] Plugin loads in UE 5.5+ without errors
- [ ] Chat panel opens from Tools menu
- [ ] Chat panel docks correctly and persists across editor restarts
- [ ] User can configure API key in Project Settings
- [ ] User can send a message and receive streaming response
- [ ] Tool calls from AI are executed and results returned
- [ ] Operation feed shows tool calls in real-time
- [ ] Context bar shows currently open asset
- [ ] @mention search finds project assets
- [ ] Focus profile dropdown filters tools
- [ ] MCP server starts on configurable port
- [ ] External MCP client can connect and complete handshake
- [ ] External MCP client can list and call tools
- [ ] Project index builds and searches work
- [ ] All operations dispatch to game thread correctly
- [ ] No crashes or memory leaks in basic usage
