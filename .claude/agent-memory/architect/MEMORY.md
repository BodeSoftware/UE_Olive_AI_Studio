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

### Phase B Coder Task: FOlivePlanExecutor - Feb 2026
- `plans/phase-b-task-planexecutor.md` -- Complete coder task for OlivePlanExecutor.h/cpp
- **Critical decision: Direct pin connection, NOT GraphWriter.ConnectPins()**. The executor uses `FOlivePinConnector::Get().Connect(SourcePin, TargetPin)` directly with `UEdGraphPin*` from `Node->FindPin()`. This bypasses GraphWriter's node ID cache entirely, which is necessary because reused event nodes are not in the cache.
- **GraphWriter.FindNodeById fallback** does NOT match by UObject name. It only checks: (1) cache, (2) GUID, (3) `node_X` sequential index. So using `GetName()` as a node ID would fail for ConnectPins. This is why we bypass GraphWriter for wiring.
- **Event reuse node IDs**: For reused events, use `ExistingNode->NodeGuid.ToString()` as the node ID stored in StepToNodeMap (for result reporting only, not used for wiring).
- **SetPinDefault is also direct**: Uses `Graph->GetSchema()->TrySetDefaultValue(*Pin, DefaultValue)` on the `UEdGraphPin*`, not GraphWriter.SetPinDefault().
- **Phase 6 layout inlined**: Basic BFS column/row layout is implemented directly in `PhaseAutoLayout()`. The full `FOliveGraphLayoutEngine` class (Phase D) can enhance this later.
- **FOliveBatchExecutionScope** must be active when the executor runs (suppresses inner transactions from PinConnector). The caller's write pipeline lambda creates this scope.

### Phase A Coder Tasks Produced - Feb 2026
- `plans/phase-a-task-pinmanifest.md` -- Complete coder task for OlivePinManifest.h/cpp
- `plans/phase-a-task-functionresolver.md` -- Complete coder task for OliveFunctionResolver.h/cpp
- **Alias map verification**: All 120+ entries verified against UE 5.5 engine source. Notable corrections:
  - `K2_GetRelativeLocation`/`K2_GetRelativeRotation` do NOT exist as UFUNCTIONs (getters are inline accessors). Removed from aliases.
  - `GetObjectClass` does not exist on KismetSystemLibrary. Removed.
  - `Power` maps to `MultiplyMultiply_FloatFloat` (not `Pow` which doesn't exist).
  - `K2_ClearAllTimersForObject` is on FTimerManager (not BlueprintCallable). Removed.
- **Pin type mapping**: PinManifest replicates the `MapPinCategory` table from OlivePinSerializer (lines 287-334 of OlivePinSerializer.cpp) to avoid coupling. Also adds container type detection (IsArray/IsSet/IsMap) and struct subcategory specialization (Vector/Rotator/Transform/Color/LinearColor).
- **UE 5.5 API verified**: `UEdGraphSchema_K2::TypeToText(const FEdGraphPinType&)` exists at line 1016 of EdGraphSchema_K2.h. Returns FText.

### Phase C Coder Task: Integration - Feb 2026
- `plans/phase-c-task-integration.md` -- Complete coder task for wiring Phase A+B into existing codebase
- **Execution order**: C1 (FunctionResolver into PlanResolver) -> C3 (BatchScope into PlanExecutor) -> C2 (PlanExecutor into ApplyPlanJson) -> C5 (PreviewPlanJson v2.0) -> C4 (Worker prompt)
- **C1 key decision**: FOliveFunctionResolver replaces direct catalog search in `ResolveCallOp`. On resolver failure without explicit TargetClass, still accepts the call (factory validates at creation time). With explicit TargetClass, failure is an error.
- **C2 key decision**: Version-gating via `bIsV2Plan = (Plan.SchemaVersion == TEXT("2.0"))`. v1.0 path is verbatim copy of original. v2.0 path creates `FOlivePlanExecutor` inside the executor lambda. Both paths are in the same function.
- **C2 ResultData propagation**: FOliveWriteResult::ExecutionError may not have a 4th-param overload for ResultData. Coder must check and use 2-step construction if needed: `ErrorResult.ResultData = ResultData`.
- **C3 key decision**: `FOliveBatchExecutionScope` wraps phases 3-5 inside the executor's `Execute()` method. This makes the executor self-contained. The outer caller's BatchScope (in the apply lambda) also exists, and nesting is safe.
- **C5 key decision**: v2.0 preview skips lowering entirely. Returns `resolved_steps` array with `resolved_function` and `resolved_class` per step so AI can verify resolution.
- **Pin manifests NOT serialized in result**: Phase C uses a `self_correction_hint` string instead of serialized manifest JSON. The AI is told to use `blueprint.read` for actual pin names. Full manifest serialization deferred to a follow-up.

### Self-Correction Fix Plan - Feb 2026
- `plans/self-correction-fix-tasks.md` -- 7 fixes across 3 phases for self-correction system
- **Root cause**: CLI prompt via `-p` arg exceeds Win32 32KB cmd-line limit after 3+ agentic iterations. Process crashes, crash classified as Terminal (no retry), and even when corrections reach AI, nothing forces retry.
- **Fix 1 (stdin pipe)**: `FPlatformProcess::CreatePipe(R, W, bWritePipeLocal=true)` for stdin. Pass ReadEnd to CreateProc's PipeReadChild param. Use `uint8*` WritePipe overload (FString overload auto-appends `\n`). Close write end after writing to signal EOF.
- **Fix 2 (crash classification)**: Match `"process exited with code"` as Transient in ClassifyError Tier 2.
- **Fix 6 (correction directive)**: Injected as User role message (not System) for cross-provider compatibility. Appended in ContinueAfterToolResults before SendToProvider.
- **Fix 7 (block premature completion)**: Max 2 re-prompts. `EOliveRunOutcome::PartialSuccess` already exists in the enum. bHasPendingCorrections cleared when a batch has 0 failures.
- **`EOliveRunOutcome` enum**: Completed, PartialSuccess, Failed, Cancelled -- defined in OliveBrainState.h line 36.

## File Structure
- Tool handlers: `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` (very large file, 3000+ lines)
- Schemas: `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintSchemas.cpp`
- Pipeline: `Source/OliveAIEditor/Blueprint/Private/Pipeline/OliveWritePipeline.cpp`
- Node catalog: `Source/OliveAIEditor/Blueprint/Private/Catalog/OliveNodeCatalog.cpp`
- IR structs: `Source/OliveAIRuntime/Public/IR/CommonIR.h` (FOliveIRGraph, FOliveIRNode, FOliveIRMessage, etc.)
- Plan executor: `Source/OliveAIEditor/Blueprint/Public/Plan/OlivePlanExecutor.h` (new)
- Pin manifest: `Source/OliveAIEditor/Blueprint/Public/Plan/OlivePinManifest.h` (new)
- Function resolver: `Source/OliveAIEditor/Blueprint/Public/Plan/OliveFunctionResolver.h` (new)
- Layout engine: `Source/OliveAIEditor/Blueprint/Public/Plan/OliveGraphLayoutEngine.h` (new)
