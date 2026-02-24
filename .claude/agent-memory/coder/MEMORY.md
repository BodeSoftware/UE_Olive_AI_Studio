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
- Phase 1 (nodes) = FAIL-FAST, Phases 3/4/5 (wiring) = CONTINUE-ON-FAILURE, Phase 6 (layout) = ALWAYS SUCCEEDS
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

## Current Task: Fix Blueprint Class Resolution (4 tasks)
- Design plan: `plans/fix-blueprint-class-resolution-design.md`
- Task 1: Create `FOliveClassResolver` (new .h/.cpp) — 6-step resolution chain + LRU cache
- Task 2: Rewire 3 callers (OliveNodeFactory, OliveBlueprintWriter, OliveFunctionResolver) to use it
- Task 3: Improve PLAN_INVALID_REF_FORMAT error message in OliveIRSchema.cpp
- Task 4: Improve BP_CONNECT_PINS_FAILED + BP_ADD_NODE_FAILED self-correction hints in OliveSelfCorrectionPolicy.cpp
- Task 2 is blocked by Task 1

## UE 5.5 API Quirks
- **Float/Double PinType**: In UE 5.5, `PC_Float` and `PC_Double` must NOT be used as `PinCategory`. Instead use `PinCategory = PC_Real` with `PinSubCategory = PC_Float` (or `PC_Double`). Using `PC_Float` directly as category causes "Can't parse default value" compile warnings because the engine can't resolve an FProperty from a bare `PC_Float` category.
- Source: `EdGraphSchema_K2.cpp` lines 3503-3512 (ConvertPropertyToPinType sets PC_Real+subcategory for both FFloatProperty and FDoubleProperty)

## Phase 2 Task 6 (Large-Graph Read Mode)
- Constants: `OLIVE_LARGE_GRAPH_THRESHOLD = 500`, `OLIVE_GRAPH_PAGE_SIZE = 100` in OliveGraphReader.h
- `ReadGraphSummary()`: builds NodeIdMap, counts connections, but leaves Nodes array empty
- `ReadGraphPage()`: builds FULL NodeIdMap, serializes only [Offset, Offset+Limit) slice
- Tool handlers detect large graphs and auto-return summary; `page` param for paging; `mode=full` forces full read
- Summary metadata (event_nodes, node_type_breakdown) built in anonymous namespace helper `AttachLargeGraphSummaryMetadata()`
