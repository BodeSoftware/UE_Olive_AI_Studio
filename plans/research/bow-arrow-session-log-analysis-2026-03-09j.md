# Research: Bow-Arrow Session Log Analysis — Run 09j (2026-03-09)

## Question
Comprehensive analysis of the latest bow-and-arrow generation pipeline run: task, timing, tool statistics, plan_json success rate, get_template usage, new tool usage, failure categorization, compile results, and reviewer verdict. Compare to prior baselines.

---

## Findings

### 1. Task
**User request:** `create a bow and arrow system for @BP_ThirdPersonCharacter`

**Assets created/modified:**
- `/Game/Weapons/BP_Bow` (new, Actor)
- `/Game/Weapons/BP_ArrowProjectile` (new, Actor)
- `/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter` (modified — added variables, functions, event graph logic)

**3 assets planned, 3 touched.** No scope miss this run — BP_Bow was correctly created as a separate weapon actor.

---

### 2. Pipeline Timing

All timestamps relative to `06:55:03` (run start — auto-snapshot taken).

| Phase | Start | End | Duration |
|-------|-------|-----|----------|
| Scout (CLI, no LLM) | 06:55:03 | 06:55:18 | **14.6s** |
| Planner (MCP, 15-turn) | 06:55:18 | 06:56:57 | **99.3s** (1:39) |
| CLI pipeline total | — | — | **113.9s** (1:54) |
| Builder (autonomous run) | 06:56:57 | 07:17:50 | **~20:53** |
| Reviewer | 07:17:50 | 07:18:31 | **41.1s** |
| **Total wall clock** | 06:55:03 | 07:18:31 | **~23:28** |

Notes:
- Scout loaded 3 template overviews (25,668 chars), 3 references.
- Planner received a 22,056-char prompt and produced an 11,621-char plan (3 assets).
- Builder tools/list returned 53/86 tools (filtered by 6 prefixes).
- Autonomous run completed with exit code 0, 50 tool calls logged, 12,152 chars accumulated.

---

### 3. Tool Call Statistics

Total MCP tool calls observed (Builder phase only, from tool result lines):

| Tool | Calls | Success | Failed | Notes |
|------|-------|---------|--------|-------|
| blueprint.get_template | **10** | 10 | 0 | **All Planner phase** (see §5) |
| olive.get_recipe | 4 | 4 | 0 | All Planner phase |
| blueprint.create | 2 | 2 | 0 | |
| blueprint.add_component | 6 | 6 | 0 | |
| blueprint.add_variable | 18 | 15 | 3 | 2× WriteRateLimit throttle, 1× WriteRateLimit |
| blueprint.add_function | 16 | 14 | 2 | 1× WriteRateLimit, 1× unknown |
| blueprint.apply_plan_json | **27** | **16** | **11** | See §4 |
| blueprint.modify_component | 2 | 2 | 0 | |
| blueprint.compile | 5 | 4 | 1 | 1× compile error (ActorComponent Target) |
| blueprint.read | 19 | 19 | 0 | Includes paged reads of ThirdPersonChar EventGraph |
| blueprint.remove_node | **46** | 44 | 2 | 2× WriteRateLimit (large node-wipe operation) |
| blueprint.remove_function | 2 | 1 | 1 | 'FireArrow' not found (hallucinated) |
| blueprint.add_node | 4 | 4 | 0 | |
| blueprint.get_node_pins | 3 | 1 | 2 | Node ID lookup failures |
| blueprint.disconnect_pins | 2 | 2 | 0 | |
| blueprint.connect_pins | 4 | 4 | 0 | |
| olive.search_community_blueprints | 5 | 5 | 0 | All Scout phase |
| **TOTAL** | **175** | **157** | **18** | **~89.7% overall tool success** |

Notes:
- The 46 `remove_node` calls were a large deliberate node-wipe on BP_ThirdPersonCharacter's EventGraph (the Builder read the existing 50-node event graph page-by-page then mass-deleted nodes before rebuilding).
- `blueprint.describe_function` — **zero calls** (not used this run).
- `@self` syntax — **zero explicit `@self` calls by the Builder**. The `@self.auto` refs seen in logs were in Planner-authored IR (resolver notes). The Builder used `@self.auto` in its own plan_json steps but this appears to be the already-supported resolver syntax, not a new dedicated tool.

---

### 4. plan_json Success Rate

**27 apply_plan_json calls total.**

| Outcome | Count |
|---------|-------|
| SUCCESS | 16 |
| FAILED | 11 |
| **Success rate** | **59.3%** |

#### Failed call breakdown (11 failures):

| # | Asset | Graph / Function | Root Cause |
|---|-------|-------|-----------|
| 1 | BP_Bow | InitBow | Data wire failed: AttachToComponent parent pin — `GetMesh` returned `UObject*` not castable to `SceneComponent*` directly (1 wire fail → rollback) |
| 2 | BP_ArrowProjectile | InitArrow | `SetFloatPropertyByName` function not found (×3 steps), `TrailParticle` variable not found. **Source: Planner's build plan** (hallucinated function, not Builder invention) |
| 3 | BP_ArrowProjectile | InitArrow | `TrailParticle` variable not found (still referencing non-existent var after modification) |
| 4 | BP_ArrowProjectile | InitArrow | Compile FAILED: "This blueprint (self) is not a ActorComponent, therefore 'Target' must have a connection." — `SetComponentTickEnabled` called without explicit Target wire |
| 5 | BP_ArrowProjectile | ApplyHitDamage | `@self.auto` on `DamageCauser` — execution failed (connection fail) |
| 6 | BP_ArrowProjectile | ApplyHitDamage | Same pattern (retry) |
| 7 | BP_ThirdPersonCharacter | EventGraph / **FireArrow** | `fire_evt.then → check_valid.execute: TypesIncompatible`. **Root cause: orphaned exec pin** on `fire_evt` custom event node (bOrphanedPin=true from prior rolled-back plan wiring). First attempt (17 steps) also failed on `InRot` pin type mismatch. |
| 8 | BP_ThirdPersonCharacter | EventGraph / **FireArrow** | Same orphaned exec pin. Second retry (16 steps). |
| 9 | BP_ThirdPersonCharacter | EventGraph / **FireArrow** | Same orphaned exec pin. Third retry (10 steps). Builder then spent ~2 min reading graph, tried `remove_function 'FireArrow'` (failed — it's a custom event, not a function graph), then wiped 46 nodes. |
| 10 | BP_ThirdPersonCharacter | EventGraph | IA_EquipBow not found: "Enhanced Input Action 'IA_EquipBow' not found. Available: [IA_Jump, IA_Look, IA_Move]" |
| 11 | BP_ThirdPersonCharacter | EventGraph | (Earlier attempt, same data wiring issue) |

**Dominant failures:**
1. **Hallucinated function: `SetFloatPropertyByName`** — This function does not exist in UE5. It appeared in the **Planner's build plan** (not invented by the Builder). Intended to set ProjectileMovementComponent properties (InitialSpeed, MaxSpeed, GravityScale) via reflection. Not in alias map. Builder self-corrected by switching to `blueprint.modify_component` after 3 failures.
2. **`TrailParticle` variable reference** — Planner planned a `TrailParticle` variable; Builder skipped it during the variable-creation phase. First plan_json attempt failed when it tried to reference it. After failure, dropped the trail activation entirely.
3. **FireArrow orphaned exec pin (3×)** — Three consecutive `apply_plan_json` failures on the FireArrow EventGraph (07:05:57–07:06:42). Root cause: `fire_evt` custom event node had `bOrphanedPin=true` on its `then` exec output from a prior rolled-back plan_json attempt that had wired it. The resolver correctly reused the existing `FireArrow` node but did not clear the orphaned pin state. Error message: `"fire_evt.then → check_valid.execute: Cannot connect Exec to Exec: TypesIncompatible"` — a misleading diagnostic (the real issue is the orphaned flag). NockArrow itself succeeded; it was FireArrow that failed.
4. **IA_EquipBow missing** — Builder tried to use an Input Action asset that doesn't exist in the project. Self-corrected by using `blueprint.add_node` for raw `InputKeyEvent` nodes instead.
5. **ActorComponent Target** — `SetComponentTickEnabled` called without explicit Target wired on a non-ActorComponent BP (compile error caught by pipeline).

---

### 5. get_template Calls — Planner vs Builder Split

Total `get_template` calls: **10** — all occurred during **Planner phase** (06:55:28–06:55:43), before Builder launch.

| Call | Template ID | Pattern |
|------|------------|---------|
| 1 | combatfs_ranged_component | (overview) |
| 2 | combatfs_arrow_component | (overview) |
| 3 | combatfs_combat_status_component | (overview) |
| 4 | combatfs_ranged_component | CreateArrow |
| 5 | combatfs_ranged_component | SetArrowVelocity |
| 6 | combatfs_ranged_component | AimSetup |
| 7 | combatfs_ranged_component | LaunchConditions |
| 8 | combatfs_arrow_component | SetupArrowVariables |
| 9 | combatfs_arrow_component | ApplyHitDamage |
| 10 | combatfs_arrow_component | EventGraph |

**Builder get_template calls: 0.** The Builder made zero direct `get_template` calls this run — it relied entirely on what the Planner injected into the build plan. This is a significant improvement over 08g (13 get_template by Planner, 0 by Builder) and confirms the "fallback-only" directive is working for the Builder.

---

### 6. New Tools / Syntax

**`blueprint.describe_function`:** Zero calls. Not used.

**`@self` syntax:** Four occurrences of `@self.auto` appeared in Builder plan_json steps (e.g., `DamageCauser: "@self.auto"`, `NewOwner: "@self.auto"`). These are the existing resolver-supported syntax for passing `self` as a function argument, not a new tool. The IR schema validator logs these as warnings ("references unknown step 'self' — may be component/param name (resolver will handle)") and the resolver handles them. This is expected behavior.

**Large node-wipe pattern (new behavior observed):** The Builder performed a 46-call `remove_node` sweep on BP_ThirdPersonCharacter's EventGraph between 07:11:51 and 07:15:39. This appears to be the Builder reading the existing 50-node event graph (5 pages at 10 nodes each, then one full 50-node read at 07:10:10), identifying unwanted nodes, and manually deleting them before laying down new logic with `apply_plan_json`. This is expensive (46 calls + ~4 minutes) but effective — subsequent plan_json on that graph succeeded.

---

### 7. Compile Results

| Blueprint | Final Status | Details |
|-----------|-------------|---------|
| BP_Bow | SUCCESS | Compiled clean |
| BP_ArrowProjectile | SUCCESS (final) | Had 1 compile error mid-run (ActorComponent Target), resolved |
| BP_ThirdPersonCharacter | **SUCCESS** | 0 errors, 0 warnings — `07:17:38:386` |

All 3 blueprints compiled successfully at run end.

**Compile error encountered (resolved):**
- `BP_ArrowProjectile` at 07:00:08: "This blueprint (self) is not a ActorComponent, therefore 'Target' must have a connection." — caused by `SetComponentTickEnabled` without explicit Target. Builder self-corrected by removing the offending nodes (2× `remove_node`) and recompiling.

---

### 8. Reviewer Verdict

```
LogOliveAgentPipeline: Reviewer: SATISFIED, 0 missing, 0 deviations (41.1s)
```

**SATISFIED. Zero missing items. Zero deviations.** Reviewer took 41.1s.

---

### 9. Comparison to Prior Runs

| Metric | 08g | 08h | 08i | **09j (this run)** |
|--------|-----|-----|-----|----------|
| Total time | 13:39 | 20:10 | 14:55 | **23:28** |
| Pipeline (Scout+Planner) | 91.8s | ~120s | ~90s | **113.9s** |
| Builder time | ~11:45 | ~17:50 | ~12:45 | **~20:53** |
| Reviewer time | 24.1s | ~25s | ~25s | **41.1s** |
| plan_json success rate | 55% (12/22) | 50% (10/20) | 80% (12/15) | **59.3% (16/27)** |
| Overall tool success | ~88% | ~95% | ~96.6% | **~89.7%** |
| Builder get_template calls | 0 | 0 | 0 | **0** |
| Planner get_template calls | 10 | ~8 | ~6 | **10** |
| Assets planned | 3 | 2 | 2 | **3** |
| Reviewer verdict | SATISFIED | SATISFIED | SATISFIED | **SATISFIED** |
| Final compile | All SUCCESS | All SUCCESS | All SUCCESS | **All SUCCESS** |

**Regressions vs 08i:**
- plan_json success rate dropped from 80% back to 59.3% — a significant regression.
- Total time increased from 14:55 to 23:28 (+8:33) — largely the 46-node wipe on ThirdPersonChar.
- The node-wipe pattern (46 `remove_node` calls) consumed ~4 minutes and was not seen in prior runs — new behavior, possibly introduced by the Builder seeing a 50-node pre-existing EventGraph and deciding to clear it.

**Improvements vs 08g:**
- `SetFloatPropertyByName` hallucination now self-corrects faster (Builder switched to `modify_component`).
- IA_EquipBow missing asset detected and worked around with raw input key events.

---

## Failure Analysis Summary

| Failure Category | Occurrences | Category |
|-----------------|-------------|---------|
| Hallucinated function (`SetFloatPropertyByName`) — from Planner | 3 | Planner knowledge gap (function doesn't exist in UE5) |
| Variable not yet created (`TrailParticle`) — Builder skipped it | 2 | Ordering / premature reference |
| FireArrow orphaned exec pin (`fire_evt.then` bOrphanedPin=true) | 3 | Infrastructure bug — misleading "TypesIncompatible" error |
| Missing Input Action asset (`IA_EquipBow`) | 1 | Missing asset reference |
| ActorComponent Target unconnected | 1 | Missing Target wire on component call |
| `remove_function` on custom event (not a function graph) | 1 | Tool misuse — custom events are not in FunctionGraphs |
| Rate limit throttle (`WriteRateLimit`) | 5 | Throughput / burst writing |

**The FireArrow orphaned exec pin is an infrastructure bug.** It is the primary driver of the 46-node wipe (which adds ~8 minutes to total runtime). Without this bug, the Builder would not have accumulated 3 failures → debug loop → wipe decision. The "TypesIncompatible" error message is misleading — the real cause is `bOrphanedPin=true` on the `fire_evt` exec output pin, which is set after a rolled-back plan_json attempt and never cleared by the resolver's node-reuse path.

**NockArrow itself succeeded** on first attempt (07:05:35). The "NockArrow failure" label in MEMORY.md was incorrect — it should read "FireArrow orphaned exec pin failure."

The **large node-wipe** (46 `remove_node` calls, 07:11:51–07:15:39) is a new behavior that significantly inflated total runtime. The trigger chain was:
1. **3× FireArrow plan_json failures** (07:05:57–07:06:42) due to orphaned exec pin on `fire_evt` node
2. Builder tried `get_node_pins` with stale UUID-style node ID — FAILED
3. Builder tried `remove_function 'FireArrow'` — FAILED ("Function 'FireArrow' not found" — it's a custom event, not a function graph)
4. Builder read EventGraph 5 times in sequence (summary, pages 0/1/2/3, then full 50-node read at 07:10:10)
5. ~100 seconds of Builder thinking with the 50-node full graph in context (07:10:10–07:11:51)
6. Mass deletion: 46 `remove_node` calls over ~4 minutes (with WriteRateLimit pauses at 30 ops/min)
7. After wipe: 5 EventGraph nodes remaining, 7 function graphs untouched
8. Post-wipe plan_json for NockArrow and FireArrow both succeeded on first attempt

This was an **autonomous Builder decision**, not part of the Planner's plan. The Builder determined that the accumulated failed-plan debris made surgical repair impossible and chose to wipe and rebuild from scratch.

---

## Recommendations

1. **Fix the orphaned exec pin bug — this is the primary regression driver.** When a plan_json attempt fails and rolls back, any exec pins that were wired during that attempt may be left with `bOrphanedPin=true`. The resolver's node-reuse path (for custom events and other existing nodes) does not clear this flag before attempting new connections. Fix: in `PhaseWireExec`, before connecting `Pin->bOrphanedPin` pins, call `OwningNode->ReconstructNode()` to clear the orphaned state. Also add a specific `EOliveWiringFailureReason::OrphanedPin` to `BuildWiringDiagnostic` (the current fall-through gives the misleading "TypesIncompatible" message). See `plans/research/spawn-actor-expose-on-spawn-and-exec-rollback.md` for the documented fix approach.

2. **The 46-node wipe consumed ~8 minutes of the 23:28 total runtime.** Without the orphaned exec pin bug, the Builder would not have hit 3 failures → entered the debug loop → decided to wipe. Fixing #1 directly eliminates the wipe. Independently, adding a `blueprint.clear_graph` or `replace_graph=true` plan_json mode would let the Builder make that decision much faster (1 call instead of 46+).

3. **`SetFloatPropertyByName` came from the Planner, not the Builder.** The Planner hallucinated a reflection-based property setter that doesn't exist in UE5. Add `SetFloatPropertyByName` to the alias map with a descriptive error: "Not a UE5 function. To set ProjectileMovementComponent properties, use blueprint.modify_component." Also consider adding this to the Planner's knowledge injection block alongside other common "doesn't exist" patterns.

4. **TrailParticle variable ordering failure was a Builder omission.** The Planner planned for it; the Builder skipped it. Phase 0 validation could catch forward references to variables that don't exist on the BP yet — a check similar to the COMPONENT_FUNCTION_ON_ACTOR validation. Alternatively, enforce in the IR that variables must be declared before being referenced in function plans.

5. **`remove_function` fails on custom events.** The Builder tried `remove_function 'FireArrow'` and got "Function 'FireArrow' not found." Custom events are not in `FunctionGraphs` — they live on the EventGraph. The tool's error message should distinguish: "FireArrow is a custom event, not a function. Use blueprint.remove_node with the node_id of the custom event node."

6. **plan_json success rate regressed to 59.3% from 80%.** The primary drivers were: FireArrow orphaned exec pin (3 failures → wipe cascade) and SetFloatPropertyByName (3 failures). Without those two clusters, the rate would be 22/27 = 81.5%, matching 08i. Fix #1 eliminates the first cluster; fix #3 addresses the second.

7. **Reviewer: SATISFIED with 0 deviations** despite all mid-run turbulence. The self-correction loop is effective at the outcome level. The cost is time, not correctness.

8. **Planner `get_template` count (10) is unchanged from 08g.** The pattern of 3 overview fetches + 7 targeted function fetches is consistent and appropriate. No changes needed here.

9. **WriteRateLimit throttle caused 5 failures** (2× add_variable, 1× add_function, 2× remove_node). These are transient — Builder retried and succeeded. The 30 ops/min limit is hitting the ceiling during burst phases. The 46-node wipe was the worst offender (must pause mid-wipe). Raising the limit or adding a `blueprint.clear_graph` would help.

10. **IA_EquipBow self-correction was successful** (Builder pivoted to raw InputKey nodes). This is good Builder behavior. The Planner should also check whether referenced Input Action assets exist (via project.search) before encoding them in the build plan, to avoid the initial failure.
