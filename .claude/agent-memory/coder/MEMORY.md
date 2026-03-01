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
- Stateless static utility class; `ComputeLayout()` -> `TMap<FString, FOliveLayoutEntry>`; `ApplyLayout()` -> void
- Log category: LogOliveGraphLayout

## CLI Provider Base Class (NeoStack T4 Refactored)
- `FOliveCLIProviderBase` at `Public/Providers/OliveCLIProviderBase.h` / `Private/Providers/OliveCLIProviderBase.cpp`
- Abstract base for CLI providers; inherits IOliveAIProvider
- Two paths: Orchestrated (SendMessage, per-turn) and Autonomous (SendMessageAutonomous, MCP)
- `LaunchCLIProcess()`: shared process lifecycle used by both paths
- `RequestGeneration` counter prevents stale async completions; `AliveGuard` prevents use-after-free
- Tool filtering: `SetToolFilter()`/`ClearToolFilter()` on MCPServer; `HandleToolsCall` NOT filtered
- See CLAUDE.md for full details on prompt routing, auto-continue, asset state injection

## UE 5.5 API Quirks
- **Float/Double PinType**: In UE 5.5, `PC_Float` and `PC_Double` must NOT be used as `PinCategory`. Instead use `PinCategory = PC_Real` with `PinSubCategory = PC_Float` (or `PC_Double`). Using `PC_Float` directly as category causes "Can't parse default value" compile warnings because the engine can't resolve an FProperty from a bare `PC_Float` category.
- Source: `EdGraphSchema_K2.cpp` lines 3503-3512 (ConvertPropertyToPinType sets PC_Real+subcategory for both FFloatProperty and FDoubleProperty)
- **FJsonValue::GetType()** is a **protected** virtual method in UE 5.5 (`JsonValue.h:117`). Cannot be called from outside the class. Use `Pair.Value->Type` (public `EJson` enum) instead with `static_cast<int32>()` for logging.

## Phase 3: Error Recovery (Completed)
- Progressive error disclosure, plan dedup, blueprint context injection in prompts
- See `plans/` for full design details

## Phase 4: Template System (Completed)
- `FOliveTemplateSystem` singleton at `Blueprint/Public/Template/` and `Blueprint/Private/Template/`
- Factory templates create BPs; reference templates provide pattern docs
- `ApplyTemplate()` instantiates factory templates; `GetTemplateContent()` reads reference/factory content

## Pipeline Reliability (Completed)
- Phase 1: Granular fallback + retry policy (max 20 correction cycles, loop detection -> step-by-step fallback)
- Phase 2: Graph context threading (FOliveGraphContext, function params, latent-in-function detection)

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

## Delegate Ops (call_delegate, call_dispatcher, bind_dispatcher)
- `OlivePlanOps::CallDelegate = "call_delegate"` and `CallDispatcher = "call_dispatcher"` (alias) in BlueprintPlanIR.h
- `OlivePlanOps::BindDispatcher = "bind_dispatcher"` in BlueprintPlanIR.h
- `OliveNodeTypes::CallDelegate = "CallDelegate"` and `BindDelegate = "BindDelegate"` in OliveNodeFactory.h
- `ResolveCallDelegateOp()`: validates target against BP's NewVariables with `PC_MCDelegate` category; handles both call_delegate and call_dispatcher ops
- `ResolveBindDelegateOp()`: same validation as call_delegate, sets NodeType to BindDelegate
- `CreateCallDelegateNode()`: finds `FMulticastDelegateProperty` on SkeletonGeneratedClass (fallback GeneratedClass), uses `UK2Node_CallDelegate` + `SetFromProperty(DelegateProp, true, OwnerClass)`
- `CreateBindDelegateNode()`: same property lookup, uses `UK2Node_AddDelegate` instead of `UK2Node_CallDelegate`
- Includes: `K2Node_CallDelegate.h`, `K2Node_AddDelegate.h` (from BlueprintGraph module)
- Both ops: `bIsPure = false` (delegate ops have exec pins)
- UK2Node_AddDelegate has a "Delegate" pin (friendly name "Event") for wiring to custom events
- gun.json template: dispatcher steps use `call_delegate` instead of `call`; timer uses `SetTimerByFunctionName` instead of `K2_SetTimerDelegate`

## Timeout & Plan Reliability Fixes (Partial - C1, B1-B3)
- **C1**: Post-pipeline rollback now checks `status == "partial_success"` in ResultData before rolling back. Partial success (nodes created, some wiring failed) is preserved; only total failures trigger rollback.
- **B1**: `ResolveStep()` remaps `op: "entry"` -> `op: "event"` early (alias handling). Event dispatch in function graphs checks if target matches function name / "entry" / empty and maps to `FunctionInput` instead of `ResolveEventOp`.
- **B2**: `ExpandComponentRefs()` now builds `BlueprintVariableNames` set from `Blueprint->NewVariables`. Bare `@VarName` and dotted `@VarName.hint` refs matching BP variables synthesize `_synth_getvar_xxx` get_var steps. Priority: step ID > function param > SCS component > BP variable.
- **B3**: `ExpandBranchConditions()` new static resolver pass, called after `ExpandPlanInputs` in `Resolve()`. Detects branch steps where Condition `@ref` points to a get_var of non-boolean variable. Synthesizes `Greater_IntInt` (int) or `Greater_DoubleDouble` (float/real) comparison step. UE 5.5 pin category for float/double is `"real"` (not "float"/"double").

## Component Bound Events (UK2Node_ComponentBoundEvent)
- `OliveNodeTypes::ComponentBoundEvent = "ComponentBoundEvent"` in OliveNodeFactory.h
- `CreateComponentBoundEventNode()` on FOliveNodeFactory: finds SCS node by component_name, finds FMulticastDelegateProperty on ComponentClass, finds FObjectProperty on GeneratedClass, uses `InitializeComponentBoundEventParams(ComponentProp, DelegateProp)`
- Required properties: `delegate_name` (e.g., "OnComponentBeginOverlap"), `component_name` (e.g., "CollisionComp")
- Resolver: `ResolveEventOp` detects non-native event targets by scanning SCS component delegates; fuzzy match with/without "On" prefix
- Executor: `FindExistingComponentBoundEventNode()` matches by DelegatePropertyName + ComponentPropertyName for reuse
- Auto-chain works because Plan.Steps[].Op stays "event" (resolver only changes NodeType)
- Plan syntax: `{"op":"event","target":"OnComponentBeginOverlap","properties":{"component_name":"CollisionComp"}}`
- Fallback: if resolver can't find SCS match (e.g., no component_name hint, inherited components), passes through to NodeFactory's CreateEventNode which has its own SCS scan

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

## Resolver Removal: FOliveFunctionResolver Bypassed (Completed)
- `FOliveFunctionResolver` is now dead code (kept for one release cycle, not called)
- `ResolveCallOp()` in OliveBlueprintPlanResolver.cpp calls `FOliveNodeFactory::Get().FindFunctionEx()` directly
- `FindFunction()` is now PUBLIC on FOliveNodeFactory (was private)
- `FindFunctionEx()` wraps FindFunction + collects SearchedLocations on failure for detailed error messages
- `FOliveFunctionSearchResult` struct: Function, MatchMethod, MatchedClassName, SearchedLocations, BuildSearchedLocationsString()
- `FindFunction()` enhanced search order: alias map -> specified class -> GeneratedClass -> FunctionGraphs -> parent class hierarchy -> SCS component classes -> implemented interfaces -> library classes -> universal BFL search (TObjectIterator) -> K2_ fuzzy match (Steps 1-5 classes only)
- Each class tried with exact name first, then K2_ prefix variant (inline lambda `TryClassWithK2`)
- `GetAliasMap()` static public method on FOliveNodeFactory (copied verbatim from OliveFunctionResolver)
- No more "accepted as-is" fallback path -- unknown functions fail at resolution with FUNCTION_NOT_FOUND error
- `ResolvedOwningClass`, `bIsPure`, `bIsLatent` still populated from `UFunction*` introspection

## FN-4: Interface Message Call Support (Completed)
- `EOliveFunctionMatchMethod` enum in OliveNodeFactory.h: None, ExactName, AliasMap, GeneratedClass, FunctionGraph, ParentClassSearch, ComponentClassSearch, InterfaceSearch, LibrarySearch, UniversalLibrarySearch, FuzzyK2Match
- `FindFunction()` has optional `EOliveFunctionMatchMethod* OutMatchMethod` out-param
- Step 4b in FindFunction: iterates `Blueprint->ImplementedInterfaces`, tries each InterfaceDesc.Interface
- InterfaceSearch always reported as InterfaceSearch (even if alias was used, since node type depends on it)
- `CreateCallFunctionNode()`: when MatchMethod==InterfaceSearch, creates `UK2Node_Message` instead of `UK2Node_CallFunction`
- UK2Node_Message setup: `FunctionReference.SetFromField<UFunction>(Function, false)` (bIsConsideredSelfContext=false preserves interface as MemberParent)
- `FOliveResolvedStep.bIsInterfaceCall` flag set by ResolveCallOp, propagated via `is_interface_call` property
- Interface message calls forced `bIsPure = false` (UK2Node_Message::IsNodePure returns false)
- Include: `K2Node_Message.h` in OliveNodeFactory.cpp

## ExpandedPlan Fix (Round 2 Task 2)
- `FOlivePlanResolveResult` now carries `ExpandedPlan` field -- the plan after all pre-processing
- All post-resolve code MUST use `ResolveResult.ExpandedPlan` instead of the original `Plan` variable

## Phase 2: Tool Alias Map (P2-1, Completed)
- `FOliveToolAlias` struct + `GetToolAliases()` in OliveToolRegistry.cpp; `ResolveAlias()`/`IsToolAlias()` public
- Aliases never appear in tools/list; `ExecuteTool()` resolves aliases before normalize+lookup

## P2-2: Blueprint Read Tool Consolidation (Completed)
- 7 tools merged into 1 unified `blueprint.read` with `section` param
- `section` values: `all` (default), `summary`, `graph`, `variables`, `components`, `hierarchy`, `overridable_functions`
- `graph_name` required when section="graph"; searches UbergraphPages + FunctionGraphs + MacroGraphs
- Old tools (`read_function`, `read_event_graph`, `read_variables`, `read_components`, `read_hierarchy`, `list_overridable_functions`) redirect via alias map
- Handler method names: `HandleBlueprintRead` (router) -> `HandleReadSectionAll`, `HandleReadSectionGraph`, etc.
- Only 2 reader tools registered: `blueprint.read`, `blueprint.get_node_pins`
- Updated: tool packs (OliveToolPacks.json + OliveToolPackManager.cpp fallback), PIE whitelist (OliveValidationEngine.cpp)

## P2-3: Function Tool Consolidation (Completed)
- `blueprint.add_function`: unified handler with `function_type` enum (function/custom_event/event_dispatcher/override)
  - `HandleBlueprintAddFunction` validates path+BP, routes to `HandleAddFunctionType_*` helpers
  - Old tools (add_custom_event, add_event_dispatcher, override_function) removed from registration
  - Old tool names still work via redirect aliases in OliveToolRegistry.cpp (set function_type)
  - Schema: path required, function_type optional (default "function"), accepts signature/name/params/function_name
  - Validation engine updated: checks `function_type` param instead of old tool names for duplicate-layer rule
  - Tool packs (OliveToolPacks.json + OliveToolPackManager.cpp) cleaned of old tool names
  - System prompts updated to reference `function_type=` syntax

## P2-4: Variable Upsert + Template Merge (Completed)
- `blueprint.add_variable` is now upsert: creates if missing, updates if present
  - Detects modify_variable alias format ({name, changes}) and routes to ModifyVariable writer
  - `modify_only` bool param: when true, errors if variable doesn't exist
  - When variable exists via standard format, extracts modifications from FOliveIRVariable fields
  - `blueprint.modify_variable` registration REMOVED (alias in OliveToolRegistry redirects)
  - `BlueprintModifyVariable` schema REMOVED from OliveBlueprintSchemas
- `blueprint.create` now accepts optional `template_id` + `template_params` + `preset`
  - When `template_id` set, delegates to `HandleBlueprintCreateFromTemplate(TemplateId, AssetPath, Params)`
  - `parent_class` only required when NOT using template
  - `blueprint.create_from_template` registration REMOVED (alias redirects)
  - `BlueprintCreateFromTemplate` schema REMOVED
  - `HandleBlueprintCreateFromTemplate` signature changed: takes (TemplateId, AssetPath, Params) not (Params)
  - Accepts both `template_params` and `parameters` keys for backward compat
- Updated: OliveToolPacks.json, OliveValidationEngine.h, OliveCLIProviderBase.cpp, OliveTemplateSystem.cpp

## P2-5: BT/Blackboard Tool Consolidation (Completed)
- `behaviortree.add_node`: unified handler with `node_kind` enum (composite/task/decorator/service)
  - `HandleBehaviorTreeAddNode` routes to internal handlers; maps `class` -> kind-specific class param
  - Old tools (add_composite, add_task, add_decorator, add_service) removed from registration
  - Old tool names still work via redirect aliases in OliveToolRegistry.cpp
- `blackboard.add_key` upsert: checks if key exists, delegates to `HandleBlackboardModifyKey` on match
  - `blackboard.modify_key` registration removed; handler kept as private internal helper

## Python Tool (editor.run_python) - Autonomy Phase A
- Files: `Python/Public/MCP/OlivePythonSchemas.h`, `Python/Private/MCP/OlivePythonSchemas.cpp`, `Python/Public/MCP/OlivePythonToolHandlers.h`, `Python/Private/MCP/OlivePythonToolHandlers.cpp`
- Singleton `FOlivePythonToolHandlers::Get()` with `RegisterAllTools()`/`UnregisterAllTools()` pattern
- `IPythonScriptPlugin::Get()` returns nullptr if module not loaded; `IsPythonAvailable()` is instance method
- `FPythonCommandEx` default ExecutionMode is `ExecuteFile` (handles multi-line scripts); no need to set explicitly
- `FPythonLogOutputEntry::Type` is `EPythonLogOutputType` (Info/Warning/Error)
- Tags: `{blueprint, cpp, python, editor, write}` -- visible in all focus profiles
- Safety: auto-snapshot via FOliveSnapshotManager, try/except wrapper, persistent log at `Saved/OliveAI/PythonScripts.log`
- **UE API quirk**: `LogPath` is a UE global log category (from EngineLogs.h); avoid naming local variables `LogPath`
- Build.cs: `"Python"` in SubModules + `"PythonScriptPlugin"` in PrivateDependencyModuleNames
- uplugin: `PythonScriptPlugin` added to Plugins array

## FindFunction Interface Fix (Autonomy Phase A, T7)
- FindFunction Step 1: when specified class is UInterface or Blueprint Interface, reports InterfaceSearch (not ExactName)
- Two-part check: `Class->IsChildOf(UInterface::StaticClass())` for native interfaces, `Cast<UBlueprint>(ClassGeneratedBy)->BlueprintType == BPTYPE_Interface` for Blueprint Interfaces
- Bypasses `ReportMatch` lambda (which would override to AliasMap if alias was used) by setting `*OutMatchMethod` directly
