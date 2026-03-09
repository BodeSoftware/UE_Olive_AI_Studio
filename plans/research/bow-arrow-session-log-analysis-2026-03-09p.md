# Research: Bow and Arrow Session Log Analysis — Run 09p

## Question
Analyze run 09p (log timestamp 2026-03-09, session starting ~20:57) for timeline, tool call performance, diagnostic findings (especially EXEC WIRE REJECTED), failure root causes, and agent behavior. Compare to runs 09m/n/o.

---

## Findings

### 1. Executive Summary

Run 09p is the best-performing run to date. The agent completed a full bow and arrow system (BP_Arrow + BP_Bow + BP_ThirdPersonCharacter integration) in **7 minutes 2 seconds** with a **92.1% overall tool success rate** (70/76). plan_json success rate was **55.6%** (5/9), which is a regression from 09m (60%) but improved over 09n (36%) and 09o (44%). All three blueprints compiled SUCCESS. The critical new diagnostic **EXEC WIRE REJECTED fired exactly once** and revealed the root cause of the exec→exec TypesIncompatible failure: UE's `CanCreateConnection` returned response code 2 ("Replace existing output connections"), meaning the source exec pin *already had an outgoing link* when the second connection was attempted — not an orphan issue, not a type mismatch, but a one-to-many exec output violation. The agent cleanly self-corrected using granular tools (remove_node + add_node + connect_pins sequence) without escalating.

---

### 2. Timeline

| Timestamp | Event |
|-----------|-------|
| 20:56:30 | UE module started, MCP server up on port 3000 |
| 20:57:14 | User message received: "create a bow and arrow system for @BP_ThirdPersonCharacter" |
| 20:57:14 | Auto-snapshot taken, Brain: Idle → WorkerActive |
| 20:57:14–24 | Discovery: 5× `olive.search_community_blueprints` (~10 seconds) |
| 20:57:24 | CLI launched with `--max-turns 500` |
| 20:57:25 | MCP client connected (claude-code 2.1.71), 53/86 tools visible |
| 20:57:39 | 2× `blueprint.list_templates` + 1× `olive.get_recipe` |
| 20:57:45 | 3× `blueprint.get_template` (projectile, projectile_patterns, gun) |
| 20:58:12 | `blueprint.create` BP_Arrow from "projectile" template, preset "Arrow" → SUCCESS |
| 20:58:20 | First `apply_plan_json` on BP_Arrow (BeginPlay+SetLifeSpan, 2 steps) → SUCCESS |
| 20:58:27 | Second `apply_plan_json` on BP_Arrow (OnComponentHit+damage+destroy, 5 steps) → SUCCESS + COMPILE SUCCESS |
| 20:58:40 | `blueprint.create` BP_Bow (Actor parent) → SUCCESS |
| 20:58:43–48 | 2× `add_component` + 5× `add_variable` on BP_Bow → all SUCCESS |
| 20:58:52–53 | 2× `add_function` on BP_Bow (Fire fn + ResetCanFire custom event) → SUCCESS |
| 20:59:08 | `apply_plan_json` on BP_Bow/Fire (15 steps) → **FAILED** (data wire: @bSuccess.auto unresolved) |
| 20:59:29 | Retry `apply_plan_json` on BP_Bow/Fire (14 steps, fixed) → SUCCESS + COMPILE SUCCESS |
| 20:59:34 | `apply_plan_json` on BP_Bow/EventGraph (ResetCanFire→enable, 2 steps) → SUCCESS + COMPILE SUCCESS |
| 20:59:45 | `add_variable` EquippedBow on BP_ThirdPersonCharacter → SUCCESS |
| 20:59:52 | `apply_plan_json` on BP_ThirdPersonCharacter/EventGraph (BeginPlay+spawn+attach, 6 steps) → SUCCESS + COMPILE SUCCESS |
| 21:00:02 | `add_node` InputAction(IA_Shoot) → SUCCESS (but property not set — IA_Shoot missing) |
| 21:00:14 | `apply_plan_json` on BP_ThirdPersonCharacter (5 steps, Fire not found) → **FAILED** (Fire fn not in scope) |
| 21:00:25 | Retry `apply_plan_json` (6 steps, used GetComponentByClass+target_class) → **FAILED** (1 data wire failed) |
| 21:00:41 | Retry `apply_plan_json` (12 steps, full fire logic) → **FAILED** (EXEC WIRE REJECTED — see §4) |
| 21:00:55 | `blueprint.read` EventGraph (27 nodes) — agent diagnosing state |
| 21:01:17 | `connect_pins` node_9.Pressed→node_18.execute → **FAILED** (pin 'Pressed' not found on IsValid node) |
| 21:01:32–35 | 6× `remove_node` (cleanup of failed plan nodes: node_9,15,14,13,12,11) → all SUCCESS |
| 21:01:40 | `blueprint.read` EventGraph (21 nodes) — verify cleanup |
| 21:02:06–08 | 4× `remove_node` (node_9,12,11,10 — second cleanup pass) → SUCCESS |
| 21:02:14 | `add_node` InputKey(LeftMouseButton) → SUCCESS (fallback from missing IA_Shoot) |
| 21:02:22–24 | 3× `add_node` (VariableGet EquippedBow, CallFunction IsValid, Branch) → SUCCESS |
| 21:02:29–36 | 3× `get_node_pins` (read node pin layouts for wiring) |
| 21:02:42–45 | 4× `connect_pins` (InputKey.Pressed→Branch, EquippedBow→IsValid, IsValid→Branch, Branch.then→Fire) → SUCCESS |
| 21:02:51 | `connect_pins` node_5.ReturnValue→node_8.self → **FAILED** (type mismatch, SpawnActor BP_Bow ReturnValue ≠ SetVariable target) |
| 21:03:02–08 | 5× `get_node_pins` (check actual pin types on nodes 5,8,2,3,4) |
| 21:03:13 | `connect_pins` node_2.ReturnValue→node_5.self → SUCCESS (SpawnBow→K2_AttachToComponent) |
| 21:03:18 | `remove_node` node_8 (orphaned SetVariable) + `blueprint.compile` → COMPILE SUCCESS |
| 21:03:24 | `add_node` CallFunction K2_AttachToComponent → SUCCESS |
| 21:03:29–30 | 2× `get_node_pins` (node_3, node_4) |
| 21:03:35–40 | 5× `connect_pins` + 4× `set_pin_default` (attach wiring + socket/rules config) → all SUCCESS |
| 21:03:45 | `blueprint.compile` → COMPILE SUCCESS |
| 21:03:49 | `get_node_pins` node_5 |
| 21:03:52 | `remove_node` node_5 (orphaned SpawnActor) |
| 21:03:55 | `blueprint.compile` → COMPILE SUCCESS |
| 21:04:16 | Autonomous run complete (exit code 0). Last tool call 21.6s ago, 50 tool calls logged |
| 21:04:16 | Brain: Run completed [E53BEFA64B83B13F843840B39230190D], outcome=0 |

**Total run time: 7 minutes 2 seconds** (20:57:14 → 21:04:16)

**Phase breakdown:**
- Discovery (community search): ~10s (20:57:14–24)
- CLI cold start + MCP connect: ~11s (20:57:14–25)
- Research (list_templates + get_recipe + get_template): ~20s (20:57:39–45)
- Building: ~6:00 (20:58:12–21:04:16)

---

### 3. Tool Call Table

Total: **76 MCP tool calls** (per log). CLI provider logged 50 internally — discrepancy suggests the pre-CLI discovery calls (5× search_community_blueprints) plus initial blueprint.read account for the difference.

| Tool | Count | Successes | Failures | Notes |
|------|-------|-----------|----------|-------|
| blueprint.apply_plan_json | 9 | 5 | 4 | see §4 |
| blueprint.remove_node | 12 | 12 | 0 | surgical cleanup |
| blueprint.get_node_pins | 11 | 11 | 0 | diagnostic reads |
| blueprint.connect_pins | 11 | 9 | 2 | 1 pin not found, 1 type mismatch |
| blueprint.add_variable | 6 | 6 | 0 | |
| blueprint.add_node | 6 | 6 | 0 | |
| blueprint.set_pin_default | 4 | 4 | 0 | |
| blueprint.get_template | 3 | 3 | 0 | |
| blueprint.compile | 3 | 3 | 0 | all SUCCESS |
| blueprint.read | 2 | 2 | 0 | |
| blueprint.create | 2 | 2 | 0 | |
| blueprint.add_function | 2 | 2 | 0 | |
| blueprint.add_component | 2 | 2 | 0 | |
| blueprint.list_templates | 2 | 2 | 0 | |
| olive.get_recipe | 1 | 1 | 0 | |
| olive.search_community_blueprints | 5 | 5 | 0 | pre-CLI discovery |

**Overall tool success rate: 70/76 = 92.1%**
**plan_json success rate: 5/9 = 55.6%**

---

### 4. Diagnostic Findings

#### EXEC WIRE REJECTED — CONFIRMED FIRING (1×)

**The diagnostic is working and produced a complete, accurate diagnosis:**

```
[2026.03.09-21.00.41:840][926]LogOlivePinConnector: Warning: EXEC WIRE REJECTED:
  K2Node_CustomEvent_0.then -> K2Node_IfThenElse_0.execute
  | Response: 2 'Replace existing output connections'
  | Src(orphan=0 hidden=0 links=1 dir=1) Tgt(orphan=0 hidden=0 links=0 dir=0)
```

**Root cause revealed:** Response code 2 = `CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE` is NOT the issue here. Response code 2 in exec context means "Replace existing output connections" — UE's schema rejected the wire because `K2Node_CustomEvent_0.then` **already had 1 outgoing link** (`links=1`). The target Branch node's `execute` pin was empty (`links=0`). This is NOT a bOrphanedPin issue (orphan=0 on both sides). This is a second connection attempt on an exec out pin that was already wired — from a **previous, unrolled plan attempt that partially committed**.

**Why it happened:** The agent's third apply_plan_json attempt at 21:00:41 tried to wire `OnFirePressed (custom_event)` → `branch_valid`. But from the second failed attempt at 21:00:25, the custom event node (node_7, created at 21:00:25) had already been wired to the `fire` node's execute. The rollback in attempt 2 removed the new nodes but left the custom event reuse node (step 1 hit `Reused existing custom event node 'OnFirePressed'`) still wired. On attempt 3, the plan again reused the same `OnFirePressed` event node, which still had its `then` pin linked to the `fire` node from the previous plan's committed partial work.

**The error message shown to the agent was misleading:** The agent received "Cannot connect Exec to Exec: TypesIncompatible" — which is the standard BuildWiringDiagnostic fallback. The EXEC WIRE REJECTED log shows the real reason was `links=1` on the source, not type incompatibility. The error message causes the agent to chase a false lead (type errors, wrong pins).

**Confirmed: bOrphanedPin is NOT the source of this failure.** `orphan=0` on both source and target. This is a different issue: **exec source already linked**.

#### PreResolvedFunction — CONFIRMED WORKING

23 occurrences. The resolver→executor contract is working correctly. Every resolved function in the plan is passed directly to the executor, which logs "Used pre-resolved function 'Class::FuncName'" and skips the re-search. Examples from this run: `Actor::SetLifeSpan`, `GameplayStatics::ApplyDamage`, `Actor::K2_DestroyActor`, `KismetMathLibrary::Greater_IntInt`, `KismetSystemLibrary::K2_SetTimer`, `KismetMathLibrary::GetForwardVector` (alias fallback working), `BP_Bow_C::Fire`.

#### GetForwardVector Alias Fallback — CONFIRMED WORKING

```
ResolveCallOp: alias-resolved 'GetForwardVector' -> 'Actor::GetActorForwardVector' but step has unmatched input pins: [InRot].
Attempting fallback search for original name 'GetForwardVector'.
ResolveCallOp: Alias fallback succeeded -- 'GetForwardVector' rerouted from 'Actor::GetActorForwardVector'
to 'KismetMathLibrary::GetForwardVector' (input pins [InRot] matched)
```

This fix (added in 09m) is confirmed working. The resolver correctly detects that `InRot` is not a pin on `GetActorForwardVector` and falls back to `KismetMathLibrary::GetForwardVector`.

#### RewriteAccessorCalls — NOT PRESENT

Zero occurrences. Function was correctly deleted. No regression.

#### EnsurePinNotOrphaned — NOT PRESENT

Zero occurrences. The bOrphanedPin cleanup code did not fire at all in this run. This confirms the exec rejection in this run is NOT bOrphanedPin-driven (unlike runs 09n/09o).

#### VARIABLE_NOT_FOUND — NOT TRIGGERED

The new Phase 0 check was not exercised in this run.

#### UPROPERTY/Auto-Rewrite — NOT TRIGGERED

No auto-rewrite from call → get_var/set_var observed.

#### TypesIncompatible — 1 occurrence

Only on the EXEC WIRE REJECTED failure (line 2766). The underlying cause is the exec source already having a link, not a true type mismatch. The downstream error message is misleading.

#### CollapseExecThroughPureSteps — Working

Fired multiple times correctly, collapsing pure steps out of exec chains before execution.

#### make_struct → MakeTransform auto-reroute — Working

`Step 'make_tf': Auto-rerouting make_struct 'Transform' -> call 'MakeTransform'` — the Transform struct reroute is working.

#### Phase 5.5 Unwired FunctionResult pin — New persistent issue

In two apply_plan_json calls on BP_Bow/Fire, Phase 5.5 reported:
```
Phase 5.5: Unwired FunctionResult data pin 'bSuccess' (bool) in function 'Fire'
```
On the first attempt, this combined with a data wire failure to cause rollback. On the second (successful) attempt, the same warning appeared but did not cause rollback — the result was committed with an unwired output pin, and the Blueprint compiled successfully (UE treats unwired function output pins as returning default value). This is acceptable behavior.

---

### 5. Failure Root Causes

#### Failure 1: apply_plan_json on BP_Bow/Fire, attempt 1 (20:59:08) — data wire unresolved

**Error:** `Data wire FAILED: Source step 'bSuccess' not found (referenced by @bSuccess.auto)`

**Root cause:** The plan used `"op": "set_var"` with `"target": "bSuccess"` to set the function output, and in the `ret` step referenced `@bSuccess.auto`. But `set_var` on a function output resolves to a virtual FunctionOutput step — and virtual steps have no data output pin. The `@bSuccess.auto` reference should have pointed to the FunctionResult node directly, not to the output of the set_var step. The agent fixed this on retry by removing the chained `@bSuccess.auto` reference and pointing `ret` directly to the FunctionResult virtual step.

**Exec wire failures (2):** Also in this attempt: `FAILED: No exec output pin matching '' on source step 'init_fail'` and `'set_success'` — both are FunctionOutput virtual steps, which have no exec output. The plan incorrectly tried to chain exec flow through virtual FunctionOutput steps.

**Agent behavior:** Self-corrected on retry at 20:59:29 with a cleaner 14-step plan that avoided both the data reference issue and the FunctionOutput exec-chain issue.

#### Failure 2: apply_plan_json on BP_ThirdPersonCharacter (21:00:14) — Fire function not found

**Error:** `ResolveCallOp FAILED: function 'Fire' could not be resolved (target_class='')`

**Root cause:** The plan called `Fire` without specifying `target_class`. At resolve time, `BP_Bow_C` is a Blueprint class that has been compiled in this session, but `Fire` lives on it. The resolver searched `BP_ThirdPersonCharacter_C`'s hierarchy and libraries but not `BP_Bow_C`. Without `target_class` specified, cross-Blueprint function resolution fails.

**Agent behavior:** Self-corrected on next attempt by adding `target_class: "BP_Bow_C"`.

#### Failure 3: apply_plan_json on BP_ThirdPersonCharacter (21:00:25) — data wire failed

**Error:** Phase 4 — 1 data wire failed (no detail logged beyond count). Plan had `@get_bow.~ArrowSpawnPoint` tilde reference. The GetComponentByClass node returned `ActorComponent*` but `K2_GetComponentToWorld` expects `SceneComponent*`.

**Root cause:** The `~` (tilde) accessor works in plan_json for resolving component references through the resolver, but here the tilde was used on the return value of `GetComponentByClass`, not on the Blueprint variable. `GetComponentByClass` returns `ActorComponent*` — `K2_GetComponentToWorld.Target` expects `SceneComponent*`. The autocast path failed silently (no type conversion node inserted).

**Agent behavior:** Attempted full plan rewrite on next try.

#### Failure 4: apply_plan_json on BP_ThirdPersonCharacter (21:00:41) — EXEC WIRE REJECTED

**Full error line:**
```
LogOlivePinConnector: Warning: EXEC WIRE REJECTED: K2Node_CustomEvent_0.then -> K2Node_IfThenElse_0.execute
| Response: 2 'Replace existing output connections'
| Src(orphan=0 hidden=0 links=1 dir=1) Tgt(orphan=0 hidden=0 links=0 dir=0)
```

**Root cause:** The `OnFirePressed` custom event node was reused (correct — it already existed from attempt 2's partial commit). However, the node's `then` exec output pin already had a link to the `Fire` call node from a previous plan that partially committed before rollback. When attempt 3 tried to wire `OnFirePressed.then → branch_valid.execute`, UE rejected it because `then` already had `links=1`. The rollback did not sever the wiring on the reused custom event node — the rolled-back plan created new nodes, then rolled those back, but the custom event node (which was *reused*, not created) kept its wiring from the commit that wasn't rolled back.

**This is the key finding: partial commits on reused nodes survive rollback.** When a plan reuses an existing node (event or custom event), and that plan later rolls back, any new wiring the plan added to that pre-existing node's pins persists after rollback because the node itself is not in the rollback scope. The `FScopedTransaction` rolls back only objects that were `Modify()`d during *this* transaction — but the original event node may have been modified in a *previous* transaction (the partial success at 21:00:25), which is already committed.

**Agent behavior:** After this failure, the agent read the EventGraph (27 nodes), cleaned up 6+ stranded nodes via `remove_node`, then rebuilt the fire logic manually using granular tools (add_node + connect_pins). This produced a working graph that compiled successfully.

#### Failure 5: connect_pins node_9.Pressed→node_18.execute (21:01:17)

**Error:** `Source pin 'Pressed' not found on node 'node_9'. Available output pins: Return Value (Bool)`

**Root cause:** node_9 was an `IsValid` CallFunction node (not an input event node). The agent confused node IDs after reading the graph. `IsValid` has no `Pressed` pin — that's an InputKey/InputAction pin. The agent was trying to wire an IsValid return value to an exec, which is wrong both topologically and pin-name-wise.

**Agent behavior:** Recovered. Removed node_9 (the IsValid node) and switched to a granular `add_node(InputKey)` + `add_node(Branch)` approach.

#### Failure 6: connect_pins node_5.ReturnValue→node_8.self (21:02:51)

**Error:** Execution failed (no error text logged beyond pipeline error). node_5 was the `SpawnActor<BP_Bow>` ReturnValue (type `BP_Bow_C*`) and node_8 was a `SetVariable` node for `EquippedBow`. The agent confused the node ordering after the graph read — `SetVariable` does not have a `self` pin.

**Agent behavior:** Used `get_node_pins` on 5 nodes to diagnose, then correctly wired `SpawnBow.ReturnValue → K2_AttachToComponent.self`.

---

### 6. Agent Behavior Observations

**Discovery pattern:** 5× `search_community_blueprints` in 10 seconds (pre-CLI), then immediately after CLI connect: 2× `list_templates` + 1× `get_recipe` + 3× `get_template`. Total research time before first write: ~58 seconds. This matches the "fallback-only" directive — the agent researched templates before building, used them as reference, then built from scratch rather than cloning.

**Build strategy:** Clean and efficient. Created BP_Arrow first (from projectile template + preset), then BP_Bow (from scratch with granular setup), then integrated into BP_ThirdPersonCharacter. No node-wipe, no function gutting.

**Cast usage:** Zero explicit `cast` ops in any plan_json. The agent correctly identified target classes and used `target_class` in resolver hints instead of casting.

**Granular tool fallback:** After the third plan_json failure on BP_ThirdPersonCharacter's fire logic, the agent switched completely to granular tools (add_node + connect_pins + set_pin_default) and succeeded. This is exactly the intended fallback behavior.

**get_node_pins usage:** 11 calls, heavily used after the EXEC WIRE REJECTED failure to re-audit node pin layouts before rewiring. The agent was clearly diagnosing from first principles rather than guessing.

**InputAction missing:** `IA_Shoot` does not exist in the project, so `InputAction` node had no `input_action_name` set (reflection property not found). Agent fell back to `InputKey(LeftMouseButton)` — pragmatic choice.

**Template reads:** 3× `get_template` (projectile, projectile_patterns, gun) were all pre-research reads, not fallback reads during building. Template "fallback-only" guidance was followed.

**blueprint.describe_function:** Not used. The agent relied on resolver results and `get_node_pins` for function discovery.

**No unnecessary casts:** Previous runs showed "dumb cast" (GetMesh → GetComponentByClass downgrade). In this run, `GetMesh` failed FindFunction (line 2473) but the resolver gracefully resolved the BP_Bow component mesh access via the SCS variable path (`Mesh` SCS variable) rather than calling GetMesh. Clean resolution.

---

### 7. Compile Results

| Blueprint | Compile Result |
|-----------|---------------|
| BP_Arrow | SUCCESS (multiple times) |
| BP_Bow | SUCCESS (3 times) |
| BP_ThirdPersonCharacter | SUCCESS (3 times: 21:03:18, 21:03:45, 21:03:55) |

All BPs compiled with 0 errors, 0 warnings. Final state is fully working.

---

### 8. Comparison to Previous Runs

| Run | Total Time | plan_json success | Tool Success | Key Issue |
|-----|-----------|-------------------|-------------|-----------|
| 09m | 10:03 | 60% (9/15) | 86.4% | GetForwardVector alias executor bypass (bug) |
| 09n | 8:14 | 36% (4/11) | ~85% | bOrphanedPin on exec (fired 1x), @get_class.auto mismatch |
| 09o | 12:02 | 44% (4/9) | 76.1% | bOrphanedPin NOT firing (bug), PreResolvedFunction confirmed |
| **09p** | **7:02** | **55.6% (5/9)** | **92.1%** | **EXEC WIRE REJECTED confirmed: exec source already linked (links=1), NOT orphan** |

This run sets a new speed record. The EXEC WIRE REJECTED diagnostic is the most significant new finding — it reveals the root cause is neither bOrphanedPin nor type incompatibility, but a plan's attempt to add a second exec link to a reused node's already-wired exec output.

---

## Recommendations

1. **Fix the EXEC WIRE REJECTED error message (P0).** The error sent to the agent is "Cannot connect Exec to Exec: TypesIncompatible" which is wrong. The real reason is `links=1` on the source (already wired). `BuildWiringDiagnostic` should check `SourcePin->LinkedTo.Num() > 0` and `SourcePin->PinType.PinCategory == PC_Exec` and return a message like: "Exec pin 'then' on node X is already wired to another node — exec outputs can only connect to one target. Disconnect the existing link first or reorder execution." This one change would prevent the agent from chasing false type-mismatch leads.

2. **Fix partial-commit wiring on reused nodes surviving rollback (P0).** When a plan reuses an existing event/custom event node AND subsequently rolls back, the new wiring added to that pre-existing node's pins must also be rolled back. Current behavior: `FScopedTransaction` only rolls back objects modified in the current transaction. The plan executor should ensure that when reusing a node (`Reused existing event node`), it calls `Modify()` on that node at the start of the transaction so that any pin wiring changes are captured in the rollback scope. Without this, the reused node's wiring leaks across rollback boundaries.

3. **Improve EXEC WIRE REJECTED diagnostic alternatives (P1).** The current alternatives listed are: [high] "check pin selection" (wrong advice for this error), [low] "use editor.run_python". A better alternative for this specific failure: [high] "Read the existing node's exec wiring with `blueprint.get_node_pins` — if the source exec pin already has a link, use `blueprint.remove_connection` or reorder exec_after/exec_outputs in the plan."

4. **Add `blueprint.remove_connection` tool (P1).** The agent correctly handled the exec-already-linked situation by reading the graph + removing offending nodes. But removing nodes is destructive. A `remove_connection(source_pin, target_pin)` tool would allow surgical unlinking without node deletion. This would cut the 10+ remove_node + re-add_node sequence to 1 operation.

5. **CollapseExecThroughPureSteps auto-fix for FunctionOutput exec chains (P1).** The plan executor's Phase 3 should detect when exec wiring tries to originate from a virtual FunctionOutput step (init_fail → check_fire) and skip it with a clear warning rather than logging FAILED. The resolver already marks FunctionOutput steps as virtual — the executor should not attempt to wire exec from them at all.

6. **Fire function target_class resolution (P2).** When `call` op references a function that exists on another Blueprint class (e.g., `Fire` on `BP_Bow_C`) and no `target_class` is specified, the resolver currently fails with a search-trail error. Consider: if the input wiring contains a reference to a variable of known type (e.g., `@get_bow.auto` is typed `BP_Bow_C*`), the resolver could infer `target_class = BP_Bow_C` automatically. This would eliminate the "Fire not found" failure class without requiring the agent to specify target_class.

7. **GetMesh missing from FindFunction (P2).** `GetMesh` failed FindFunction because it is not a UFUNCTION on Character/Pawn — it is a UPROPERTY accessor. The catalog should either: (a) alias `GetMesh` to a get_var op on the `Mesh` SCS variable, or (b) document this in the error message ("GetMesh is a SCS component variable — use get_var with target='Mesh' instead of call"). The agent got around it this run by using SCS variable access directly, but only because the resolver happened to handle it via `Mesh` variable name.

8. **plan_json rate 55.6% remains below 09m (60%) (P2).** The 4 failures in this run were: data-wire-unresolved (bSuccess FunctionOutput ref), function-not-found (Fire no target_class), tilde-component-type-mismatch, and exec-already-linked. Three of these four are addressable by fixes 2, 5, and 6 above. Only the tilde-component failure (#3) is a genuinely hard problem (requires knowing that GetComponentByClass returns ActorComponent* not SceneComponent*). A note in the error message suggesting to use `target_class` on GetComponentByClass or use the direct SCS variable instead would help.

9. **Memory note: EXEC WIRE REJECTED root cause is "exec source already linked from prior plan partial commit", not bOrphanedPin.** The bOrphanedPin path (from prior runs 09n, 09o) was NOT involved in this run. The two failure modes are distinct and require separate fixes.

10. **run_time record (7:02) is primarily from efficient granular-tool recovery.** The agent's 6-minute build phase included ~2 minutes of cleanup and rewiring after the plan_json failures. If fixes #1 and #2 land, the third plan_json attempt on BP_ThirdPersonCharacter would likely succeed on the first or second try, saving ~3-4 minutes. Sub-5-minute runs appear achievable.
