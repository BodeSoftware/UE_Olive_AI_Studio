# Research: Bow-Arrow Session Log Analysis — Run 08d

## Question
Analyze the run after (a) softening the verify-first directive and (b) raising the runtime limit to 1800s. Compare against 08b (best run, 88% success, 19 min) and 08c (regression, 77% success, 36 min / killed at 900s). Did quality recover?

---

## Findings

### 1. Timeline — Phase Start/End/Duration

| Phase | Start (UTC-5) | End | Duration | Notes |
|-------|------------|-----|----------|-------|
| UE startup + HotReload build | 00:38:52 | 05:39:37 | ~5 min | 131 actions, DLL rebuilt |
| Plugin initialized / MCP ready | 05:39:37 | — | — | line 1647 |
| User message received | 05:43:08 | — | — | line 1787 |
| Scout (CLI, no LLM) | 05:43:08 | 05:43:24 | 16.0s | 5 community BP searches, 3 overviews loaded |
| Planner (MCP, 15 turns) | 05:43:25 | 05:44:44 | 79.7s | 6,982-char plan output; 13 `get_template` calls |
| Validator (C++) | 05:44:44 | 05:44:44 | 0.004s | 1 issue, non-blocking |
| Total pipeline | — | 05:44:44 | **95.7s** | line 1906 |
| Builder (autonomous Claude Code, `--max-turns 500`) | 05:44:44 | 06:02:32 | **~17.8 min** | exit code 0, 50 tool calls logged |
| Reviewer (async) | 06:02:32 | 06:03:08 | 35.9s | SATISFIED |
| Brain reset to Idle | 06:03:08 | — | — | run completed, outcome=0 |
| **Total wall clock (user msg → idle)** | 05:43:08 | 06:03:08 | **~20.0 min** | |

**Session note:** UE editor was open from 00:38 to 06:04 (user manually closed). No wall-clock kill. 1800s limit was never approached — Builder completed in ~17.8 min.

---

### 2. What Was Built

Three assets created/modified under `/Game/BowAndArrow/` and `/Game/ThirdPerson/Blueprints/`:

**BP_Arrow** (`Actor`, created fresh at `/Game/BowAndArrow/BP_Arrow`)
- Components: CollisionSphere (SphereComponent), ArrowMesh (StaticMeshComponent), ProjectileMovement (ProjectileMovementComponent)
- Variables: ArrowSpeed (float, 3000.0), GravityScale (float, 0.5), Damage (float, 20.0), bArrowFired (bool), bArrowStuck (bool)
- Functions: InitializeArrow(Speed, Gravity), LaunchArrow(Velocity)
- EventGraph: BeginPlay (deactivate projectile + disable collision), CollisionSphere.OnComponentBeginOverlap handler (stick on impact, apply damage), granular nodes for GetActorForwardVector → ApplyPointDamage
- Final compile: SUCCESS, Errors: 0, Warnings: 0

**BP_BowAndArrowComponent** (`ActorComponent`, created at `/Game/BowAndArrow/BP_BowAndArrowComponent`)
- Variables: CharacterRef (Character object ref), BowAiming (bool), ArrowLoaded (bool), DrawBow (bool), PreventFireSpam (bool), SpawnedArrow (BP_Arrow_C ref), ArrowSpeed (float, 3000.0), GravityScale (float, 0.5), ArrowClass (BP_Arrow_C class ref)
- Event Dispatcher: OnBowAimingChanged(bIsAiming: bool)
- Functions: StoreComponents, AimSetup, ReleaseAimSetup, CreateArrow, LaunchConditions (pure), GetVectorInfrontOfCamera, SetArrowVelocity(EndLocation→OutVelocity), FireArrow, DestroyArrow, ResetAllVariables, ResetFireCooldown (added mid-session)
- EventGraph: BeginPlay → StoreComponents
- All functions have substantive logic (not stubs — see section 5)
- Final compile: SUCCESS, Errors: 0, Warnings: 0

**BP_ThirdPersonCharacter** (modified, `/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter`)
- BowAndArrowComp (BP_BowAndArrowComponent_C) added as component
- EventGraph: RightMouseButton (Pressed→AimSetup, Released→ReleaseAimSetup), LeftMouseButton (Pressed→FireArrow) wired via K2Node_CallFunctionOnMember
- Old failed nodes (node_5, node_6, node_7) cleaned up via remove_node
- Final compile: SUCCESS, Errors: 0, Warnings: 0

**Completeness assessment:** Full system delivered. All 11 functions of BP_BowAndArrowComponent have logic. Input bindings wired. BP_Arrow has full lifecycle (init → launch → impact → stick/damage). Reviewer reported SATISFIED, 0 missing, 0 deviations.

---

### 3. Tool Call Scorecard

From `MCP tools/call result:` lines (Builder phase only — Planner used `blueprint.get_template` only):

| Tool | Success | Failed | Total | Success % |
|------|---------|--------|-------|-----------|
| blueprint.get_template | 15 | 0 | 15 | 100% |
| blueprint.apply_plan_json | 13 | 10 | 23 | 57% |
| blueprint.compile | 10 | 0 | 10 | 100% |
| blueprint.add_variable | 14 | 0 | 14 | 100% |
| blueprint.add_function | 14 | 2 | 16 | 88% |
| blueprint.add_node | 13 | 3 | 16 | 81% |
| blueprint.connect_pins | 16 | 2 | 18 | 89% |
| blueprint.remove_node | 8 | 0 | 8 | 100% |
| blueprint.add_component | 4 | 1 | 5 | 80% |
| blueprint.get_node_pins | 2 | 0 | 2 | 100% |
| blueprint.disconnect_pins | 1 | 0 | 1 | 100% |
| blueprint.create | 2 | 0 | 2 | 100% |
| **TOTAL** | **112** | **18** | **130** | **86%** |

Scout phase (not MCP): 5x `olive.search_community_blueprints` — all success.
Planner phase (filtered MCP): 13x `blueprint.get_template` — all success (included in table above).

**Overall tool success rate: 86%** (112/130). Up from 77% in 08c. Slightly below 08b's 88%.

**plan_json success rate: 57%** (13/23 apply calls). This is the lowest of the three runs (08b: 67%, 08c: 67%). However — the 10 failures are genuine retry events, not terminal failures. The Builder recovered from all of them.

---

### 4. Every Failure — Line, Tool, Error, Root Cause

**Failure 1 — lines 2064–2076** — `blueprint.apply_plan_json` FAILED
- Target: BP_Arrow / InitializeArrow
- Error: `SetInitialSpeed` and `SetMaxSpeed` not found on ProjectileMovementComponent
- Root cause: These are FProperty members (not UFunctions) on UProjectileMovementComponent. The Builder correctly identified them as settable but used function call syntax. `SetFloatPropertyByName` / `SetDoublePropertyByName` were attempted next.
- **Catalog gap:** ProjectileMovementComponent has no SetInitialSpeed/SetMaxSpeed UFUNCTION. The correct approach is direct property access via `set_var` or `SetDoublePropertyByName`.

**Failure 2 — lines 2097–2111** — `blueprint.apply_plan_json` FAILED (retry of above)
- Error: `SetFloatPropertyByName` not found. The function name is correct in KismetSystemLibrary but the Builder did not provide a class hint.
- Root cause: FindFunction with no `target_class` and no alias for `SetFloatPropertyByName` (it was added in UE 5.5 and may not be in the alias map at 216 entries, or the universal scan missed it).
- **Second catalog gap.**

**Failure 3 — lines 2144–2146** — `blueprint.apply_plan_json` FAILED
- Error: `LogOlivePlanValidator: Phase 0 FAILED: 1 errors, 0 warnings`
- Root cause: An EXEC_WIRING_CONFLICT or COMPONENT_FUNCTION_ON_ACTOR structural violation. Exact validator message truncated. Builder moved on after this.

**Failure 4 — lines 2629–2630** — `blueprint.apply_plan_json` FAILED
- Target: BP_Arrow / EventGraph (OnComponentBeginOverlap plan)
- Error: Phase 5 - 1 default failed; also a compile FAILED immediately after at line 2623: `'Hit from Direction' in action 'Apply Point Damage' must have an input wired into it`
- Root cause: The ApplyPointDamage node's `HitFromDirection` pin requires a wired input (by-ref parameter), but the plan used a literal default. Builder caught this on the next turn and added GetActorForwardVector → connect to HitFromDirection granularly.

**Failure 5 — lines 2780–2781** — `blueprint.add_function` FAILED (WriteRateLimit)
- Error: `Validation failed for blueprint.add_function: rule WriteRateLimit`
- Root cause: Hit the MaxWriteOpsPerMinute = 30 rate limit. Builder was creating functions for BP_BowAndArrowComponent rapidly.
- **Note:** This is a throttle artifact. Builder waited and retried — line 2820 shows a second WriteRateLimit hit before the retry at line 2835 succeeded.

**Failure 6 — lines 2820–2821** — `blueprint.add_function` FAILED (WriteRateLimit) — same root cause as above.

**Failure 7 — lines 3060–3073** — `blueprint.apply_plan_json` FAILED (Fix B triggered)
- Target: BP_BowAndArrowComponent / GetVectorInfrontOfCamera
- Error: `Data wire FAILED: No input pin matching 'InRot' on step 'get_fwd' (type: CallFunction). Available: self (Actor Object Reference)` — **marking for rollback**
- Root cause: Builder tried to wire Rotator output from GetControlRotation directly to `GetForwardVector` which uses pin name `InRot`. The function resolved as GetForwardVector from a context that didn't expose that pin name. Builder pivoted to granular add_node path.
- **Fix B (rollback) triggered and worked correctly.**

**Failure 8 — lines 3412–3424** — `blueprint.apply_plan_json` FAILED (Fix B triggered)
- Target: BP_BowAndArrowComponent / CreateArrow
- Error: `Data wire FAILED: @get_char_loc.auto: No output pin on step 'get_char_loc' matches target type 'Transform'. Available: ReturnValue (Vector)` — **marking for rollback** (2 wires failed)
- Root cause: GetActorLocation returns Vector, but spawn_actor SpawnTransform expects Transform. Builder pivoted to MakeTransform node insertion (granular path).
- **Fix B triggered and worked correctly.** Builder added MakeTransform node (line 3426) and connected Location→MakeTransform→SpawnTransform.

**Failure 9 — lines 3474–3485** — `blueprint.connect_pins` and `blueprint.add_node` FAILED
- Error for connect_pins: pin connection failed (node_31.ReturnValue → node_32.Parent) — type mismatch (SpawnedArrow is BP_Arrow ref, AttachToActor needs SceneComponent)
- Error for add_node: `Cast node requires 'target_class' property` — Builder sent `{"type":"Cast","properties":{"TargetType":"SceneComponent"}}` with wrong key
- Root cause: Two sequential mistakes on the same cast node. Builder self-corrected on retry (line 3487: used `target_class` key, succeeded).

**Failure 10 — lines 3838–3839** — `blueprint.apply_plan_json` FAILED
- Target: BP_BowAndArrowComponent / DestroyArrow
- Error: `GetBoolPropertyByName` not found — same class-less lookup issue as Failure 2.
- Root cause: The function exists in KismetSystemLibrary but Builder omitted `target_class`. Builder replaced with a variable access approach on retry (succeeded at line 3841+).

**Failure 11 — lines 4032–4033** — `blueprint.apply_plan_json` FAILED
- Target: BP_BowAndArrowComponent / FireArrow
- Error: `Plan resolution failed: 12/13 steps resolved, 1 errors, 2 warnings` — `do_once` op failed to resolve.
- Root cause: The `do_once` op resolves to a StandardMacros macro instance. The plan placed it inside a function graph (FireArrow), not the event graph. `do_once` may not be valid in function graphs. Builder removed it in the successful retry.

**Failure 12 — lines 4179–4180** — `blueprint.apply_plan_json` FAILED
- Target: BP_BowAndArrowComponent / FireArrow
- Error: 1 exec wire failed (`spam_on.then → get_vel.execute` — TypesIncompatible). This is because `spam_on` was a `set_var` node with an already-wired `then` pin (to `get_end`), and the plan double-claimed it.
- Root cause: EXEC_WIRING_CONFLICT — same node's exec output assigned twice (exec_after on two different steps). Builder fixed by moving to single-chain exec, then using `blueprint.connect_pins` to add the missing exec wire granularly.

**Failure 13 — lines 4284–4285** — `blueprint.add_component` FAILED
- Error: `Component class 'BP_BowAndArrowComponent' not found`
- Root cause: Builder used the Blueprint asset name without `_C` suffix. Retry with `BP_BowAndArrowComponent_C` succeeded immediately (line 4287).

**Failure 14 — lines 4328–4329** — `blueprint.apply_plan_json` FAILED
- Target: BP_ThirdPersonCharacter / EventGraph
- Error: `Enhanced Input Action 'IA_Aim' not found. Available Input Actions in project: [IA_Jump, IA_Look, IA_Move]`
- Root cause: The Planner assumed IA_Aim exists. It does not in this project. Builder pivoted to raw InputKey nodes (RightMouseButton, LeftMouseButton) — the correct fallback for a vanilla ThirdPerson template.

**Failure 15 — lines 4355–4356** — `blueprint.add_node` FAILED
- Error: `FindFunction('AimSetup' [resolved='AimSetup'], class=''): FAILED` — Builder forgot to include `class` in the properties (passed `function_name` and `class` but FindFunction searched with no class).
- Root cause: The `add_node` with `type: CallFunction` requires function_name to be in the default search scope of BP_ThirdPersonCharacter. AimSetup lives on BP_BowAndArrowComponent_C which is not in that scope. Builder pivoted to `K2Node_CallFunctionOnMember` (correct path).

**Failure 16 — lines 4532+, 4576–4577** — `blueprint.add_node` FAILED
- Error: K2Node_CallFunction with FunctionReference.MemberParent property not setting correctly
- Root cause: `FunctionReference.MemberName` and `FunctionReference.MemberParent` are not direct UProperties on UK2Node_CallFunction accessible via reflection. Builder tried and failed, then used the `K2Node_CallFunctionOnMember` approach which worked (line 4373+).

**Failures 17–18** — `blueprint.connect_pins` FAILED (lines 3532–3533, plus an earlier instance)
- One was the exec pin on a node that was already connected; the other was a type mismatch on the cast node Parent pin.
- Both self-corrected.

---

### 5. Did the Builder Simplify / Gut Any Functions?

**No.** This is the clearest answer in the log.

AimSetup (the function gutted to a PrintString stub in run 08c) is fully implemented here:

```
AimSetup graph — 9 nodes successfully created and wired:
  node_36: GetVariable(BowAiming)
  node_37: Branch(Condition=@BowAiming)
  node_38: SetVariable(BowAiming=true)
  node_39: GetVariable(CharacterRef)
  node_40: CallFunction(GetComponentByClass) [GetCharacterMovement rewrite]
  node_41: CallFunction(SetBoolPropertyByName) [orient-to-movement flag]
  node_42: CallFunction(CreateArrow)
  node_43: CallDelegate(OnBowAimingChanged)
  node_44: Comment
Compile: SUCCESS, Errors: 0
```

ReleaseAimSetup: 10 nodes (symmetric inverse of AimSetup, includes bool resets + CMC restore + delegate broadcast). Compile SUCCESS.

FireArrow: 13 nodes (LaunchConditions gate → PreventFireSpam → GetVectorInfrontOfCamera → SetArrowVelocity → DetachFromActor → LaunchArrow → reset vars → K2_SetTimer for cooldown). Compile SUCCESS after one granular exec pin connect_pins fix.

DestroyArrow: Retried once (GetBoolPropertyByName failure) then implemented with is_valid guard + ArrowLoaded check + SetArrowLoaded(false) + K2Node_DestroyActor.

GetVectorInfrontOfCamera: Failed the plan_json path (InRot pin name), then Builder added Conv_RotatorToVector node granularly and wired Rotator→Conv→Multiply(ArrowSpeed)→Return. Compiles clean.

CreateArrow: Failed once (Transform mismatch), Builder added MakeTransform node + wired Location→MakeTransform→SpawnTransform + AttachToActor (with cast to SceneComponent via K2Node_DynamicCast).

**Zero PrintString stubs observed anywhere in the log.** No log entries containing "print_string" or "PrintString" in the graph mutation phase.

---

### 6. Did Compile-Per-Function Work?

Yes. The Builder compiled after completing each major section:

| Compile # | Time | Asset | Result |
|-----------|------|-------|--------|
| 1 | 05:45:53 | BP_Arrow (initial) | SUCCESS |
| 2 | 05:46:53 | BP_Arrow (after InitializeArrow plan 3) | SUCCESS |
| 3 | 05:47:03 | BP_Arrow (after LaunchArrow) | SUCCESS |
| 4 | 05:47:11 | BP_Arrow (after BeginPlay plan) | SUCCESS |
| 5 | 05:47:53 | BP_Arrow (after overlap plan) | **FAILED** — HitFromDirection by-ref |
| 6 | 05:48:21 | BP_Arrow (after granular HitFromDirection fix) | SUCCESS |
| 7 | 05:50:01 | BP_BowAndArrowComponent (initial) | SUCCESS |
| 8 | 05:50:15 | BP_BAC (after StoreComponents) | SUCCESS |
| 9 | 05:50:21 | BP_BAC (after BeginPlay→StoreComponents) | SUCCESS |
| 10 | 05:51:11 | BP_BAC (after GetVectorInfrontOfCamera partial) | SUCCESS |
| 11 | 05:51:19 | BP_BAC (after LaunchConditions) | SUCCESS |
| 12 | 05:51:33 | BP_BAC (after SetArrowVelocity) | SUCCESS |
| 13 | 05:53:22 | BP_BAC (after CreateArrow partial) | SUCCESS |
| 14 | 05:53:34 | BP_BAC (after AimSetup) | SUCCESS |
| 15 | 05:53:46 | BP_BAC (after ReleaseAimSetup) | SUCCESS |
| 16 | 05:56:01 | BP_BAC (after DestroyArrow) | SUCCESS |
| 17 | 05:56:57 | BP_BAC (after ResetFireCooldown) | SUCCESS |
| 18 | 05:57:46 | BP_BAC (after FireArrow partial + granular exec fix) | SUCCESS |
| 19 | 05:57:53 | BP_BAC (after ResetAllVariables — auto-compile) | SUCCESS |
| 20 | 06:00:15 | BP_ThirdPersonCharacter | **FAILED** — Target not connected on K2Node_CallFunctionOnMember |
| 21 | 06:01:41 | BP_ThirdPersonCharacter (after custom_event plan) | SUCCESS |
| 22 | 06:02:14 | BP_ThirdPersonCharacter (final verify) | SUCCESS |
| 23 | 06:02:17 | BP_Arrow (final verify) | SUCCESS |
| 24 | 06:02:18 | BP_BAC (final verify) | SUCCESS |

**24 total compile events. 2 failures, both self-corrected.** Compile-per-function strategy is working — the HitFromDirection failure (compile 5) triggered an immediate targeted fix (granular GetActorForwardVector node + connect) without needing broader self-correction.

---

### 7. Did Fix B (Data Wire Rollback) Trigger?

Yes — **twice**, and both times it worked correctly:

1. **Line 3060** — `GetVectorInfrontOfCamera`: `InRot` pin not found on GetForwardVector. Rollback triggered. Builder pivoted to granular granular Conv_RotatorToVector insertion.
2. **Line 3412** — `CreateArrow`: `@get_char_loc.auto` returned Vector but SpawnTransform needed Transform. Rollback triggered. Builder added MakeTransform node and re-routed.

Both rollbacks led to successful corrections without loss of prior work (nodes from earlier plan calls were not affected, per Phase 5.5 scoping that only checks `Context.NodeIdToStepId` nodes).

**Fix B is confirmed working and was consequential here** — two functions that would have produced broken partial graphs instead came out clean.

---

### 8. Did the 1800s Runtime Limit Help?

Yes, substantively. The Builder ran for ~17.8 minutes (1068 seconds) without any termination event. In run 08c, the 900s (15 min) limit killed the process mid-work. In this run:
- Builder completed all functions and cleaned up after itself
- Final compiles and cleanup (lines 4741–4761) happened at ~06:02, 18 minutes in
- Reviewer (06:02:32–06:03:08) ran cleanly after the Builder exited normally (exit code 0)
- No "idle for", "terminating", or "timed out" entries anywhere in the Builder phase

The extra 900 seconds of headroom was used — the Builder needed ~17.8 min vs the 08c 15-min kill. Whether the change in quality (no gutting) is from the softer verify-first directive, the extra time, or both is unclear, but the outcome is unambiguously better.

---

### 9. Auto-Continues — How Many? Were They Needed?

**Zero auto-continues.** No `IsContinuationMessage`, `BuildContinuationPrompt`, or continuation-related log entries appear anywhere. The Builder ran as a single `--max-turns 500` session and exited cleanly (exit code 0, line 4763).

This is expected — with 500-turn headroom and 1800s runtime, the Builder had enough space to complete naturally without early exit.

---

### 10. How Did the Builder Handle Unknown Pin Names?

Several strategies observed, ranked by quality:

**Strategy A — Type auto-match (lines 2232, 2235, 2238):** When pin name was ambiguous, the system used type-based matching: `Warning: Type auto-match: 2 output pins match type, using first ('Speed')`. This resolved correctly in context (ArrowSpeed variable getter returned a float; plan used `@entry.Speed` auto-match). Three instances, all benign.

**Strategy B — Granular fallback (lines 3084–3118, 3426–3578):** When a plan_json pin wire failed (InRot, SpawnTransform type mismatch), Builder read existing node pins via `blueprint.get_node_pins`, then added intermediate nodes (`Conv_RotatorToVector`, `MakeTransform`) and wired with explicit `blueprint.connect_pins` using concrete node_id.pin_name references. This is the correct recovery path and it worked.

**Strategy C — Node type escalation (lines 4373–4411):** When `blueprint.add_node` with `type: CallFunction` failed to find component methods (AimSetup, ReleaseAimSetup, FireArrow on BP_BowAndArrowComponent_C), Builder escalated to `K2Node_CallFunctionOnMember` with `member_variable: BowAndArrowComp`. This worked (lines 4385–4411).

**Strategy D — Using @step.auto:** The Builder used `@step.auto` references throughout the plan_json steps (e.g., `@get_aim.auto`, `@get_char.auto`, `@cast_char.auto`). These resolved correctly in all successful plans. No instances of `@step.auto` causing incorrect type matches beyond the two rollback cases handled by Fix B.

**No `@step.auto` misuse.** The auto-match warning at lines 2232–2238 shows two Speed-typed pins; the system picked the first correctly. No cases of auto-match connecting to wrong semantic output.

**Zero function simplification.** When the Builder could not implement a function cleanly in plan_json, it did not fall back to PrintString. Instead it either retried with corrected parameters or used granular tools to bridge the gap.

---

## Summary Comparison: 08b vs 08c vs 08d

| Metric | 08b (baseline) | 08c (regression) | 08d (this run) |
|--------|---------------|------------------|----------------|
| Total wall clock | ~19 min | ~36 min (killed at 15 min, stuck) | **~20 min** |
| Builder runtime | ~15 min | Killed at 900s | ~17.8 min, clean exit |
| Tool success rate | 88% | 77% | **86%** |
| plan_json success % | 67% | 67% | 57% |
| plan_json retries | low | medium | **higher — but all recovered** |
| PrintString stubs | 0 | 1 (StartAim gutted) | **0** |
| Final compile status | SUCCESS | Unknown (killed) | **ALL SUCCESS** |
| Reviewer verdict | n/a | n/a | **SATISFIED, 0 missing, 0 deviations** |
| Fix B (rollback) triggers | confirmed x2 | confirmed x2 | **confirmed x2** |
| Auto-continues | unknown | unknown | **0** |
| describe_node_type calls | multiple | multiple (2 failed) | **0 — not used** |
| SetInitialSpeed/MaxSpeed lookup | n/a | failed → gutted | **failed → retried correctly** |

---

## Recommendations

1. **Quality recovered.** The 08c regression was primarily caused by the 900s kill mid-work and possibly the hard verify-first directive forcing premature simplification. 08d has no gutted functions, all compiles clean, Reviewer SATISFIED.

2. **plan_json success rate dropped to 57% but that is not alarming.** The 10 failures are all genuine first-attempt errors that the Builder corrected. No failure was terminal. The Builder treated failed plan_json calls as diagnostic information and pivoted correctly. More plan_json attempts with lower per-attempt success is acceptable if overall output quality is high.

3. **Two catalog gaps confirmed (again) — same as 08c:**
   - `SetInitialSpeed` and `SetMaxSpeed` on `ProjectileMovementComponent` — these are FProperty members, not UFunctions. The alias map does not map them. Correct resolution: `set_var` on `InitialSpeed`/`MaxSpeed` after getting the component, not a function call. This should be added to the alias map or documented in a "property not function" hint.
   - `SetFloatPropertyByName` / `GetBoolPropertyByName` with no `target_class` — FindFunction's universal scan missed these in KismetSystemLibrary. Root cause unknown; could be that the scan is scoped by some criteria that excludes those. Note: `SetDoublePropertyByName` (line 2148) with `target_class: KismetSystemLibrary` DID succeed on the third attempt.

4. **`do_once` op is likely invalid in function graphs.** Line 4031 shows plan resolution failure for `do_once` in the `FireArrow` function. The macro may be ubergraph-only. This should be validated and if so, the plan validator should reject `do_once`/`flip_flop`/`gate` ops in function graph context.

5. **`blueprint.add_node` with `type: CallFunction` does not scope function search to component classes.** The Builder had to use `K2Node_CallFunctionOnMember` to call `AimSetup` on `BowAndArrowComp`. This is a known limitation but worth documenting: granular `add_node` for component method calls should use `K2Node_CallFunctionOnMember` not `CallFunction`. The Planner should emit this guidance.

6. **IA_Aim not found (Enhanced Input)** — The Planner assumed IA_Aim exists. The system correctly reported available IAs (`IA_Jump, IA_Look, IA_Move`) in the error. Builder pivoted to raw InputKey nodes. This is acceptable behavior but the Planner should query available Input Actions during Scout rather than assuming.

7. **WriteRateLimit hit twice** — Two `blueprint.add_function` calls hit `MaxWriteOpsPerMinute = 30`. Consider whether 30 is the right limit for autonomous Builder sessions. At peak, the Builder was issuing ~2 write ops/second during the variable + function setup phase. This caused 2 extra failed tool calls and one minute of implicit delay.

8. **Fix B is production-ready.** Both rollback triggers in this run led to correct, targeted manual repairs. The "marking for rollback" message + clean transaction cancellation worked exactly as designed.

9. **The 1800s limit should be the new standard.** The Builder needed ~17.8 minutes for a 3-asset, 11-function system. 900s was genuinely inadequate for tasks of this complexity. Recommend formalizing 1800s as default or computing from asset count in the Planner phase.

10. **Reviewer is working.** SATISFIED with 0 missing / 0 deviations in 35.9s is a credible result given the actual output state (all compiles succeeded, all functions implemented). No false positives or false negatives observed.
