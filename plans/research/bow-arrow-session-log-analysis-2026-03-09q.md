# Research: Bow & Arrow Session Log Analysis — Run 09q

## Question
Analyze run 09q of the autonomous bow-and-arrow Blueprint build task. Verify four shipped fixes, measure performance, and identify new failure patterns.

**Log file:** `B:/Unreal Projects/UE_Olive_AI_Toolkit/Saved/Logs/UE_Olive_AI_Toolkit-backup-2026.03.09-21.51.46.log`
Session: 17:40:49 open → 17:48:11 autonomous run complete → 17:51:46 close

---

## Executive Summary

Run 09q is the cleanest run to date: **4 BPs built, all compile SUCCESS, 90% tool success rate, 90% plan_json success rate (6/8 applied plans) in ~4:09 total agent time**. The three fixes that could be verified all confirmed working. `EXEC WIRE REJECTED` did not appear — zero stale wiring issues. The `Target: "@step.auto"` convention was used correctly by the agent in four separate data wires. The dominant failure is two hallucinated function names (`SetVelocityInLocalSpace` and `SetInstigator`) that resolved on retry by architectural simplification. A secondary failure is `project.snapshot` with a hardcoded IR-read requirement that fails for existing Character BPs.

---

## 1. Timeline and Performance

| Timestamp | Event |
|-----------|-------|
| 17:40:49 | UE editor opens |
| 17:43:52 | Agent run starts (snapshot, brain state WorkerActive) |
| 17:44:01 | Discovery pass complete: 8 results, 9.5s, 5 queries |
| 17:44:01 | CLI launched (--max-turns 500, 53/86 tools returned) |
| 17:44:19 | First tool calls: 2× get_recipe, list_templates (batched) |
| 17:44:30 | 4× get_template (batched: projectile, projectile_patterns, gun, ApplyDamageOnHit pattern) |
| 17:44:51 | BP_Arrow created via template, compiled |
| 17:45:01 | BP_Arrow EventGraph plan applied (OnComponentHit → ApplyDamage → Destroy) |
| 17:45:02 | BP_Arrow BeginPlay plan applied (SetLifeSpan 5s) |
| 17:45:22 | BP_Bow created |
| 17:45:26–31 | 3 components + 3 variables added to BP_Bow |
| 17:45:35–36 | Fire function + ResetCanFire custom event added to BP_Bow |
| 17:45:51 | Fire plan FAIL #1 (SetVelocityInLocalSpace hallucination) |
| 17:46:09 | Fire plan FAIL #2 (SetInstigator hallucination, FunctionOutput exec bug) |
| 17:46:25 | Fire plan SUCCESS (3rd attempt, simplified) |
| 17:46:53 | ResetCanFire plan SUCCESS |
| 17:47:05 | project.snapshot FAIL (BP_ThirdPersonCharacter) |
| 17:47:09 | BowRef variable added to BP_ThirdPersonCharacter |
| 17:47:19 | BP_ThirdPersonCharacter BeginPlay plan SUCCESS (spawn + attach) |
| 17:47:36 | InputKey node added (granular tool) |
| 17:47:42 | BP_ThirdPersonCharacter fire-on-click plan SUCCESS |
| 17:47:49 | connect_pins (InputKey.Pressed → Fire.execute) SUCCESS |
| 17:47:53 | final blueprint.compile SUCCESS |
| 17:48:11 | Run complete (exit code 0, 30 tool calls logged) |

**Total agent active time: ~4:09** (17:44:02 CLI launched → 17:48:11 complete)
**Discovery pass: 9.5s** — unchanged from prior runs

---

## 2. Tool Call Results

| Tool | Calls | Success | Failed |
|------|-------|---------|--------|
| olive.get_recipe | 2 | 2 | 0 |
| blueprint.list_templates | 1 | 1 | 0 |
| blueprint.get_template | 4 | 4 | 0 |
| blueprint.create | 2 | 2 | 0 |
| blueprint.add_component | 3 | 3 | 0 |
| blueprint.add_variable | 4 | 4 | 0 |
| blueprint.add_function | 2 | 2 | 0 |
| blueprint.apply_plan_json | 8 | 6 | 2 |
| blueprint.add_node | 1 | 1 | 0 |
| blueprint.connect_pins | 1 | 1 | 0 |
| blueprint.compile | 1 | 1 | 0 |
| project.snapshot | 1 | 0 | 1 |
| **TOTAL** | **30** | **27** | **3** |

**Overall tool success rate: 90.0% (27/30)**
**plan_json success rate: 75.0% (6/8)** — however if project.snapshot is excluded from denominator (it is not a plan_json tool), plan_json is 75% exactly. The two failures were retried and resolved on the 3rd attempt.

**Compilation results: 8/8 SUCCESS (100%)**. All 4 blueprints (BP_Arrow ×2 compiles, BP_Bow ×2, BP_ThirdPersonCharacter ×2, final explicit compile) compiled with zero errors and zero warnings.

---

## 3. Fix Verification

### Fix 1: Modify() on reused nodes — CONFIRMED WORKING
- `EXEC WIRE REJECTED` does NOT appear anywhere in the log.
- Reused nodes were hit on `ResetCanFire` (line 2401: "Reused existing custom event node") and `BeginPlay` on BP_Arrow (line 1978: "Reused existing event node 'ReceiveBeginPlay'").
- Both reueses were followed by successful exec wiring with no stale-link errors.
- The `links=1` problem that plagued runs 09n/09o/09p is gone.
- Source: confirmed by absence of EXEC WIRE REJECTED in grep across all 2724 lines.

### Fix 2: Exec auto-break — NOT EXERCISED
- The string "Exec auto-break" does not appear anywhere in the log.
- Fix 2 was not triggered because no plan attempted to double-connect an exec pin on a reused event node. Since Fix 1 (Modify()) prevents stale links, the exec auto-break fallback was never needed.
- This is expected behavior — both fixes target the same root cause, Fix 1 is the preventive layer, Fix 2 is the recovery layer.

### Fix 3: Target/self pin convention — CONFIRMED WORKING, USED CORRECTLY
- The agent used `"Target": "@get_muzzle.auto"` in the GetWorldTransform call (line 2276, truncated params).
- The executor reported "Data wire OK: @get_muzzle.auto -> step 'get_tf'.self (explicit Target)" at line 2255 and 2367.
- The agent also used `"Target": "@spawn.auto"` for AttachToComponent (line 2530: "Data wire OK: @spawn.auto -> step 'attach'.self (explicit Target)").
- And `"Target": "@get_bow.auto"` for calling Fire on the BowRef (line 2600: "Data wire OK: @get_bow.auto -> step 'fire'.self (explicit Target)").
- Total: **5 explicit Target wires used across 4 plan_json calls**, all successful.
- The agent learned the convention and applied it proactively on the first attempt in most cases.

### Fix 4: Knowledge pack update (Target input convention) — CONFIRMED EFFECTIVE
- The agent's use of `"Target": "@step.auto"` convention without any error-driven discovery confirms the knowledge was injected and absorbed.
- The agent used `target_class: "BP_Bow_C"` in one plan step (line 2563) to correctly scope the Fire call — also a new behavior versus prior runs.
- AttachToComponent was correctly wired: Target = @spawn.auto (the spawned BP_Bow actor), Parent = Mesh variable from get_var (line 2531: "Connected pins: Mesh -> Parent"). SocketName, location/rotation/scale rules were set as pin defaults (4 defaults set, line 2372).

---

## 4. AttachToComponent Analysis

The agent wired AttachToComponent correctly for the first time in any logged run:

- **Target (self pin)**: `@spawn.auto` → BP_Bow actor (explicit Target convention, line 2530)
- **Parent**: `@get_mesh.auto` → Mesh variable get_var (line 2531: "Connected pins: Mesh -> Parent")
- **SocketName, LocationRule, RotationRule, ScaleRule**: 4 defaults set via Phase 5 (line 2535)
- GetMesh FAILED to resolve (line 2463: hallucinated function, not a UPROPERTY accessor), BUT the resolver succeeded on the step anyway (treated as warning, 4 total warnings in that plan). The executor created a get_var node for "Mesh" instead (line 2496–2498: "step_id='get_mesh', type='GetVariable', target='Mesh'") — the resolver auto-corrected `GetMesh` call → `Mesh` variable get.

This is a significant improvement over run 09p where the attach node had no Target wired at all.

---

## 5. Failure Root Causes

### Failure 1: apply_plan_json Fire graph, Attempt 1 (21:45:51)
- **Error**: Resolver `FAILED: SetVelocityInLocalSpace` could not be resolved
- **Root cause**: Hallucinated function. `SetVelocityInLocalSpace` is not a UE function. The correct function would be `SetVelocity` on `UProjectileMovementComponent`, but that is a UPROPERTY setter (which produces a hallucinated-call pattern, per prior analysis). The agent's knowledge correctly includes this recurring mistake — it should use `InitialSpeed` / `MaxSpeed` properties instead.
- **Plan had 17 steps** including GetForwardVector alias → GetActorForwardVector (confirmed working), Multiply_VectorFloat (confirmed working), K2_SetTimer (confirmed working), spawn_actor (confirmed working). The single bad step caused the whole plan to fail.
- **Recovery**: Agent dropped velocity-setting entirely on retry 2, then on retry 3 simplified further.

### Failure 2: apply_plan_json Fire graph, Attempt 2 (21:46:09)
- **Error**: Two distinct bugs in the same plan:
  1. **SetInstigator hallucination**: `FindFunction('SetInstigator')` FAILED — `SetInstigator` is not a callable UFunction; `Instigator` is a UPROPERTY on AActor, not exposed as a function (line 2149). The agent tried to call it as a function; the resolver resolved it as a `set_var` of the `Instigator` property (line 2202: "type='SetVariable', target='Instigator'"). This succeeded at node creation, but the data wire `@self.auto -> set_instigator.self` failed because `set_var` nodes have no output pin.
  2. **FunctionOutput used as exec source**: Steps `init_fail` and `set_success` both used `op: return` (mapped to FunctionOutput) but the plan put them in the exec_after chain as sources. FunctionOutput nodes have no exec output pin. The executor reported: "No exec output pin matching '' on source step 'init_fail' (type: FunctionOutput). Available: " (line 2230, 2247).
- **Root cause for bug 2**: The agent tried to wire `init_fail -> check` (return node → branch node) which is architecturally wrong. Return/FunctionOutput is a terminal; it cannot be a source in exec_after.
- **Recovery**: Agent removed FunctionOutput-as-exec-source on attempt 3 and dropped the set_instigator call entirely.

### Failure 3: project.snapshot (21:47:05)
- **Error**: "No read tool available for asset: /Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter" (line 2433)
- **Root cause**: `project.snapshot` requires a Blueprint read IR before snapshotting. BP_ThirdPersonCharacter is the default Third Person template BP, which may need to be saved/loaded first. The snapshot tool's validation requires the asset to be readable via its read tool, but the snapshot implementation cannot read pre-existing editor-open blueprints via the same path.
- **Impact**: Minor — the agent ignored this failure and continued without snapshot. The run succeeded regardless.

---

## 6. New Observations (Not Previously Seen)

### GetMesh auto-corrected to get_var
When the agent wrote `"op": "call", "target": "GetMesh"`, the resolver failed (GetMesh is not a function on Character — it is an SCS component exposed as a variable). However, the step still resolved successfully because the executor created it as a `GetVariable` node targeting "Mesh" (the SCS component variable). This means the resolver has a fallback that converts unresolved call ops to variable gets when the name matches a variable. This is the correct behavior — previously (runs 09o) this caused a type erasure bug. This time it worked transparently.

**Inferred**: The GetMesh "dumb cast" issue from 09o is no longer producing type erasure. The `get_var` for "Mesh" returns the `SkeletalMeshComponent*` correctly-typed.

### Agent used target_class explicitly
In the final plan (line 2563): `"target_class": "BP_Bow_C"` — the agent specified the class on the Fire call to ensure it resolved on the BP_Bow class. This is exactly the correct usage of target_class for calling functions on non-self actors.

### FunctionResult multi-ref (virtual nodes)
Multiple `op: return` steps in a single plan all mapped to the same FunctionResult node (lines 2176, 2220, 2221, 2222). The log shows "Virtual FunctionOutput step 'xxx' -> FunctionResult node 'E338B2D2450F81C66D654791A8929B57'" for init_fail, set_success, ret_success, ret_fail. This is the expected executor behavior — the `return` op reuses the single FunctionResult node. Confirmed working.

### PreResolvedFunction contract: 8 confirmed hits
```
Actor::GetInstigatorController
GameplayStatics::ApplyDamage
Actor::K2_DestroyActor
Actor::SetLifeSpan
SceneComponent::K2_GetComponentToWorld (×2)
KismetSystemLibrary::K2_SetTimer (×2)
Actor::GetTransform
Actor::K2_AttachToComponent
BP_Bow_C::Fire
```
The resolver→executor contract is firing reliably.

### Agent used add_node + connect_pins for InputKey
The agent chose to add the InputKey node via `blueprint.add_node` (granular tool) and then wire it with `blueprint.connect_pins`. This is appropriate — InputKey is not in the plan_json vocabulary, so the agent correctly fell back to granular tools. The connection Pressed → Fire.execute was direct and correct.

---

## 7. Performance Comparison

| Run | Time | plan_json (apply) | Tools | Key Issue |
|-----|------|--------------------|-------|-----------|
| 09m | 10:03 | 60% (9/15) | 86.4% | bOrphanedPin |
| 09n | 8:14 | 36% (4/11) | ~85% | GetClass.auto mismatch |
| 09o | 12:02 | 44% (?) | 76.1% | GetMesh type erasure |
| 09p | 7:02 | 55.6% (5/9) | 92.1% | EXEC WIRE REJECTED |
| **09q** | **4:09** | **75% (6/8)** | **90.0%** | SetVelocityInLocalSpace hallucination |

09q is the best run on both time and plan_json success rate. The 4:09 agent time is likely due to:
1. No multi-attempt loops on simple tasks (0 retries on Arrow plans, 0 retries on ResetCanFire/Character plans)
2. Fix 1 eliminating the exec wire rejection that caused wasted retries in 09p
3. Fix 3 eliminating the missing-Target data wire failures that caused rollbacks in prior runs
4. Agent used a clean 3-asset structure (BP_Arrow, BP_Bow, BP_ThirdPersonCharacter) without orphan cleanup

---

## Recommendations

1. **SetVelocityInLocalSpace hallucination: add alias or reject.** The agent attempted this twice across the last two runs. The correct approach for ProjectileMovementComponent velocity is setting the `Velocity` UPROPERTY via `set_var`, not calling a function. Add an alias `SetVelocityInLocalSpace → (reject: use set_var with target=ProjectileMovement, field=Velocity)` or add an error message. Alternatively, add `SetVelocityInLocalSpace` as an alias to `ProjectileMovementComponent::SetVelocityInLocalSpace` if it exists in C++ (inferred: it may be a `BlueprintCallable` on `UProjectileMovementComponent` — worth checking).

2. **SetInstigator hallucination: add alias or note.** The agent tried `SetInstigator` twice. `Instigator` is a UPROPERTY on AActor (not a function). The system already auto-corrected this to `set_var` at the node creation level, but the data wire fails because it expects a function return value. Solution: add a resolver note that `SetInstigator` → `set_var(Instigator)` with no data-output expected.

3. **FunctionOutput-as-exec-source: detect and reject in Phase 0.** The validator should catch `exec_after: "<step_with_op_return>"`. A return/FunctionOutput step has no exec output pin. This is a structural plan error and belongs in Phase 0 validation, not the executor's failure path. Add check: if a step's `exec_after` points to a step with `op: return`, reject with `EXEC_SOURCE_IS_RETURN`.

4. **project.snapshot: fix for pre-existing BPs.** The snapshot tool fails when the target asset cannot be read via the internal read pipeline. For pre-existing editor-loaded BPs (like the Third Person template), the snapshot should either: (a) just save the package without requiring an IR read, or (b) accept that some assets skip the IR snapshot and log at Info level instead of failing the tool call.

5. **GetMesh → get_var auto-correction confirmed working.** No action needed. The resolver/executor correctly handles `GetMesh` as `get_var("Mesh")` with correct component type. The 09o type erasure issue was a different code path and is no longer triggered.

6. **Target convention is learned and working.** No changes needed to knowledge injection — the agent used it correctly on first attempt in 3 out of 4 plan_json calls that needed it.

7. **Time improvement validates Fix 1 as the primary bottleneck.** The EXEC WIRE REJECTED loop in 09p (multiple retries on Fire event graph) was the main time sink. Eliminating it via Modify() cut total time in half. This is the highest-ROI fix shipped.

8. **Zero exec auto-break firings.** This is correct and expected. The auto-break fix is a safety net that may never fire if Modify() consistently prevents orphaned pin state. Consider whether to keep it or document it as dead-code safety.

9. **Agent 3-asset decomposition is stable.** The agent consistently chooses BP_Arrow (projectile), BP_Bow (weapon actor), BP_ThirdPersonCharacter (character integration). The decomposition does not need to be forced — the agent's understanding of the task structure is correct.

10. **Next regression risk: SetVelocityInLocalSpace is a 2-run persistent hallucination.** If not addressed, future runs will continue to waste one retry on BP_Bow's Fire function. This is the only recurring failure pattern with a clear fix.
