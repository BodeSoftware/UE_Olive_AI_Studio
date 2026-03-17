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

## bOrphanedPin Fix (Point-of-Use Pattern)
- `EnsurePinNotOrphaned()` static helper in OlivePlanExecutor.cpp: checks `bOrphanedPin`, calls `ReconstructNode()`, re-finds pin by FName
- Applied at all pin access points: WireExecConnection, WireDataConnection, auto-chain paths (entry alias, event-to-follower, stub-wire break)
- Previous attempts (executor post-failure cleanup + handler post-transaction cleanup) were ineffective: executor cleanup runs inside FScopedTransaction and gets rolled back; handler cleanup condition was reachable but pins may not have bOrphanedPin at that point (depends on UE undo system behavior)
- After `ReconstructNode()`, old pin pointer is INVALID -- must re-find by name
- Fallback loops for exec pins should also check `!Pin->bOrphanedPin`

## Resolver-Executor Contract (Single Resolution Authority)
- `FOliveResolvedStep.ResolvedFunction` carries `UFunction*` from resolver to executor
- **Critical**: `PreResolvedFunction` consumed in `CreateCallFunctionNode()` (curated path), NOT `CreateNodeByClass` (universal fallback). `CreateNode()` dispatches to curated creators for all OliveNodeTypes, only falls through to `CreateNodeByClass` for raw UK2Node class names.
- `ValidateNodeType()` skips redundant FindFunction when `PreResolvedFunction != nullptr`
- Executor only sets PreResolvedFunction for `CallFunction` (not `CallDelegate` — delegates use property-based resolution, leaking the pointer would pollute next call)
- UPROPERTY auto-rewrite: resolver detects `PROPERTY MATCH:` in SearchedLocations, rewrites call→set_var/get_var. Now checks BOTH original SearchResult AND LastCastSearchResult (from cast-target fallback). External class → `external_class` property → `SetExternalMember()` in node factory.
- OliveNodeTypes constants: `SetVariable`/`GetVariable` (NOT `VariableSet`/`VariableGet`)
- **Transaction rollback pitfall**: bOrphanedPin cleanup inside Execute() gets rolled back by Transaction->Cancel(). Post-rollback cleanup must happen AFTER pipeline returns (in tool handler, not executor).

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

## CLI Provider Base Class (Persistent Sessions, 2026-03-16)
- `FOliveCLIProviderBase` at `Public/Providers/OliveCLIProviderBase.h` / `Private/Providers/OliveCLIProviderBase.cpp`
- Abstract base for CLI providers; inherits IOliveAIProvider
- Two paths: Orchestrated (SendMessage, per-turn) and Autonomous (SendMessageAutonomous, MCP)
- `LaunchCLIProcess()`: shared process lifecycle used by both paths
- `RequestGeneration` counter prevents stale async completions; `AliveGuard` prevents use-after-free
- Tool filtering: `SetToolFilter()`/`ClearToolFilter()` on MCPServer; `HandleToolsCall` NOT filtered
- **Session Resume** (2026-03-16): `CLISessionId` + `bHasActiveSession` for persistent conversations
  - `SupportsSessionResume()` virtual: Claude=true, Codex=false (default)
  - First message: `--session-id <uuid>`, full sandbox setup, discovery, decomposition
  - Subsequent messages: `--resume <uuid>`, raw user message only (no sandbox, no discovery)
  - `ResetSession()`: kills process, clears session ID + sandbox, forces fresh session
  - `CancelRequest()` preserves session state (CLISessionId, bHasActiveSession stay intact)
  - **DELETED**: `FAutonomousRunContext`, `LastRunContext`, auto-continue system, `IsContinuationMessage()`, `BuildContinuationPrompt()`, `ScanEmptyFunctionGraphs()`, `FEmptyFunctionInfo`, `FSparseEventGraphInfo`, `bLastRunTimedOut`, `bLastRunWasRuntimeLimit`, `AutoContinueCount`, `bIsAutoContinuation`, `MaxAutoContinues`, `IsWriteOperation()`, `BuildAssetStateSummary()` no-arg overload
  - `IOliveAIProvider::ResetSession()` added as virtual no-op default

## UE 5.5 API Quirks
- **Float/Double PinType**: In UE 5.5, `PC_Float` and `PC_Double` must NOT be used as `PinCategory`. Instead use `PinCategory = PC_Real` with `PinSubCategory = PC_Float` (or `PC_Double`). Using `PC_Float` directly as category causes "Can't parse default value" compile warnings because the engine can't resolve an FProperty from a bare `PC_Float` category.
- Source: `EdGraphSchema_K2.cpp` lines 3503-3512 (ConvertPropertyToPinType sets PC_Real+subcategory for both FFloatProperty and FDoubleProperty)
- **FJsonValue::GetType()** is a **protected** virtual method in UE 5.5 (`JsonValue.h:117`). Cannot be called from outside the class. Use `Pair.Value->Type` (public `EJson` enum) instead with `static_cast<int32>()` for logging.
- **FString::Printf requires literal format string**: In UE 5.5, `FString::Printf` uses a template with `static_assert` requiring the format string be a `TCHAR` literal (not a variable). Use two separate literal strings with a ternary selector instead of `Printf` with `%s` for SQL variants.

## Completed Phases (see plans/ for details)
- Phase 3: Error Recovery, Phase 4: Template System, Pipeline Reliability, Large-Graph Paging, Tool Resilience (T0)

## Delegate/Event/Interface/Input Ops (see CLAUDE.md for full details)
- Delegate ops: call_delegate, call_dispatcher, bind_dispatcher
- Component bound events: UK2Node_ComponentBoundEvent via resolver auto-detection
- Interface events: bIsInterfaceEvent flag, FunctionCanBePlacedAsEvent() filter
- Enhanced Input Actions: IA_ prefix detection, UK2Node_EnhancedInputAction
- Interface calls: EOliveFunctionMatchMethod::InterfaceSearch -> UK2Node_Message

## Key Architecture Notes
- `ExpandedPlan` field on FOlivePlanResolveResult -- post-resolve code MUST use it, not original Plan
- FindFunction search: alias -> specified class -> GeneratedClass -> FunctionGraphs -> parent -> SCS -> interfaces -> libraries -> universal BFL -> K2_ fuzzy
- FindFunctionEx wraps FindFunction with SearchedLocations for error messages

## Phase 2: Tool Alias Map (P2-1, Completed)
- `FOliveToolAlias` struct + `GetToolAliases()` in OliveToolRegistry.cpp; `ResolveAlias()`/`IsToolAlias()` public
- Aliases never appear in tools/list; `ExecuteTool()` resolves aliases before normalize+lookup

## Tool Consolidation (P2-2 through P2-5, Completed)
- `blueprint.read` unified (section param), `blueprint.add_function` unified (function_type), `blueprint.add_variable` upsert, `behaviortree.add_node` unified (node_kind)
- Old tool names work via redirect aliases in OliveToolRegistry.cpp

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

## MakeStruct/BreakStruct Auto-Reroute (Resolver)
- `ResolveStructOp()`: non-BlueprintType structs auto-reroute to call ops (e.g., `make_struct Rotator` -> `call MakeRotator`)

## blueprint.create_timeline Tool (Completed)
- Handler: `HandleBlueprintCreateTimeline` in OliveBlueprintToolHandlers.cpp (~560 lines)
- Schema: `BlueprintCreateTimeline()` in OliveBlueprintSchemas.cpp; `NumberProp()` helper for float-default params
- Guard: `OliveNodeTypes::Timeline` in OliveNodeFactory.h; NodeCreators map returns nullptr with error directing to create_timeline
- `CacheExternalNode()` on FOliveGraphWriter: generates ID + caches for nodes created outside AddNode
- Registered in `RegisterGraphWriterTools()` with tags `{blueprint, write, graph, timeline}`
- API gotchas from research: `GetTrackPin()` is NOT IMPLEMENTED (use `FindPin(TrackName)`); curve outer MUST be `Blueprint->GeneratedClass` with `RF_Public`; `AddDisplayTrack()` MUST be called per track; set `TimelineName` on node BEFORE `AllocateDefaultPins`; add ALL tracks to template BEFORE `AllocateDefaultPins`
- Pre-pipeline checks: DoesSupportTimelines, duplicate name, reserved pin names, graph must be ubergraph
- If GeneratedClass is null, auto-compiles via FOliveCompileManager before proceeding
- Result includes pin manifest (via BuildPinManifest) + tracks_created array
- Template files updated: interactable_door.json, interactable_gate.json prefer Timeline over Tick

## Autocast / Auto-Conversion Integration (Completed)
- **PinConnector::Connect()** now uses `CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE` response classification instead of broken custom conversion path. UE's `TryCreateConnection()` handles both direct wires AND conversion node insertion automatically.
- **Dead code removed**: `GetConversionOptions()`, `InsertConversionNode()`, `CanAutoConvert()`, `CreateConversionNode()` -- all deleted from both .h and .cpp
- **CanConnect()** now returns true for autocast-compatible pairs (checks `CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE` in addition to `CanSafeConnect()`)
- **GraphWriter::ConnectPins** (`connect_pins` tool) now passes `bAllowConversion=true` (was false)
- **PlanExecutor WireDataConnection**: probe-based conversion detection using `CanCreateConnection()` before `Connect()` -- more robust than post-hoc LinkedTo heuristics for multi-connection scenarios
- **SplitPin fallback**: when `Connector.Connect()` fails and source is a splittable struct (`PC_Struct`, `CanSplitStructPin`), auto-splits via `K2Schema->SplitPin()` and connects appropriate sub-pin
- **ResolveSubPinSuffix()**: static helper in OlivePlanExecutor.cpp anonymous namespace. Resolution: explicit hint suffix > target pin name exact match > target name ends-with > default first component. Known structs: Vector(X/Y/Z), Vector2D(X/Y), Rotator(Roll/Pitch/Yaw), LinearColor(R/G/B/A), etc.
- Sub-pin naming: `{ParentPinName}_{ComponentName}` (e.g., `ReturnValue_X`, `Location_Pitch`)
- ConversionNote for SplitPin: `ConversionNodeType = "SplitPin(X)"` etc.

## Template System (Reference-Only Migration, 2026-03-13)
- `OliveLibraryCloner.h/.cpp` DELETED (~3700 lines); `ApplyTemplate()`, `MergeParameters()`, `SubstituteParameters()`, `EvaluateConditionals()` DELETED from OliveTemplateSystem (~980 lines)
- Tools removed: `blueprint.create_from_library`, `blueprint.create` template_id path, `create_from_template` alias
- Templates are now reference-only: `blueprint.list_templates` (search) + `blueprint.get_template` (read) survive unchanged
- `FOliveTemplateSystem` retains: Initialize, Shutdown, Reload, FindTemplate, GetAllTemplates, GetTemplatesByType, SearchTemplates, GetCatalogBlock, GetTemplateContent
- `FOliveLibraryIndex` fully retained (lazy-load + inverted search index)
- Factory templates still on disk with `${param}` tokens -- read as reference, AI substitutes own values
- Knowledge files updated: cli_blueprint.txt, recipe_routing.txt, Worker_Blueprint.txt -- all "clone directly" / "template_id" language removed

## Class API Helper + Error Recovery (Phases 1-2, error-recovery-design.md)
- `OliveClassAPIHelper.h/cpp` at `Blueprint/Public/Writer/` and `Blueprint/Private/Writer/`
- Static-only class (deleted constructor); no instances, no singleton
- `GetCallableFunctions()`: TFieldIterator<UFunction> + FUNC_BlueprintCallable filter + 22 prefix exclusions
- `GetVisibleProperties()`: TFieldIterator<FProperty> + CPF_BlueprintVisible filter, returns TPair<Name, TypeString>
- `ScoreSimilarity()`: 3-criteria scoring (substring=80/70, prefix=up-to-40, CamelCase-word=up-to-30) -- extracted from OliveNodeFactory fuzzy match
- `BuildScopedSuggestions()`: cross-match detection (property name contains search or vice versa) for "likely fix" output
- `FormatCompactAPISummary()`: "### ClassName\nFunctions: ...\nProperties: ..." markdown block
- Resolver integration: `ResolveCallOp()` tries TargetClass (via FOliveClassResolver::Resolve) then SCS scan (best-score>=30) before catalog fuzzy fallback
- **FOliveNodeFactory::FindClass is PRIVATE** -- use `FOliveClassResolver::Resolve()` from outside the class
- Self-correction: FUNCTION_NOT_FOUND now progressive (attempt 1=check scoped suggestions, 2=property/K2_, 3+=read components)

## C++ Parent Dispatcher Collision Fix (2026-03-13, run-regression-fixes Phase 1+2)
- AddEventDispatcher: parent class `FindPropertyByName` + `CastField<FMulticastDelegateProperty>` check before transaction
- ResolveCallDelegateOp/ResolveBindDelegateOp: `TFieldIterator<FMulticastDelegateProperty>` on `BP->ParentClass` after NewVariables loop
- ResolveCallOp auto-reroute: same TFieldIterator search after NewVariables dispatcher check
- `blueprint.create` response enrichment: `inherited_variables`, `inherited_dispatchers`, `inherited_components` arrays on success

## Agent Pipeline (REMOVED - Single-Agent Revert 2026-03-09, Session Resume 2026-03-16)
- Multi-agent pipeline, auto-continue, continuation prompts all DELETED
- Single-agent flow: first message gets discovery+decomposition, subsequent messages use `--resume`
- Settings kept: `bEnableTemplateDiscoveryPass`, `bEnableLLMKeywordExpansion`
