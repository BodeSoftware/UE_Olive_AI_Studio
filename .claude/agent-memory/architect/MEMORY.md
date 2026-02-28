# Architect Agent Memory

## Key Patterns

- **Tool handler pattern**: validate params -> load Blueprint -> build FOliveWriteRequest -> bind executor lambda -> `ExecuteWithOptionalConfirmation`
- **Pre-pipeline validation**: Cheap checks (type validation, duplicate detection) belong BEFORE the write pipeline, not inside the executor lambda. This avoids unnecessary transaction overhead.
- **FOliveWriteResult factory methods**: Use `::ExecutionError()`, `::Success()`, `::ValidationError()`, `::ConfirmationNeeded()` -- do not construct manually.
- **FOliveToolResult vs FOliveWriteResult**: Tool handlers return `FOliveToolResult`; pipeline returns `FOliveWriteResult` which converts via `.ToToolResult()`.
- **Singleton pattern**: All service classes (NodeCatalog, NodeFactory, GraphWriter, WritePipeline, BlueprintReader) are singletons via `static Foo& Get()`.

## Architecture Decisions

### Phase 2 (Graph Edit Integrity) - Feb 2026
- Node type validation added to NodeFactory as `ValidateNodeType()` method, called before creation attempt.
- Fuzzy suggestions use `FOliveNodeCatalog::FuzzyMatch()` returning simple struct (not USTRUCT) with TypeId/DisplayName/Score.
- Duplicate native event check happens in HandleBlueprintAddNode before pipeline entry, with defense-in-depth in CreateEventNode.
- Node removal broken-link capture done via new `CaptureNodeConnections()` on GraphWriter, called before `RemoveNode()`.
- Orphaned exec flow detection added to `VerifyBlueprintStructure` in Stage 5, scoped to the affected graph only.
- Large graph threshold: 500 nodes. Page size: 100 (max 200). Full NodeIdMap built even for paged reads to preserve cross-page connection references.
- `FOliveIRMessage` needs `TSharedPtr<FJsonObject> Context` field for structured context on warnings (additive, non-breaking IR change).

## Error Code Convention
- Use `SCREAMING_SNAKE_CASE` for error codes
- Existing: `VALIDATION_MISSING_PARAM`, `ASSET_NOT_FOUND`, `BP_ADD_NODE_FAILED`, `BP_REMOVE_NODE_FAILED`
- Phase 2 added: `NODE_TYPE_UNKNOWN`, `DUPLICATE_NATIVE_EVENT`, `ORPHANED_EXEC_FLOW` (warning)

### Phase 3 (Chat UX Resilience) - Feb 2026
- ConversationManager ownership moves from SOliveAIChatPanel to FOliveEditorChatSession singleton.
- Panel holds weak ref, rebinds delegates on open/close. Closing panel does NOT cancel operations.
- FOliveMessageQueue: FIFO queue (max 5) for user messages during processing. Drains one at a time on completion.
- FOliveProviderRetryManager: wraps provider with exponential backoff (1s/2s/4s, max 3 retries). Rate-limit Retry-After honored up to 120s.
- Provider error classification uses parseable prefix format `[HTTP:{code}:RetryAfter={s}]` to avoid breaking IOliveAIProvider interface.
- FOlivePromptDistiller::Distill() return type changed from void to FOliveDistillationResult for truncation metadata.
- Truncation note injected into model-visible context when distillation summarizes messages.
- Response truncation detected via FinishReason=="length", warning appended to message.
- Focus profile switch deferred when processing; applied on completion.
- FNotificationInfo toast for background completion when panel is closed.
- Design doc: `plans/phase3-chat-ux-resilience-design.md`

### Graph-From-Description (Post-Creation Pin Introspection) - Feb 2026
- **Core insight**: After CreateNode + AllocateDefaultPins, the UEdGraphNode* has all real pin data. Introspect AFTER creation, never guess beforehand.
- **Schema version gating**: `schema_version: "2.0"` routes to new `FOlivePlanExecutor`; `"1.0"` routes to existing `FOliveBlueprintPlanLowerer`. Both share enhanced `FOliveFunctionResolver`.
- **Multi-phase execution**: Phase 1 (create nodes + build manifests) -> Phase 3 (exec wiring via manifest) -> Phase 4 (data wiring via manifest) -> Phase 5 (defaults via manifest) -> Phase 6 (auto-layout)
- **Pin resolution fallback chain**: exact name -> display name -> case-insensitive -> fuzzy (Levenshtein + substring) -> type-based. Implemented in `FOlivePinManifest::FindPinSmart()`.
- **New @ref syntax** (additive, backward compat): `@step.auto` (type match), `@step.~fuzzy` (fuzzy match). Standard `@step.pinName` now uses smart fallback chain.
- **Smart function resolution**: `FOliveFunctionResolver` with chain: exact -> K2_ prefix -> alias map -> catalog -> broad library search. Replaces the hardcoded 3-class `FindFunction` in NodeFactory.
- **Continue-on-failure**: Node creation is fail-fast, but wiring phases (exec, data, defaults) continue on failure and accumulate errors. Result includes `pin_manifests` for self-correction.
- **Event reuse**: If plan includes an event that already exists, reuse the existing node instead of failing.
- **Auto-layout**: `FOliveGraphLayoutEngine` uses BFS from exec roots, column/row assignment, branch offsets, pure node placement.
- **Plan path enforcement**: Soft warning in HandleBlueprintAddNode after 3+ granular ops per turn, configurable via `PlanPathWarningThreshold` setting.
- **New error codes**: `FUNCTION_NOT_FOUND`, `FUNCTION_AMBIGUOUS`, `NODE_CREATION_FAILED`, `EXEC_PIN_NOT_FOUND`, `DATA_PIN_NOT_FOUND`, `DATA_PIN_AMBIGUOUS`, `DEFAULT_PIN_NOT_FOUND`, `EVENT_ALREADY_EXISTS`, `PURE_NODE_EXEC_SKIP`
- **Design doc**: `plans/graph-from-description-design.md`
- **New files**: `OlivePlanExecutor.h/cpp`, `OlivePinManifest.h/cpp`, `OliveFunctionResolver.h/cpp`, `OliveGraphLayoutEngine.h/cpp` (all under `Blueprint/Public|Private/Plan/`)
- **NOT a singleton**: `FOlivePlanExecutor` is instantiated per execution, not a singleton. It holds no persistent state.

### Critical Fix Plan (Rollback/Resolver/Loop) - Feb 2026
- `plans/critical_fix_implementation.md` -- 6 tasks (T1-T6) for 3 cascading fixes
- **Fix 1 (Plan Rollback)**: Post-pipeline cleanup in tool handler. After `ExecuteWithOptionalConfirmation`, if compile failed AND step_to_node_map exists, call `RollbackPlanNodes()` to remove created nodes. Uses separate FScopedTransaction. Reused event nodes (tracked via `ReusedStepIds`) are NOT removed. `Writer.ClearNodeCache()` after removals (RemoveFromCache is private).
- **Fix 1 key insight**: Pipeline transaction commits in Stage 4, compile errors detected in Stage 6 set bSuccess=false but transaction already committed. Rollback MUST happen post-pipeline, not inside the executor.
- **Replace mode**: `Mode` parsed at line 6431 but NEVER captured into v2.0 lambda. T2 adds capture + pre-cleanup (remove non-event/non-entry nodes before executing plan).
- **Fix 2 (Component-Aware Resolver)**: Insert SCS component class scan between parent hierarchy and common libraries in `GetSearchOrder`. New `ComponentClassSearch` enum value on `FOliveFunctionMatch::EMatchMethod`. BroadSearch gets `UBlueprint*` param for relevance scoring: component-on-BP=90, library=70, unrelated gameplay=40. Threshold=60 rejects low-confidence broad matches.
- **Fix 3 (Stale Loop Detection)**: Add `ResolvedFunctionNames`/`ResolvedClassNames` to context, flow through result to ResultData. SelfCorrectionPolicy cross-references compile error text against plan names. If no compile error mentions any plan class/function, classify as stale -- skip loop detector, inject "STALE COMPILE ERROR" guidance recommending mode:"replace".
- **Implementation lanes**: Lane A: T1->T2->T5->T6. Lane B: T3->T4. ~8.5 hours total.

### CLI Provider Universal Fixes - Feb 2026
- `plans/cli-provider-implementation-plan.md` -- Detailed implementation plan for resolver/executor/CLI fixes
- **R1 (event name mapping) already implemented**: EventNameMap exists in ResolveEventOp at PlanResolver.cpp lines 602-613.
- **R7 (INVALID_EXEC_REF guidance) already implemented**: SelfCorrectionPolicy.cpp lines 296-302.
- **R5 (event auto-chain)**: Extends existing function entry auto-chain (PlanExecutor.cpp lines 482-639) to also auto-chain from event/custom_event nodes. Key: hoist TargetedStepIds to wider scope, iterate events in plan order, wire to next impure orphan.
- **R6 (partial success = error)**: When `PlanResult.bPartial==true`, return `ExecutionError("PLAN_PARTIAL_SUCCESS")` instead of `Success()`. This makes SelfCorrectionPolicy detect it via `HasToolFailure`. The `status:"partial_success"` in ResultData distinguishes from total failure. Pipeline still commits transaction (nodes persist for repair).
- **R3 (auto-conversion)**: Change `Connector.Connect(src, tgt, false)` to `true` in PhaseWireData (line 1066). Add FOliveConversionNote for transparency logging.
- **R2 (SpawnActor expansion)**: New `ExpandPlanInputs()` on PlanResolver. Detects Location/Rotation inputs on spawn_actor, synthesizes MakeTransform step with `_synth_` prefix ID. Guard: skip if SpawnTransform already present.
- **R4 (alias gaps)**: Add ~15 aliases: MakeTransform, BreakTransform, MakeVector, BreakVector, MakeRotator, BreakRotator, etc.
- **FOliveResolverNote**: New struct for resolver transparency. Fields: Field, OriginalValue, ResolvedValue, Reason. Added to FOliveResolvedStep and FOlivePlanResolveResult.
- **CLIBase extraction**: FOliveCLIProviderBase with virtual hooks GetExecutablePath(), GetCLIArguments(), ParseOutputLine(). FOliveClaudeCodeProvider shrinks to ~100 lines.
- **New error code**: `PLAN_PARTIAL_SUCCESS` -- all nodes created but some wiring failed.

### Resilience + Prompt Slimming - Feb 2026
- `plans/resilience-implementation-tasks.md` -- 13 tasks across 3 phases
- **Phase 1a**: Self-loop guard in PhaseWireExec. Guard BOTH exec_after AND exec_outputs. For exec_after, use if/else (NOT continue) because continue would skip exec_outputs processing for that step.
- **Phase 1b REVERSAL**: Partial success now returns `FOliveWriteResult::Success(ResultData)` instead of `ExecutionError`. This REVERSES the R6 decision from CLI Provider Universal Fixes. The pipeline sees bSuccess=true, does NOT cancel transaction, nodes persist. The AI gets `status:"partial_success"` + `wiring_errors` in a success result.
- **PLAN_PARTIAL_SUCCESS self-correction removed**: Since `HasToolFailure()` checks `bSuccess==false` and partial success now returns true, the self-correction case is dead code. Removed entirely (Option A).
- **Write pipeline interaction**: Pipeline line 194 checks `!ExecuteResult.bSuccess`. Partial success returning Success() bypasses this, flows to Stage 5 (Verify), which may report compile errors from broken wires -- this is desired (AI gets maximum info).
- **Phase 3 recipe refactor**: Tool keeps name `olive.get_recipe`, schema changes from `{category?, name?}` to `{query: string}`. LoadRecipeLibrary updated to parse TAGS format (format_version 2.0). Keyword matching: tag match = 2pts, content substring = 1pt.
- **Manifest tags already exist** in `_manifest.json` but are unused by current code. Phase 3 puts tags IN the .txt files instead (self-contained entries).

### Gun Fix (Orphan/Component/Compile) - Feb 2026
- `plans/gun-fix-implementation-tasks.md` -- 5 tasks fixing 3 issues from gun+bullet test
- **CRITICAL BUG FOUND**: `HasCompileFailure()` looks for `compile_result` at JSON top level, but `FOliveToolResult::ToJson()` nests it inside `"data"`. Compile error self-correction has been BROKEN for ALL tools. Fix: try top-level first, then fall back to `data.compile_result`.
- **JSON nesting**: `FOliveToolResult::ToJson()` (OliveToolRegistry.cpp line 203) puts `ResultData` inside `"data"`. So pipeline fields (`compile_result`, `asset_path`, `compile_status`) are at `data.X`, not top-level. `HasToolFailure()` works because it checks top-level `success` (set by ToJson directly). `HasCompileFailure()` breaks because it checks top-level `compile_result` (which is inside `data`).
- **Component guard**: New error code `COMPONENT_NOT_VARIABLE` in ResolveGetVarOp/ResolveSetVarOp. SCS traversal uses same pattern as `FOliveComponentWriter::FindSCSNode()`. Includes: `Engine/SimpleConstructionScript.h`, `Engine/SCS_Node.h`.
- **Orphan cleanup**: `CleanupCreatedNodes` lambda in `PhaseCreateNodes`. Must skip reused event nodes (tracked via `ReusedStepIds` set). `Graph->RemoveNode()` inside transaction scope is defense-in-depth.

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
