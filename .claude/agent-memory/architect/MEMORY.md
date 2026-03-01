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
- `plans/phase3-error-recovery-design.md` -- Design for Changes 3.1-3.4
- **Change 3.1 (plan dedup) key decision**: Pass ToolCallArgs INTO `Evaluate()` as new parameter instead of accessing HistoryStore. Avoids cross-module coupling. Hash = CRC32 of tool+asset_path+graph_name+condensed_plan_JSON.
- **Change 3.1 requires**: `FOliveLoopDetector::HashString()` moved from private to public. `FOliveSelfCorrectionPolicy` gains `PreviousPlanHashes` TMap and `BuildPlanHash()` helper.
- **Change 3.2 (progressive disclosure)**: Attempt 1 = code-only header + guidance. Attempt 2 = full header + guidance. Attempt 3+ = full + escalation.
- **Change 3.3 (stale strings)**: `PLAN_RESOLVE_FAILED` now says "set_var on a component" (was "get_var for a component"). `PLAN_VALIDATION_FAILED` now mentions auto-wiring.
- **Change 3.4 (BP context)**: `BuildBlueprintContextBlock()` on FOlivePromptAssembler. Called from GetActiveContext() for bIsBlueprint assets. Adds component/variable lists. ~25-38 tokens per typical BP. Requires `Blueprint.h`, `SimpleConstructionScript.h`, `SCS_Node.h` includes in OlivePromptAssembler.cpp.
- **No FindLatest() needed**: Master plan suggested it but design avoids HistoryStore access entirely.
- **No GetHistoryStore() accessor needed**: SelfCorrectionPolicy gets plan JSON via Evaluate parameter.

### Phase 2 Auto-Wiring (Master Plan v2) - Feb 2026
- `plans/phase2-auto-wiring-design.md` -- Detailed design for Changes 2.1-2.4
- **Phase 1.5 (auto-wire component targets)**: New phase after PhaseCreateNodes. Injects UK2Node_VariableGet + TryCreateConnection for exactly-1-match SCS components. Runs OUTSIDE BatchScope (uses Schema->TryCreateConnection directly, not PinConnector).
- **Phase 5.5 (pre-compile validation + auto-fix)**: New phase between SetDefaults and AutoLayout. Runs INSIDE BatchScope. Recovers orphaned impure nodes via exec_after plan intent. Defense-in-depth for unwired component Targets.
- **New context fields**: AutoFixCount (int32), PreCompileIssues (TArray<FString>), NodeIdToStepId (TMap<FString,FString>), Plan (const FOliveIRBlueprintPlan*), FindStepIdForNode(UEdGraphNode*) method.
- **Master plan discrepancies resolved**: `Context.GetCreatedNode()` -> use existing `GetNodePtr()` + Cast. `Context.GetNodeId(Node)` -> new `FindStepIdForNode()` (reverse lookup via StepToNodePtr iteration). `Context.Plan` -> added as raw pointer (set in Execute, valid during execution only).
- **Validator softening**: CheckComponentFunctionTargets downgrades to warning when exactly 1 SCS component matches. Error when 0 or >1 matches. Needs `SimpleConstructionScript.h` and `SCS_Node.h` includes in OlivePlanValidator.cpp.
- **bIsRequired**: New bool on FOlivePinManifestEntry. True for: exec input on non-event nodes, Self pin on non-static CallFunction. Populated in Build(), serialized as `"required": true` in ToJson(). Needs K2Node.h, K2Node_Event.h, K2Node_CustomEvent.h includes in OlivePinManifest.cpp.
- **Implementation order**: 2.4 (bIsRequired) -> 2.1 (Phase 1.5) -> 2.2 (validator) -> 2.3 (Phase 5.5)
- **No tool handler changes needed**: AutoFixCount and PreCompileIssues flow through existing PlanResult.Warnings array -> ResultData warnings JSON -> tool result.

### Phase 4 Template System - Feb 2026
- `plans/phase4_template_implementation.md` -- 6-task implementation plan
- **FOliveTemplateSystem**: Singleton at `Blueprint/Public/Template/OliveTemplateSystem.h`. Scans `Content/Templates/` recursively for JSON files. Builds auto-generated catalog block. Provides `ApplyTemplate()` for factory templates.
- **File loading pattern**: Uses `IPlatformFile::IterateDirectoryRecursively()`, same as recipe loading in CrossSystemToolHandlers. Plugin dir resolved via `FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("UE_Olive_AI_Studio/Content/Templates"))`.
- **Template tools always registered**: `RegisterTemplateTools()` called unconditionally in `RegisterAllTools()`. Handlers return errors if no templates loaded. Avoids startup ordering issues.
- **Catalog injection**: Dynamic, in `GetCapabilityKnowledge()` -- appends catalog block for Auto/Blueprint profiles. NOT injected during LoadPromptTemplates to avoid startup ordering dependency.
- **Startup position**: `FOliveTemplateSystem::Get().Initialize()` inserted after CrossSystem tools, before FocusProfileManager.
- **ApplyTemplate execution**: All operations (create BP, add vars, add dispatchers, create functions, execute plans) happen inside a single write pipeline executor lambda. Uses `FOliveBlueprintWriter::Get()` directly, NOT tool handler re-entry.
- **No Build.cs changes**: Blueprint sub-module already has recursive include paths that pick up `Blueprint/Public/Template/` and `Blueprint/Private/Template/`.
- **New error codes**: `TEMPLATE_NOT_FOUND`, `TEMPLATE_NOT_FACTORY`, `TEMPLATE_APPLY_CREATE_FAILED`, `TEMPLATE_APPLY_PLAN_FAILED`
- **New files**: `OliveTemplateSystem.h/cpp` (under `Blueprint/Public|Private/Template/`), `Content/Templates/factory/stat_component.json`, `Content/Templates/reference/component_patterns.json`

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

### Auto-Wire Type Fixes - Feb 2026
- `plans/auto-wire-type-fixes-design.md` -- 3 issues: type matching, JSON array coercion, exec chain inference
- **Issue 1 (type matching)**: 3 sites in OlivePlanExecutor.cpp use exact `PinCategory == && PinSubCategoryObject ==` comparisons. Replace with `GetDefault<UEdGraphSchema_K2>()->ArePinTypesCompatible(Output, Input, CallingContext)`. Note: first arg is Output pin type, second is Input pin type. For `FindTypeCompatibleOutput` (manifest-level), add schema-based fallback in `WireDataConnection` using real `UEdGraphPin*` from context nodes.
- **Issue 2 (JSON array coercion)**: `JsonToStringMap` in BlueprintPlanIR.cpp calls `AsString()` on all values -- Array types log error + return "". Fix: use `TryGetString()` first (handles String/Number/Boolean), then explicit Array fallback (take first element + warn). Also add `exec_after` array fallback via `TryGetArrayField`.
- **Issue 3 (exec chain inference)**: Minimal fix: remove empty strings from `HasIncomingExec` set (caused by Issue 2 coercion). Full graph-aware inference deferred unless interleaved step ordering observed in logs.
- **Implementation order**: Issue 2 (root cause) -> Issue 1 (highest impact) -> Issue 3 (depends on 2)
- **No new error codes, no header changes, no new files.**
