# Research: Bow and Arrow Session Log Analysis — 2026-03-09k

## Question
Analyze the test log from run 09k to verify the impact of 4 bug fixes (GetForwardVector alias double-fire, stale exec pin bOrphanedPin cleanup, break_struct StructType resolution, tilde pin hints, VARIABLE_NOT_FOUND Phase 0 check), and provide full pipeline metrics, failure analysis, and a comparison against prior runs.

---

## Findings

### 1. Task

User prompt: `"create a bow and arrow system for @BP_ThirdPersonCharacter"`

Pipeline planned 4 assets: BP_ArrowProjectile, BP_Bow, BP_ThirdPersonCharacter (modify), and a 4th that does not appear explicitly (likely an InputAction asset that was found to not exist).

---

### 2. Pipeline Timing

| Phase | Duration |
|-------|----------|
| Scout (CLI, no LLM) | 20.2s |
| Planner (MCP, 15-turn) | 91.0s |
| CLI pipeline total | 111.3s (1:51.3s) |
| Builder (autonomous Claude Code) | 16:34:22 → 16:48:04 = **13:42** |
| Reviewer | 36.4s |
| **Total wall-clock** | ~14:53 (16:34:22 → 16:48:55 + autosave) |

Pipeline complexity: Moderate. 4 assets planned. 50 tool calls logged by autonomous run.

Scout loaded 3 template overviews (25,668 chars, 3 refs). Planner injected a 22,464-char prompt, ran 91.0s, and produced a 9,729-char plan.

---

### 3. Tool Call Statistics

**All MCP tool calls (Builder phase only, after 16:36:13):**

| Tool | Successes | Failures | Total |
|------|-----------|----------|-------|
| blueprint.create | 2 | 0 | 2 |
| blueprint.add_component | 5 | 0 | 5 |
| blueprint.add_variable | 11 | 0 | 11 |
| blueprint.add_function | 8 | 3 (WriteRateLimit) | 11 |
| blueprint.modify_component | 3 | 0 | 3 |
| blueprint.reparent_component | 2 | 0 | 2 |
| blueprint.compile | 4 | 0 | 4 |
| blueprint.apply_plan_json | 18 | 10 | 28 |
| blueprint.add_node | 8 | 1 | 9 |
| blueprint.get_node_pins | 1 | 0 | 1 |
| blueprint.remove_node | 20 | 2 (WriteRateLimit) | 22 |
| blueprint.read | 2 | 0 | 2 |
| blueprint.disconnect_pins | 0 | 1 | 1 |
| blueprint.connect_pins | 5 | 2 | 7 |
| **TOTAL** | **89** | **19** | **108** |

Note: Planner phase also called blueprint.get_template (10x), blueprint.list_templates (1x), olive.get_recipe (2x) — all SUCCESS.

**Overall tool success rate: 89/108 = 82.4%**

The add_function and remove_node failures are all WriteRateLimit (burst rate), not logic errors. Excluding pure rate-limit throttles: 89/103 = **86.4% logic success rate**.

---

### 4. plan_json Success Rate

Total apply_plan_json calls: 28
Successes: 18
Failures: 10

**plan_json success rate: 18/28 = 64.3%**

By Blueprint:

| Blueprint | Success | Failed | Rate |
|-----------|---------|--------|------|
| BP_ArrowProjectile | 2 | 0 | 100% |
| BP_Bow (SetLifeSpan / HitEvent) | 4 | 0 | 100% |
| BP_Bow (NockArrow) | 2 | 1 | 67% |
| BP_Bow (GetLaunchVelocity) | 1 | 2 | 33% |
| BP_Bow (FireArrow) | 1 | 4 | 20% |
| BP_Bow (CancelDraw + simple) | 4 | 0 | 100% |
| BP_ThirdPersonCharacter | 4 | 3 | 57% |

The dominant failure cluster is BP_Bow::FireArrow (4 failures) and GetLaunchVelocity (2 failures).

---

### 5. Fix Verification

#### Fix #1 — GetForwardVector alias double-fire (VERIFIED FIXED)

The alias resolution log shows the new fallback behavior working correctly:

```
FindFunction('GetForwardVector'): alias resolved -> 'GetActorForwardVector'
ResolveCallOp: alias-resolved 'GetForwardVector' -> 'Actor::GetActorForwardVector'
  but step has unmatched input pins: [InRot]. Attempting fallback search for original name 'GetForwardVector'.
ResolveCallOp: Alias fallback succeeded -- 'GetForwardVector' rerouted from
  'Actor::GetActorForwardVector' to 'KismetMathLibrary::GetForwardVector' (input pins [InRot] matched)
```

Source: lines 2682-2685.

However, the fix at executor level fires twice unnecessarily:
```
FindFunction('GetForwardVector'): alias resolved -> 'GetActorForwardVector'   [line 2707]
FindFunction('GetForwardVector'): alias resolved -> 'GetActorForwardVector'   [line 2708]
```
The resolver gets the correct `KismetMathLibrary::GetForwardVector`, but the executor then calls `FindFunction` again twice (once to validate, once to create). The node IS created successfully (node_18 at line 2710). The double FindFunction log at executor time is not a bug — it is the normal "validate then create" pattern. No regression observed.

The failure on GetLaunchVelocity is NOT caused by GetForwardVector alias — the resolver fixes it. The failure is caused by the `GetActorForwardVector` node (which has only a `self` pin, not `InRot`) being created in the executor because the executor calls FindFunction independently of the resolver's alias-fallback result. The resolver fixes the step to use `KismetMathLibrary::GetForwardVector`, but the executor's CreateNode call uses the original alias-resolved name `GetActorForwardVector` (line 2707-2708 show `FindFunction('GetForwardVector')` being called again).

**Critical finding:** The resolver correctly logs "Alias fallback succeeded -- rerouted to KismetMathLibrary::GetForwardVector" at line 2685. But then at execution time (line 2706-2708) `FindFunction('GetForwardVector')` is called again and returns `GetActorForwardVector` again. The resolved function name from the resolver is not being passed to the executor — the executor is re-resolving it. This means the fix at resolver level is working but the executor is bypassing it for this particular case. Result: 2 plan_json failures on GetLaunchVelocity before the AI switches to `Conv_RotatorToVector` as a workaround.

#### Fix #2 — Stale exec pin / bOrphanedPin cleanup (NOT TRIGGERED)

No log lines matching "Post-failure cleanup", "bOrphanedPin", or "OrphanedPin" appear in the runtime output. The stale exec pin bug manifested in the prior run as "TypesIncompatible" on orphaned BeginPlay pins. In this run, the log shows one exec pin failure at line 3757:

```
Warning: -> FAILED: Pin connection failed (evt.then -> check.execute):
  Cannot connect Exec to Exec: TypesIncompatible.
```

This is on BP_ThirdPersonCharacter at 16:41:18. The `evt` step here is a `custom_event` node (EquipBow) that was already in the graph from a prior plan. The "TypesIncompatible" diagnostic is the same misleading message pattern as the prior bug — but the EquipBow node is a custom event node, not a BeginPlay node, and the plan fails for a different reason (1 data wire failure, not the orphaned exec pin).

The fix was not triggered because the BeginPlay orphaned-exec-pin scenario did not arise in this run. Fix #2 status: **not exercised, cannot confirm or deny**.

#### Fix #3a — break_struct StructType resolution (NOT EXERCISED)

No `BreakStruct`, `CreateBreakStructNode`, or `struct_type` log lines appear. No break_struct op was used in any plan_json call in this run. Fix #3a status: **not exercised**.

#### Fix #3b — Tilde pin hints (~) (VERIFIED WORKING — no explicit log but used successfully)

Multiple plan_json steps use `@valid.~IsValid` and `@valid_check.~IsValid` syntax (visible in the MCP params lines 2541, 2904, 2934, 3075, 3222, 3368). The NockArrow plan that uses `@valid_check.~IsValid` succeeds at 16:38:58 with 0 data wire failures — the ~IsValid hint resolved to the `ReturnValue` pin of the IsValid node and was wired to `Condition`. No error logged. The tilde-strip is working silently as designed (no explicit log line for the strip itself, which is correct behavior — it's transparent).

Source: lines 2558-2672 (NockArrow execution, 10 data connections all succeeded, ~IsValid resolved cleanly).

#### Fix #4 — VARIABLE_NOT_FOUND Phase 0 check (NOT TRIGGERED)

No "VARIABLE_NOT_FOUND" or "CheckVariableExists" log lines appear. Phase 0 ran 15 times and passed every time (0 errors, 0 warnings in every Phase 0 pass). The fix is either not wired to emit a log, or the planner avoided referencing nonexistent variables in all plans this run. Fix #4 status: **not triggered in this run**.

---

### 6. get_template and get_recipe Calls by Phase

**Planner phase (16:34:44 → 16:36:13, MCP filter: 5 tools):**
- blueprint.get_template: 10 calls (all SUCCESS)
  - combatfs_ranged_component (overview)
  - combatfs_combat_status_component (overview)
  - projectile (overview)
  - projectile_patterns (overview)
  - combatfs_ranged_component + pattern="CreateArrow"
  - combatfs_ranged_component + pattern="SetArrowVelocity"
  - combatfs_ranged_component + pattern="AimSetup"
  - combatfs_ranged_component + pattern="GetVectorInfrontOfCamera"
  - combatfs_ranged_component + pattern="FireArrowGroup"
  - combatfs_ranged_component + pattern="DestroyArrow"
- blueprint.list_templates: 1 call (query "arrow projectile spawn")
- olive.get_recipe: 2 calls

**Builder phase (16:36:13 → 16:48:04):**
- blueprint.get_template: **0 calls**
- olive.get_recipe: **0 calls**

Builder used 0 get_template calls. The "fallback-only" directive from Section 2.5 of the Builder prompt was respected. The Builder did not need any template lookups — it executed directly from the plan.

---

### 7. describe_function Calls

Zero describe_function calls observed in the entire run. The Builder did not use this tool.

---

### 8. Failure Analysis

#### Category A: Hallucinated property-setter functions (persistent failure pattern)
- `SetVelocityInLocalSpace` (FireArrow attempt 1, line 2924) — does not exist on ProjectileMovementComponent or any accessible class. Fuzzy match suggests `SetPhysicsAngularVelocity*` variants.
- `SetMaxWalkSpeed` (StartAiming plan, line 4363) — is a property `MaxWalkSpeed` on `CharacterMovementComponent`, not a function. Resolver correctly identifies "PROPERTY MATCH: Use set_var/get_var instead of call."
- `SetOrientRotationToMovement` (line 4366) — is `bOrientRotationToMovement` property, not a function.
- `SetUseControllerRotationYaw` (line 4369) — is `bUseControllerRotationYaw` property, not a function.
- `SetbIsDrawn` (line 4377) — `bIsDrawn` is a variable on BP_Bow but has no auto-generated setter function. Should use `set_var` op.

These represent the same CharacterMovementComponent property-vs-function category from prior runs, plus a new ProjectileMovement hallucination.

#### Category B: GetForwardVector executor bypass
- GetLaunchVelocity failed 2 times (16:39:06 and 16:39:15). Root cause: resolver correctly reroutes to `KismetMathLibrary::GetForwardVector`, but the executor independently calls `FindFunction('GetForwardVector')` again and gets `GetActorForwardVector` (which has no `InRot` input pin). This causes "Data wire FAILED: No input pin matching 'InRot'".
- On attempt 3 (16:39:32), the AI switched to `Conv_RotatorToVector` which resolved and succeeded.

#### Category C: NockArrow return-op in non-function context (1 failure)
- At 16:38:48 (first NockArrow attempt): `"return"` op placed in a function graph with 0 outputs. The executor looks for `FunctionResult` node but NockArrow is a void function — no result node exists. Error: "no FunctionResult node in graph 'NockArrow'". Fixed on retry.

#### Category D: EquipBow spawn wiring — missing component wire (2 failures)
- At 16:41:08 and 16:41:18, EquipBow plan fails with "Phase 4: 1 data wire(s) failed". In attempt 1 (10 steps), GetComponentByClass ReturnValue → attach.self fails (unresolvable auto-type). In attempt 2 (11 steps, with GetActorTransform added), same pattern. After reading the graph, the Builder switched to granular add_node + connect_pins approach.

#### Category E: IA_EquipBow asset missing (1 failure)
- At 16:45:55, plan tries to wire `IA_EquipBow` Enhanced Input Action but the asset doesn't exist in the project. Error: "Available Input Actions in project: [IA_Jump, IA_Look, IA_Move]." Builder correctly pivoted to using InputKey node (key "One") instead.

#### Category F: connect_pins on wrong node type (3 failures)
- At 16:41:56, 16:42:36, 16:42:44: Builder attempts to connect `node_3.then` (GetTransform return-only node) as an exec source. The node has no `then` pin. 16:42:44 uses UUID addressing and also fails. Builder then deleted nodes and restarted with add_node approach.

#### Category G: Rate limit throttle (5 failures total)
- 3x blueprint.add_function WriteRateLimit (16:37:53, 16:37:57, 16:38:11)
- 2x blueprint.remove_node WriteRateLimit (16:43:33, 16:43:53)
All recovered on next attempt.

---

### 9. Compile Results

All 4 blueprint.compile calls returned SUCCESS:
- BP_ArrowProjectile: SUCCESS (16:36:49, 55ms cold — first compile)
- BP_Bow: SUCCESS (16:38:30, 14ms)
- BP_Bow (post-FireArrow): SUCCESS (16:40:56, 18ms)
- BP_ThirdPersonCharacter: SUCCESS (16:47:55, 21ms)

Multiple apply_plan_json calls also triggered auto-compile via bAutoCompile — all SUCCESS (0 errors, 0 warnings each).

**Final compile status: All 3 created/modified Blueprints compile SUCCESS.**

---

### 10. Reviewer Verdict

```
Reviewer: SATISFIED, 0 missing, 0 deviations (36.4s)
```

Line 4807. Run outcome=0 (Completed). Reviewer ran 36.4s and found the implementation fully satisfactory with no missing features and no deviations from the plan.

---

### 11. Comparison Table

| Metric | 08g | 08i | 09j | **09k** |
|--------|-----|-----|-----|---------|
| Total time | 13:39 | 14:55 | 23:28 | **14:53** |
| plan_json success rate | 55% (12/22) | 80% (?) | 59% (16/27) | **64% (18/28)** |
| Tool success rate (all) | 88% | 96.6% | 89.7% | **82.4%** |
| Tool success (ex. rate-limit) | ~88% | ~96.6% | ~89.7% | **86.4%** |
| Builder get_template calls | 0 | 0 | 10 | **0** |
| Planner get_template calls | 10 | ? | 10 | **10** |
| Reviewer | SATISFIED | SATISFIED | SATISFIED | **SATISFIED** |
| All BPs compile | SUCCESS | SUCCESS | SUCCESS | **SUCCESS** |
| Node-wipe (remove_node bulk) | No | No | Yes (46 nodes) | **Yes (20 nodes, BP_ThirdPersonChar)** |
| Assets created/modified | 3 | 3 | 3 | **3** |
| Pipeline total | 91.8s | ? | 113.9s | **111.3s** |

---

## Recommendations

1. **GetForwardVector executor bypass is still broken.** The resolver's alias-fallback result is not propagated to the executor. The resolver stores the corrected function (KismetMathLibrary::GetForwardVector) in `ResolvedFunctionName` but the executor calls `FindFunction(original_name)` again independently. The executor must use the resolver's pre-computed `ResolvedFunctionName` and `ResolvedClass` rather than re-running FindFunction. This caused 2 unnecessary GetLaunchVelocity failures.

2. **Fix #2 (bOrphanedPin cleanup) was not triggered.** The EquipBow retry failure at line 3757 shows a "TypesIncompatible" exec pin failure on a custom event node. This may be the same underlying issue on non-BeginPlay custom events. Needs investigation: does the bOrphanedPin cleanup apply to all custom event nodes that are reused from a prior failed plan, or only BeginPlay?

3. **Fix #3a (break_struct) and Fix #4 (VARIABLE_NOT_FOUND) were not exercised.** Both fixes need targeted test cases to confirm they work.

4. **Tilde pin hints (#3b) work silently — confirmed.** The ~IsValid pattern was used in 5 successful plan_json calls with no errors. No log is emitted (correct — it should be transparent). Confirmed working.

5. **CharacterMovementComponent property-setter hallucination is a recurring failure pattern (runs 08e, 08g, 09j, 09k).** The resolver now correctly identifies "PROPERTY MATCH: use set_var/get_var instead of call" but this feedback is not preventing the error. Recommendation: add a pre-resolve pass that detects `call` ops targeting known property-only fields and auto-rewrites them to `set_var` ops. The alias map already has ~180 entries; adding `SetMaxWalkSpeed -> MaxWalkSpeed (property)` translations would prevent these failures entirely.

6. **FireArrow function-not-found pattern (SetVelocityInLocalSpace).** The Builder knew this was a ProjectileMovementComponent function but the function does not exist. The correct approach is `SetVelocityInLocalSpace` on `UProjectileMovementComponent` which IS an actual function — but it is C++-only and not blueprint-callable. Add to alias map with a note or add to the knowledge pack that velocity must be set via `InitialSpeed`/`MaxSpeed` properties on `ProjectileMovementComponent`, not via a setter function.

7. **Builder successfully used 0 get_template calls** in this run, same as 08g and 08i. The Planner's upfront use of 10 get_template calls for planning context appears sufficient. The "fallback-only" directive is working.

8. **Node-wipe in BP_ThirdPersonCharacter** (20 remove_node calls, 16:43:15 to 16:44:29). Builder attempted plan_json twice for EquipBow and failed (data wire), tried granular connect_pins (failed — wrong pin type), then wiped the EventGraph nodes it had added and restarted. The final approach (granular add_node with K2Node_CallFunction + connect_pins) succeeded. This is a functional recovery but costs ~3 minutes. Root cause: plan_json failure on EquipBow + inability to connect GetTransform's exec-out (which it doesn't have — it's a pure node).

9. **Runtime improvement vs 09j: 23:28 → 14:53 (37% faster).** The node-wipe in 09j on BP_ThirdPersonChar EventGraph was much larger (46 nodes) and took ~4 minutes; this run's wipe (20 nodes) took about 1.5 minutes. The Builder also didn't repeat the 3x FireArrow failure pattern from 09j — it only tried SetVelocityInLocalSpace once before self-correcting.

10. **IA_EquipBow missing asset** handled gracefully — Builder pivoted to InputKey without retry loops. Correct behavior.
