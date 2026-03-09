# Architect Agent Memory

## Key Patterns

- **Tool handler pattern**: validate params -> load Blueprint -> build FOliveWriteRequest -> bind executor lambda -> `ExecuteWithOptionalConfirmation`
- **Pre-pipeline validation**: Cheap checks (type validation, duplicate detection) belong BEFORE the write pipeline, not inside the executor lambda. This avoids unnecessary transaction overhead.
- **FOliveWriteResult factory methods**: Use `::ExecutionError()`, `::Success()`, `::ValidationError()`, `::ConfirmationNeeded()` -- do not construct manually.
- **FOliveToolResult vs FOliveWriteResult**: Tool handlers return `FOliveToolResult`; pipeline returns `FOliveWriteResult` which converts via `.ToToolResult()`.
- **Singleton pattern**: All service classes (NodeCatalog, NodeFactory, GraphWriter, WritePipeline, BlueprintReader) are singletons via `static Foo& Get()`.
- **Tick-pump blocking pattern**: `FTSTicker::GetCoreTicker().Tick(0.01f)` + `FPlatformProcess::Sleep(0.01f)` for synchronous LLM calls (FOliveUtilityModel).

## Architecture Decisions

> Older completed design decisions archived to `completed-designs.md`.

### Pipeline Fixes 08f - Mar 2026
- `plans/pipeline-fixes-08f.md` -- 5 fixes for agent pipeline issues
- **Compile tool bug**: `HandleBlueprintCompile` returned `Success()` even on `!CompileResult.bSuccess`. Fix: return `Error("COMPILE_FAILED", ...)` with inline error messages.
- **Planner knowledge**: Load `events_vs_functions.txt` + `blueprint_design_patterns.txt` (sections 0-3) from disk via `LoadKnowledgePack()` helper. Replaces hardcoded 15-line inline block.
- **Stale node IDs**: Append rollback warning to `PLAN_EXECUTION_FAILED` error message so Builder knows to re-read with `blueprint.read`.
- **remove_function schema**: `force` param handler exists (line 4078) but schema didn't advertise it. Add `force` bool to `BlueprintRemoveFunction()`.
- **Status messages**: Emit synthetic `OnChunk` during pipeline execution in `SendMessageAutonomous()` to break 117s silence.

## Error Code Convention
- Use `SCREAMING_SNAKE_CASE` for error codes
- See design docs for full list.

### Autonomous Mode Decisions - Feb 2026
> See `autonomous-mode-decisions.md` for full details on NeoStack migration, timeout fixes, and efficiency rounds 2-3.
- **NeoStack**: `bUseAutonomousMCPMode` flag, `SendMessageAutonomous()`, MCP tool discovery, no orchestration loop.
- **Timeout**: Activity-based idle timeout (`AutonomousIdleToolSeconds=120`), hard limit 900s.
- **Round 3**: "continue" context injection (`FAutonomousRunContext`), template result enrichment.

## File Structure
- Tool handlers: `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` (very large file, 5000+ lines)
- Schemas: `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintSchemas.cpp`
- Pipeline: `Source/OliveAIEditor/Blueprint/Private/Pipeline/OliveWritePipeline.cpp`
- Node catalog: `Source/OliveAIEditor/Blueprint/Private/Catalog/OliveNodeCatalog.cpp`
- IR structs: `Source/OliveAIRuntime/Public/IR/CommonIR.h` (FOliveIRGraph, FOliveIRNode, FOliveIRMessage, etc.)
- Plan executor: `Source/OliveAIEditor/Blueprint/Public/Plan/OlivePlanExecutor.h`
- Template system: `Source/OliveAIEditor/Blueprint/Public/Template/OliveTemplateSystem.h`
- Utility model: `Source/OliveAIEditor/Public/Services/OliveUtilityModel.h`
- Agent pipeline: DELETED (was `Source/OliveAIEditor/Public/Brain/OliveAgentPipeline.h`)
- Agent config: DELETED (was `Source/OliveAIEditor/Public/Brain/OliveAgentConfig.h`)

### Agent Pipeline - DELETED Mar 2026
- `plans/single-agent-revert-design.md` -- Reverted multi-agent pipeline back to single-agent flow
- **Reason**: 60-180s overhead for 5-7 LLM sub-agent calls; pipeline timeouts caused sparse fallbacks; complexity not justified by quality gains
- **Reverted to**: Discovery pass (`FOliveUtilityModel::RunDiscoveryPass`) + decomposition directive + single autonomous agent
- **Kept**: All tool-layer improvements (resolver-executor contract, Phase 0 validation, describe_function, UPROPERTY auto-rewrite, stale pin cleanup)
- **Dropped**: FOliveAgentPipeline, FOliveAgentConfig, per-agent model settings, Reviewer, Build Plan, FormatForPromptInjection, Section 3.25 pin reference, Component API map
- **Files DELETED (Option B)**: OliveAgentPipeline.h/.cpp, OliveAgentConfig.h -- clean delete, all transitive deps removed
- **6 dependent files cleaned**: CLIProviderBase.h/.cpp, ConversationManager.h/.cpp, OliveAISettings.h/.cpp
- **System prompt**: AGENTS.md updated with Planning/Research/Building/Self-Correction sections
- **Original design**: `plans/agent-pipeline-design.md` (historical reference)

### Codex CLI Provider - Mar 2026
- `plans/codex-cli-provider-design.md` -- FOliveCodexProvider subclass of CLIProviderBase
- See `autonomous-mode-decisions.md` for details. Key: direct HTTP MCP (no bridge), `codex exec --json`.

### Library Clone Tool - Mar 2026
- `plans/library-clone-design.md` -- `blueprint.create_from_library` tool
- **FOliveLibraryCloner** NOT a singleton (per-operation). 3 modes: structure, portable, full.
- 6-phase per-graph pipeline: Classify -> Create -> WireExec -> WireData -> SetDefaults -> AutoLayout

### Error Recovery - Mar 2026
- `plans/error-recovery-design.md` -- Fix FUNCTION_NOT_FOUND wasting 3-4 turns on wrong-class fuzzy matches
- **Fix 1**: Class-scoped function+property suggestions in `ResolveCallOp()` error path. Replaces catalog-wide fuzzy match.
  - Property cross-match detects `SetSpeed` -> `MaxSpeed` and tells Builder to use `set_var` not `call`
  - Falls back to catalog fuzzy match when no target class can be resolved
- **Fix 2**: Progressive self-correction in `BuildToolErrorMessage()` for FUNCTION_NOT_FOUND
  - Attempt 1: read class-scoped suggestions, check for property matches
  - Attempt 2: call `blueprint.read` to inspect components
  - Attempt 3+: escalate to `add_node`, `editor.run_python`, or skip
- **Fix 3**: Aider-style component API map in `FormatForPromptInjection()` (Section 3.5)
  - `BuildComponentAPIMap()` enumerates functions+properties per component class from Build Plan
  - Capped at 3000 chars total, 15 functions + 10 properties per class
  - Populated after `ParseBuildPlan()` in both API and CLI paths
- **New files**: `OliveClassAPIHelper.h/.cpp` -- shared helper for class API enumeration (used by Fix 1 and Fix 3)
- **Key filtering rules**: Skip DEPRECATED_, Internal_, PostEditChange, OnRep_ functions; only BlueprintVisible/ReadOnly properties
- **4-phase implementation**: Shared helper -> Fix 1 (resolver) -> Fix 2 (self-correction) -> Fix 3 (pipeline)

### Planner Pin Enrichment - DEPRECATED Mar 2026
- Superseded by single-agent revert. Agent uses `blueprint.describe_function` on demand instead of pre-computed Section 3.25.
- Key insight preserved: `describe_node_type` resolves K2Node classes, NOT function signatures. Use `describe_function` for pin names.

### Plan Executor Fixes 09j - Mar 2026
- `plans/plan-executor-fixes-09j-design.md` -- 4 bugs, 59% -> 80% plan_json regression
- **Bug #1 (GetForwardVector)**: ALREADY FIXED in e14162e. `_resolved` flag + `bSkipAlias` prevents double-aliasing.
- **Bug #2 (stale exec pin)**: After failed plan_json, reused node pins retain `bOrphanedPin=true`. Fix: iterate `Context.ReusedStepIds` on failure, clear `bOrphanedPin` on all pins. Junior task, ~15 lines in `Execute()`.
- **Bug #3 (break_struct)**: (a) Add BreakStruct/MakeStruct handling to `CreateNodeByClass()` defense-in-depth. (b) Strip `~` prefix from pin hints in `ParseDataRef()`. Senior + Junior.
- **Bug #4 (VARIABLE_NOT_FOUND)**: New Phase 0 check. `get_var`/`set_var` steps must reference existing variables. Duplicate `BlueprintHasVariable()` into validator. Skip FunctionInput/FunctionOutput nodes. Junior, ~60 lines.
- **Key insight**: `_resolved` property on node Properties tells NodeFactory to skip alias map. Pattern: resolver resolves -> marks `_resolved` -> executor passes through -> factory skips alias step 0.

### Resolver-Executor Contract - Mar 2026
- `plans/resolver-executor-contract-design.md` -- Resolver becomes single resolution authority for `call` ops
- **Core change**: `FOliveResolvedStep.ResolvedFunction` (UFunction*) carries resolved function from resolver to executor
- **NodeFactory**: `SetPreResolvedFunction(UFunction*)` -- consumed-once setter, executor calls before `AddNode`. When set, `CreateNodeByClass` skips `FindFunction` entirely.
- **UPROPERTY auto-rewrite**: Resolver detects `PROPERTY MATCH:` in SearchedLocations on failure, rewrites `call` -> `set_var`/`get_var` with resolver note
- **Audit result**: Executor's `FindFunction` uses ZERO graph-derived context -- same inputs (function_name, target_class, Blueprint*) the resolver already resolved. No legitimate reason for re-resolution.
- **`_resolved` flag**: Becomes dead code (kept for backward compat with `add_node` tool path). Superseded by `SetPreResolvedFunction`.
- **Risk**: LOW. UFunction* stable within single frame, same game thread, consumed immediately.
- **4 tasks**: Add field (~15 lines) -> NodeFactory setter (~30 lines) -> Executor threading (~15 lines) -> UPROPERTY rewrite (~50 lines)

### Error Messages 08g - Mar 2026
- `plans/error-messages-08g-design.md` -- 3 targeted improvements to reduce plan_json first-failure-to-fix
- **Change 1**: UPROPERTY detection in `FindFunctionEx()` -- after search trail, scan classes for matching property names. Strips Set/Get prefix, checks BlueprintVisible properties. Appends `PROPERTY MATCH:` to SearchedLocations.
- **Change 2**: Pin listing in `connect_pins` -- `BuildAvailablePinsList()` helper lists name+type of available pins on failure. Applied to ConnectPins, DisconnectPins, DisconnectAllFromPin, SetPinDefault.
- **Change 3**: `GetActorTransform` alias fix -- remove self-referential no-op at line 3087 and harmful redirect at line 2851. C++ function is `GetTransform`, not `GetActorTransform`.
- **No new files**: Only modifies `OliveNodeFactory.cpp` and `OliveGraphWriter.cpp`.
- **OliveNodeFactory.cpp needs**: `#include "Writer/OliveClassAPIHelper.h"` (not currently included).

## UE 5.5 API Quirks
- `TWeakObjectPtr` does NOT have `IsNull()` -- use `.Get() != nullptr` or `IsValid()` instead
- `K2Node::AllocateDefaultPins()` calls `FindBlueprintForNodeChecked()` -- transient graphs MUST have a `UBlueprint` outer
- Timer functions: `K2_SetTimer`, `K2_ClearTimer`, `K2_PauseTimer` etc. (NOT `K2_SetTimerByFunctionName`)

## FindFunction Search Order (7+1 steps)
- Alias map -> specified class -> BP GeneratedClass + FunctionGraphs -> parent hierarchy -> SCS components -> interfaces -> 11 hardcoded libraries -> universal UBlueprintFunctionLibrary scan -> K2_ fuzzy match
- `FindFunctionEx()` returns `FOliveFunctionSearchResult` with search trail on failure

## Component Bound Events
- `CreateComponentBoundEventNode()` uses `InitializeComponentBoundEventParams(FObjectProperty*, FMulticastDelegateProperty*)`
- Resolver detects via SCS `FMulticastDelegateProperty` scan in `ResolveEventOp`

## Compile Error Propagation
- Stage 6 (Report): if `CR.HasErrors()`, sets `bSuccess=false`. Transaction already committed in Stage 4.
- Self-correction triggers via BOTH `HasToolFailure()` AND `HasCompileFailure()` (nested)
- `Blueprint->Status == BS_Error` forces `bSuccess=false` even without per-node errors

## Autocast/Auto-Conversion
- `TryCreateConnection()` is the correct call path (same as Blueprint editor)
- SplitPin fallback for Vector->Float (no autocast exists for that direction)
- `connect_pins` tool: `bAllowConversion=true`

## Interface Events
- `ResolveEventOp` searches `ImplementedInterfaces` after SCS scan
- `SetFromField<UFunction>(Func, false)` + `bOverrideFunction = true`
- BPI functions on `SkeletonGeneratedClass`, NOT `InterfaceDesc.Interface` directly
