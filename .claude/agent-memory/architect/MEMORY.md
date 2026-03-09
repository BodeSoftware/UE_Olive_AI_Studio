# Architect Agent Memory

## Key Patterns

- **Tool handler pattern**: validate params -> load Blueprint -> build FOliveWriteRequest -> bind executor lambda -> `ExecuteWithOptionalConfirmation`
- **Pre-pipeline validation**: Cheap checks (type validation, duplicate detection) belong BEFORE the write pipeline, not inside the executor lambda. This avoids unnecessary transaction overhead.
- **FOliveWriteResult factory methods**: Use `::ExecutionError()`, `::Success()`, `::ValidationError()`, `::ConfirmationNeeded()` -- do not construct manually.
- **FOliveToolResult vs FOliveWriteResult**: Tool handlers return `FOliveToolResult`; pipeline returns `FOliveWriteResult` which converts via `.ToToolResult()`.
- **Singleton pattern**: All service classes (NodeCatalog, NodeFactory, GraphWriter, WritePipeline, BlueprintReader) are singletons via `static Foo& Get()`.
- **Tick-pump blocking pattern**: `FTSTicker::GetCoreTicker().Tick(0.01f)` + `FPlatformProcess::Sleep(0.01f)` for synchronous LLM calls (FOliveUtilityModel, FOliveAgentPipeline).

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
- Agent pipeline: `Source/OliveAIEditor/Public/Brain/OliveAgentPipeline.h` (NEW)
- Agent config: `Source/OliveAIEditor/Public/Brain/OliveAgentConfig.h` (NEW)

### Agent Pipeline - Mar 2026
- `plans/agent-pipeline-design.md` -- Always-on pipeline replacing discovery pass + decomposition directive
- **Pipeline**: Router -> Scout -> [Researcher if not Simple] -> Architect -> Validator (C++) -> Builder -> Reviewer
- **NOT a singleton**: Instantiate per run (like FOlivePlanExecutor, FOliveLibraryCloner).
- **3 new files**: `OliveAgentConfig.h`, `OliveAgentPipeline.h`, `OliveAgentPipeline.cpp`
- **Settings**: `bCustomizeAgentModels` checkbox. When false, all agents use utility model provider/model. When true, per-agent provider+model selectors appear (EditCondition/EditConditionHides).
- **`GetAgentModelConfig(EOliveAgentRole)`**: Returns `FOliveAgentModelConfig` with provider/model/apikey/baseurl. Falls through to CLI `--print` if no API key.
- **`SendAgentCompletion()`**: Per-role model resolution + tick-pump blocking LLM call. Reuses same pattern as `FOliveUtilityModel::TrySendCompletion()`.
- **Per-agent params**: Router (T=0.0, 64 tok, 10s), Scout (T=0.0, 256 tok, 10s), Researcher (T=0.2, 512 tok, 15s), Architect (T=0.2, 2048 tok, 30s), Reviewer (T=0.0, 512 tok, 15s).
- **Router defaults to Moderate on failure** (safe middle ground).
- **Validator is C++ only** (no LLM): `TryResolveClass()`, `TryResolveComponentClass()`, `IsValidInterface()` via FindFirstObjectSafe + alias map.
- **`FormatForPromptInjection()`**: Produces markdown with Task Analysis, Reference Templates, Build Plan, Validator Warnings, Existing Assets, Execution directive.
- **Replaces**: CLIProviderBase lines 570-621 (discovery pass + decomposition directive).
- **Reviewer**: Runs after Builder completes. One correction pass max (`bIsReviewerCorrectionPass` flag).
- **Integration**: Autonomous path (CLIProviderBase.cpp), Orchestrated path (ConversationManager.cpp: SendUserMessage + BuildSystemMessage + HandleComplete).
- **Build Plan schema**: Order, per-asset sections (Action, Parent Class, Components, Variables, Dispatchers, Interfaces, Functions, Events), Interactions.
- **10-phase implementation order**: Foundation -> Router -> Scout -> Researcher -> Architect -> Validator -> CLIProviderBase -> ConversationManager -> Reviewer -> Polish.

### Planner MCP Tools - Mar 2026
- `plans/planner-mcp-design.md` -- Give Planner agent MCP tool access instead of single-shot `--print`
- **Inline process** (NOT via LaunchCLIProcess): Spawn + blocking read loop on game thread, tick-pump enables MCP tool handling
- **Key insight**: `FTSTicker::Tick()` in read loop processes MCP HTTP requests, enabling tool round-trips
- **3 new methods**: `RunPlannerWithTools()`, `SetupPlannerSandbox()`, `ParseStreamJsonFinalText()`
- **Sandbox**: `Saved/OliveAI/PlannerSandbox/` with `.mcp.json` + minimal `CLAUDE.md`
- **CLI args**: `--print --max-turns 15 --output-format stream-json --verbose --dangerously-skip-permissions`
- **Tool filter**: Uses existing `SetToolFilter()` with exact tool names as prefixes (unique enough)
- **Allowed tools**: `blueprint.get_template`, `blueprint.list_templates`, `blueprint.describe`
- **Timeout**: 180s hard limit (up from 120s single-shot)
- **Fallback**: If MCP not running or process fails, falls back to existing `RunPlanner()` (single-shot)
- **Prompt**: Compact template headers (~3-5K) instead of full overviews (~27-31K)
- **No new files, no new settings**: All changes in OliveAgentPipeline.h/.cpp

### Pipeline Quality Fix - Mar 2026
- `plans/pipeline-quality-fix-design.md` -- Fix Builder producing PrintString-only logic
- **Root causes**: (1) Architect sees template summaries, not implementations; (2) Execution directive kills Builder research agency
- **Scout enhancement**: After `RunDiscoveryPass()`, auto-load top 2 library templates' matched functions via `GetFunctionContent()` (pure C++, no LLM). Stored in new `FOliveScoutResult::TemplateContent` field.
- **Architect enhancement**: Receives `TemplateContent` + 2 new rules to base function descriptions on observed patterns.
- **Execution directive**: Conditional -- if `bWantsSimpleLogic` (detects "stub", "placeholder", etc.), allows PrintString. Otherwise explicitly encourages `get_template` research before writing `plan_json`.
- **GetAgentModelConfig speed fix**: CLI providers probe utility model (HTTP) before falling through to `claude --print`. Saves 10-20s for CLI-only users.
- **No new files**: Only modifies OliveAgentConfig.h, OliveAgentPipeline.cpp, OliveAISettings.cpp.

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

### Planner Pin Enrichment - Mar 2026
- `plans/planner-pin-enrichment-design.md` -- Fix ~55% plan_json success rate caused by hallucinated pin names
- **Root cause**: Planner produces vague function descriptions ("call ApplyPointDamage to deal damage") without pin names. Builder guesses from LLM training memory.
- **Option A (prompt)**: Add pin name instruction to `BuildPlannerSystemPrompt()` -- tell Planner to include exact pin names from template node graphs it reads
- **Option E (C++ enrichment)**: New `BuildFunctionPinReference()` method on `FOliveAgentPipeline`. Extracts function names from Build Plan text via regex, resolves each to `UFunction*` via `FindFunctionEx()`, formats signatures with `TFieldIterator<FProperty>`, appends as Section 3.25 in `FormatForPromptInjection()`.
- **Combined A+E**: Prompt teaches Planner to include pins from templates; C++ catches the rest via reflection. Covers ~95%+ of functions.
- **Key detail**: `describe_node_type` is wrong tool -- it resolves K2Node classes, not function signatures. Returns generic CallFunction pins without function-specific parameters.
- **Budget**: 2500 chars for pin reference section (~30 function signatures). Uses `FOliveClassAPIHelper::GetPropertyTypeString()` for types.
- **by-ref detection**: `CPF_ReferenceParm` flag (not `CPF_OutParm` alone). These cause "must have input wired" compile errors.
- **No new files**: Only modifies OliveAgentPipeline.h/.cpp
- **Senior assignment**: ~200 lines new C++ in critical path

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
