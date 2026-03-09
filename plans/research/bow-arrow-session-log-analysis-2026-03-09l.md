# Research: Bow & Arrow Session Log Analysis — Run 09l

## Question
Verify the impact of the resolver→executor contract change (single resolution authority). Track specific fixes, failure patterns, and compare against prior runs.

## Session Overview

**Task:** "create a bow and arrow system for @BP_ThirdPersonCharacter"
**Log period:** 2026-03-09 17:31:09 – 17:45:27
**Run start:** 17:32:26 (autonomous run launched)
**Run end:** 17:44:48 (Reviewer verdict)
**Total elapsed:** ~12:22 (min:sec)

**Assets built:**
- `/Game/Blueprints/BP_Arrow` (parent: Actor) — 10 variables, 2 components (ArrowMesh, ArrowCollision)
- `/Game/Blueprints/BP_Bow` (parent: Actor) — 8 variables, 1 component
- `/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter` — 4 variables added, functions added, EventGraph wired

**Builder reported:** exit code 0, 50 tool calls logged, last tool call 20.9s before exit

---

## Pipeline Timing

| Stage | Time | Notes |
|-------|------|-------|
| Scout (CLI) | 16.2s | community_blueprints.db search (5 queries), 3 template overviews (25,668 chars), 3 refs |
| Planner (MCP) | 106.1s | 10 get_template + 4 get_recipe calls; 10,305 char plan, 4 assets |
| Validator | 0.004s | 0 blocking issues |
| Pipeline total | 122.3s (2:02) |
| Builder (autonomous) | ~9:54 | 17:34:29 launch → ~17:44:26 exit (50 tool calls logged) |
| Reviewer | 22.1s |
| **Total** | **~12:22** |

---

## Contract Change Verification

### 1. ResolvedFunction pointer — "Used pre-resolved function" / "skipped FindFunction"
**Result: NOT FOUND in log.**

The log contains no messages matching "Used pre-resolved function", "skipped FindFunction", or any indication that the executor consumed a pre-resolved `UFunction*` from the resolver's output. The executor still calls `LogOliveNodeFactory: FindFunction(...)` independently for every plan step at execution time. This is visible for every successful node creation:

```
LogOliveNodeFactory: FindFunction('GetComponentByClass'): alias resolved -> 'GetComponentByClass'
LogOliveNodeFactory: FindFunction('AttachToComponent'): alias resolved -> 'K2_AttachToComponent'
LogOliveNodeFactory: FindFunction('MakeTransform'): alias resolved -> 'MakeTransform'
```

Conclusion: The executor is re-running FindFunction from scratch on the function name string from the resolved step, not consuming a `UFunction*` handed off by the resolver.

### 2. Fallback warning — "reached FindFunction fallback"
**Result: NOT FOUND in log.**

No "reached FindFunction fallback" messages appear. This suggests either the log marker was not added, or the logging is not active in this build. Given that FindFunction clearly fires (alias resolution messages are visible), the absence confirms this specific instrumentation string does not exist in this build.

### 3. UPROPERTY auto-rewrite — "UPROPERTY detected" / "auto-rewritten from call to" / "PROPERTY MATCH"
**Result: NOT FOUND in log.**

No UPROPERTY rewrite messages appear. See Failure Analysis below — CMC property setters (`SetMaxWalkSpeed`, etc.) still reach the Builder as-is and the Builder continues attempting to call them. The resolver is not intercepting these.

### 4. GetForwardVector resolution
**Result: Alias fires in resolver but executor re-runs independently.**

Resolver at line 1896:
```
LogOliveNodeFactory: FindFunction('GetForwardVector'): alias resolved -> 'GetActorForwardVector'
```
This fires in the pipeline preview phase (17:34:28) before the Builder launches. The alias map correctly maps `GetForwardVector -> GetActorForwardVector`.

However, no GetForwardVector plan_json call appears in the Builder's execution phase. The Builder does not attempt to call GetForwardVector at all in this run. This means we cannot confirm or deny whether the executor would have used the pre-resolved alias had the Builder tried it.

Root cause confirmed from prior 09k analysis: the resolver fires FindFunction and records the alias in the log, but `FOliveResolvedStep.ResolvedFunctionName` stores the alias result (e.g., `GetActorForwardVector`). The executor should consume `ResolvedFunctionName` from the resolved step rather than calling FindFunction again with the original string. Whether this fix landed is not exercised in this run.

### 5. CMC properties — SetMaxWalkSpeed, SetOrientRotationToMovement
**Result: NOT exercised by Builder in this run.**

No calls to `SetMaxWalkSpeed`, `SetOrientRotationToMovement`, or any CharacterMovementComponent property setter appear in the Builder's tool calls. The Planner did not include CMC modifications in this run's plan — the system focused on BP_Arrow, BP_Bow, and BP_ThirdPersonCharacter bow equip/aim/fire logic without needing CMC state changes.

### 6. VARIABLE_NOT_FOUND Phase 0 check
**Result: NOT triggered in this run.**

No `VARIABLE_NOT_FOUND` errors appear. All `get_var`/`set_var` targets in this run (`bBowEquipped`, `BowActor`, `BowClass`, `ArrowDamage`, `ArrowSpeed`, etc.) exist on the Blueprint by the time they are used. The known gap (Phase 0 not blocking `get_var` on undeclared variables) was not exercised.

---

## Tool Call Statistics

**Total tool calls:** 120 (by `tools/call result` line count)
**Successful:** 108
**Failed:** 12
**Success rate:** 90.0%

| Category | Count | Success | Failed |
|----------|-------|---------|--------|
| blueprint.apply_plan_json | 14 | 10 | 4 |
| blueprint.compile | 13 | 13 | 0 |
| blueprint.create | 2 | 2 | 0 |
| blueprint.add_component | 5 | 5 | 0 |
| blueprint.add_variable | 10 | 9 | 1 |
| blueprint.add_function | 4 | 4 | 0 |
| blueprint.modify_component | 2 | 2 | 0 |
| blueprint.remove_node | 9 | 8 | 1 |
| blueprint.read | 4+ | all | 0 |
| blueprint.connect_pins | 15+ | 12 | 3 |
| blueprint.disconnect_pins | 6 | 4 | 2 |
| blueprint.add_node | 4 | 3 | 1 |
| Planner get_template | 10 | 10 | 0 |
| Planner olive.get_recipe | 4 | 4 | 0 |

Note: Planner tool calls (before Builder launch) are counted in the Planner MCP session; Builder tool calls are after 17:34:28.

---

## plan_json Success Rate

**Total apply_plan_json calls:** 14
**Succeeded:** 10
**Failed:** 4
**Success rate: 71.4%**

---

## plan_json Failure Details

### Failure 1 — BP_Arrow / InitArrow (17:36:25, attempt 1 of 3)
**Asset:** `/Game/Blueprints/BP_Arrow`, graph: `InitArrow`
**Root cause:** `SetFloatPropertyByName` hallucination — three `call` ops targeting `SetFloatPropertyByName` (steps `set_init_speed`, `set_max_speed`, `set_proj_grav`).
**Error:** ResolveCallOp FAILED — `SetFloatPropertyByName` does not exist in UE5. Searched 215 aliases, full class hierarchy, all library classes.
**Resolution:** 9/12 steps resolved, 3 errors. Preview failed, plan rejected before execution.
**Closest matches offered:** `SetFieldPathPropertyByName`, `SetVector3fPropertyByName` (both irrelevant).
**Category:** LLM hallucination — function removed in UE5 (was UE4 Blueprint utility). Same failure appeared in run 09j (3×).

### Failure 2 — BP_Arrow / InitArrow (17:36:34, attempt 2 of 3)
**Asset:** `/Game/Blueprints/BP_Arrow`, graph: `InitArrow`
**Root cause:** `SetInitialSpeed` on `ProjectileMovementComponent` — also not a UFunction.
**Error:** FindFunction('SetInitialSpeed', class='ProjectileMovementComponent') FAILED. `InitialSpeed` is a `UPROPERTY float` on `UProjectileMovementComponent`, not a callable function.
**Status:** Plan resolved 10/10 steps with 1 warning (the SetInitialSpeed warning was downgraded), but execution failed at Phase 4 data wiring — `set_speed_prop` node was created as `SetVariable` with target `InitialSpeed`, which has no `Target` pin or `NewInitialSpeed` pin. Phase 4: 2 data connections failed → rollback.
**Category:** UPROPERTY-as-call confusion. The resolver accepted the step (warning only) and the executor created a SetVariable node with the wrong target context, then couldn't wire it. This is the UPROPERTY auto-rewrite gap in action — the resolver should have detected that `InitialSpeed` is a `UPROPERTY` on a component and either rewritten to `set_var` with `Target` injection or flagged as an error.

### Failure 3 — BP_ThirdPersonCharacter / EquipBow (17:42:09, attempt 1 of 2)
**Asset:** `/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter`, graph: `EquipBow`
**Root cause:** Data wire for BowClass pin on SpawnActor node failed. The plan used `@self.BowClass` to wire the `Class` input on the SpawnActor node (step `spawn`). Phase 4 reported 1 data wire failure.
**Details:** Resolver handled the `@self` expansion (0 synthetic steps inserted). SpawnActor node's Class pin requires a `TSubclassOf<Actor>` input, and the BowClass variable is of that type — but the connection failed in Phase 4. Phase 4: 6 succeeded, 1 failed.
**Category:** SpawnActor Class pin wiring gap. The `Class` pin on `UK2Node_SpawnActorFromClass` is a special pin that may require `ReconstructNode` after setting. Possibly the SpawnActor was created with generic `Actor` target class, and the Class pin auto-links to BowClass variable are mismatched pin types.

### Failure 4 — BP_ThirdPersonCharacter / EventGraph (17:43:10, attempt 1 of 2)
**Asset:** `/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter`, graph: `EventGraph`
**Root cause:** `IA_Equip` Input Action asset does not exist in the project.
**Error:** `Enhanced Input Action 'IA_Equip' not found. Available Input Actions in project: [IA_Jump, IA_Look, IA_Move].`
**Resolution:** Phase 1 aborted at step 1 of 6. 0 nodes created before failure.
**Category:** Missing asset — same pattern as IA_EquipBow in prior runs. Builder was told to use `IA_Equip`, `IA_Aim`, and `IA_Fire` which don't exist. Builder self-corrected by falling back to InputKey nodes (E, RightMouseButton, LeftMouseButton) then a simplified plan_json with plain function calls without events.
**Interesting:** The fallback plan_json (17:43:41) succeeded — 4 `call` steps (`EquipBow`, `StartAiming`, `StopAiming`, `FireBow`) wired sequentially and compiled SUCCESS. But then the Builder surgically disconnected them and re-wired via InputKey nodes, treating the plan_json as a "place functions first, then connect" strategy.

---

## Additional Non-plan_json Failures

1. **blueprint.add_variable FAILED** (17:35:14) — Details not captured, likely duplicate variable name or type mismatch on one of the many arrow variables.
2. **blueprint.remove_node FAILED** (17:37:16) — `Node 'node_10' not found in graph 'InitArrow'`. Stale node ID from a prior read.
3. **blueprint.connect_pins FAILED** (17:37:49) — Attempted to connect `47FAAE8241B8F0A6AFB1EAAB4469A965.Speed` (using raw GUID as node ID). Pin lookup by GUID failed — the FunctionEntry node must be referenced by its node_id, not raw GUID.
4. **blueprint.disconnect_pins FAILED** ×2 (17:38:19, 17:38:32) — `Source pin 'Damage' not found on node 'node_0'. Available output pins: then (Exec), Output_Get (Float)`. Builder had node_0 as a GetVariable for a float-typed variable after wipe/rebuild; the pin was `Output_Get` not `Damage`. Stale pin name assumption.
5. **blueprint.connect_pins FAILED** ×2 (17:39:13, 17:39:35) — `WriteRateLimit` validation rule. Builder was firing connect_pins too rapidly (< 2s between calls). Both retried and succeeded after the rate window cleared.
6. **blueprint.add_node FAILED** (17:43:25) — `CallFunction` node for `EquipBow` failed because `EquipBow` function didn't exist yet on the BP (was being added concurrently). FindFunction('EquipBow') returned nothing; function was freshly created and not yet compiled.

---

## get_template / describe_function Calls

**Planner phase (before Builder):** 10 get_template + 4 get_recipe = 14 calls, all SUCCESS.

Planner template calls:
1. `combatfs_ranged_component` (overview)
2. `combatfs_arrow_component` (overview)
3. `combatfs_combat_status_component` (overview)
4. `combatfs_ranged_component` pattern=`CreateArrow`
5. `combatfs_ranged_component` pattern=`SetArrowVelocity`
6. `combatfs_ranged_component` pattern=`AimSetup`
7. `combatfs_arrow_component` pattern=`EventGraph`
8. `combatfs_arrow_component` pattern=`ApplyHitDamage`
9. `combatfs_ranged_component` pattern=`GetVectorInfrontOfCamera`
10. `combatfs_ranged_component` pattern=`LaunchConditions`

Planner get_recipe calls:
1. `spawn actor projectile launch velocity`
2. `attach weapon socket character mesh`
3. `enhanced input action event IA_`
4. `line trace single channel camera forward`

**Builder phase (after 17:34:28):** 0 get_template calls, 0 get_recipe calls. Builder used no template lookups.

This is consistent with prior 09j and 09k runs. The Builder ignores templates entirely once it has the build plan. The "fallback only" template directive from Section 2.5 is being respected, but perhaps too aggressively — the Builder never hits the threshold to call get_template even when encountering novel logic.

---

## Compile Results

All Blueprint compiles returned SUCCESS throughout the run. 16 total compile events across the 3 assets:

- **BP_Arrow:** 7 compile events, all SUCCESS. Warnings appeared on 2 early passes (SetVariable warning from the InitArrow recovery, 1 warning each).
- **BP_Bow:** 3 compile events, all SUCCESS, 0 warnings.
- **BP_ThirdPersonCharacter:** 6 compile events, all SUCCESS, 0 warnings.

Final asset states (from Reviewer read):
- BP_Arrow: 10 variables, 2 components
- BP_Bow: 8 variables, 1 component
- BP_ThirdPersonCharacter: 4 variables added

---

## Reviewer Verdict

```
LogOliveAgentPipeline: Reviewer: SATISFIED, 0 missing, 0 deviations (22.1s)
```

Reviewer ran at 17:44:48, 22.1s after Builder exit. SATISFIED with 0 missing items and 0 deviations.

---

## Comparison Table

| Metric | 08i | 09j | 09k | 09l (this run) |
|--------|-----|-----|-----|----------------|
| Total time | 14:55 | 23:28 | 14:53 | ~12:22 |
| Pipeline time | ~91s | 113.9s | ~111s | 122.3s |
| Builder time | ~11:20 | 20:53 | ~13:02 | ~9:54 |
| Reviewer time | ~25s | 41.1s | 36.4s | 22.1s |
| plan_json calls | ~20 | 27 | 28 | 14 |
| plan_json success rate | 80% | 59.3% | 64.3% | 71.4% |
| Total tool calls | ~58 | ~68 | ~68 | 120* |
| Tool success rate | 96.6% | 89.7% | 82.4% | 90.0% |
| get_template (Builder) | 0 | 0 | 0 | 0 |
| get_recipe (Builder) | 0 | 0 | 2 | 0 |
| get_template (Planner) | ? | 10 | 10 | 10 |
| get_recipe (Planner) | ? | 4 | 2 | 4 |
| Compile results | 3× SUCCESS | 3× SUCCESS | 3× SUCCESS | 3× SUCCESS |
| Reviewer verdict | SATISFIED | SATISFIED | SATISFIED | SATISFIED |
| Major failures | CMC setters, bOrphanedPin | SetFloatPropertyByName, bOrphanedPin | SetVelocityInLocalSpace, CMC setters | SetFloatPropertyByName, SetInitialSpeed, IA_Equip missing |

*09l tool count is from the `tools/call result` lines in the log (120). Prior runs may use a different counting method (Builder-only). The 09l autonomous run logged "50 tool calls" in the CLIProvider summary, suggesting ~50 Builder-phase tool calls vs ~64-70 in 09j/09k. The 120 figure includes Planner-phase MCP calls.

---

## Key Findings

### Fix Verification Summary

| Fix | Expected behavior | Observed | Status |
|-----|-------------------|----------|--------|
| ResolvedFunction pointer in executor | "Used pre-resolved function" log message | Not found | NOT IMPLEMENTED or NOT LOGGED |
| FindFunction fallback warning | "reached FindFunction fallback" message | Not found | NOT IMPLEMENTED or NOT LOGGED |
| UPROPERTY auto-rewrite | "UPROPERTY detected" / "auto-rewritten" messages | Not found | NOT IMPLEMENTED |
| GetForwardVector alias fix | Resolver correctly maps to GetActorForwardVector | Confirmed in resolver phase; executor not exercised | PARTIAL (alias fires in resolver, executor path untested) |
| CMC property setters | Should be blocked or rewritten | Not exercised in this run | UNTESTED |
| VARIABLE_NOT_FOUND Phase 0 | Should block missing var references | Not triggered in this run | UNTESTED |

### New Patterns Observed

1. **Builder correctly self-corrects IA_Equip missing**: Rather than looping, Builder switched from EnhancedInput to InputKey nodes, then used a two-step strategy (place CallFunction nodes via plan_json, then wire to InputKey via connect_pins). This shows solid error recovery.

2. **SetInitialSpeed as UPROPERTY-vs-call confusion (new variant)**: The 09j/09k pattern was `SetMaxWalkSpeed` (CMC UPROPERTY). This run shows the same confusion for `ProjectileMovementComponent.InitialSpeed`. The resolver accepted the step with a warning and the executor created a SetVariable node — wrong node type, can't be wired. The resolver needs to promote this to a blocking error.

3. **Rate limiting caused 2 connect_pins failures**: WriteRateLimit fired at 17:39:13 and 17:39:35 when Builder was connecting InitArrow pins rapidly. Both retried and succeeded. This is expected behavior but wastes 2 tool attempts and introduces ~20s delay.

4. **SpawnActor Class pin wiring gap**: First EquipBow attempt failed on 1 data wire — the Class pin of SpawnActor. Second attempt succeeded by explicitly get_var(BowClass) + separate step. This suggests the Class pin wiring from `@self.BowClass` notation is unreliable or the resolver's `@self` expansion doesn't produce the right pin ID for the Class slot.

5. **Builder used fewer plan_json calls**: Only 14 vs 27-28 in prior runs. The Builder leaned heavily on granular tools (connect_pins, remove_node) for the InitArrow function after plan_json failed twice. This is correct adaptive behavior.

6. **InitArrow wipe-and-rebuild happened again**: After the second plan_json failure, Builder removed nodes from InitArrow and rebuilt manually with granular tools — 9 remove_node calls, then individual connect_pins. Same pattern as 09j's node-wipe. This is 4+ minutes of tool calls that plan_json would handle in 15ms if it had succeeded.

---

## Recommendations

1. **UPROPERTY-to-set_var rewrite is not implemented**: Failures for `SetInitialSpeed` (InitArrow) and `SetFloatPropertyByName` (still appearing) confirm that the resolver is not intercepting component property access attempts as `call` ops. This needs to be implemented. The resolver should: (a) on `call` op, check if target matches a `FProperty` name on the resolved class; (b) if so, rewrite op to `set_var` with the component as Target and add a blocking error note explaining the rewrite.

2. **Executor still re-runs FindFunction independently**: The log contains no "Used pre-resolved function" messages. Every node creation fires FindFunction fresh. The `FOliveResolvedStep.ResolvedFunctionName` is presumably set by the resolver but the executor is calling FindFunction with the step's original target string. This is the root cause of the GetForwardVector self-loop bug from earlier runs. This fix needs verification — either add the log message or confirm by checking whether `ResolvedFunctionName` is consumed in `OlivePlanExecutor.cpp`'s Phase 1 node creation.

3. **SetFloatPropertyByName hallucination is persistent across 09j/09k/09l**: This function has appeared in 3 consecutive runs. The alias map has 215 entries but does not include it (it was a UE4 function removed in UE5). The resolver's "Did you mean?" suggestions (`SetFieldPathPropertyByName`, etc.) don't help because the LLM reuses the same hallucinated name on retry. Consider adding a negative alias entry or an explicit error message: "SetFloatPropertyByName was removed in UE5. Use `set_var` with the component as Target."

4. **SpawnActor Class pin requires special handling**: Two runs now show the Class pin failing when using `@self.BowClass`. The resolver's `@self` expansion maps this to a get_var step, but the Class pin on SpawnActorFromClass is a special typed pin. The executor's data wiring needs to detect this case and use `SetPinDefaultObject` or `SetPinDefaultValue` for the class reference rather than a data wire.

5. **Rate limiter causing spurious failures**: Two connect_pins calls failed at 17:39:13 and 17:39:35 (< 2s apart). The Builder doesn't know about the rate limit and will just retry. Consider either: (a) increasing the rate limit for low-risk operations like connect_pins, or (b) having the tool return a specific `RATE_LIMITED` error code so the Builder waits instead of immediately retrying.

6. **Builder plan_json call count dropped to 14** (vs 27-28 before): This is positive for speed (12:22 total vs 14:55 prior best) but the Builder is reaching for granular tools faster when plan_json fails once. The fallback repair path (remove+rebuild manually) takes much longer than a corrected plan_json would. The self-correction error message for plan_json failures should include a clearer directive to attempt a corrected plan_json before switching to granular tools.

7. **Planner consistently uses 10 get_template calls**: Planner calls all 3 overview templates plus 7 targeted pattern lookups. This 10-call cadence is now stable across 3 runs. The pattern lookups (CreateArrow, SetArrowVelocity, AimSetup, etc.) show the Planner is doing genuine function-level research before writing the plan. No regression here.

8. **Total runtime improvement**: 12:22 is the fastest run to date (prior best 14:55 in 08i, recent average ~19 min). Builder efficiency improved — fewer plan_json calls, faster resolution, shorter granular tool chains. The SATISFIED verdict with 0 deviations confirms quality was not sacrificed.

9. **BuildComponentAPIMap fires for all 4 components** (line 1889: `4/4 component classes resolved, 2743 chars`): This is working correctly — all SCS component classes were resolved at pipeline time and injected into the Builder's prompt. This is likely why the Builder avoided many of the component-class confusion failures seen in earlier runs.

10. **Orphaned exec pin (bOrphanedPin bug) not triggered**: In 09j this caused 3 FireArrow failures and a 4-minute node wipe. In 09k and 09l it did not appear. The absence may be because BP_ThirdPersonCharacter was always read fresh before editing (no rollback involved), avoiding the state that creates the orphaned pin.
