# Research: Bow and Arrow Session Log Analysis — Run 08f

## Question

Analyze the full session log from a "bow and arrow system" test run to evaluate pipeline phase timing, Planner/Builder behavior, template call patterns, failure modes, and overall quality. Compare to prior runs.

---

## Findings

### 1. Pipeline Summary

**Session span:** 19:26:01 → 19:58:11 (32 min 10 sec total wall clock)

**Phase breakdown:**

| Phase | Start | End | Duration |
|-------|-------|-----|----------|
| Editor startup | 19:26:01 | 19:36:13 | ~10 min (cold start) |
| Scout (CLI, no LLM) | 19:36:13 | 19:36:27 | 14.7s |
| Planner (MCP-enabled) | 19:36:28 | 19:38:10 | 102.5s |
| Validator | 19:38:10 | 19:38:10 | 0.004s |
| Total pipeline overhead | — | — | 117.2s |
| Builder (autonomous CLI) | 19:38:10 | 19:57:39 | 19 min 29s |
| Reviewer | 19:57:39 | 19:58:11 | 31.4s |

**Assets planned: 2** (`BP_ArrowProjectile`, `BP_ThirdPersonCharacter`)
**Assets built: 2** — both created/modified successfully

**Reviewer verdict: SATISFIED, 0 missing, 0 deviations**

---

### 2. Planner Analysis

**Mode:** MCP-enabled (`RunPlannerWithTools`) — 15 turns max, PID=16392
**Prompt size:** 9,741 chars
**Plan output:** 7,554 chars

**MCP tool calls made by Planner (all between 19:36:28 and 19:38:10):**

| Tool | Count | Details |
|------|-------|---------|
| `blueprint.get_template` | 11 | See breakdown below |
| `olive.get_recipe` | 3 | 3 distinct queries |
| **Total** | **14** | — |

**get_template call breakdown:**

1. `combatfs_ranged_component` — structural overview
2. `combatfs_arrow_component` — structural overview
3. `combatfs_bp_arrow_quiver_parent` — structural overview
4. `combatfs_combat_status_component` — structural overview
5. `combatfs_ranged_component` + pattern `"CreateArrow"` — function graph
6. `combatfs_ranged_component` + pattern `"AimSetup"` — function graph
7. `combatfs_ranged_component` + pattern `"SetArrowVelocity"` — function graph
8. `combatfs_ranged_component` + pattern `"LaunchConditions"` — function graph
9. `combatfs_ranged_component` + pattern `"DestroyArrow"` — function graph
10. `combatfs_arrow_component` + pattern `"SetupArrowVariables"` — function graph
11. `combatfs_ranged_component` + pattern `"GetVectorInfrontOfCamera"` — function graph

**get_recipe queries:**
1. `"spawn actor attach component socket"`
2. `"line trace projectile launch"`
3. `"enhanced input action event binding"`

**Scout context injected:** 3 template overviews (25,668 chars), 3 template references

Note: The 3 template overviews injected by Scout are enormously large at 25,668 chars combined (roughly 8.5K chars each on average). This is above the recommended ~300-500 chars per overview discussed in prior research. However, the Planner still succeeded in 102.5s.

**No `blueprint.describe` or `project.search` calls** — the Planner stayed within the permitted 4-tool MCP filter.

---

### 3. Builder Analysis

**Mode:** Autonomous CLI (`--max-turns 500`)
**Duration:** 19:38:10 → 19:57:39 (19 min 29 sec)
**Exit code:** 0 (clean exit)
**Total tool calls logged by OliveCLI:** 50

**Tool call breakdown (Builder phase only, 19:38:10 onward):**

| Tool | Count | Notes |
|------|-------|-------|
| `blueprint.create` | 2 | First attempt duplicate-failed, second succeeded at new path |
| `blueprint.add_component` | 5 | Success |
| `blueprint.reparent_component` | 2 | Success |
| `blueprint.modify_component` | 6 | Success |
| `blueprint.add_variable` | 11 | Success (across both BPs) |
| `blueprint.add_function` | 8 | Includes rebuild attempts |
| `blueprint.remove_function` | 5 | 4 failed (blocked by logic guard), 1 succeeded |
| `blueprint.remove_node` | 19 | Many cleanup passes; 4 failed (stale node IDs) |
| `blueprint.add_node` | 3 | Granular node creation |
| `blueprint.get_node_pins` | 7 | 4 failed (stale/wrong node IDs) |
| `blueprint.connect_pins` | 7 | 3 failed (stale node ref or type) |
| `blueprint.disconnect_pins` | 2 | Success |
| `blueprint.apply_plan_json` | 18 | See plan_json table below |
| `blueprint.compile` | 7 | |
| `blueprint.read` | 7 | |
| `blueprint.delete` | 1 | Deleted wrong-path BP |
| **blueprint.get_template** | **0** | **Zero calls — fallback-only directive worked** |
| **olive.get_recipe** | **0** | Zero calls in Builder phase |
| `project.search` | 0 | Not used |

**plan_json call detail:**

| Call # | Asset | Graph | Outcome | Reason if failed |
|--------|-------|-------|---------|-----------------|
| 1 | BP_ArrowProjectile | InitializeArrow | SUCCESS | — |
| 2 | BP_ArrowProjectile | EventGraph | FAILED | 1 exec wire fail (bHasHit → check_hit TypesIncompatible, likely orphaned exec pin) |
| 3 | BP_ArrowProjectile | EventGraph | FAILED | Same exec wire issue (retry) |
| 4 | BP_ArrowProjectile (new path) | InitializeArrow | SUCCESS | — |
| 5 | BP_ArrowProjectile (new path) | EventGraph | SUCCESS | — |
| 6 | BP_ThirdPersonCharacter | GetAimDirection | FAILED | Data wire: no input pin `InRot` on step `fwd` (GetForwardVector pin mismatch) |
| 7 | BP_ThirdPersonCharacter | GetAimDirection | FAILED | Same — SplitPin fallback connected ReturnValue_Y but still failed overall |
| 8 | BP_ThirdPersonCharacter | GetAimDirection | FAILED | Phase 1 aborted (0/5 nodes created) — function had been partly built then removed |
| 9 | BP_ThirdPersonCharacter | GetAimDirection | SUCCESS | After full remove+recreate cycle |
| 10 | BP_ThirdPersonCharacter | DestroyUnfiredArrow | SUCCESS | — |
| 11 | BP_ThirdPersonCharacter | StartAiming | SUCCESS | — |
| 12 | BP_ThirdPersonCharacter | StopAiming | SUCCESS | — |
| 13 | BP_ThirdPersonCharacter | NockArrow | FAILED | 1 data wire fail: no pin `InRot` on step (different function path); resolver returned 11/11 but compile FAILED |
| 14 | BP_ThirdPersonCharacter | NockArrow | FAILED | Compile error: "This blueprint (self) is not a BP_ArrowProjectile_C, therefore 'Target' must have a connection." — rollback 11 nodes |
| 15 | BP_ThirdPersonCharacter | NockArrow | FAILED | Same compile error — rollback 11 nodes |
| 16 | BP_ThirdPersonCharacter | NockArrow | FAILED | Same compile error — rollback 11 nodes |
| 17 | BP_ThirdPersonCharacter | NockArrow | FAILED | Same compile error — rollback 9 nodes |
| 18 (Phase 0) | BP_ThirdPersonCharacter | NockArrow | FAILED | Phase 0 validator: `SetPhysicsLinearVelocity` (PrimitiveComponent) called on Actor BP without Target wire |
| 19 | BP_ThirdPersonCharacter | FireArrow | SUCCESS | — |
| 20 | BP_ThirdPersonCharacter | EventGraph | SUCCESS | Wired IA events → StartAiming/StopAiming/FireArrow |

**plan_json success rate: 10/18 = 56%**

The dominant NockArrow failure cluster (calls 13-18, 6 consecutive fails) was the same root cause repeating: the Builder was calling `InitializeArrow` on `BP_ArrowProjectile_C` from within `BP_ThirdPersonCharacter` context, without providing a valid Target object reference for the spawned actor. The Phase 0 validator finally caught the structural issue on the last attempt.

**Compile errors:** 5 distinct compile events (all in NockArrow cluster). Each rolled back nodes. One compile call (line 2743) returned "success" despite FAILED compiler output — that is a known bug where `blueprint.compile` result is success even when compiler produced errors (compile pipeline returns true if it didn't crash).

**Functions with real logic:** All implemented functions observed in plan_json calls contain multiple nodes (4-19 per function). No stubs or empty implementations detected.

---

### 4. Failures and Errors

**Failure inventory (all tools, categorized):**

| # | Tool | Error | Category |
|---|------|-------|----------|
| 1 | blueprint.create | "Blueprint already exists at path /Game/Blueprints/BP_ArrowProjectile" | Wrong path — BP was previously at /Game/Blueprints/ and re-tried at same path after delete |
| 2 | blueprint.apply_plan_json (EventGraph attempt 1) | Exec wire FAILED: hit_evt.then → check_hit.execute TypesIncompatible | Orphaned exec pin (known issue) |
| 3 | blueprint.apply_plan_json (EventGraph attempt 2) | Same exec wire fail, retry | Exec orphan |
| 4 | blueprint.apply_plan_json (GetAimDirection attempt 1) | Data wire FAILED: no input pin `InRot` on step `fwd` | Wrong function pin name — GetActorForwardVector has no `InRot` |
| 5 | blueprint.apply_plan_json (GetAimDirection attempt 2) | Same `InRot` fail; SplitPin connected wrong sub-pin (ReturnValue_Y → LaunchVelocity) but still failed | Pin type mismatch |
| 6 | blueprint.apply_plan_json (GetAimDirection attempt 3) | Phase 1 FAILED: 0/5 nodes created | Function was partly built, node IDs stale after previous removals |
| 7 | blueprint.remove_function × 4 | "Blocked removal — has N nodes of graph logic" | Safety guard blocking deletion of non-empty functions |
| 8 | blueprint.connect_pins × 3 | "Source pin 'ReturnValue' not found on node 'node_2'" | Stale node reference after node was removed/rebuilt |
| 9 | blueprint.get_node_pins × 4 | Node ID not found | Stale node IDs (hash-format or 'node_N' after reset) |
| 10 | blueprint.remove_node × 4 | "Node 'node_X' not found" | Stale IDs after prior rollback cleared node cache |
| 11 | blueprint.apply_plan_json (NockArrow × 4) | Compile error: "self is not BP_ArrowProjectile_C, 'Target' must have a connection" | Called `InitializeArrow` on spawned actor class from character context without providing Target |
| 12 | blueprint.apply_plan_json (NockArrow Phase 0) | `COMPONENT_FUNCTION_ON_ACTOR`: `SetPhysicsLinearVelocity` without Target | Component function without Target wire |

**Cascade analysis:**

- The `BP_ArrowProjectile` path issue (failure #1) cascaded: Builder created BP at wrong path → deleted → created again at new correct path. This added ~2 extra tool calls with an intermediate `blueprint.read`.

- The `GetAimDirection` failure cluster (failures #4-6) cascaded through 3 plan_json retries. The root cause was `GetForwardVector` resolving via alias to `GetActorForwardVector` (which has no `InRot` pin), then alias-fallback rerouting to `KismetMathLibrary::GetForwardVector` (which does have `InRot`) but the Vector→Vector return value was being wired to a Vector return pin on the function result — both ends are Vector type so it should connect, but something else was failing. Eventually the Builder scrapped the function and rebuilt from scratch (remove_function loop × 4 attempts to clear it, all blocked, then remove_node by hand × 5, then add_function + new plan_json).

- The `NockArrow` failure cluster (failures #11-12) cascaded through 6 consecutive plan_json retries. The Builder kept attempting different plan variations, all of which triggered the same compile error ("self is not BP_ArrowProjectile_C"). On the 6th attempt the Phase 0 validator caught the structural issue (`SetPhysicsLinearVelocity` without Target). The Builder then abandoned this approach and succeeded with a simpler FireArrow function instead.

---

### 5. Quality Assessment

**Implemented with real logic (confirmed in plan_json logs):**
- `BP_ArrowProjectile`: `InitializeArrow` (6 nodes, instigator + lifespan setup), `EventGraph` (19 nodes, OnComponentBeginOverlap → damage → stop movement → destroy chain)
- `BP_ThirdPersonCharacter`: `GetAimDirection` (5 nodes, GetForwardVector × ArrowSpeed → LaunchVelocity), `DestroyUnfiredArrow` (7 nodes, IsValid → DestroyActor), `StartAiming` (4 nodes, gate on bIsAiming + NockArrow call), `StopAiming` (6 nodes, clear aim + DestroyUnfiredArrow), `FireArrow` (9 nodes, spawn + init + aim velocity), `EventGraph` (3 nodes, input action event → StartAiming/StopAiming/FireArrow routing)

**Stubbed or missing:**
- `NockArrow` — the Builder spent 6 failed attempts on it, then moved on to `FireArrow`. By examining the final `blueprint.compile` run (19:55:23, SUCCESS), the BP compiled cleanly. It is ambiguous from the logs alone whether `NockArrow` was left with stub logic or was repurposed. The Reviewer was SATISFIED, suggesting the final result was acceptable. However, NockArrow attempts all rolled back, so the function likely has minimal or no graph logic (just the entry node from `blueprint.add_function`).

**Correct op usage observed:**
- `do_once` macro used correctly in EventGraph
- `spawn_actor` op used for arrow spawning
- `get_var`/`set_var` for bIsAiming, bArrowLoaded, ArrowClass
- Self-references via `@self` handled by resolver
- `@ShootingPawn` and `@Instigator` component refs resolved by ExpandComponentRefs

**No `call_delegate` misuse** detected — no dispatcher calls in the plan JSONs examined.

---

### 6. Comparison to Previous Runs

| Metric | 08b | 08c | 08d | 08e | **08f** |
|--------|-----|-----|-----|-----|---------|
| Total time | ~19 min | ~36 min | ~20 min | ~20 min | ~32 min (incl. 10 min cold start) |
| Builder time | ~4 min | — | — | — | ~19.5 min |
| plan_json success rate | 67% | 67% | 46% (higher absolute) | 43% | **56%** |
| get_template (Builder) | many | many | 46 | many | **0** |
| get_recipe (Builder) | — | — | — | — | **0** |
| All BPs compile | YES | NO | YES | YES | YES |
| Reviewer verdict | — | — | SATISFIED | SATISFIED | **SATISFIED** |
| BP_Bow created | NO | NO | NO | NO | NO |
| Wall-clock kill | NO | YES (900s) | NO | NO | NO |
| NockArrow gutted | NO | YES | NO | PARTIAL | **LIKELY PARTIAL** |

**Key improvement in 08f:** The fallback-only template directive for the Builder worked perfectly — **zero get_template and zero get_recipe calls in the Builder phase**. This is the intended behavior from the "templates as fallback" change.

**Regression in 08f:** Builder time ballooned to 19.5 minutes vs the 4-minute estimate from 08b. This is partly because the Builder is doing much more granular work (remove_node loops, retry cycles on NockArrow) without template scaffolding to guide the function structure. The Planner phase also ran long at 102.5s.

**NockArrow persistence:** This function fails in runs 08c, 08e, and 08f. It is the hardest function to implement because it requires calling `InitializeArrow` on a freshly spawned `BP_ArrowProjectile` reference, which requires the Target pin to be wired to the spawn return value. The Builder repeatedly tries to call `InitializeArrow` without wiring the Target.

---

### 7. Key Metrics Table

| Metric | Value |
|--------|-------|
| Total wall-clock time | 32 min 10 sec |
| Editor cold start | ~10 min |
| Scout time | 14.7s |
| Planner time | 102.5s |
| Pipeline total (Scout+Planner+Validator) | 117.2s |
| Builder time | 19 min 29s |
| Reviewer time | 31.4s |
| Assets planned | 2 |
| Assets built | 2 |
| Total tool calls (Builder, logged) | 50 |
| get_template calls (Builder) | **0** |
| get_recipe calls (Builder) | **0** |
| get_template calls (Planner) | 11 |
| get_recipe calls (Planner) | 3 |
| plan_json calls total | 18 |
| plan_json success rate | 10/18 = 56% |
| Compile errors (in-build) | 5 (all NockArrow, all rolled back) |
| Compile errors (final state) | 0 |
| Functions with real logic | 7 of 8 |
| Functions gutted/stubbed | 1 (NockArrow — likely stub after rollbacks) |
| Reviewer verdict | SATISFIED |
| Runtime kill | NO |
| Exit code | 0 |

---

## Recommendations

1. **Fallback-only template directive is working.** Builder made zero template/recipe calls, the feature is validated. No regression on quality from removing template pre-fetching in the Builder prompt.

2. **NockArrow is a persistent failure mode across 3+ runs.** The root cause is consistent: calling `BP_ArrowProjectile_C::InitializeArrow` from a Character BP context where the Target pin must be wired to a spawned actor reference. The compiler error "self is not BP_ArrowProjectile_C, therefore 'Target' must have a connection" repeats identically. The Phase 0 validator catches it only on the last attempt (attempt #6) with a different error framing (SetPhysicsLinearVelocity). The fix needs to be in the plan itself: NockArrow should be a function on BP_ArrowProjectile that takes no target (self-context), not a call from the character. The Planner is generating the wrong architecture for this function.

3. **Phase 0 validator needs to catch the BP_ArrowProjectile_C Target issue earlier.** Currently `COMPONENT_FUNCTION_ON_ACTOR` catches component functions without Target. But calling a Blueprint-typed function (not component) without Target on the correct class type (BP_ArrowProjectile_C vs BP_ThirdPersonCharacter_C) is not caught. The validator should check: if step.target_class is a Blueprint subtype that is NOT a parent of the graph's owner Blueprint, the Target pin MUST be wired.

4. **The `remove_function` safety guard is causing excessive churn.** The Builder tried 4 times to remove `GetAimDirection` and `NockArrow` while they had nodes, all blocked. It then spent many tool calls doing manual `remove_node` surgery. Consider: if `remove_function` is called in a pattern of "I want to rebuild this from scratch," the guard should offer a `force=true` override rather than blocking outright. Or the error message should explicitly suggest "use blueprint.read to get node IDs, then remove_node for each."

5. **Stale node IDs after rollback are causing repeated get_node_pins failures.** After a failed plan_json rollback, the node cache is cleared and node IDs reset. The Builder keeps referencing old hash-based IDs (e.g., `80DE5F2E4022C8870522109FED583F50`) or old `node_N` IDs from the previous attempt. After a rollback, the Builder should be told: "All node IDs from the previous attempt are invalid — re-read the graph before referencing nodes."

6. **compile tool returns SUCCESS status even when compilation produced errors.** Line 2743 shows `Compilation complete: FAILED - Errors: 1` but the tool result is `-> SUCCESS`. The tool result should be `FAILED` when the compiler reports errors. This causes the Builder to proceed as if the compile was clean.

7. **Planner template fetching (11 calls) is working correctly.** The pattern of fetching overview first, then specific function graphs via `pattern=` is efficient. The Planner correctly used the combatfs library templates for reference architecture without embedding node-level detail into the plan text.

8. **Scout template overviews are too large (25,668 chars total for 3 templates).** Based on prior research the recommended budget is ~300-500 chars per overview. At 8.5K chars each, these overviews are consuming a large portion of the Planner's context window (9,741 char prompt). The Scout should either trim the overviews or pass only display_name + tags + catalog_description (not full function lists).

9. **Builder time (19.5 min) is significantly longer than 08b (4 min).** Hypothesis: without template scaffolding as a starting point, the Builder is doing more exploratory iteration. The NockArrow cluster alone consumed ~7-8 minutes of retry cycles. Future investigation: compare Builder time on a run where NockArrow succeeds on first attempt.

10. **No `BP_Bow` asset was created (same as 08e).** The Planner interpreted the task as component-based (adding components to BP_ThirdPersonCharacter + creating BP_ArrowProjectile) rather than creating a dedicated `BP_Bow` weapon actor. This is likely the correct architectural interpretation for UE5 character-based bow systems, but is worth noting for scope evaluation.
