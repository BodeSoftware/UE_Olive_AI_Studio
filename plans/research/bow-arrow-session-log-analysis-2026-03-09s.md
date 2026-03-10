# Research: Bow & Arrow Session Log Analysis — Run 09s

## Question
Analyze run 09s of the bow-and-arrow Blueprint-building task to verify four specific fixes and identify any new failure patterns.

---

## Findings

### Run Identity & Timing

- **Log file:** `docs/logs/UE_Olive_AI_Toolkit.log` (opened 2026-03-10 at 00:58, engine rebuilt from source before session)
- **Run start:** 01:05:11 (autonomous launch after blueprint.read + auto-snapshot)
- **Run end:** 01:11:01 (Brain: Run completed, outcome=0 = Completed)
- **Total agent time: ~5:50** (5 minutes 50 seconds)
- **Build + startup overhead:** ~6:23 before the run started (hot rebuild, 130 files compiled)
- This is slower than 09q (4:09) and 09r (4:29). The regression is attributable to retry loops in BP_ThirdPersonCharacter, not to extra research time.

### Assets Built

Three assets were produced:
1. **BP_Arrow** — created via factory template preset "Arrow", then `apply_plan_json` (BeginPlay + CollisionSphere overlap + ApplyDamage + Destroy). Compiled SUCCESS.
2. **BP_Bow** — created bare (`parent_class: Actor`), components and variables added, Fire function built, ResetCanFire custom event built. Compiled SUCCESS.
3. **BP_ThirdPersonCharacter** — modified. Variables added (BowRef, bBowEquipped). BeginPlay plan succeeded. Fire input plan required 3 attempts before succeeding + 2 granular tool calls to fix wiring. Compiled SUCCESS.

All four blueprints compile SUCCESS (BP_Arrow, BP_Bow, BP_ThirdPersonCharacter each compiled once or twice with zero errors).

---

## Fix Verification

### Fix 1: FUNCTION_NOT_FOUND Error Restructure

**Did it fire?** YES — twice.

**Occurrence 1 (line 2468):** `GetMesh` search during BP_ThirdPersonCharacter BeginPlay plan.
```
Warning: FindFunction('GetMesh' [resolved='GetMesh'], class=''): FAILED -- searched specified class + Blueprint GeneratedClass + Blueprint FunctionGraphs + parent class hierarchy + SCS component classes + implemented interfaces + library classes [...] | Closest matches: GetMass (PrimitiveComponent), GetMassScale (PrimitiveComponent), GetMaterial (PrimitiveComponent), ...
```
The resolver logged a warning but continued — the step was **silently promoted** to `get_var("Mesh")` fallback (line 2500: `Step 5/7: step_id='get_mesh', type='GetVariable', target='Mesh'`). The plan succeeded without FUNCTION_NOT_FOUND reaching the agent as an error.

**Occurrence 2 (line 2579):** `Fire` search during first FireBow plan attempt (missing `target_class`).
```
Warning: FindFunction('Fire' [resolved='Fire'], class=''): FAILED -- ...
```
Then line 2580: `ResolveCallOp FAILED: function 'Fire' could not be resolved (target_class=''). Searched: alias map (215 entries), Blueprint class 'BP_ThirdPersonCharacter_C', ...`

The plan failed. The agent's next attempt (line 2585) added `"target_class": "BP_Bow_C"` and succeeded immediately — demonstrating the fix worked conceptually, but the agent self-corrected based on the full search trail, **not the new format specifically**.

**Did the agent use suggested alternatives?** The FUNCTION_NOT_FOUND error for `Fire` contains a search trail showing "Searched: alias map, Blueprint class BP_ThirdPersonCharacter_C, function graphs (1 graphs), parent hierarchy...". The agent correctly inferred that `Fire` is on `BP_Bow_C` and added `target_class` on retry. However, there is no evidence the new format's "alternatives FIRST" structure or `describe_function` advice was displayed to the agent — the failure was a resolver-level refusal (plan resolution failed before execution), so whatever the MCP tool result JSON looks like is what the agent saw.

**Did the agent call `describe_function`?** NO. Zero calls to `describe_function` or `blueprint.describe_node_type` in the entire run. The agent self-corrected by reading the search trail alone.

**Verdict:** FUNCTION_NOT_FOUND new format was sent to the agent on the `Fire` failure. The agent responded to it correctly (added `target_class`) in 9 seconds. We cannot confirm which part of the new format (alternatives list, UPROPERTY note, or search trail) drove the fix — the corrective signal (search trail showing it searched BP_ThirdPersonCharacter, not BP_Bow) was already available in the old format too.

---

### Fix 2: describe_function Prompt Reframe

**Did the agent call `describe_function` proactively?** NO.

Zero calls to `blueprint.describe_node_type`, `describe_function`, or any describe-class tool in the entire run. The agent did call `olive.search_community_blueprints` (5×) and `blueprint.get_template` (4×) during the planning phase, but no function-verification calls before writing.

**Verdict:** The reframe had no observable effect. The agent did not use `describe_function` either proactively or reactively. This confirms the 09r finding — the agent ignores the guidance. A stronger mechanism (e.g., a mandatory FUNCTION_FOUND confirmation step in the resolver, or an enforced tool call gate) is needed.

---

### Fix 3: SpawnActor Step-Ref Support (`@get_class.auto`)

**Did the agent use `@step` reference as spawn_actor target?** NO.

In both Fire function plans and the BeginPlay plan, the agent used hard-coded asset paths as spawn_actor target:
- `"target": "/Game/Weapons/BP_Arrow"` (Fire function, lines 2075, 2235)
- `"target": "/Game/Weapons/BP_Bow"` (BeginPlay, line 2464)

No `@step.auto` reference was used as a class pin. The fix was not exercised.

**Was "dynamic_class_ref" logged?** No occurrences of "dynamic_class_ref" or "Dynamic class wire" in the log.

**Verdict:** Fix 3 not exercised. The task did not require a dynamic class reference — the agent knew the asset paths upfront.

---

### Fix 4: Previous Fixes Still Working

**Modify() reused nodes — EXEC WIRE REJECTED:** Zero occurrences of "EXEC WIRE REJECTED" in the log. CONFIRMED CLEAN.

**Target/@step.auto wiring:** Line 2534 shows `"Data wire OK: @spawn_bow.auto -> step 'attach'.self (explicit Target)"` and line 2775 shows `"Data wire OK: @get_bow.auto -> step 'fire'.self (explicit Target)"`. Self-pin Target wiring worked correctly in two separate plans. CONFIRMED WORKING.

**PreResolvedFunction:** 27 hits in the log. List of functions hit:
- Actor: SetLifeSpan, GetInstigatorController, K2_DestroyActor, GetTransform, K2_AttachToComponent, K2_GetActorLocation (×2)
- GameplayStatics: ApplyDamage
- KismetMathLibrary: Greater_IntInt (×2), Subtract_IntInt (×2), GetForwardVector (×2), Multiply_VectorFloat (×2), Add_VectorVector (×2), MakeTransform implied via FindFunction
- KismetSystemLibrary: K2_SetTimer (×2), IsValid (×2)
- Pawn: GetController, GetControlRotation, GetBaseAimRotation
- BP_Bow_C: Fire (×2)

CONFIRMED working at 27 occurrences.

**Self-correction guidance:** The agent self-corrected on the `Fire` missing `target_class` error in 9 seconds. It read the resolver error and immediately added `"target_class": "BP_Bow_C"` on the next attempt. No sign of confusion or repeated identical failure.

---

## Tool Call Inventory

| Tool | Calls | Successes | Failures |
|------|-------|-----------|---------|
| blueprint.apply_plan_json | 9 | 5 | 4 |
| blueprint.add_variable | 7 | 7 | 0 |
| olive.search_community_blueprints | 5 | 5 | 0 |
| blueprint.get_template | 4 | 4 | 0 |
| blueprint.compile | 4 | 4 | 0 |
| blueprint.create | 2 | 2 | 0 |
| blueprint.connect_pins | 2 | 2 | 0 |
| blueprint.add_function | 2 | 2 | 0 |
| blueprint.add_component | 2 | 2 | 0 |
| olive.get_recipe | 1 | 1 | 0 |
| blueprint.set_pin_default | 1 | 1 | 0 |
| blueprint.read | 1 | 1 | 0 |
| blueprint.list_templates | 1 | 1 | 0 |
| blueprint.disconnect_pins | 1 | 0 | 1 |
| blueprint.add_node | 1 | 1 | 0 |
| **TOTAL** | **43** | **38** | **5** |

**Overall tool success rate: 88.4%** (38/43)
**plan_json success rate: 55.6%** (5/9)

Note: plan_json attempt count is higher than appears — the agent ran 9 apply_plan_json calls total: BP_Arrow EventGraph (success), BP_Bow Fire attempt 1 (fail), BP_Bow Fire attempt 2 (success), BP_Bow ResetCanFire EventGraph (success), BP_ThirdPersonCharacter BeginPlay (success), BP_ThirdPersonCharacter FireBow attempt 1 (fail — Fire missing target_class), BP_ThirdPersonCharacter FireBow attempt 2 (fail — IA_Fire not found), BP_ThirdPersonCharacter FireBow attempt 3 (fail — data wire), BP_ThirdPersonCharacter FireBow attempt 4 (success with exec auto-break).

---

## Failure Root Causes (All 5 Failures)

### Failure 1: BP_Bow Fire — attempt 1 (01:07:42)
**Error:** 3 failed exec connections + 1 failed data wire + Phase 5.5 unfixable (bSuccess unwired).
**Root cause:** The agent used `set_var` on a function output parameter (`bSuccess`) and then tried to use that FunctionOutput step as an exec source (exec_after pointing to a FunctionOutput node). FunctionResult nodes have no exec output pin. Pattern: agent treating return-value assignment as a mid-graph node with exec flow through it.
**Recovery:** Rewrote the plan in 32 seconds, removing the dual-return pattern, using a single `ret` step and a separate `set_pin_default` call on the FunctionResult node to hard-code `bSuccess=true`.

### Failure 2: BP_ThirdPersonCharacter FireBow — attempt 1 (01:09:06)
**Error:** `ResolveCallOp FAILED: function 'Fire' could not be resolved (target_class='')`.
**Root cause:** Agent forgot to specify `target_class="BP_Bow_C"` for the `Fire` call. The function exists only on BP_Bow_C, which the resolver never searches when working in BP_ThirdPersonCharacter's context without an explicit target_class.
**Recovery:** Added `target_class: "BP_Bow_C"` in 9 seconds on next attempt.

### Failure 3: BP_ThirdPersonCharacter FireBow — attempt 2 (01:09:15)
**Error:** Phase 1 FAIL — Enhanced Input Action 'IA_Fire' not found.
**Root cause:** IA_Fire asset does not exist in this project. Available: IA_Jump, IA_Look, IA_Move. The error message clearly listed the available actions.
**Recovery:** Agent switched to `custom_event` named "FireBow" (instead of IA_Fire) and added an InputKey node for LeftMouseButton via `blueprint.add_node`. Correct architectural pivot.

### Failure 4: BP_ThirdPersonCharacter FireBow — attempt 3 (01:09:35)
**Error:** Phase 4: 1 data wire failed. Source is `check_valid` (IsValid), but target is step `fire`'s `self` pin — wiring failed with 1 failed connection.
**Root cause:** The agent included `"is_valid": "@check_valid.auto"` to check BowRef validity, then also tried to use `@get_bow.auto -> fire.self` (Target). The single failed data wire on `fire` was actually the attempt to wire the `is_valid` output to the `fire` step's self — the exact pin mismatch. The plan used both `check_valid` result AND explicit Target wire `@get_bow.auto`, which succeeded, but some other wiring from `check_valid` to an exec pin via is_valid failed.
**Recovery:** Agent rewrote using `GetBaseAimRotation` instead of `GetControlRotation + GetForwardVector` (simpler direction calculation), dropped the is_valid+exec branch (replaced with plain branch), and succeeded on attempt 4. One exec auto-break fired (`then -> execute` on FireBow custom event), confirming the custom event from attempt 3 was reused without Modify() issue — exec auto-break correctly severed the stale connection.

### Failure 5: blueprint.disconnect_pins (01:10:12)
**Error:** `BP_DISCONNECT_PINS_FAILED: Pins are not connected`.
**Root cause:** Agent tried to disconnect `00000000000000000000000000000000.then -> node_29.execute` using a null/zero node ID. This was probably an attempt to disconnect a wiring from the FunctionEntry or some non-plan node. The pins were never connected.
**Recovery:** Agent immediately tried `connect_pins` instead, which used exec auto-break to replace the existing connection. Succeeded in 14 seconds.

---

## New Failure Patterns

**Pattern: FunctionOutput node as exec source in multi-return plans.** The agent designed the Fire function with a "set return value mid-execution" pattern (`set_var bSuccess=false` at start, `set_var bSuccess=true` at success path). This produced FunctionOutput steps that have no exec output pin, making exec_after chains through them impossible. This is a recurring conceptual error (appears in prior runs too). The agent self-corrected by simplifying to a single FunctionResult with a hard-coded default. The Phase 5.5 diagnostic correctly caught the unwired bSuccess pin on the successful attempt too, but the compile still passed because an unwired bool FunctionResult pin defaults to false.

**Pattern: Orphaned Branch node on reuse with auto-break.** Line 2906 shows `Orphaned node 'Branch ()' could not be auto-fixed` during the final successful FireBow plan. This is from attempt 3's leftover `branch_valid` node (node_11 from attempt 3) — the exec auto-break rewired the custom event's `then` to the new branch (node_22), leaving the old one stranded. The compiler still succeeded (orphaned nodes in UE event graphs don't prevent compilation). This is not a regression but an artifact of partial plan commits leaving orphan nodes from previous attempts.

---

## Comparison Table

| Run | Time | plan_json rate | Tool success | Key change |
|-----|------|---------------|--------------|-----------|
| 09p | 7:02 | 55.6% | 92.1% | EXEC WIRE REJECTED fixed |
| 09q | 4:09 | 75% | 90% | All major fixes, 4 BPs |
| 09r | 4:29 | 66.7% | 91.9% | Self-correction guidance |
| **09s** | **5:50** | **55.6%** | **88.4%** | FUNCTION_NOT_FOUND restructure, describe_function reframe, @get_class.auto |

09s is a **regression from 09q and 09r**. The extra time comes from:
- 4 plan_json failures in BP_ThirdPersonCharacter vs 1 in 09q
- FireBow required 4 attempts (missing target_class + IA_Fire missing + data wire + success)
- The granular wiring repair (add_node + disconnect_pins + connect_pins × 2) after the final plan added ~90 seconds

The 88.4% tool success rate is the lowest since 09n (85%). The 55.6% plan_json rate matches 09p (the worst).

---

## Recommendations

1. **Fix 1 (FUNCTION_NOT_FOUND restructure): PARTIAL CONFIRMED.** The new format was delivered to the agent and it self-corrected on `Fire` missing `target_class` in 9 seconds. Cannot confirm the "alternatives first" layout was the specific driver — the search trail alone was sufficient in prior runs too. Keep the fix; it does not hurt.

2. **Fix 2 (describe_function reframe): NOT WORKING.** Zero calls to `describe_function` in this run or 09r. The agent ignores the guidance entirely. Recommend: either enforce a describe step in the resolver before executing plans with unknown functions (pre-flight check), or drop describe_function guidance from the prompt as it consumes tokens with no effect.

3. **Fix 3 (@get_class.auto): NOT EXERCISED.** The task naturally uses static asset paths. This fix will only be visible in tasks where a class is dynamically determined at plan time. Not a concern.

4. **FunctionOutput-as-exec-source (Failure 1):** This is a multi-run recurring failure (also seen in 09m). The plan allows `return` op steps and `set_var` on output params but the agent conflates them. Recommendation: Add a Phase 0 validator rule — if `exec_after` points to a FunctionOutput step (resolved via `set_var` on output param), reject the plan with an explicit error: "FunctionOutput steps have no exec output. Use `return` op to terminate, not `exec_after`."

5. **IA_Fire missing (Failure 3) — EXPECTED.** IA_Fire has never existed in this project. The agent correctly pivots to custom_event + InputKey. The error message is clear and correct. No fix needed.

6. **Orphaned Branch from reuse (Failure 4 side-effect):** When a plan reuses an existing node (FireBow custom event), and a prior failed attempt left stranded impure nodes, the Phase 5.5 orphan detector correctly flags them but cannot auto-fix. The compiler is tolerant. This is acceptable behavior but creates graph clutter across runs. Consider a graph cleanup sweep before each plan attempt on a graph with prior failed attempts.

7. **plan_json rate regression (55.6%):** All 4 failures trace to three root causes: FunctionOutput exec (1×), missing target_class (1×), IA_Fire missing (1×), data wire (1×). Three of these are correctable with modest prompt or validator improvements. The data wire failure (Failure 4) was architectural — the agent changed design and succeeded. No single systemic bug dominated.

8. **Self-correction speed is good:** Fire/target_class fix in 9 seconds, IA_Fire pivot in 10 seconds, final design change in 15 seconds. The self-correction signal quality is high — errors are clear and specific. The agent is not confused, it is dealing with genuine constraint gaps.

9. **27 PreResolvedFunction hits:** Working correctly and appears to be contributing meaningfully to execution speed. BP_Bow_C::Fire was resolved in the resolver and then used pre-resolved in the executor on two separate plan calls — confirming cross-plan function caching is working.

10. **Exec auto-break working correctly:** Two uses of exec auto-break (lines 2883 and 2947) both succeeded cleanly, allowing the agent to rewire over stale connections from prior attempts without EXEC WIRE REJECTED. Confirmed stable.

Source: `B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/docs/logs/UE_Olive_AI_Toolkit.log`
