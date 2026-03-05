# Architect Agent Memory

## Key Patterns

- **Tool handler pattern**: validate params -> load Blueprint -> build FOliveWriteRequest -> bind executor lambda -> `ExecuteWithOptionalConfirmation`
- **Pre-pipeline validation**: Cheap checks (type validation, duplicate detection) belong BEFORE the write pipeline, not inside the executor lambda. This avoids unnecessary transaction overhead.
- **FOliveWriteResult factory methods**: Use `::ExecutionError()`, `::Success()`, `::ValidationError()`, `::ConfirmationNeeded()` -- do not construct manually.
- **FOliveToolResult vs FOliveWriteResult**: Tool handlers return `FOliveToolResult`; pipeline returns `FOliveWriteResult` which converts via `.ToToolResult()`.
- **Singleton pattern**: All service classes (NodeCatalog, NodeFactory, GraphWriter, WritePipeline, BlueprintReader) are singletons via `static Foo& Get()`.

## Architecture Decisions

### Early Phases (2, 3, Graph-From-Description, Critical Fixes) - Feb 2026
> Older design decisions moved to `autonomous-mode-decisions.md` and prior design docs.
> Key stable facts: SCREAMING_SNAKE_CASE error codes, Pipeline Stage 4 commits transaction, Stage 6 detects compile errors, partial success returns Success() with wiring_errors, HasCompileFailure checks `data.compile_result` (nested).

## Error Code Convention
- Use `SCREAMING_SNAKE_CASE` for error codes
- See design docs for full list. Recent: `GHOST_NODE_PREVENTED`, `INTERFACE_GRAPH_CONFLICT`, `VALIDATION_EMPTY_SCRIPT`, `PYTHON_PLUGIN_NOT_AVAILABLE`, `PYTHON_NOT_AVAILABLE`, `PYTHON_EXECUTION_FAILED`

### Phase 3 Error Recovery (Master Plan v2) - Feb 2026
- `plans/phase3-error-recovery-design.md` -- Changes 3.1-3.4: plan dedup (CRC32 hash via Evaluate param), progressive disclosure (3 tiers), stale strings, BP context injection.

### Phase 2 Auto-Wiring (Master Plan v2) - Feb 2026
- `plans/phase2-auto-wiring-design.md` -- Phase 1.5 (auto-wire component targets), Phase 5.5 (pre-compile validation + auto-fix), bIsRequired on pin manifest, validator softening.

### Phase 4 Template System - Feb 2026
- `plans/phase4_template_implementation.md` -- FOliveTemplateSystem singleton. Scans `Content/Templates/` recursively. Catalog injection in `GetCapabilityKnowledge()`. ApplyTemplate uses `FOliveBlueprintWriter::Get()` directly (no tool re-entry). Error codes: `TEMPLATE_NOT_FOUND`, `TEMPLATE_NOT_FACTORY`, `TEMPLATE_APPLY_CREATE_FAILED`, `TEMPLATE_APPLY_PLAN_FAILED`.

### Priority 0: Universal Node Manipulation - Feb 2026
- `plans/priority0_universal_node.md` -- 5 tasks: validator fix, error classification, capability routing, universal add_node, get_node_pins
- **Universal add_node fallback**: When type is NOT in `NodeCreators` map, `FindK2NodeClass()` searches for UK2Node subclass via: FindFirstObject -> K2Node_/UK2Node_ prefix -> strip U prefix -> StaticLoadClass across 6 engine packages.
- **Properties set BEFORE AllocateDefaultPins**: Many K2Nodes use UPROPERTY values during pin allocation (e.g., K2Node_ComponentBoundEvent reads DelegatePropertyName). Set props first, AllocateDefaultPins second, ReconstructNode as safety net.
- **SetNodePropertiesViaReflection**: Mirrors OliveGraphWriter.cpp lines 352-390 pattern. Type dispatch: Bool/Int/Float/Double/Str/Name/Text/Object, with generic ImportText_Direct fallback for FKey, enums, structs.
- **Property feedback deferred**: OutSetProperties/OutSkippedProperties tracked in CreateNodeByClass but NOT yet threaded to tool result (would require FOliveBlueprintWriteResult API change). Pin manifest provides indirect feedback.
- **EOliveErrorCategory**: New enum {FixableMistake, UnsupportedFeature, Ambiguous}. Only USER_DENIED is Category B. BP_ADD_NODE_FAILED is Category C (retry once, then escalate). Everything else is Category A.
- **Validator string-literal fix**: CheckComponentFunctionTargets now accepts non-@ref strings that match SCS component variable names (case-sensitive exact match).
- **get_node_pins tool**: Read-only wrapper around anonymous-namespace `BuildPinManifest()`. Tagged `{blueprint, read}`. Returns pin manifest + node_class + node_title.
- **node_routing knowledge pack**: New `Content/SystemPrompts/Knowledge/node_routing.txt` added to Auto/Blueprint profiles.
- **New error code**: `NODE_NOT_FOUND` (get_node_pins when node_id not in cache).
- **PlanExecutor unchanged**: Plan resolver maps ops to curated OliveNodeTypes, so universal fallback only activates via direct add_node tool calls.
- **CallParentFunction gap found**: Declared in OliveNodeTypes namespace but never registered in InitializeNodeCreators. Universal fallback would handle it, but a curated creator would be better (follow-up).

### Blueprint Pipeline Reliability Upgrade - Feb 2026
- `plans/blueprint-pipeline-reliability-impl.md` -- 3-phase implementation plan (verified line numbers)
- **Phase 1 (Safety Net)**: Granular fallback after 3 plan failures with same error. `bIsInGranularFallback` + `LastPlanFailureReason` on SelfCorrectionPolicy. LoopDetector.Reset() on switch gives fresh budget. `BuildRollbackAwareMessage()` for rollback + retries remaining, `BuildGranularFallbackMessage()` for forced switch. MaxCorrectionCyclesPerWorker raised to 20 (backstop only). Template compile errors surfaced via `compile_result` JSON structure in ResultData.
- **Phase 2 (Graph Context)**: New `FOliveGraphContext` struct in OliveBlueprintPlanResolver.h. `BuildFromBlueprint()` scans UbergraphPages/FunctionGraphs/MacroGraphs, extracts InputParamNames/OutputParamNames from UK2Node_FunctionEntry/Result UserDefinedPins. `bIsLatent` on FOliveResolvedStep (from UFunction Latent metadata or Delay op). `CheckLatentInFunctionGraph` validator rejects latent in function graphs. `FunctionInput`/`FunctionOutput` virtual OliveNodeTypes map to existing FunctionEntry/FunctionResult nodes in PhaseCreateNodes (added to ReusedStepIds to survive rollback). All Resolve()/Validate() signatures get optional `const FOliveGraphContext&` parameter with default.
- **Phase 3 (Lookup Tables)**: 10 Float->Double aliases in GetAliasMap(). NodeFactory.FindFunction() expanded from 3 to 11 library classes (adding KismetMath, KismetString, KismetArray, GameplayStatics, SceneComponent, PrimitiveComponent, Pawn, Character).
- **New error code**: `LATENT_IN_FUNCTION`
- **Key line numbers verified**: SelfCorrectionPolicy.cpp Evaluate() at line 15, compile branch lines 72-123, tool failure branch lines 126-187, Reset() at line 194. NodeFactory.cpp FindFunction() at line 1063, library classes at lines 1098-1102. FunctionResolver.cpp GetAliasMap() at line 697, last alias at line 930. PlanValidator.cpp Validate() at line 16, checks at lines 40-41. PlanExecutor.cpp PhaseCreateNodes at line 244, event reuse ends at line 365.
- **Handoff plan line numbers are STALE**: References like "line ~73044" are completely wrong. Always verify against actual code.

### Priority 1+2: Component Variable Fixes - Feb 2026
- `plans/priority1_2_component_fixes.md` -- 6 tasks fixing remaining gaps after master plan Phase 1+2
- **KEY FINDING: Most master plan Phase 1 and Phase 2 changes were already implemented.** BlueprintHasVariable already has SCS check, lenient fallback already removed, Phase 1.5/5.5 already exist, bIsRequired already added, validator already softened.
- **Remaining gaps (6 tasks)**: ReadVariables omits SCS components (T1), resolver warn-but-allow for missing vars (T2), dead code in NodeFactory (T3), Phase 1.5 ignores string-literal Target (T4), set_var error message polish (T5), recipe update (T6).
- **T1 key decision**: Set `Var.DefinedIn = TEXT("component")` and `Var.bBlueprintReadWrite = false` on `FOliveIRVariable`. No struct changes needed.
- **FOliveIRVariable struct location**: `Source/OliveAIRuntime/Public/IR/CommonIR.h` line 458. Field is `DefinedIn` (not `Source`). Type info uses `FOliveIRType.ClassName` (not `TypeName`).
- **FOliveIRType struct location**: `Source/OliveAIRuntime/Public/IR/OliveIRTypes.h` line 49. Object types use `Category = EOliveIRTypeCategory::Object` + `ClassName`.
- **All 6 tasks are independent** -- can run fully in parallel with 2-3 coders.

### Resolver Removal (Function Resolution Consolidation) - Feb 2026
- `plans/resolver-removal-and-template-check.md` -- 6 tasks (T1-T6)
- **Core change**: `FOliveFunctionResolver` removed from plan pipeline. `FOliveNodeFactory::FindFunction()` becomes the single validation point for function resolution.
- **Alias map moved**: `GetAliasMap()` (~150 entries) copied from FunctionResolver to NodeFactory as public static method.
- **K2 prefix fallback moved**: Added inline in FindFunction -- each class gets tried with exact name, then K2_ variant, before moving to next class.
- **FindFunction enhanced**: Now searches parent class hierarchy + SCS component classes (previously only resolver did this).
- **"Accepted as-is" path eliminated**: ResolveCallOp now returns `FUNCTION_NOT_FOUND` error when FindFunction fails, instead of silently accepting.
- **ResolvedOwningClass/bIsPure/bIsLatent preserved**: ResolveCallOp still populates these from the UFunction* returned by FindFunction. Downstream consumers (PlanValidator, PlanExecutor Phase 1.5) unchanged.
- **Files kept as dead code**: OliveFunctionResolver.h/.cpp not deleted for one release cycle (rollback safety).
- **Template check rule**: Added to AGENTS.md and sandbox CLAUDE.md -- "call list_templates before modifying existing Blueprints."

### Autonomous Mode Decisions - Feb 2026
> See `autonomous-mode-decisions.md` for full details on NeoStack migration, timeout fixes, and efficiency rounds 2-3.
- **NeoStack**: `bUseAutonomousMCPMode` flag, `SendMessageAutonomous()`, MCP tool discovery, no orchestration loop.
- **Timeout**: Activity-based idle timeout (`AutonomousIdleToolSeconds=120`), hard limit 900s. Partial success rollback fix.
- **Round 2**: `call_delegate` op, ExpandedPlan propagation bug fix, mandatory recipe lookup.
- **Round 3**: Anti-stall interleave guidance, "continue" context injection (`FAutonomousRunContext`), template result enrichment (`function_details`). `FOnMCPToolCalled` delegate extended to 3 params (adds Arguments JSON).

## File Structure
- Tool handlers: `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` (very large file, 5000+ lines)
- Schemas: `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintSchemas.cpp`
- Pipeline: `Source/OliveAIEditor/Blueprint/Private/Pipeline/OliveWritePipeline.cpp`
- Node catalog: `Source/OliveAIEditor/Blueprint/Private/Catalog/OliveNodeCatalog.cpp`
- IR structs: `Source/OliveAIRuntime/Public/IR/CommonIR.h` (FOliveIRGraph, FOliveIRNode, FOliveIRMessage, etc.)
- Plan executor: `Source/OliveAIEditor/Blueprint/Public/Plan/OlivePlanExecutor.h`
- Pin manifest: `Source/OliveAIEditor/Blueprint/Public/Plan/OlivePinManifest.h`
- Function resolver: `Source/OliveAIEditor/Blueprint/Public/Plan/OliveFunctionResolver.h`
- Layout engine: `Source/OliveAIEditor/Blueprint/Public/Plan/OliveGraphLayoutEngine.h`
- Class resolver: `Source/OliveAIEditor/Blueprint/Public/OliveClassResolver.h`
- Template system: `Source/OliveAIEditor/Blueprint/Public/Template/OliveTemplateSystem.h`

### Autonomy & Improvisation - Feb 2026
- `plans/autonomy-implementation.md` -- 14 tasks across 6 priorities
- **Priority 1 (Prompts)**: Rewrite 5 content files. Remove prescriptive language, present 3 approaches (plan_json/granular/Python) as equals. No C++ changes.
- **Priority 2 (editor.run_python)**: New `Python` sub-module under `Source/OliveAIEditor/Python/`. Single tool `editor.run_python`. Uses `IPythonScriptPlugin::ExecPythonCommandEx(FPythonCommandEx&)`.
- **Python API (verified UE 5.5)**: `IPythonScriptPlugin::Get()` returns nullptr if module not loaded. `IsPythonAvailable()` is instance method. `FPythonLogOutputEntry::Type` is `EPythonLogOutputType` (not SeverityType). Default `ExecutionMode` is `ExecuteFile` (handles multi-line). NO `IsAvailable()` static method.
- **Python safety**: Snapshot before every execution, persistent log at `Saved/OliveAI/PythonScripts.log`, try/except wrapper. No size/complexity limits.
- **PythonScriptPlugin**: Hard dependency in Build.cs (module ships with UE 5.5, always available for linking). Runtime check via `Get() != nullptr`.
- **Priority 3 (Interface fix)**: ~5-line fix in `FindFunction` Step 1. When specified class is UInterface or BlueprintType==BPTYPE_Interface, report `InterfaceSearch` instead of `ExactName`. Bypass `ReportMatch` lambda (set OutMatchMethod directly like Step 6 does).
- **Priority 4 (Templates)**: Rewrite `pickup_interaction.json` to descriptive style. No C++ changes.
- **Priority 5**: Already covered by Priority 1 prompt rewrites.
- **Priority 6 (Granular reliability)**: Enhance add_node error messages with Python fallback suggestion. Minimal changes.
- **New error codes**: `VALIDATION_EMPTY_SCRIPT`, `PYTHON_PLUGIN_NOT_AVAILABLE`, `PYTHON_NOT_AVAILABLE`, `PYTHON_EXECUTION_FAILED`
- **New files**: `Python/Public/MCP/OlivePythonToolHandlers.h`, `Python/Private/MCP/OlivePythonToolHandlers.cpp`, `Python/Public/MCP/OlivePythonSchemas.h`, `Python/Private/MCP/OlivePythonSchemas.cpp`

### AI Freedom Design - Feb 2026
- `plans/ai-freedom-design.md` -- 3-phase design: Unblock (5 false negatives), Simplify (111->76 tools), Discover (enhanced reads + describe_node_type)
- **Phase 1 (5 false negatives)**: FN-1 OverrideFunction interface search, FN-2 FindInterfaceClass asset registry fallback, FN-3 EventTick alias additions, FN-4 FindFunction interface search + UK2Node_Message, FN-5 FunctionGraphs scan for uncompiled user functions
- **Validation audit**: KEEP 4 (PIE, CppCompile, PathSafety, RateLimit), RELAX 4 (DuplicateLayer->warning, BTNodeExists class->warning, BPNaming short->remove, BPAssetType read->warning), REMOVE 3 (AssetExists, CppOnlyMode, BPNodeIdFormat)
- **Tool pack system removed**: FOliveToolPackManager deprecated. Focus profiles handle domain filtering. Claude Code MCP Tool Search handles token concerns.
- **Phase 2 tool consolidation**: 7 BP read tools -> 1 with `section` param. 4 function creation tools -> 1 with `function_type` param. 4 BT add tools -> 1 with `node_kind` param. Tool alias map in FOliveToolRegistry for backward compat (one release cycle).
- **Phase 2 removals**: batch_write (plan-JSON handles), create_ai_character, move_to_cpp, index_build, index_status, get_config, get_dependencies (in get_asset_info), get_referencers (in get_asset_info)
- **Recipe mandate removed**: olive.get_recipe becomes advisory, not mandatory. 12-step cap removed (real limit is PlanJsonMaxSteps=128).
- **Phase 3 discovery**: FOliveIRGraphSummary (name + node_count) per function. CompileErrors array on FOliveIRBlueprint. New blueprint.describe_node_type tool. FOliveFunctionSearchResult struct replacing bare UFunction* return.
- **New error code**: `INTERFACE_FUNCTION_FOUND` (info)
- **New enum value**: `EMatchMethod::InterfaceSearch`

### Log Analysis Fixes - Feb 2026
- `plans/log-analysis-fixes.md` -- 7 tasks (T1-T7) for 5 issues from real autonomous run log
- **Issue 1 (Ghost K2Node_CallFunction)**: CreateNodeByClass gains FMemberReference special-case for UK2Node_CallFunction. Extracts function_name/function_class from Properties, calls `SetFromFunction(UFunction*)` BEFORE AllocateDefaultPins. Zero-pin guard removes ghost node and fails with `GHOST_NODE_PREVENTED`. `CreateNodeByClass` gets new `UBlueprint* Blueprint = nullptr` param.
- **Issue 2 (Compile Error Masking)**: `FOliveCompileManager::Compile()` now checks `Blueprint->Status == BS_Error` as authoritative fallback when no per-node errors found. Also captures compiler messages via `FCompilerResultsLog*` overload of `CompileBlueprint`. Dedup prevents double-reporting.
- **Issue 3 (Interface Graph Conflict)**: `FOliveBlueprintWriter::AddInterface()` pre-flights function name collision check before calling `ImplementNewInterface`. Scans interface's FUNC_BlueprintEvent functions against existing FunctionGraphs/UbergraphPages. Error code `INTERFACE_GRAPH_CONFLICT`.
- **Issue 4 (Cast-Aware Resolution)**: `ResolveCallOp` fallback searches cast target class when primary resolution fails. Pre-builds `CastTargetMap` in `Resolve()` (step_id -> cast target class). Passed to `ResolveStep`/`ResolveCallOp`. New `ResolveStep`/`ResolveCallOp` param: `const TMap<FString,FString>& CastTargetMap`.
- **Issue 5 (@entry Alias)**: `ExpandMissingComponentTargets` handles `@entry.ParamName` and `@GraphName.ParamName` in function graphs. Synthesizes `_synth_param_` step (same as bare `@ParamName` path). Case-insensitive "entry" alias check.
- **New error codes**: `GHOST_NODE_PREVENTED`, `INTERFACE_GRAPH_CONFLICT`
- **Key insight**: `SetNodePropertiesViaReflection` cannot handle nested struct fields like `FMemberReference` -- only direct UProperties on the node class. Special-casing is required for K2Node types that store critical config in nested structs.

### Blueprint Interface Tools - Mar 2026
- `plans/blueprint-interface-tools-design.md` -- 5 tasks: create_interface tool, VariableGet FMemberReference fix, DoesImplementInterface alias, recipe, template update
- **`blueprint.create_interface` tool**: New writer method `CreateBlueprintInterface()` on FOliveBlueprintWriter. Uses `FKismetEditorUtilities::CreateBlueprint(UInterface::StaticClass(), Package, Name, BPTYPE_Interface, ...)`. Function signatures added via `CreateNewGraph` + `AddFunctionGraph<UClass>` + `UserDefinedPins` on entry/result nodes.
- **Confirmation tier**: Tier 1 (auto-execute) via `OperationCategory = "create"`. Same as blueprint.create.
- **VariableGet fix**: `CreateNodeByClass` gains `UK2Node_Variable` FMemberReference special-case (mirrors existing UK2Node_CallFunction block). Calls `VariableReference.SetSelfMember()` BEFORE AllocateDefaultPins. No zero-pin guard (unlike CallFunction) because variable nodes resolve at compile time.
- **DoesImplementInterface aliases**: `ImplementsInterface`, `HasInterface`, `CheckInterface` -> `DoesImplementInterface` in GetAliasMap().
- **New error code**: `BPI_CREATE_FAILED`
- **New recipe**: `Content/SystemPrompts/Knowledge/recipes/blueprint/interface_pattern.txt`
- **Tags for registration**: `{blueprint, write, create, interface}`, family `blueprint`

### Phase 5.5 Warning Escalation + Events Knowledge + Community Browse - Mar 2026
- `plans/phase5-5-knowledge-browse-design.md` -- 3 features, 8 tasks
- **Feature 1 (Warning Escalation)**: `INTERFACE_FUNCTION_HINT` strings split from `warnings` into `design_warnings` top-level array + `has_design_warnings` boolean. NOT a new result status; plan genuinely succeeded. Self-correction policy NOT triggered (no retry, just informational). Two locations in BlueprintToolHandlers.cpp: executor lambda where PlanResult.Warnings -> ResultData, and pipeline forwarding section.
- **Feature 2 (Events vs Functions Knowledge)**: New `Content/SystemPrompts/Knowledge/events_vs_functions.txt`. Pack ID `events_vs_functions` added to Auto/Blueprint in `ProfileCapabilityPackIds` (OlivePromptAssembler.cpp line 544). ~400 tokens. TAGS: event function interface async timeline delay latent synchronous return output implementable.
- **Feature 3 (Community Browse)**: `mode` param ("browse"/"detail") + `ids` param (array of slugs) on `olive.search_community_blueprints`. Browse: SUBSTR(compact,1,150) as description, slug as id, no functions/variables/components. IDs fetch: direct SQL IN clause, no FTS. Browse max_results cap 20, detail cap 10. Default mode "detail" for backward compat.
- **Key design decision**: No `SUCCESS_WITH_WARNINGS` status. `has_design_warnings: true` at top level is the visibility forcing function. LLMs notice top-level booleans more reliably than array contents.
- **All 3 features independent** -- can implement in parallel.

### Auto-Wire Type Fixes - Feb 2026
- `plans/auto-wire-type-fixes-design.md` -- 3 issues: type matching, JSON array coercion, exec chain inference
- **Issue 1 (type matching)**: 3 sites in OlivePlanExecutor.cpp use exact `PinCategory == && PinSubCategoryObject ==` comparisons. Replace with `GetDefault<UEdGraphSchema_K2>()->ArePinTypesCompatible(Output, Input, CallingContext)`. Note: first arg is Output pin type, second is Input pin type. For `FindTypeCompatibleOutput` (manifest-level), add schema-based fallback in `WireDataConnection` using real `UEdGraphPin*` from context nodes.
- **Issue 2 (JSON array coercion)**: `JsonToStringMap` in BlueprintPlanIR.cpp calls `AsString()` on all values -- Array types log error + return "". Fix: use `TryGetString()` first (handles String/Number/Boolean), then explicit Array fallback (take first element + warn). Also add `exec_after` array fallback via `TryGetArrayField`.
- **Issue 3 (exec chain inference)**: Minimal fix: remove empty strings from `HasIncomingExec` set (caused by Issue 2 coercion). Full graph-aware inference deferred unless interleaved step ordering observed in logs.
- **Implementation order**: Issue 2 (root cause) -> Issue 1 (highest impact) -> Issue 3 (depends on 2)
- **No new error codes, no header changes, no new files.**

### Templates & Recipe: Interactables + Events/Functions - Mar 2026
- `plans/templates-recipe-design.md` -- 5 tasks (all content except 1 line of C++)
- **Events & Functions recipe**: `Content/SystemPrompts/Knowledge/recipes/blueprint/events_and_functions.txt`. Covers event-vs-function decision, interface output trap, hybrid pattern, Tick+FInterpTo for smooth movement. Registered in `_manifest.json`.
- **Factory templates**: `interactable_door.json` (pivot rotation via RInterpTo) and `interactable_gate.json` (slider translation via VInterpTo). Both use Tick-driven interpolation, no Timeline nodes. No graph logic in templates (AI writes ToggleDoor/ToggleGate + Tick via plan_json).
- **Reference template**: `interactable_patterns.json` (5 patterns, 46 lines). EventBasedInterfaces, TickDrivenInterpolation, StateTogglePattern, PivotAndSliderComponents, InterfaceIntegration.
- **list_templates nudge**: One `SetStringField("note", ...)` line in HandleBlueprintListTemplates after count field. Soft guidance, not mandatory.
- **No C++ structural changes**: All templates and recipes loaded by existing scanning systems. Only C++ change is the one-line nudge.
- **Design principle**: Suggestive, not mandatory. AI freedom to improvise. Templates create structure, AI writes logic.

### Timeline Tool - Mar 2026
- `plans/timeline-tool-design.md` -- Single tool `blueprint.create_timeline`
- No plan_json `timeline` op. CacheExternalNode() on GraphWriter. NodeTypes guard (nullptr creator).
- GetTrackPin() LINKER ERROR -- use FindPin(TrackName). Curve outer: GeneratedClass with RF_Public.

### Autocast / Auto-Conversion Integration - Mar 2026
- `plans/autocast-design.md` -- 8 tasks. Critical: `CanSafeConnect()` excluded autocast. Fix: `TryCreateConnection()` directly. SplitPin fallback for struct->scalar.

### Interface Event Resolution - Mar 2026
- `plans/interface-event-design.md` -- 4 changes, 3 files, no new files/error codes
- **Gap**: ResolveEventOp + CreateEventNode never search ImplementedInterfaces. No-output interface functions should be UK2Node_Event in EventGraph.
- **New field**: `bIsInterfaceEvent` on FOliveResolvedStep. Resolver tags with `interface_class` path in Properties.
- **Resolver**: Insert after SCS scan (line 2128), before pass-through. `FunctionCanBePlacedAsEvent()` gate.
- **Factory**: Two paths: fast (resolver-tagged) + slow (direct search). `SetFromField<UFunction>(Func, false)` + `bOverrideFunction = true`.
- **Executor**: `FindExistingEventNode` extended to search `ImplementedInterfaces` after parent class.
- **BPI**: Use `SkeletonGeneratedClass` via `ClassGeneratedBy`. Native C++ interfaces use `InterfaceDesc.Interface` directly.

### Enriched Wiring Error Messages - Mar 2026
- `plans/error-messages-design.md` -- 6 tasks: diagnostic struct, BWR field, suggestion engine, PlanExecutor/connect_pins/self-correction integration
- **FOliveWiringDiagnostic** in new `OliveWiringDiagnostic.h` (avoids circular dep). `EOliveWiringFailureReason` enum + `TArray<FOliveWiringAlternative>` ordered suggestions.
- **New error codes**: `DATA_WIRE_INCOMPATIBLE` (PlanExecutor), `BP_CONNECT_PINS_INCOMPATIBLE` (connect_pins tool).
- **`TOptional<FOliveWiringDiagnostic>`** added to `FOliveBlueprintWriteResult`. Non-breaking.
- **Fires only on CONNECT_RESPONSE_DISALLOW** after autocast + SplitPin both fail.
- **Depends on autocast integration** being complete first.

### Agent Planning System (Multi-Asset Decomposition) - Mar 2026
- `plans/agent-planning-system-design.md` -- 3 changes, 3 files
- **Root cause**: Agent skips asset decomposition because stdin sends it straight to template research. Template anchoring (projectile factory has "Arrow" preset) creates gravity well that frames multi-entity tasks as single-asset.
- **Fix**: Structured decomposition directive in stdin (imperative channel) with ASSETS: format and two worked examples (bow+arrow, door+key). Research nudge repositioned AFTER decomposition.
- **3 files changed**: OliveCLIProviderBase.cpp (stdin injection, lines 509-515), blueprint_design_patterns.txt (new Section 0), cli_blueprint.txt (expand MULTI-ASSET line)
- **Key insight**: stdin directives > CLAUDE.md suggestions. Structured format prompts > advisory text. "List your assets" works; "plan ALL assets" doesn't.
- **No C++ structural changes**: Only string content in the stdin message builder. No new classes, no pipeline changes, no tool changes.
- **Escalation path**: If agent still skips decomposition, next step is two-phase CLI (first turn = decomposition only with --max-turns 1).

### Log Improvements - Mar 2026
- `plans/log-improvements-design.md` -- 6 tasks (T1-T6), 4 items
- **modify_component fix**: `modified_properties_count` now reports `Properties.Num() - WriteResult.Warnings.Num()` (actual successes). Adds `requested_properties_count` and `failed_properties_count`. Guard: `FMath::Max(0, ...)` for edge case.
- **Orphan delta tracking**: `OrphanBaselines` TMap + `bRunActive` flag on FOliveWritePipeline. Lazy baseline (first check per graph captures count). Delta reported on subsequent checks. Full count when no active run. ConversationManager sets `bRunActive = true` at `BeginRun()` (2 sites). Baselines cleared on next run start.
- **interaction_caller.json**: Reference template (6 patterns). Covers overlap detection, input discovery, EIA vs InputKey, validity check, full caller architecture. Critical: states tool split (editor.run_python for IA/IMC assets, plan_json for event wiring).
- **input_handling recipe**: Decision tree for EIA vs InputKey. Tags for manifest registration.
