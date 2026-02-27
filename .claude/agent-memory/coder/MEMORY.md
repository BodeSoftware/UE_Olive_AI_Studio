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

## CLI Provider Base Class (NeoStack T4 Refactored)
- `FOliveCLIProviderBase` at `Public/Providers/OliveCLIProviderBase.h` / `Private/Providers/OliveCLIProviderBase.cpp`
- Abstract base for CLI providers; inherits IOliveAIProvider
- Virtual hooks: `GetExecutablePath()`, `GetCLIArguments()`, `GetCLIArgumentsAutonomous()`, `ParseOutputLine()`, `GetWorkingDirectory()`, `RequiresNodeRunner()`, `GetCLIName()`
- Error string "process exited with code" preserved in BOTH HandleResponseComplete and HandleResponseCompleteAutonomous
- Log category: `LogOliveCLIProvider` (base), `LogOliveClaudeCode` (Claude-specific)
- **LaunchCLIProcess()**: extracted shared process lifecycle (spawn, stdin, read loop, exit) used by both SendMessage and SendMessageAutonomous
  - Callers set bIsBusy/callbacks/generation BEFORE calling; LaunchCLIProcess captures current generation for staleness checks
  - `OnProcessExit` TFunction called on game thread inside Guard+Generation+Lock checks
  - `MoveTemp(OnProcessExit)` into background lambda; copied into completion game-thread lambda
- **SendMessage()** (orchestrated): builds prompts on game thread, escapes system prompt, calls LaunchCLIProcess with HandleResponseComplete
- **SendMessageAutonomous()** (autonomous/MCP): no prompt building, no tool schema serialization, calls LaunchCLIProcess with HandleResponseCompleteAutonomous
- **HandleResponseCompleteAutonomous()**: emits AccumulatedResponse via OnComplete, no tool_call parsing (tools go through MCP)
- **Generation counter**: `std::atomic<uint32> RequestGeneration` prevents stale async completions
  - Incremented in `SendMessage()`/`SendMessageAutonomous()` (start) and `CancelRequest()` (invalidate)
  - `LaunchCLIProcess` reads it via `.load()` and checks in all 3 game-thread dispatches (line-parse, buffer-flush, completion)
- **Idle timeout**: `CLI_IDLE_TIMEOUT_SECONDS = 90.0` in anonymous namespace; resets on any stdout output; kills hung processes
- **Auto-continue**: on idle timeout + write-op progress + AutoContinueCount < MaxAutoContinues (3), relaunches with BuildContinuationPrompt. Uses AsyncTask to avoid re-entrancy. `bLastRunWasRuntimeLimit` flag distinguishes runtime limit (no auto-continue) from idle stall.
  - `bIsAutoContinuation` flag: set before AsyncTask dispatch, consumed at top of SendMessageAutonomous. Prevents counter reset on auto-continue calls.
  - `IsWriteOperation()` in anonymous namespace: checks tool name suffix for write ops (create, apply, add, set_, connect, etc.). Only write-op stalls trigger auto-continue.
  - `BuildContinuationPrompt` now calls `BuildAssetStateSummary()` (loads UBlueprints on game thread) to inject asset state (components, variables, functions with node counts, compile status)
  - BuildContinuationPrompt called INSIDE the AsyncTask lambda (game thread safe for UObject loading)
- **Tool filtering**: `FOliveMCPServer::SetToolFilter(TSet<FString>)` / `ClearToolFilter()` restrict `HandleToolsList` by prefix. `DetermineToolPrefixes()` in anonymous namespace infers domain from user message. Set before LaunchCLIProcess, cleared in HandleResponseCompleteAutonomous and CancelRequest. `HandleToolsCall` is NOT filtered (any tool still callable).
- **@-mention asset state injection**: `SetInitialContextAssets()` (public) sets `InitialContextAssetPaths` on the provider. `SendMessageAutonomous()` calls `BuildAssetStateSummary(InitialContextAssetPaths)` to inject pre-read state into initial prompt. ConversationManager calls `SetInitialContextAssets(ActiveContextPaths)` via `static_cast<FOliveCLIProviderBase*>` (safe: IsAutonomousProvider() gates entry). `BuildAssetStateSummary(const TArray<FString>&)` is the primary overload; no-arg version is inline wrapper reading `LastRunContext.ModifiedAssetPaths`.

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

## Pipeline Reliability Phase 1: Granular Fallback + Retry Policy (Completed)
- `FOliveRetryPolicy.MaxCorrectionCyclesPerWorker` raised from 5 to 20 (hard backstop)
- `FOliveRetryPolicy.bAllowGranularFallback = true` flag added
- `FOliveSelfCorrectionPolicy` new fields: `bIsInGranularFallback`, `LastPlanFailureReason` (reset in `Reset()`)
- `HasCompileFailure()` now takes `int32& OutRolledBackNodeCount` out-param; extracts from `data.rolled_back_nodes`
- Loop detection split: `IsLooping||IsOscillating` separate from `IsBudgetExhausted` (hard backstop)
- Granular fallback: on loop detection, if not already in fallback mode, resets loop detector & switches to step-by-step
- `BuildRollbackAwareMessage()`: tells AI to resubmit corrected plan, warns about deleted node IDs
- `BuildGranularFallbackMessage()`: forces AI to use add_node/connect_pins/set_pin_default instead of plan_json
- Tool failure branch also extracts `rolled_back_nodes` via JSON parse + applies same granular fallback pattern
- Template system: `compile_result` JSON object added to ResultData; `FOliveIRMessage` with COMPILE_FAILED code added to Result.Messages when compile fails
- `FOliveToolResult::Success()` takes only `TSharedPtr<FJsonObject>` -- no message param. Use `Result.Messages.Add()` for warnings.

## Pipeline Reliability Phase 2: Graph Context Threading (Completed)
- `FOliveGraphContext` struct in `OliveBlueprintPlanResolver.h`: GraphName, bIsFunctionGraph, bIsMacroGraph, InputParamNames, OutputParamNames, Graph*
- `BuildFromBlueprint()` searches UbergraphPages, FunctionGraphs, MacroGraphs; extracts UserDefinedPins from UK2Node_FunctionEntry/Result
- `bIsLatent` on `FOliveResolvedStep`: set from `UFunction::HasMetaData("Latent")` in ResolveCallOp, or explicitly for Delay op
- GraphContext threaded through: `Resolve()`, `ResolveStep()`, `ResolveGetVarOp()`, `ResolveSetVarOp()`, `Validate()`
- Function param detection: get_var matching InputParamNames -> FunctionInput type; set_var matching OutputParamNames -> FunctionOutput type
- `OliveNodeTypes::FunctionInput` / `FunctionOutput` are virtual types (no actual node creation; reuse FunctionEntry/Result)
- `CheckLatentInFunctionGraph()` in validator: rejects latent steps in function graphs with `LATENT_IN_FUNCTION` error code
- Executor: FunctionInput/FunctionOutput virtual steps find existing FunctionEntry/FunctionResult nodes, add to ReusedStepIds
- `OlivePlanValidator.h` includes `OliveBlueprintPlanResolver.h` (needed for FOliveGraphContext default parameter)
- All callers updated: 4 in OliveBlueprintToolHandlers.cpp (preview+apply: Resolve+Validate), 2 in OliveTemplateSystem.cpp (func+EG Resolve)
- Variable name in tool handlers: `GraphTarget` (not `GraphName`)

## Phase 2 Task 6 (Large-Graph Read Mode)
- Constants: `OLIVE_LARGE_GRAPH_THRESHOLD = 500`, `OLIVE_GRAPH_PAGE_SIZE = 100` in OliveGraphReader.h
- `ReadGraphSummary()`: builds NodeIdMap, counts connections, but leaves Nodes array empty
- `ReadGraphPage()`: builds FULL NodeIdMap, serializes only [Offset, Offset+Limit) slice
- Tool handlers detect large graphs and auto-return summary; `page` param for paging; `mode=full` forces full read
- Summary metadata (event_nodes, node_type_breakdown) built in anonymous namespace helper `AttachLargeGraphSummaryMetadata()`

## NeoStack T0: Tool Resilience Hardening (Completed)
- **0A**: `NormalizeBlueprintParams()` replaced by `NormalizeToolParams()` in `OliveToolRegistry.cpp` anonymous namespace
  - Per-family normalizers: `NormalizeBlueprintParams`, `NormalizeBTParams`, `NormalizePCGParams`, `NormalizeCppParams`, `NormalizeProjectParams`
  - Shared helpers: `TryApplyAlias()` (string fields), `TryApplyFieldAlias()` (object/array fields), `GetToolFamily()`
  - Blueprint: path aliases + `plan_json<-plan/steps`, `function_name<-name/function`, `parent_class<-parent/base_class`, `template_id<-template/id`
  - Cpp: tool-specific `name` disambiguation (class_name for read_class/create_class, property_name for add_property, etc.)
- **0B**: All `GetStringField()` for required params replaced with `TryGetStringField()+empty check+3-part error` in BT/PCG/Cpp/CrossSystem handlers
  - Files: `BehaviorTree/Private/MCP/OliveBTToolHandlers.cpp`, `PCG/Private/MCP/OlivePCGToolHandlers.cpp`, `Cpp/Private/MCP/OliveCppToolHandlers.cpp`, `CrossSystem/Private/MCP/OliveCrossSystemToolHandlers.cpp`
- **0C**: Type parsing aliases expanded in `ParseTypeFromParams()` (str/fstring->string, fvector/vec/vec3->vector, etc.); BT `ParseKeyType()` made case-insensitive
- **0D**: Suggestion strings added to all bare errors in `OliveGraphBatchExecutor.cpp` and non-blueprint handlers
- Note: `FOliveBlueprintWriteResult::Error()` takes only message+path (no suggestion param), so batch executor errors embed suggestions in the message itself

## call_delegate Op (Round 2 Task 1)
- `OlivePlanOps::CallDelegate = "call_delegate"` added to BlueprintPlanIR.h vocabulary
- `OliveNodeTypes::CallDelegate = "CallDelegate"` in OliveNodeFactory.h
- `ResolveCallDelegateOp()`: validates target against BP's NewVariables with `PC_MCDelegate` category
- `CreateCallDelegateNode()`: finds `FMulticastDelegateProperty` on SkeletonGeneratedClass (fallback GeneratedClass), uses `UK2Node_CallDelegate` + `SetFromProperty(DelegateProp, true, OwnerClass)`
- Include: `K2Node_CallDelegate.h` (from BlueprintGraph module, also has `K2Node_BaseMCDelegate.h`)
- `bIsPure = false` (delegate broadcasts have exec pins)
- gun.json template: dispatcher steps use `call_delegate` instead of `call`; timer uses `SetTimerByFunctionName` instead of `K2_SetTimerDelegate`

## Timeout & Plan Reliability Fixes (Partial - C1, B1-B3)
- **C1**: Post-pipeline rollback now checks `status == "partial_success"` in ResultData before rolling back. Partial success (nodes created, some wiring failed) is preserved; only total failures trigger rollback.
- **B1**: `ResolveStep()` remaps `op: "entry"` -> `op: "event"` early (alias handling). Event dispatch in function graphs checks if target matches function name / "entry" / empty and maps to `FunctionInput` instead of `ResolveEventOp`.
- **B2**: `ExpandComponentRefs()` now builds `BlueprintVariableNames` set from `Blueprint->NewVariables`. Bare `@VarName` and dotted `@VarName.hint` refs matching BP variables synthesize `_synth_getvar_xxx` get_var steps. Priority: step ID > function param > SCS component > BP variable.
- **B3**: `ExpandBranchConditions()` new static resolver pass, called after `ExpandPlanInputs` in `Resolve()`. Detects branch steps where Condition `@ref` points to a get_var of non-boolean variable. Synthesizes `Greater_IntInt` (int) or `Greater_DoubleDouble` (float/real) comparison step. UE 5.5 pin category for float/double is `"real"` (not "float"/"double").

## Enhanced Input Action Support (K2Node_EnhancedInputAction)
- `OliveNodeTypes::EnhancedInputAction = "EnhancedInputAction"` added to OliveNodeFactory.h
- `CreateEnhancedInputActionNode()` on FOliveNodeFactory: searches AssetRegistry for UInputAction by name, creates UK2Node_EnhancedInputAction
- Property: `TObjectPtr<const UInputAction> InputAction` (set BEFORE AllocateDefaultPins)
- Headers: `K2Node_EnhancedInputAction.h` (from InputBlueprintNodes module), `InputAction.h` (from EnhancedInput module)
- Build.cs deps: `EnhancedInput`, `InputBlueprintNodes`; uplugin dep: `EnhancedInput` plugin
- Three-path resolution in CreateEventNode: (1) native function override, (2) component delegate, (3) Enhanced Input Action (IA_ prefix)
- Plan resolver: `ResolveEventOp` detects `IA_` prefix targets, sets NodeType to EnhancedInputAction + `input_action_name` property
- Plan executor: `FindExistingEnhancedInputNode()` for reuse detection (matches by UInputAction asset name)
- Asset search: AssetRegistry by class -> direct LoadObject with common paths (/Game/Input/Actions/, /Game/Input/, /Game/)
- Duplicate detection: only one UK2Node_EnhancedInputAction per UInputAction per graph
- Auto-chain in PhaseWireExec works because Plan.Steps[].Op is still "event" (resolver only changes NodeType, not Op)

## ExpandedPlan Fix (Round 2 Task 2)
- `FOlivePlanResolveResult` now carries `ExpandedPlan` field -- the plan after all pre-processing (ExpandComponentRefs, ExpandPlanInputs, ExpandBranchConditions)
- All post-resolve code MUST use `ResolveResult.ExpandedPlan` instead of the original `Plan` variable
- Apply handler: drift detection moved AFTER Resolve() so fingerprint matches preview's expanded-plan fingerprint
- Tool handlers pattern: `FOliveIRBlueprintPlan& ExpandedPlan = ResolveResult.ExpandedPlan;` alias after Resolve
- Template system: both function graph and event graph executor calls use `ResolveResult.ExpandedPlan`
- Lambda capture in apply handler: `FOliveIRBlueprintPlan CapturedPlan = ExpandedPlan;` (not original Plan)
