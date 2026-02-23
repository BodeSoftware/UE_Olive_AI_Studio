# Plan: Add Intent-Level Blueprint Plan JSON Orchestrator (Preview + Apply)

## Summary
This plan adds a higher-level JSON contract for Blueprint graph authoring to eliminate micro-call thrash and stale node-id failures seen in `docs/logs/UE_Olive_AI_Toolkit.log` (repeated `blueprint.add_node`/`blueprint.connect_pins` failures like `Source node 'node_1' not found`).

The implementation is additive and keeps your existing writer stack.
Execution remains: Validate -> Confirm -> Transact -> Execute -> Verify -> Report.
New tools become orchestration entrypoints; existing `blueprint.*` and `project.batch_write` remain actuators/fallback.

## Scope
1. v1 supports Event Graph and Function Graph logic authoring.
2. v1 supports `merge` mode only (no destructive graph replacement in this phase).
3. v1 includes Preview + Apply + structured repair hints.
4. Existing granular tools remain fully supported and unchanged for backward compatibility.

## Public APIs, Interfaces, and Types
1. New MCP tool: `blueprint.preview_plan_json`.
Request fields:
`asset_path`, `graph_target`, `mode`, `plan_json`.
Response fields:
`normalized_plan`, `plan_summary`, `diff`, `warnings`, `preview_fingerprint`, `requires_confirmation`, `confirmation_token` (when tier requires).

2. New MCP tool: `blueprint.apply_plan_json`.
Request fields:
`asset_path`, `graph_target`, `mode`, `plan_json`, `preview_fingerprint` (required if coming from preview), `confirmation_token` (when required), `auto_repair_once` (optional bool).
Response fields:
`applied_ops_count`, `step_to_node_map`, `compile_result`, `verification_messages`, `ui_actions`.

3. New shared IR contracts (runtime module).
Add:
`Source/OliveAIRuntime/Public/IR/BlueprintPlanIR.h`
`Source/OliveAIRuntime/Private/IR/BlueprintPlanIR.cpp`
Core structs:
`FOliveIRBlueprintPlan`, `FOliveIRBlueprintPlanTarget`, `FOliveIRBlueprintPlanStep`, `FOliveIRBlueprintPlanRef`, `FOliveIRBlueprintPlanErrorLocation`.
All include `ToJson`/`FromJson`.

4. Schema validation extension.
Add:
`ValidateBlueprintPlanJson` to `Source/OliveAIRuntime/Public/IR/OliveIRSchema.h` and implementation in `Source/OliveAIRuntime/Private/IR/OliveIRSchema.cpp`.

5. Editor-side resolver/lowerer service.
Add:
`Source/OliveAIEditor/Blueprint/Public/Plan/OliveBlueprintPlanResolver.h`
`Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`
`Source/OliveAIEditor/Blueprint/Public/Plan/OliveBlueprintPlanLowerer.h`
`Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanLowerer.cpp`

6. Reusable batch execution service.
Add:
`Source/OliveAIEditor/CrossSystem/Public/Services/OliveGraphBatchExecutor.h`
`Source/OliveAIEditor/CrossSystem/Private/Services/OliveGraphBatchExecutor.cpp`
Used by both `project.batch_write` and `blueprint.apply_plan_json`.

7. Tool schema additions.
Extend:
`Source/OliveAIEditor/Blueprint/Public/MCP/OliveBlueprintSchemas.h`
`Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintSchemas.cpp`

8. Tool handler additions.
Extend:
`Source/OliveAIEditor/Blueprint/Public/MCP/OliveBlueprintToolHandlers.h`
`Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`

9. Settings additions.
Extend:
`Source/OliveAIEditor/Public/Settings/OliveAISettings.h`
`Config/DefaultOliveAI.ini`
Add settings:
`bEnableBlueprintPlanJsonTools` (default false),
`bAllowPlanAutoRepairOnce` (default false),
`PlanJsonMaxSteps` (default 128),
`PlanJsonRequirePreviewForApply` (default true).

## Locked Design Decisions
1. Plan JSON is intent-level only.
No GUIDs, no `node_*`, no raw pin IDs in input contract.

2. Deterministic lowering is mandatory.
Given same asset state + same plan, lowered ops must be byte-identical.

3. Compile once at end.
No per-step compile.

4. Drift check is required.
`preview_fingerprint` is computed in preview and validated in apply; mismatch returns `GRAPH_DRIFT` without mutation.

5. Auto-repair is limited.
At most one automatic repair pass (`auto_repair_once=true`), only for known deterministic fix classes.

6. v1 op vocabulary is closed.
Allowed ops are finite and schema-enforced (example set: `get_var`, `set_var`, `call`, `flow.branch`, `flow.sequence`, `math.add/sub/mul/div`, `math.lt/le/gt/ge/eq/ne`, `return`).

## Implementation Plan
1. Foundation: plan IR + schema + schema registration wiring.
Files:
`Source/OliveAIRuntime/Public/IR/BlueprintPlanIR.h`
`Source/OliveAIRuntime/Private/IR/BlueprintPlanIR.cpp`
`Source/OliveAIRuntime/Public/IR/OliveIRSchema.h`
`Source/OliveAIRuntime/Private/IR/OliveIRSchema.cpp`
`Source/OliveAIEditor/Public/Services/OliveValidationEngine.h`
`Source/OliveAIEditor/Private/Services/OliveValidationEngine.cpp`
`Source/OliveAIEditor/Private/MCP/OliveToolRegistry.cpp`
Work:
Define strict schema with `schema_version`, references (`@step`, `$input`), required fields, allowed ops.
Add engine-level validation result codes with JSON pointer locations.
Wire tool schema registration so `FOliveSchemaValidationRule` actually validates registered tool schemas on execution.
Done when:
Invalid plan JSON is rejected before any asset load with deterministic error code and pointer path.

2. Resolver and lowerer.
Files:
`Source/OliveAIEditor/Blueprint/Public/Plan/OliveBlueprintPlanResolver.h`
`Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`
`Source/OliveAIEditor/Blueprint/Public/Plan/OliveBlueprintPlanLowerer.h`
`Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanLowerer.cpp`
Work:
Resolve intent ops to concrete node intents using `FOliveNodeCatalog` and reflection metadata.
Resolve overloads deterministically with explicit tie-break rules.
Insert implicit conversion/cast nodes when required.
Emit lowered op list with stable op ids and a `step_id -> lowered op ids` mapping.
Done when:
Same plan resolves identically across repeated runs, and ambiguous cases return explicit alternatives.

3. Shared graph batch executor extraction.
Files:
`Source/OliveAIEditor/CrossSystem/Public/Services/OliveGraphBatchExecutor.h`
`Source/OliveAIEditor/CrossSystem/Private/Services/OliveGraphBatchExecutor.cpp`
`Source/OliveAIEditor/CrossSystem/Private/MCP/OliveCrossSystemToolHandlers.cpp`
Work:
Extract current `project.batch_write` dispatch/template logic into a reusable service.
Support two execution modes:
`OwnTransaction` for `project.batch_write`,
`ExternalTransaction` for `blueprint.apply_plan_json` inside write pipeline.
Keep allowlist behavior and template substitution logic.
Done when:
`project.batch_write` behavior is unchanged, and new caller can execute same op stream without nested transaction bugs.

4. New MCP schemas and handlers.
Files:
`Source/OliveAIEditor/Blueprint/Public/MCP/OliveBlueprintSchemas.h`
`Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintSchemas.cpp`
`Source/OliveAIEditor/Blueprint/Public/MCP/OliveBlueprintToolHandlers.h`
`Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`
Work:
Register `blueprint.preview_plan_json` and `blueprint.apply_plan_json`.
`preview` path:
validate -> resolve/lower -> compute diff -> compute fingerprint -> return preview payload.
`apply` path:
validate -> confirm tier routing -> transact via `FOliveWritePipeline` -> execute lowered ops through `OliveGraphBatchExecutor` -> verify compile/structure -> report.
Return `step_to_node_map`, compile diagnostics, and structured repair hints.
Done when:
Preview is non-mutating and apply performs one atomic commit with rollback on failure.

5. Diff and fingerprint implementation.
Files:
`Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`
`Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`
Work:
Use graph summary/full IR from `FOliveGraphReader` to compute:
`nodes_added`, `nodes_removed` (always 0 in merge mode), `connections_added`, `connections_removed`.
Compute deterministic fingerprint from target graph IR summary and plan hash.
Enforce fingerprint check during apply.
Done when:
Apply fails fast with `GRAPH_DRIFT` if graph changed since preview.

6. UI integration in chat panel.
Files:
`Source/OliveAIEditor/Public/UI/SOliveAIMessageList.h`
`Source/OliveAIEditor/Private/UI/SOliveAIMessageList.cpp`
`Source/OliveAIEditor/Public/UI/SOliveAIChatPanel.h`
`Source/OliveAIEditor/Private/UI/SOliveAIChatPanel.cpp`
Work:
Add preview card rendering for `blueprint.preview_plan_json` result.
Buttons:
`Apply`, `Cancel`, `Edit JSON`.
`Edit JSON` behavior:
inject normalized plan JSON into chat input for user edit and resend.
Add optional checkbox:
`Auto-repair on failure (1 retry)`.
Done when:
User can complete Preview -> Apply without manual tool syntax.

7. Prompting and routing updates.
Files:
`Content/SystemPrompts/BaseSystemPrompt.txt`
`Content/SystemPrompts/Base.txt`
`Content/SystemPrompts/Worker_Blueprint.txt`
`Content/SystemPrompts/Knowledge/blueprint_authoring.txt`
Work:
Policy:
use `blueprint.preview_plan_json` + `blueprint.apply_plan_json` for graph tasks with 3+ operations.
Fallback:
use granular tools only when plan op unsupported.
Enforce “read current graph before generate plan” rule.
Done when:
Agent chooses plan path by default for multi-step graph logic.

8. Observability and docs.
Files:
`docs/` and `plans/`
Work:
Add log markers for plan lifecycle:
`PLAN_VALIDATE`, `PLAN_RESOLVE`, `PLAN_LOWER`, `PLAN_APPLY`, `PLAN_VERIFY`, `PLAN_REPAIR`.
Capture metrics:
tool calls per task, retries per task, apply success rate, rollback rate, compile fail classes.
Write operator docs with sample plan JSON and error catalog.
Done when:
Performance and reliability deltas are measurable across sessions.

## Test Cases and Scenarios
1. Happy path function creation.
Input plan creates 8-15 node function in `/Game/BP_Gun`.
Expected:
single preview + single apply, compile success, correct `step_to_node_map`.

2. Missing required plan field.
Remove required arg in one step.
Expected:
validation error with `location_pointer` (example `/steps/3/b`), no mutation.

3. Ambiguous overload.
Call function with multiple candidates.
Expected:
preview warning with alternatives, apply blocked until explicit disambiguation.

4. Type mismatch repairable.
Plan omits cast where deterministic cast can be inserted.
Expected:
apply fails with structured hint; with `auto_repair_once=true`, one repaired attempt succeeds.

5. Drift detection.
Preview plan, manually edit graph, then apply old preview.
Expected:
`GRAPH_DRIFT` error, no mutation.

6. Atomic rollback.
Force failure in one lowered op mid-apply.
Expected:
full rollback; no partial nodes/connections remain.

7. Regression guard for existing batch tool.
Run existing `project.batch_write` scenarios.
Expected:
unchanged behavior and performance envelope.

8. Performance comparison against current baseline.
Replay scenario similar to Feb 22 log.
Expected:
multi-step graph task uses <=2 top-level tools (preview/apply) and materially fewer retries.

## Rollout Plan
1. Phase 1 internal flag-off ship.
Merge code with `bEnableBlueprintPlanJsonTools=false` default.

2. Phase 2 opt-in testing.
Enable for internal profile; collect metrics for one week.

3. Phase 3 default-on for Blueprint profile.
Keep granular fallback; keep old tools exposed.

4. Phase 4 evaluate `replace` mode.
Only after merge-mode reliability target is met.

## Acceptance Criteria
1. `blueprint.preview_plan_json` is non-mutating and deterministic.
2. `blueprint.apply_plan_json` always runs atomically with rollback on any failure.
3. `step_to_node_map` is returned on success for all created steps.
4. Structured failures include `error_code`, `location_pointer`, and repair guidance.
5. Multi-step graph authoring in chat defaults to preview/apply path.
6. Retry-thrash class from log (`node_1 not found` loops) is eliminated for plan-path tasks.

## Assumptions and Defaults
1. Default scope is EventGraph and FunctionGraph only.
2. Default mode is `merge`; `replace` is deferred.
3. Default requires preview before apply.
4. Default auto-repair is disabled unless user opts in per apply.
5. Existing granular `blueprint.*` and `project.batch_write` remain supported and unchanged for compatibility.
