# Research: Bow and Arrow Session Log Analysis — Run 09r (2026-03-09)

## Question
Analyze the autonomous MCP agent run building a bow and arrow system. Primary focus: verify the new FUNCTION_NOT_FOUND self-correction guidance (directive vs. passive). Secondary: confirm previous fixes still hold.

---

## Findings

### 1. Executive Summary

Run 09r completed in **4:29** (22:56:43 → 23:01:12), producing 2 Blueprints fully built (BP_Arrow, BP_Bow) plus modifications to BP_ThirdPersonCharacter. All 3 BPs compiled SUCCESS. Tool success rate was **91.9% (34/37)**, and plan_json success rate was **66.7% (6/9)**. The `FUNCTION_NOT_FOUND` fix showed **partial effectiveness**: on attempt 2, the agent did NOT call `describe_function` — it instead dropped the hallucinated function and replaced it with a `set_var` op (correct semantic substitute). EXEC WIRE REJECTED did not fire. SetInstigator hallucination recurred from 09q but the agent **self-corrected without guidance** on attempt 3 by treating it as a UPROPERTY `set_var`. The `@get_class.auto` resolver failure is a new tool-side bug unrelated to the new guidance.

---

### 2. Timeline

| Timestamp | Event |
|-----------|-------|
| 22:56:43 | Autonomous run launched |
| 22:56:52 | 5× olive.search_community_blueprints (pre-pipeline discovery) |
| 22:57:09 | list_templates, get_recipe, 3× get_template (research phase) |
| 22:57:41 | blueprint.create BP_Arrow (from template, preset=Arrow) |
| 22:57:55 | apply_plan_json BP_Arrow EventGraph (8 steps) → SUCCESS |
| 22:58:00 | apply_plan_json BP_Arrow BeginPlay (2 steps) → SUCCESS |
| 22:58:11 | blueprint.create BP_Bow |
| 22:58:16 | 2× add_component (BowMesh, MuzzlePoint) |
| 22:58:20 | 4× add_variable (ArrowClass, ArrowSpeed, bCanFire, FireCooldown) |
| 22:58:24 | 2× add_function (Fire, ResetCanFire custom_event) |
| 22:58:29 | compile BP_Bow |
| 22:58:49 | apply_plan_json Fire function (19 steps, SetVelocityInLocalSpace fails) → FAILED |
| 22:59:06 | apply_plan_json Fire function retry (SetInstigator + @get_class.auto bug) → FAILED |
| 22:59:15 | apply_plan_json Fire function retry 3 (SetInstigator → set_var, hardcoded BP_Arrow path) → SUCCESS |
| 22:59:36 | connect_pins (AsPawn → Instigator) |
| 22:59:41 | apply_plan_json ResetCanFire event → SUCCESS |
| 22:59:53 | 2× add_variable to BP_ThirdPersonCharacter (BowClass, BowRef) |
| 22:59:57 | compile BP_ThirdPersonCharacter |
| 23:00:05 | apply_plan_json ThirdPersonChar BeginPlay (spawn+attach) → SUCCESS |
| 23:00:17 | add_node InputKey (LeftMouseButton) |
| 23:00:23 | apply_plan_json (Fire without target_class) → FAILED |
| 23:00:28 | apply_plan_json (Fire with target_class="BP_Bow") → SUCCESS |
| 23:00:39 | add_node Branch, 3× connect_pins (manual input wiring) |
| 23:00:54 | compile BP_Arrow, BP_Bow, BP_ThirdPersonCharacter → all SUCCESS |
| 23:01:12 | Run complete (exit 0), 37 tool calls |

**Total agent active time: 4:29** (vs. 09q: 4:09 — slight regression of 20s, within noise)

---

### 3. Tool Call Results

**Total: 37 tool calls logged**

| Tool | Count | Success | Failed |
|------|-------|---------|--------|
| blueprint.apply_plan_json | 9 | 6 | 3 |
| blueprint.add_variable | 6 | 6 | 0 |
| blueprint.compile | 5 | 5 | 0 |
| blueprint.connect_pins | 4 | 4 | 0 |
| blueprint.get_template | 3 | 3 | 0 |
| blueprint.create | 2 | 2 | 0 |
| blueprint.add_node | 2 | 2 | 0 |
| blueprint.add_function | 2 | 2 | 0 |
| blueprint.add_component | 2 | 2 | 0 |
| olive.search_community_blueprints | 5 | 5 | 0 |
| olive.get_recipe | 1 | 1 | 0 |
| blueprint.list_templates | 1 | 1 | 0 |

**Overall tool success rate: 91.9% (34/37)**
**plan_json success rate: 66.7% (6/9)**

---

### 4. Fix Verification

#### Fix 1: FUNCTION_NOT_FOUND Self-Correction Guidance

The new directive guidance was exercised across 3 attempt sequence on `Fire` function in BP_Bow:

**Attempt 1 (22:58:49) — FAILED**
- Two FUNCTION_NOT_FOUND errors: `SetVelocityInLocalSpace` (no suggestions relevant) AND `SetInstigator` (warnings only, plan still resolved 18/19 — SetVelocityInLocalSpace was the blocking error)
- Error reported to agent via standard error response with suggestions: `SetPhysicsMaxAngularVelocityInDegrees`, `SetPhysicsAngularVelocityInRadians`, etc.
- **QUESTION: Did agent "USE one of those names exactly"?** Verdict: NO. It discarded `SetVelocityInLocalSpace` entirely on attempt 2, which is a better response than blindly substituting an irrelevant physics function. The suggestions for SetVelocityInLocalSpace were all wrong-class physics functions. The agent correctly recognized this and changed approach.

**Attempt 2 (22:59:06) — FAILED (different error)**
- Agent dropped `SetVelocityInLocalSpace`, still included `SetInstigator` (still hallucinated)
- Fixed `set_var` → `return` op for False branch (correct)
- Used `@get_class.auto` as spawn target — this caused a **tool-side bug**: `@get_class.auto` is a step reference, not a literal class name; `CreateSpawnActorNode` can't resolve `@get_class.auto` as a class string
- Plan RESOLVED (14/14 steps, 0 errors) because resolver accepted the step-ref as valid
- Executor FAILED in Phase 1 at spawn step: `FindClass('@get_class.auto'): FAILED`
- **QUESTION: Did agent call `describe_function`?** Verdict: NO. The error on attempt 2 was not FUNCTION_NOT_FOUND but rather the @get_class.auto executor bug — so the guidance wasn't triggered. The `SetInstigator` error was a WARNING (not blocking) because its resolution continued as a soft fail.

**Attempt 3 (22:59:15) — SUCCESS**
- Agent made two key changes:
  1. Used hardcoded `/Game/BowSystem/BP_Arrow` as spawn target (correct fix for @get_class.auto bug)
  2. Changed `set_instigator` from `op: "call"` to `op: "set_var", target: "Instigator"` — treating Instigator as a UPROPERTY, not a function
- The `set_var` on `Instigator` resolved correctly as `SetVariable` node (line 2328-2331 in log)
- Plan executed to SUCCESS, compiled SUCCESS (0 errors, 0 warnings)
- **Verdict: Agent did NOT call describe_function on attempt 3, but it did take the "fundamentally different approach" the guidance calls for on attempt 3+.** However, this was driven by the agent's own knowledge, not the explicit guidance text.

**FUNCTION_NOT_FOUND for `Fire` on BP_ThirdPersonCharacter:**
- Attempt 1 (23:00:23): Called `Fire` without `target_class` — resolver searched BP_ThirdPersonCharacter hierarchy, didn't find it — FAILED
- Attempt 2 (23:00:28): Agent added `"target_class":"BP_Bow"` — resolver found `BP_Bow_C::Fire` immediately — SUCCESS
- **This is the USE THE SUGGESTIONS pattern working** — the failure message must have conveyed that `target_class` was needed, and the agent added it on the next attempt. This is a 1-retry fix, excellent.

**Overall FUNCTION_NOT_FOUND verdict:**
- Attempt 1 → changed approach (good, not just substituting noise from suggestions)
- Attempt 2 → changed approach again (good, but failed for a DIFFERENT reason)
- Attempt 3 → fundamentally different approach (set_var instead of call) — this IS the Attempt 3+ behavior we want
- `describe_function` was NEVER called — guidance text "Call describe_function" on attempt 2 was not followed
- The agent's actual behavior was better than the nominal guidance path (it reasoned independently rather than mechanically)

**Gap identified: The blocking error on attempt 2 was NOT FUNCTION_NOT_FOUND but @get_class.auto — so the self-correction guidance for FUNCTION_NOT_FOUND was not invoked for attempt 2. The two failure causes interfered.**

#### Fix 2: Exec Auto-Break (Defense in Depth)

Not triggered. No orphaned exec pins observed.

#### Fix 3: Target/@step.auto in plan_json

Confirmed working correctly throughout:
- `"Target":"@MuzzlePoint"` — resolver synthesized get_var step automatically (lines 2121-2122, 2170-2171, 2260-2261)
- `"Target":"@spawn.auto"` in attach step — wired correctly (line 2562: `Data wire OK: @spawn.auto -> step 'attach'.self`)
- `"Target":"@get_bow.auto"` on Fire call — wired correctly (line 2644)
- `"Owner":"@self.auto"` on SpawnActor — handled by ExpandComponentRefs (line 2492)

#### Fix 4: PreResolvedFunction (Modify() reuse fix)

Confirmed: 13 occurrences of "Used pre-resolved function" across the run. No EXEC WIRE REJECTED fired at any point.

**EXEC WIRE REJECTED: CONFIRMED NOT PRESENT in this run.**

---

### 5. Failure Root Cause Analysis

**Failure 1: apply_plan_json BP_Bow/Fire attempt 1 (22:58:49)**
- Root cause: `SetVelocityInLocalSpace` is not a UFunction — it is not exposed to Blueprints via any callable interface. This is a hallucination (same as runs 09k and 09q).
- Secondary warning: `SetInstigator` also not found (it's a UPROPERTY, not a function — `Instigator` is `AActor::Instigator`).
- Error code: FUNCTION_NOT_FOUND (1 blocking error, plan failed to resolve 18/19)
- Time wasted: ~16s (between 22:58:49 and 22:59:06)

**Failure 2: apply_plan_json BP_Bow/Fire attempt 2 (22:59:06)**
- Root cause: `@get_class.auto` used as the literal actor class for SpawnActor. This is a step-reference syntax that the Executor cannot resolve as a UClass string. The Resolver accepted it (treated as dynamic class reference), but `CreateSpawnActorNode` in the Executor calls `OliveClassResolver::Resolve("@get_class.auto")` — this fails because `@get_class.auto` is not a valid class path.
- Note: `SetInstigator` warnings appeared again (still hallucinated) but were non-blocking; plan resolved 14/14
- Error code: Phase 1 FAILED — "Actor class '@get_class.auto' not found" (from OliveClassResolver::Warning)
- Time wasted: ~9s (between 22:59:06 and 22:59:15)

**This is a new bug class: The Resolver accepts `@step_id.auto` as a valid spawn target (dynamic class ref), but the Executor cannot create the SpawnActor node with a step-ref as the class. These are two different code paths with different capabilities.**

**Failure 3: apply_plan_json BP_ThirdPersonCharacter Fire (23:00:23)**
- Root cause: Calling `Fire` without `target_class`. BP_ThirdPersonCharacter does not have a `Fire` function in its own hierarchy — Fire lives on BP_Bow_C. The resolver searched the character's full class hierarchy and found nothing.
- Fix: Agent added `"target_class":"BP_Bow"` on attempt 2 → SUCCESS in 5 seconds
- This is the classic "missing target_class for cross-BP calls" pattern, now self-correcting in 1 retry

---

### 6. GetMesh Behavior

`GetMesh` was called at step 3 of the ThirdPersonCharacter BeginPlay plan (line 2504):
- FindFunction reported FAILED ("GetMesh" not found)
- However, the plan still **succeeded** — the resolver silently fell back to treating `get_mesh` as `op="call"` that resolved to something via a different path (line 2532: `Step 4/6: step_id='get_mesh', type='GetVariable', target='Mesh'`)
- **Critical observation:** The resolver treated `GetMesh` as a **get_var on 'Mesh'** (the SCS component named "Mesh"), not as a function call. This is the "dumb cast" fallback — it's actually the CORRECT behavior for a Character Blueprint where the skeletal mesh is an SCS component named "Mesh". The variable get correctly returned the SkeletalMeshComponent.
- Data wire succeeded: `Connected pins: Mesh -> Parent` (line 2563) — the Mesh component pinned to AttachToComponent's Parent parameter

**Verdict: GetMesh falling through to get_var("Mesh") was the right outcome for this specific BP. The warning fires but the result is correct.**

---

### 7. Comparison Table

| Run | Time | plan_json | Tools | Key Change |
|-----|------|-----------|-------|-----------|
| 09o | 12:02 | 44% (4/9) | 76.1% | GetMesh type erasure |
| 09p | 7:02 | 55.6% (5/9) | 92.1% | EXEC WIRE REJECTED root cause |
| 09q | 4:09 | 75% (6/8) | 90% | All fixes working, 4 BPs |
| **09r** | **4:29** | **66.7% (6/9)** | **91.9%** | **FUNCTION_NOT_FOUND directive guidance** |

---

### 8. New Bugs Discovered

**Bug A: @get_class.auto as SpawnActor class (Executor/Resolver mismatch)**
- The plan `"op":"spawn_actor","target":"@get_class.auto"` means "spawn using the class stored in the variable referenced by step 'get_class'"
- The Resolver (line 2134-2135): "actor_class='@get_class.auto' resolved successfully" — it treats this as a dynamic class reference and marks the step OK
- The Executor (line 2235): `CreateSpawnActorNode: resolving actor class '@get_class.auto'` → calls `OliveClassResolver::Resolve("@get_class.auto")` → FAILS
- The step-ref data wiring (`@get_class.auto` → SpawnActor's Class pin) is supposed to happen in Phase 4 (data wiring), not Phase 1 (node creation)
- The correct behavior would be: create SpawnActor node with null/placeholder class in Phase 1, wire `@get_class.auto` as data in Phase 4
- This is the same issue noted in 09n ("@get_class.auto resolver/executor mismatch") — it recurred here
- Source: OliveNodeFactory.cpp `CreateSpawnActorNode`, which tries to resolve the class eagerly in Phase 1

**Bug B: SetVelocityInLocalSpace hallucination (persistent)**
- This is the 3rd or 4th run where the agent hallucinates `SetVelocityInLocalSpace`. It doesn't exist as a UFunction on any class.
- The "Did you mean" suggestions are irrelevant physics angular velocity functions.
- Mitigation options: (1) Add alias to reject with clear message: "SetVelocityInLocalSpace is not callable from Blueprints — use SetPhysicsLinearVelocity or SetVelocityInLocalSpace on a CharacterMovementComponent", (2) Add to alias map to reroute to `SetPhysicsLinearVelocity` on PrimitiveComponent.

**Bug C: SetInstigator hallucination (persistent)**
- `SetInstigator` is not a UFunction — `Instigator` is a `UPROPERTY(BlueprintReadWrite)` on AActor
- This fired on both attempt 1 and attempt 2 as a WARNING (not blocking since the plan resolved partially)
- The agent's own corrective action on attempt 3 (changing to `set_var, target: "Instigator"`) was correct and succeeded
- Mitigation: Add alias entry: `SetInstigator` → reject with "Use set_var with target='Instigator' instead"

---

### 9. Self-Correction Guidance Assessment

The new directive guidance for FUNCTION_NOT_FOUND was **partially effective**:

| Guidance Rule | Did Agent Follow? | Notes |
|---------------|------------------|-------|
| Attempt 1: "USE one of those names exactly" | N/A — suggestions were irrelevant | Agent correctly rejected suggestions |
| Attempt 2: "STOP guessing. Call describe_function" | NO | describe_function was never called |
| Attempt 3+: "Do NOT retry. Choose fundamentally different approach" | YES (de facto) | Agent switched SetInstigator from call → set_var and dropped SetVelocityInLocalSpace |

**Why describe_function wasn't called on attempt 2:** The error that caused attempt 2 to fail was NOT the FUNCTION_NOT_FOUND for SetInstigator — it was the @get_class.auto Phase 1 executor crash. The FUNCTION_NOT_FOUND warnings for SetInstigator were non-blocking (plan resolved 14/14 despite the warning). So the attempt-2 guidance condition ("FUNCTION_NOT_FOUND again") was not triggered from the error report the agent received. The agent got a completely different error: "Actor class '@get_class.auto' not found".

**This reveals that self-correction guidance is isolated per error type, but the agent experiences multiple simultaneous failure modes. The guidance for FUNCTION_NOT_FOUND never fired because a different error got there first.**

---

## Recommendations

1. **Fix @get_class.auto in SpawnActor executor (HIGH):** `CreateSpawnActorNode` must not try to resolve `@step_id.auto` as a literal class path. When it detects a step-ref target (starts with `@`), it should create the SpawnActor node with a null class and defer class pinning to Phase 4 data wiring. This bug has recurred across 09n and 09r — it's a reliable failure mode when agents use dynamic class variables for spawning.

2. **Add SetVelocityInLocalSpace to alias map with rejection message:** The agent has now hallucinated this function in at least 3 runs (09k, 09q, 09r). Add it to the alias map with a clear redirect: "SetVelocityInLocalSpace is not callable from Blueprints. For projectile velocity, use SetPhysicsLinearVelocity on PrimitiveComponent, or set the Velocity property on ProjectileMovementComponent via set_var."

3. **Add SetInstigator to alias map with rejection message:** Same pattern — at least 3 runs. Map: "SetInstigator is not a function. Instigator is a UPROPERTY — use set_var with target='Instigator' instead."

4. **The directive FUNCTION_NOT_FOUND guidance (attempt 2: call describe_function) is not being exercised** because the error that reaches the agent is different (the blocking error shadows the non-blocking FUNCTION_NOT_FOUND warnings). Consider: if the plan resolver finds FUNCTION_NOT_FOUND warnings (even non-blocking), report them prominently in the error response so they get picked up regardless of what the blocking error was.

5. **describe_function was never called** across this run (or any recent run observed). The guidance text says to call it but the agent never does. This is likely because: (a) FUNCTION_NOT_FOUND errors that reach the agent carry enough information (function name + "Did you mean" suggestions) that the agent reasons without needing to probe, (b) describe_function reveals pin types but not whether a function exists. Consider renaming the guidance to something more actionable: instead of "call describe_function", say "use blueprint.describe to check what functions BP_Bow_C actually has."

6. **plan_json rate regression (66.7% vs 09q's 75%):** The 3 failures in this run were all on BP_Bow's `Fire` function (3 attempts). The first two failures were avoidable with fixes #1, #2, #3 above. With those fixes, this run would have been 9/9 (100%) plan_json or 8/9 (88.9%).

7. **Run time regression (4:29 vs 09q's 4:09):** The ~20s delta is entirely attributable to the two failed Fire function attempts. With fixes, this would drop to sub-4:00.

8. **GetMesh silent fallback is working correctly** for Character blueprints where "Mesh" is an SCS component. The resolver's behavior of treating `GetMesh` as `get_var("Mesh")` is the right fallback. No fix needed.

9. **SetInstigator UPROPERTY workaround is working.** The agent discovered independently (on attempt 3) that `set_var, target: "Instigator"` is the right approach. This is actually the correct implementation behavior. Adding the alias map entry (#3 above) would prevent the 2-retry cost.

10. **All 3 BPs compiled SUCCESS.** The final product quality was high — full spawn-attach flow, Fire function with cooldown timer, damage system on arrow, input binding. The two bugs (#1, #2) cost ~25s in retries but did not prevent successful completion.
