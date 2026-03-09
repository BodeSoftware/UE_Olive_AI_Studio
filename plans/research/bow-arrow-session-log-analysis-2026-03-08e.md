# Research: Bow & Arrow Session Log Analysis — Run 08e

## Question
What happened in this session? Why was a bow Blueprint never created? Full breakdown of user request, planner output, builder activity, failures, timeline, and Reviewer verdict.

## Log File
`docs/logs/UE_Olive_AI_Toolkit.log` — Session 2026-03-08, 05:42–06:05 UTC

---

## Findings

### 1. User's Request

Line 1792:
```
create a bow and arrow system for @BP_ThirdPersonCharacter
```

User tagged `BP_ThirdPersonCharacter` as context. They wanted a functional bow and arrow system integrated into the character.

---

### 2. What the Planner Produced

**Pipeline:** CLI-only mode (Scout + Planner, 2 LLM calls). Router skipped, defaulted to Moderate complexity.

**Scout phase (16.0s):**
- Loaded 3 library template overviews (25,668 chars — extremely large, a known problem area)
- Found 10 related assets via keyword search (bow, arrow, bp_thirdpersoncharacter)
- Ran 5 `olive.search_community_blueprints` calls

**Planner phase (79.7s):**
- Received 8,099-char prompt
- Used the restricted 3-tool MCP filter (only `blueprint.get_template`, `blueprint.list_templates`, `blueprint.create_from_library`)
- Made 11 `blueprint.get_template` calls on `combatfs_ranged_component`:
  - overview (x2)
  - `combatfs_combat_status_component` overview
  - Pattern reads: `CreateArrow`, `AimSetup`, `SetArrowVelocity`, `DestroyArrow`, `LaunchConditions`, `FireArrowGroup`, `ReleaseAimSetup`, `ResetAllVariables`, `GetVectorInfrontOfCamera`

**Plan output:** 6,982 chars, **3 assets planned:**
1. `/Game/BowAndArrow/BP_Arrow` (Actor — the arrow projectile)
2. `/Game/BowAndArrow/BP_BowAndArrowComponent` (ActorComponent — the bow logic component)
3. `/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter` (existing character — to be modified)

**Validator:** 1 non-blocking issue found (0.004s). Pipeline declared valid=true, 95.7s total.

**Critical finding:** The Planner planned only 3 assets. There is **no `BP_Bow` blueprint** in the plan. The Planner interpreted "bow and arrow system" as: an Arrow projectile + a BowAndArrowComponent component + character integration. This is a valid architectural interpretation — the "bow" as a visual asset was not planned because no skeletal mesh / static mesh blueprint was requested, only the logic system. This was not a bug — it was a design choice by the Planner.

---

### 3. What the Builder Actually Created

**Builder launched at 05:44:44**, with 14,487-char stdin prompt, max 500 turns, MCP filter allowing 52/85 tools.

#### Asset 1: `/Game/BowAndArrow/BP_Arrow` (CREATED)

Line 1943 — created at 05:45:25 as `Actor` subclass.

Additions:
- Components: `CollisionSphere` (SphereComponent), `ArrowMesh` (StaticMeshComponent), `ProjectileMovement` (ProjectileMovementComponent)
- Variables: `ArrowSpeed` (float, 3000.0), `GravityScale` (float, 0.5), `Damage` (float, 20.0), `HitActors` (array?), `LifeSpan` (float)
- Functions: `InitializeArrow`, `OnHit` (signatures only — empty shells)
- Compiled successfully

Function logic: The Builder attempted 3 plan_json calls on `InitializeArrow` before succeeding:
- **Attempt 1 (FAIL):** Tried `SetInitialSpeed` and `SetMaxSpeed` on `ProjectileMovementComponent` — these functions don't exist. FindFunction searched exhaustively and found no match.
- **Attempt 2 (FAIL):** Tried `SetFloatPropertyByName` from `KismetSystemLibrary` — also doesn't exist (it's `SetDoublePropertyByName` in UE5).
- **Attempt 3 (FAIL on Phase 0):** Used `SetDoublePropertyByName` — resolved correctly, but `Activate` called on `ProjectileMovementComponent` without a Target wire on an Actor BP → `COMPONENT_FUNCTION_ON_ACTOR` error.
- **Attempt 4 (SUCCESS, line 2254):** Added explicit Target wiring for `ProjectileMovement`. Plan executed.

`InitializeArrow` and the `BeginPlay` event graph were populated (lines 2355, 2427 confirm further plan_json successes on BP_Arrow).

Final state: 9 variables, 2 visible components.

#### Asset 2: `/Game/BowAndArrow/BP_BowAndArrowComponent` (CREATED)

Line 2665 — created at 05:48:34 as `ActorComponent` subclass.

Additions:
- Variables (10): `CharacterRef` (Character*), `BowAiming` (bool), `ArrowLoaded` (bool), `DrawBow` (bool), `PreventFireSpam` (bool), `SpawnedArrow` (BP_Arrow_C*), `ArrowSpeed` (float, 3000.0), `GravityScale` (float, 0.5), `ArrowClass` (TSubclassOf<BP_Arrow_C>), 1 more
- Functions (12+): `StoreComponents`, `AimSetup`, `ReleaseAimSetup`, `GetVectorInfrontOfCamera`, `CreateArrow`, `SetArrowVelocity`, `LaunchConditions`, `FireArrow`, `FireArrowGroup`, `DestroyArrow`, `ResetAllVariables` + `OnBowAimingChanged` event dispatcher
- Rate limit hit twice (lines 2780, 2820) during rapid `add_function` calls — 2x ~47s forced waits
- Multiple plan_json calls populating function logic: successes at lines 2942, 2991; failure at 3072 (resolved with granular tools); successes at 3200, 3292, 3695, 3816; failure at 3838 (DestroyArrow plan — `do_once` macro), recovered at 3947; further successes at 4002, 4275
- Compiled successfully (line 4746: SUCCESS, 0 errors)

#### Asset 3: `/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter` (MODIFIED)

The character was modified late in the run (starting 05:58):
- `BowAndArrowComp` (BP_BowAndArrowComponent_C) added as component — first attempt FAILED (line 4284, class name without `_C` suffix), second attempt SUCCEEDED (line 4293)
- `EventGraph` modification: plan_json to add `IA_Aim` Enhanced Input Action event FAILED (line 4328) — `IA_Aim` does not exist in the project (only `IA_Jump`, `IA_Look`, `IA_Move` exist)
- Builder fell back to `add_node` with `InputKey` nodes (RightMouseButton, LeftMouseButton) — these succeeded
- Multiple failed attempts to add `CallFunction` nodes via `add_node` with various property formats (lines 4355, 4576)
- Builder pivoted to `apply_plan_json` with `custom_event` ops (CE_AimPressed, CE_AimReleased, CE_FirePressed) — **this succeeded** (line 4670, compiled SUCCESS at 4667)
- `connect_pins` used to wire RMB.Pressed → CE_AimPressed.execute, RMB.Released → CE_AimReleased.execute, LMB.Pressed → CE_FirePressed.execute — all succeeded
- Final compile of `BP_ThirdPersonCharacter`: SUCCESS, 0 errors (lines 4746, 4753)

---

### 4. Was a Bow Blueprint Created?

**No. There is no `BP_Bow` or any bow-specific Blueprint in this session.**

The Planner deliberately did not plan one. The system was architected as:
- `BP_Arrow` = the fired projectile (Actor)
- `BP_BowAndArrowComponent` = the logic controller (ActorComponent added to character)

The "bow" as a separate visual/gameplay asset (e.g., a skeletal mesh actor the character holds, an attachable weapon Blueprint) was not requested by the Planner. The Reviewer also did not flag this as missing — it declared **SATISFIED, 0 missing, 0 deviations** (line 4769).

This is likely why the user is frustrated: they expected a `BP_Bow` visual Blueprint (something that would be a weapon actor, possibly with a skeletal mesh, attach socket logic, draw animation trigger, etc.) and instead got a component-based system with no bow object.

---

### 5. Timeline

| Phase | Start | End | Duration |
|-------|-------|-----|----------|
| Engine start + plugin load | 05:39:34 | 05:39:38 | ~4s |
| User message received | 05:42:23 | — | — |
| Pre-autonomous snapshot + read | 05:43:08 | 05:43:09 | ~1s |
| Scout (community search + template load) | 05:43:09 | 05:43:25 | ~16s |
| Planner (MCP, 11 get_template calls) | 05:43:25 | 05:44:44 | ~79.7s |
| Validator | 05:44:44 | — | 0.004s |
| Builder launched | 05:44:44 | — | — |
| Builder: initial template reads | 05:45:05 | 05:45:08 | ~20s |
| Builder: BP_Arrow creation + setup | 05:45:25 | 05:47:17 | ~112s |
| Builder: BP_BowAndArrowComponent creation | 05:48:34 | 05:54:00 | ~326s |
| Builder: BP_ThirdPersonCharacter integration | 05:58:16 | 06:02:18 | ~242s |
| Builder: final compiles | 06:02:09 | 06:02:18 | ~9s |
| Builder process exit (code 0) | 06:02:32 | — | — |
| Reviewer | 06:02:32 | 06:03:08 | ~35.9s |
| Brain reset to Idle | 06:03:08 | — | — |
| **Total session** | **05:43:08** | **06:03:08** | **~20 min** |

Builder wall-clock time: 05:44:44 → 06:02:32 = **17 min 48s**. No timeout. Clean exit code 0.

---

### 6. Tool Call Scorecard

| Tool | Calls | Succeeded | Failed | Fail Rate |
|------|-------|-----------|--------|-----------|
| `blueprint.apply_plan_json` | 23 | 13 | 10 | 43% |
| `blueprint.connect_pins` | 18 | 15 | 3 | 17% |
| `blueprint.add_node` | 16 | 13 | 3 | 19% |
| `blueprint.get_template` | 15 | 15 | 0 | 0% |
| `blueprint.add_variable` | 14 | 14 | 0 | 0% |
| `blueprint.add_function` | 14 | 12 | 0* | 0% |
| `blueprint.compile` | 10 | 10 | 0 | 0% |
| `blueprint.remove_node` | 8 | 8 | 0 | 0% |
| `olive.search_community_blueprints` | 5 | 5 | 0 | 0% |
| `blueprint.add_component` | 5 | 4 | 1 | 20% |
| `blueprint.get_node_pins` | 2 | 2 | 0 | 0% |
| `blueprint.create` | 2 | 2 | 0 | 0% |
| `blueprint.read` | 1 | 1 | 0 | 0% |
| `blueprint.disconnect_pins` | 1 | 1 | 0 | 0% |
| **TOTAL** | **134** | **118** | **16** | **12%** |

*2 add_function calls hit WriteRateLimit validation and were delayed/deferred, not counted as failures.

**Overall success rate: 88%** — better than run 08c (77%) but below run 08d (86%) on plan_json specifically. The plan_json failure rate of 43% (10/23) is the standout problem.

---

### 7. Every Failure With Root Cause

| # | Line | Time | Tool | Error | Root Cause |
|---|------|------|------|-------|------------|
| 1 | 2075 | 05:46:05 | `apply_plan_json` | `SetInitialSpeed`, `SetMaxSpeed` not found on `ProjectileMovementComponent` | Builder hallucinated function names. These properties must be set via direct property access, not setter functions. |
| 2 | 2110 | 05:46:14 | `apply_plan_json` | `SetFloatPropertyByName` not found | Builder tried `SetFloatPropertyByName` which is UE4 API. UE5 uses `SetDoublePropertyByName`. |
| 3 | 2145 | 05:46:46 | `apply_plan_json` | `COMPONENT_FUNCTION_ON_ACTOR` — `Activate` on `ProjectileMovementComponent` missing Target | Phase 0 validator correctly caught component function without Target wire on Actor BP. |
| 4 | 2629 | 05:47:53 | `apply_plan_json` | Unknown (BP_Arrow function graph, plan not logged fully) | Likely a follow-on from previous correction cycle. |
| 5 | 3072 | 05:50:37 | `apply_plan_json` | Unknown (BP_BowAndArrowComponent function graph) | Builder recovered with granular tools. |
| 6 | 3423 | 05:51:54 | `apply_plan_json` | Unknown (BP_BowAndArrowComponent) | Builder recovered with granular tools. |
| 7 | 3474 | 05:52:26 | `connect_pins` | Pin compatibility failure | Likely type mismatch on a wiring attempt during granular recovery. |
| 8 | 3484 | 05:52:30 | `add_node` | Node creation failed | Likely invalid node type or missing function reference. |
| 9 | 3532 | 05:52:50 | `connect_pins` | Pin compatibility failure | Same recovery sequence. |
| 10 | 3838 | 05:55:31 | `apply_plan_json` | `do_once` in DestroyArrow — unknown failure | Macro instantiation or exec wiring issue with `do_once`. Recovered at line 3947. |
| 11 | 4032 | 05:57:10 | `apply_plan_json` | Unknown (BowAndArrowComponent function) | Recovered at 4179-area or 4275. |
| 12 | 4179 | 05:57:23 | `apply_plan_json` | Unknown | Builder pivoted to connect_pins + compile approach. |
| 13 | 4284 | 05:58:17 | `add_component` | `BP_BowAndArrowComponent` class name (missing `_C` suffix) on BP_ThirdPersonCharacter | Tried class name without `_C` suffix. Immediately retried with `_C` and succeeded. |
| 14 | 4328 | 05:58:28 | `apply_plan_json` | `IA_Aim` Enhanced Input Action not found — only `IA_Jump`, `IA_Look`, `IA_Move` exist | Project has no `IA_Aim` asset. The Planner assumed it would exist or be created. Builder had to pivot to `InputKey` nodes then custom events. |
| 15 | 4355 | 05:59:34 | `add_node` | `CallFunction` with `FunctionReference.MemberName`/`FunctionReference.MemberParent` properties format — FAILED | Wrong property format for add_node. Retried with `K2Node_CallFunctionOnMember` (succeeded) then pivoted to plan_json. |
| 16 | 4576 | 06:01:09 | `add_node` | `K2Node_CallFunction` with `FunctionReference` props — FAILED again | Same issue. Builder finally gave up on add_node and used apply_plan_json with custom_event op instead — which worked. |

---

### 8. Did the Builder Timeout? Auto-Continues?

**No timeout.** The Builder ran for 17 min 48s and exited cleanly with **exit code 0** (line 4763).

**No auto-continues.** There is no `IsContinuationMessage` or continuation prompt trigger in the logs. The session completed in a single Builder launch.

---

### 9. Did the Builder Give Up or Simplify?

**The Builder did not give up** — it completed all three planned assets with working code and successful compilations.

However, there were three notable patterns of behavior:

**Pattern A — Iterative correction on ProjectileMovement init:**
Builder tried 3 wrong approaches before finding the correct one. Each failure triggered a new plan with a different strategy. This worked correctly — 4th attempt succeeded.

**Pattern B — Rate limit delays:**
Two hits at 05:48:52 and 05:49:39 (WriteRateLimit) caused ~47s combined forced wait during rapid `add_function` calls. Builder retried successfully after the wait windows.

**Pattern C — `IA_Aim` doesn't exist → custom events fallback:**
This was the most significant course-correction. The Planner assumed `IA_Aim` existed. The Builder first tried `apply_plan_json` with `event` op targeting `IA_Aim` → FAILED (node creation abort). Then tried `add_node` with `CallFunction` → FAILED twice. Finally pivoted to `apply_plan_json` with `custom_event` ops (CE_AimPressed, CE_AimReleased, CE_FirePressed) → SUCCEEDED. This is a reasonable fallback but means the integration used raw input key + custom events instead of the Enhanced Input system, which is less ideal for a modern UE5 project.

---

### 10. Continuation Prompt Content

There was **no continuation prompt** in this session. The Builder ran to completion in one pass.

The Reviewer received a summary of:
- `BP_Arrow`: 9 variables, 2 components (line 4765)
- `BP_BowAndArrowComponent`: 10 variables, 0 components (line 4766)
- `BP_ThirdPersonCharacter`: 1 variable, 1 component (line 4768 — the `BowAndArrowComp`)

The Reviewer declared **SATISFIED, 0 missing, 0 deviations** (line 4769, 35.9s review).

---

### 11. Planner's Build Plan — Extracted Content

The plan was 6,982 chars across 3 assets. Key decisions visible from tool call params and resolver logs:

**BP_Arrow plan included:**
- `InitializeArrow` function: accept `Speed` + `Gravity` params, set ProjectileMovementComponent properties, Activate component
- `BeginPlay` event: set LifeSpan
- `OnHit` event: apply damage, destroy actor

**BP_BowAndArrowComponent plan included:**
- All functions mirroring `combatfs_ranged_component` pattern: `StoreComponents`, `AimSetup`, `ReleaseAimSetup`, `GetVectorInfrontOfCamera`, `CreateArrow`, `SetArrowVelocity`, `LaunchConditions`, `FireArrow`, `FireArrowGroup`, `DestroyArrow`, `ResetAllVariables`
- Event dispatcher: `OnBowAimingChanged`

**BP_ThirdPersonCharacter plan included:**
- Add `BowAndArrowComp` component
- Wire `IA_Aim` Enhanced Input events to `AimSetup`/`ReleaseAimSetup`
- Wire `IA_Fire` (or similar) to `FireArrow`

---

## Root Cause of "No Bow Blueprint"

The user expected a `BP_Bow` weapon Blueprint — a separate actor the character holds, with a skeletal mesh, socket attachment logic, etc. The Planner did not create one because:

1. The user's request was "create a bow and arrow system" — this is ambiguous. The Planner interpreted it as "implement the bow mechanics as a component system" rather than "create a bow weapon actor."
2. The Planner's library templates (`combatfs_ranged_component`) demonstrated a component-based architecture (logic in an ActorComponent), not a separate weapon actor. The Planner followed that pattern exactly.
3. The Reviewer was satisfied because it compared the built output against the plan — and the plan never included a `BP_Bow`. The Reviewer has no judgment about whether the plan itself was the right scope.

This is a **plan scope miss**, not a Builder failure. The Builder executed the Planner's 3-asset plan faithfully. The Reviewer correctly verified the plan was executed. No timeout, no gutted functions, no regression.

---

## Recommendations

1. **The "bow" absence is a planning scope problem.** The Planner is interpreting "bow and arrow system" as "ranged combat component system." If the user wanted a physical `BP_Bow` actor (mesh, attach, animations), they need to say so explicitly: "create a BP_Bow actor Blueprint with a skeletal mesh component, attach it to the character's hand socket, and create a bow and arrow component for the shooting logic."

2. **`IA_Aim` not existing is a pre-build validation gap.** The Planner assumed an Enhanced Input Action `IA_Aim` existed. The Validator's 1 non-blocking issue (line 1905) did not catch this. The Validator should check for referenced asset existence (InputActions, etc.) before declaring valid=true.

3. **plan_json failure rate of 43% (10/23) is high.** Three consecutive failures on `InitializeArrow` were caused by hallucinated function names (`SetInitialSpeed`, `SetMaxSpeed`, `SetFloatPropertyByName`). The alias map does not contain entries for `ProjectileMovementComponent`'s properties. Adding `describe_node_type` for `ProjectileMovementComponent` in the prompt, or adding catalog entries for property setters, would reduce this.

4. **Template overviews at 25,668 chars in Scout is expensive.** This matches the pattern flagged in prior runs. These large overviews consume most of the Planner's context budget before any planning begins.

5. **Reviewer satisfaction is plan-relative, not user-intent relative.** The Reviewer saw the plan executed correctly and said SATISFIED. It has no way to know the user wanted a physical bow actor. This is expected behavior but should be noted: Reviewer cannot catch scope mismatches between the user's intent and the Planner's interpretation.

6. **WriteRateLimit delays cost ~47s.** Two rapid `add_function` batches hit the 30/min write rate. Suggest Builder be prompted to pace function creation or batch differently.

7. **`_C` suffix for Blueprint classes in add_component remains a recurring stumble.** This is failure #13 in this run and has appeared in prior runs. The error message from the first failure should be injected directly into the next attempt's guidance.

8. **The final output does compile cleanly.** All three assets compile with 0 errors. The system is functionally correct for what was planned. The user's frustration is about scope, not quality.
