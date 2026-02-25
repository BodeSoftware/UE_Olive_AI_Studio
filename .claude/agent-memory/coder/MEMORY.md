# Coder Agent Memory

## Build System
- UBT: `"C:/Program Files/Epic Games/UE_5.5/Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe" UE_Olive_AI_ToolkitEditor Win64 Development "-Project=B:/Unreal Projects/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject" -WaitMutex`
- Build takes ~5-6 seconds for incremental changes

## Key File Paths
- Tool handlers: `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`
- Schemas: `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintSchemas.cpp`
- Graph reader: `Source/OliveAIEditor/Blueprint/Public/Reader/OliveGraphReader.h` / `Private/Reader/OliveGraphReader.cpp`
- Blueprint reader: `Source/OliveAIEditor/Blueprint/Public/Reader/OliveBlueprintReader.h`
- IR types: `Source/OliveAIRuntime/Public/IR/CommonIR.h` (FOliveIRGraph, FOliveIRNode, etc.)
- Asset resolver: `Source/OliveAIEditor/Public/Services/OliveAssetResolver.h`
- Class resolver: `Source/OliveAIEditor/Blueprint/Public/OliveClassResolver.h` / `Private/OliveClassResolver.cpp`

## Patterns
- Tool handler pattern: validate params -> resolve asset path -> load Blueprint -> execute -> serialize result
- Anonymous namespace helpers in OliveBlueprintToolHandlers.cpp for shared logic (e.g., `ExecuteWithOptionalConfirmation`, `HandleGraphReadWithPaging`)
- Schemas use `MakeSchema()`, `MakeProperties()`, `AddRequired()`, `StringProp()`, `IntProp()`, `EnumProp()` helpers
- FOliveBlueprintReader is singleton with `Get()`, has `LoadBlueprint()` (public) and `GetGraphReader()` accessor
- FOliveGraphReader builds `NodeIdMap` (TMap<const UEdGraphNode*, FString>) before serializing nodes

## Graph-From-Description (Phase B: Plan Executor)
- `OlivePlanExecutor.h/cpp` at `Blueprint/Public/Plan/` and `Blueprint/Private/Plan/`
- NOT a singleton; instantiate fresh per execution on the stack
- Uses `FOlivePinConnector::Get().Connect()` directly (NOT GraphWriter.ConnectPins) for wiring
- Uses `Node->FindPin()` + `Schema->TrySetDefaultValue()` directly for defaults
- Uses `FOliveGraphWriter::Get().AddNode()` for node creation (gets UEdGraphNode* via `GetCachedNode`)
- Event reuse: `FBlueprintEditorUtils::FindOverrideForFunction()` for native events, `UK2Node_CustomEvent::CustomFunctionName` iteration for custom events
- Phase 1 (nodes) = FAIL-FAST, Phase 1.5 (auto-wire component targets) = CONTINUE-ON-FAILURE, Phases 3/4/5 (wiring) = CONTINUE-ON-FAILURE, Phase 6 (layout) = ALWAYS SUCCEEDS
- Phase 1.5 uses `UEdGraphSchema_K2::TryCreateConnection()` directly (NOT FOlivePinConnector) -- runs OUTSIDE BatchScope
- Context fields added for Phase 1.5/5.5: `AutoFixCount`, `PreCompileIssues`, `NodeIdToStepId`, `Plan*`, `FindStepIdForNode()`
- Pin manifests built via `FOlivePinManifest::Build()` after each node creation for ground-truth pin names
- `@step.auto` = type-based auto-match, `@step.~hint` = fuzzy prefix, `@step.pinName` = standard smart resolution

## Phase D: Graph Layout Engine
- `OliveGraphLayoutEngine.h/cpp` at `Blueprint/Public/Plan/` and `Blueprint/Private/Plan/`
- Stateless static utility class (same pattern as FOliveFunctionResolver)
- `ComputeLayout(Plan, Context)` -> `TMap<FString, FOliveLayoutEntry>`; `ApplyLayout(Layout, Context)` -> void
- Internal phases: BuildExecGraph -> AssignColumns (BFS) -> AssignRows (branch-aware) -> PlacePureNodes (consumer-relative) -> ComputePositions (multi-chain stacking)
- Constants: HORIZONTAL_SPACING=350, VERTICAL_SPACING=200, BRANCH_OFFSET=250, PURE_NODE_OFFSET_Y=-120, CHAIN_GAP_ROWS=2
- Log category: LogOliveGraphLayout
- BuildConsumerMap scans inputs for @stepId refs to identify pure-node consumers

## CLI Provider Base Class Extraction (Completed)
- `FOliveCLIProviderBase` at `Public/Providers/OliveCLIProviderBase.h` / `Private/Providers/OliveCLIProviderBase.cpp`
- Abstract base for CLI providers; inherits IOliveAIProvider
- Moved from FOliveClaudeCodeProvider: process management, pipes, callbacks, SendMessage, HandleResponseComplete, BuildConversationPrompt, BuildCLISystemPrompt, CancelRequest, KillProcess
- Virtual hooks: `GetExecutablePath()`, `GetCLIArguments()`, `ParseOutputLine()`, `GetWorkingDirectory()`, `RequiresNodeRunner()`, `GetCLIName()`
- `FOliveClaudeReaderRunnable` renamed to `FOliveCLIReaderRunnable`; old name kept as `using` alias
- Error string "process exited with code" preserved for OliveProviderRetryManager::ClassifyError matching
- Log category: `LogOliveCLIProvider` (base), `LogOliveClaudeCode` (Claude-specific)

## UE 5.5 API Quirks
- **Float/Double PinType**: In UE 5.5, `PC_Float` and `PC_Double` must NOT be used as `PinCategory`. Instead use `PinCategory = PC_Real` with `PinSubCategory = PC_Float` (or `PC_Double`). Using `PC_Float` directly as category causes "Can't parse default value" compile warnings because the engine can't resolve an FProperty from a bare `PC_Float` category.
- Source: `EdGraphSchema_K2.cpp` lines 3503-3512 (ConvertPropertyToPinType sets PC_Real+subcategory for both FFloatProperty and FDoubleProperty)

## Phase 3: Error Recovery, Loop Prevention & Context Injection (Completed)
- **Task 1 (3.3)**: Updated stale guidance strings in `BuildToolErrorMessage()`:
  - PLAN_RESOLVE_FAILED: "get_var for a component" -> "set_var on a component"
  - PLAN_VALIDATION_FAILED: mentions auto-wiring for single components instead of GetComponentByClass
- **Task 2 (3.2)**: Progressive error disclosure in `BuildToolErrorMessage()`:
  - Attempt 1: short header (code only) + guidance
  - Attempt 2: full header (code + message) + guidance
  - Attempt 3+: full header + guidance + ESCALATION block
- **Task 3 (3.1)**: Plan content deduplication:
  - `FOliveLoopDetector::HashString()` moved from private to public
  - `FOliveSelfCorrectionPolicy::Evaluate()` got new `ToolCallArgs` param (optional, default nullptr)
  - `BuildPlanHash()` hashes tool+asset_path+graph_name+condensed_plan_JSON
  - `PreviousPlanHashes` TMap tracks submission counts; >1 = FeedBackErrors, >=3 = StopWorker
  - `OliveConversationManager.cpp` passes `ActiveToolCallArgs[ToolCallId]` to Evaluate
- **Task 4 (3.4)**: Blueprint context injection in prompts:
  - `BuildBlueprintContextBlock()` on FOlivePromptAssembler: loads BP, iterates SCS nodes + NewVariables
  - Called from `GetActiveContext()` when `AssetInfo->bIsBlueprint`
  - Token budget in GetActiveContext naturally handles large BPs
  - Includes: `Engine/Blueprint.h`, `Engine/SimpleConstructionScript.h`, `Engine/SCS_Node.h`

## Priority 0 Task 4: Universal add_node (Implemented)
- `CreateNodeByClass()` public method on FOliveNodeFactory: resolves any UK2Node subclass, sets properties via reflection BEFORE AllocateDefaultPins, then ReconstructNode
- `FindK2NodeClass()` const private: multi-strategy lookup (FindFirstObject -> K2Node_/UK2Node_ prefix -> U-strip -> StaticLoadClass across 6 engine packages)
- `SetNodePropertiesViaReflection()` private: type-specific fast paths (bool/int/float/double/string/name/text/object) + ImportText_Direct fallback
- `ValidateNodeType()` now falls back to FindK2NodeClass when type not in NodeCreators map
- `CreateNode()` uses `NodeCreators.Find()` instead of `NodeCreators[]`; else-branch calls CreateNodeByClass
- Curated NodeCreators always checked FIRST; universal fallback only for types NOT in the map
- Position set uniformly by existing SetNodePosition in CreateNode -- NOT in CreateNodeByClass
- Node factory: `Source/OliveAIEditor/Blueprint/Public/Writer/OliveNodeFactory.h` / `Private/Writer/OliveNodeFactory.cpp`

## Phase 4: Template System (In Progress)
- `OliveTemplateSystem.h` at `Blueprint/Public/Template/` and `OliveTemplateSystem.cpp` at `Blueprint/Private/Template/`
- Singleton with `Get()`, standard static-local pattern
- Log category: `LogOliveTemplates`
- `FOliveTemplateInfo` struct: TemplateId, TemplateType, DisplayName, CatalogDescription, CatalogExamples, Tags, FilePath, FullJson
- GetTemplatesDirectory(): `FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("UE_Olive_AI_Studio/Content/Templates"))`
- ScanDirectory uses `IPlatformFile::IterateDirectoryRecursively()` with lambda visitor `(const TCHAR*, bool) -> bool`
- LoadTemplateFile: required fields = template_id, template_type, catalog_description
- RebuildCatalog: groups by factory/reference, builds `[AVAILABLE BLUEPRINT TEMPLATES]` block
- SubstituteParameters: `${key}` token replacement with warning for unresolved tokens
- EvaluateConditionals: bool ternary `"true ? val : val"` / `"false ? val : val"` pattern
- MergeParameters: defaults -> preset (by name, case-insensitive) -> user overrides
- ApplyTemplate and GetTemplateContent: STUBS for Task 4
- Task 1 complete; remaining: Task 2 (JSON files), Task 3 (tool handlers), Task 4 (executor), Task 5 (prompt injection), Task 6 (startup integration)

## Phase 2 Task 6 (Large-Graph Read Mode)
- Constants: `OLIVE_LARGE_GRAPH_THRESHOLD = 500`, `OLIVE_GRAPH_PAGE_SIZE = 100` in OliveGraphReader.h
- `ReadGraphSummary()`: builds NodeIdMap, counts connections, but leaves Nodes array empty
- `ReadGraphPage()`: builds FULL NodeIdMap, serializes only [Offset, Offset+Limit) slice
- Tool handlers detect large graphs and auto-return summary; `page` param for paging; `mode=full` forces full read
- Summary metadata (event_nodes, node_type_breakdown) built in anonymous namespace helper `AttachLargeGraphSummaryMetadata()`
