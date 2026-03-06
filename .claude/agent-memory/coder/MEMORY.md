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
- **FString::Printf requires literal format string**: In UE 5.5, `FString::Printf` uses a template with `static_assert` requiring the format string be a `TCHAR` literal (not a variable). Use two separate literal strings with a ternary selector instead of `Printf` with `%s` for SQL variants.

## Completed Phases (see plans/ for details)
- Phase 3: Error Recovery, Phase 4: Template System, Pipeline Reliability, Large-Graph Paging, Tool Resilience (T0)

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

## Interface Event Support (UK2Node_Event with interface EventReference)
- `FOliveResolvedStep.bIsInterfaceEvent` flag in OliveBlueprintPlanResolver.h, parallel to `bIsInterfaceCall`
- Resolver: `ResolveEventOp` scans `BP->ImplementedInterfaces` after SCS delegate check, before pass-through
- Uses `UEdGraphSchema_K2::FunctionCanBePlacedAsEvent()` to filter: only no-output interface functions match
- For Blueprint Interfaces, functions live on `SkeletonGeneratedClass` (check `ClassGeneratedBy` for BPI vs native)
- Stores `interface_class` (GetPathName) in Properties for factory fast-path
- Factory: `CreateEventNode` has two paths: fast (resolver-tagged `interface_class` in Properties) and slow (direct `ImplementedInterfaces` scan for `blueprint.add_node` calls)
- Node creation: `SetFromField<UFunction>(InterfaceFunc, false)` + `bOverrideFunction = true`
- Duplicate detection: `FBlueprintEditorUtils::FindOverrideForFunction(Blueprint, InterfaceClass, EventName)`
- Executor: `FindExistingEventNode` now also iterates `ImplementedInterfaces` after parent class check
- Priority order: native events > SCS delegates > interface events > Enhanced Input Actions > error
- Plan syntax: `{"op":"event","target":"Interact"}` (same as native events, auto-detected)

## Enhanced Input Action Support (K2Node_EnhancedInputAction)
- `OliveNodeTypes::EnhancedInputAction = "EnhancedInputAction"` added to OliveNodeFactory.h
- `CreateEnhancedInputActionNode()` on FOliveNodeFactory: searches AssetRegistry for UInputAction by name, creates UK2Node_EnhancedInputAction
- Property: `TObjectPtr<const UInputAction> InputAction` (set BEFORE AllocateDefaultPins)
- Headers: `K2Node_EnhancedInputAction.h` (from InputBlueprintNodes module), `InputAction.h` (from EnhancedInput module)
- Build.cs deps: `EnhancedInput`, `InputBlueprintNodes`; uplugin dep: `EnhancedInput` plugin
- Four-path resolution in CreateEventNode: (1) native function override, (1b) interface event, (2) component delegate, (3) Enhanced Input Action (IA_ prefix)
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

## Library Clone Tool (Phase 1+2: Structure + Graph Cloning, Completed)
- `OliveLibraryCloner.h` at `Blueprint/Public/Template/`; `OliveLibraryCloner.cpp` at `Blueprint/Private/Template/`
- NOT a singleton; instantiate fresh per clone operation (like FOlivePlanExecutor)
- Log category: `LogOliveLibraryCloner`
- `FLibraryCloneResult::ToJson()` for structured reporting
- Resolution pipeline: `ApplyRemap()` (strips _C, case-insensitive) -> `FOliveClassResolver::Resolve()`
- `ResolveStruct()`: searches 9 engine module prefixes with string concatenation (NOT FString::Printf due to UE 5.5 literal requirement)
- `ResolveParentClass()`: root native ancestor strategy -- walks depends_on chain, deepest resolvable wins
- Type demotion: object->UObject*, struct->String, enum->Byte, array element types also demoted
- Variables with `defined_in: "component"` are skipped (created by AddComponent)
- Components use `components.tree` (recursive hierarchical JSON), NOT flat array
- Library template functions live in `graphs.functions[]` (not top-level `functions`)
- Function signatures extracted from `inputs`/`outputs` on the function graph JSON (not from FunctionEntry/FunctionResult nodes)
- Intermediate compile via `FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave)` -- CRITICAL for FindFunction to resolve sibling calls
- **Phase 2 Graph Cloning**: 6-phase pipeline per graph: ClassifyNodes -> CreateNodes -> WireExecConnections -> WireDataConnections -> SetPinDefaults -> AutoLayout
- `NodeMap`/`NodeIdMap` cleared per-graph (library node IDs like "node_0" repeat across graphs)
- `FCloneGraphResult.ExecGapsBridged`/`ExecGapsUnbridgeable` for exec gap repair reporting
- Classification: FunctionEntry/FunctionResult/Knot/Reroute/ControlRig always Skip; Branch/Sequence/Event/CustomEvent/Comment always Create; CallFunction resolves via FindFunction; VariableGet/Set checks `VariableExistsOnBlueprint()`; Cast resolves target class; Timeline skipped
- Exec gap repair: for skipped nodes with exactly 1 exec-in + 1 exec-out, bridge across; multiple outs = unbridgeable
- `FindPinByName()`: exact match -> case-insensitive -> space-stripped fallback
- `IsAssetReference()`: checks /Game/, /Script/ prefixes and UE quote+path format
- `TemplateClassName` field stores source template's `DisplayName` for self-call detection in CallFunction classification
- `VariableExistsOnBlueprint()` checks NewVariables + FlattenedVariableNames + SCS components

## Enriched Wiring Error Messages (Completed)
- `OliveWiringDiagnostic.h` at `Blueprint/Public/Writer/` -- standalone header to avoid circular deps
- `EOliveWiringFailureReason` enum: TypesIncompatible, StructToScalar, ScalarToStruct, ObjectCastRequired, ContainerMismatch, DirectionMismatch, SameNode, AlreadyConnected, Unknown
- `FOliveWiringAlternative` struct: Label, Action, Confidence (high/medium/low)
- `FOliveWiringDiagnostic`: Reason, SourceTypeName, TargetTypeName, pin names, SchemaMessage, WhyAutoFixFailed, Alternatives array, ToJson(), ToHumanReadable(), ReasonToString()
- `FOliveBlueprintWriteResult.WiringDiagnostic` field: `TOptional<FOliveWiringDiagnostic>` (not a UPROPERTY)
- `FOlivePinConnector::BuildWiringDiagnostic()` + `SuggestAlternatives()` private methods
- `FOliveSmartWireResult.bIsTypeIncompatible` flag for error code selection in PhaseWireData
- New error codes: `DATA_WIRE_INCOMPATIBLE` (plan_json), `BP_CONNECT_PINS_INCOMPATIBLE` (connect_pins tool)
- Self-correction guidance for both new error codes in OliveSelfCorrectionPolicy.cpp
- Both new error codes are Category A (FixableMistake) by default fallthrough
