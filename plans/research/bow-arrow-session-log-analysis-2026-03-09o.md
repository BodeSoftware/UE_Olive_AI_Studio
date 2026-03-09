# Research: Bow-Arrow Session Log Analysis — Run 09o

## Question

Full analysis of the 2026-03-09 run at 20:04–20:17, testing:
1. Point-of-use orphaned pin fix (`EnsurePinNotOrphaned`)
2. PreResolvedFunction contract (confirmed working last run)
3. Single-agent flow (no pipeline)
4. User-flagged concern: "dumb casting — it casted scene component"

---

## Findings

### Task

"create a bow and arrow system for @BP_ThirdPersonCharacter"

---

### Timeline

| Milestone | Wall-clock |
|-----------|-----------|
| UE editor started | 20:04:43 |
| Module fully initialized | 20:04:47 |
| Claude Code process launched (autonomous) | 20:05:13 |
| First MCP tool call (discovery — community blueprint search x5) | 20:05:22 |
| Discovery pass complete | 20:05:23 (9.82s, LLM=yes) |
| CLI process launched | 20:05:23 |
| First write tool call (blueprint.create BP_Bow) | 20:06:06 |
| Final tool call (blueprint.compile) | 20:16:58 |
| Run complete (exit code 0) | 20:17:15 |
| **Total wall-clock** | **~12:02** |

Discovery: 9.82s (community blueprint queries: "bow weapon", "ranged component", "arrow projectile", "aiming trace", "ammo system" — 8 results from 5 queries).

Time from discovery complete to first write: 43s (agent spent this reading templates — 3 × get_template between 20:05:41 and 20:05:43).

---

### Tool Call Statistics

Total MCP tool calls: **71** (all result lines match)

By tool:

| Tool | Calls |
|------|-------|
| blueprint.remove_node | 9 |
| blueprint.apply_plan_json | 9 |
| blueprint.read | 8 |
| blueprint.add_variable | 8 |
| blueprint.connect_pins | 7 |
| blueprint.add_component | 7 |
| blueprint.compile | 6 |
| blueprint.add_node | 5 |
| blueprint.get_template | 3 |
| blueprint.modify_component | 2 |
| blueprint.create | 2 |
| blueprint.add_function | 2 |
| blueprint.set_pin_default | 1 |
| blueprint.disconnect_pins | 1 |
| blueprint.describe_function | 1 |
| olive.search_community_blueprints | 5 (discovery pass, before CLI) |

**Failures:** 17 total

| Tool | Failures |
|------|----------|
| blueprint.apply_plan_json | 5 |
| blueprint.connect_pins | 2 |
| blueprint.add_node | 1 (FireArrow function not found — ghost node race) |
| blueprint.add_variable | 1 (WriteRateLimit hit; retry succeeded) |
| blueprint.disconnect_pins | 1 |

**Tool success rate:** 54 successes / 71 calls = **76.1%** (includes rate-limit retry as 1 fail + 1 success; if counted as 1 eventual-success = 78.9%)

---

### Fix Verification

#### 1. EnsurePinNotOrphaned / ReconstructNode

**NOT FIRING.** Zero occurrences of `EnsurePinNotOrphaned`, `ReconstructNode`, or `bOrphanedPin` in the log. The point-of-use orphaned pin fix either was not deployed, was not triggered by any code path exercised this run, or logs under a different string.

#### 2. TypesIncompatible on exec pin (orphaned pin bug)

**STILL OCCURRING — 1 INSTANCE** (line 2340):

```
Warning: -> FAILED: Pin connection failed (begin.then -> spawn_bow.execute):
Cannot connect Exec to Exec: TypesIncompatible.
```

This is the same false-positive diagnostic as in runs 09m and 09n. The real cause is `bOrphanedPin == true` on the BeginPlay event's `then` pin after a prior failed plan applied nodes to the EventGraph and rolled back.

Sequence that caused it:
- Attempt 2 (line 2188): apply_plan_json on ThirdPersonCharacter EventGraph — Phase 4, 1 data wire failed → rolled back (nodes persisted into EventGraph at that point)
- Attempt 3 (line 2277): apply_plan_json with cast step added — exec wire `begin.then -> spawn_bow.execute` fails with TypesIncompatible
- Root cause: the BeginPlay event node reuse detection (`Reused existing event node 'ReceiveBeginPlay'`) at line 2306 picks up the existing node, whose `then` pin is still orphaned from the prior rollback

The agent recovered by removing nodes (9 × remove_node) and rebuilding via a separate InitializeBow function graph, which sidestepped the orphaned EventGraph node entirely.

#### 3. PreResolvedFunction Contract

**CONFIRMED WORKING.** 24 occurrences of `pre-resolved function` across all plan_json calls. Representative examples:

```
CreateCallFunctionNode: Used pre-resolved function 'ActorComponent::SetActive'
CreateCallFunctionNode: Used pre-resolved function 'PrimitiveComponent::SetCollisionEnabled'
CreateCallFunctionNode: Used pre-resolved function 'Actor::SetLifeSpan'
CreateCallFunctionNode: Used pre-resolved function 'Actor::K2_GetRootComponent'
CreateCallFunctionNode: Used pre-resolved function 'Actor::GetComponentByClass'
CreateCallFunctionNode: Used pre-resolved function 'SceneComponent::K2_AttachToComponent'
CreateCallFunctionNode: Used pre-resolved function 'KismetMathLibrary::Greater_IntInt'
CreateCallFunctionNode: Used pre-resolved function 'GameplayStatics::GetPlayerCameraManager'
CreateCallFunctionNode: Used pre-resolved function 'Actor::K2_GetActorLocation'
CreateCallFunctionNode: Used pre-resolved function 'Actor::GetActorForwardVector'
CreateCallFunctionNode: Used pre-resolved function 'KismetMathLibrary::Multiply_VectorFloat'
CreateCallFunctionNode: Used pre-resolved function 'KismetMathLibrary::Add_VectorVector'
CreateCallFunctionNode: Used pre-resolved function 'Actor::K2_GetActorRotation'
CreateCallFunctionNode: Used pre-resolved function 'KismetMathLibrary::Subtract_IntInt'
CreateCallFunctionNode: Used pre-resolved function 'BP_ThirdPersonCharacter_C::InitializeBow'
CreateCallFunctionNode: Used pre-resolved function 'BP_ThirdPersonCharacter_C::FireArrow'
```

No executor re-running FindFunction independently was observed this run (no split between resolver and executor paths).

#### 4. bOrphanedPin

Zero occurrences of the string `bOrphanedPin` in the entire log. The field is never logged by name — the symptom manifests only as the TypesIncompatible diagnostic.

---

### Plan JSON Analysis

9 apply_plan_json calls. 4 successes, 5 failures. **Success rate: 44.4%**

| # | Asset | Graph | Result | Root Cause |
|---|-------|-------|--------|------------|
| 1 | BP_Arrow | EventGraph | SUCCESS | HandleArrowHit event + hit logic. 9 steps, clean. |
| 2 | BP_ThirdPersonCharacter | EventGraph | FAILED (resolve) | Function `K2_AttachComponentTo` not found. Closest match `K2_AttachToComponent` suggested in error. |
| 3 | BP_ThirdPersonCharacter | EventGraph | FAILED (exec wire) | Phase 4: 1 data wire failed (SpawnTransform → could not connect). Rolled back. |
| 4 | BP_ThirdPersonCharacter | EventGraph | FAILED (exec wire) | TypesIncompatible on `begin.then -> spawn_bow.execute`. **This is the orphaned pin bug.** Had cast step (`cast_mesh`). Phase 5.5 also logged orphaned SpawnActor node. |
| 5 | BP_ThirdPersonCharacter | InitializeBow | FAILED (compile) | Compile FAILED: SpawnTransform "by ref" error — SpawnActor node had `0,0,0|0,0,0|1,1,1` literal instead of wired Transform. Nodes rolled back. |
| 6 | BP_ThirdPersonCharacter | InitializeBow | SUCCESS | Same 5-step plan (get_bow → get_root → get_mesh → cast_mesh → attach) wired after agent added MakeTransform node manually. |
| 7 | BP_ThirdPersonCharacter | EventGraph | SUCCESS | 1-step: `call_init` (call InitializeBow). Clean. |
| 8 | BP_ThirdPersonCharacter | FireArrow | SUCCESS | 15-step plan. Full fire-arrow function: ammo check, camera-based spawn location, SpawnActor, decrement ammo, PrintString, DoOnce. |
| 9 | BP_ThirdPersonCharacter | EventGraph | FAILED (node create) | IA_Fire input action not found. `[IA_Jump, IA_Look, IA_Move]` listed as available. Agent fell back to InputKey node manually. |

**Failure root causes summary:**
- Orphaned exec pin bug (bOrphanedPin): 1× (attempt 4 above)
- Wrong function name (K2_AttachComponentTo vs K2_AttachToComponent): 1× (attempt 2, self-corrected)
- Data wire failure + rollback causing subsequent orphan: 1× (attempt 3 → poisoned EventGraph for attempt 4)
- SpawnTransform by-ref compile error (literal string instead of wired Transform): 1× (attempt 5)
- Missing IA_Fire input action asset: 1× (expected, asset doesn't exist)

---

### Cast Analysis (User Concern: "Dumb Casting — Casted Scene Component")

**All cast operations found:**

There is exactly one cast step used repeatedly across multiple plan attempts:

```
step_id='cast_mesh', op='cast', target='SceneComponent'
```

**What was being cast:** The return value of `GetComponentByClass(ComponentClass=SkeletalMeshComponent)` — which returns `UObject*` (typed as `ActorComponent*` in Blueprint).

**Cast target:** `SceneComponent`

**Is this necessary?** NO. This cast is doubly wrong:

1. **Wrong target type.** The agent wanted the SkeletalMeshComponent (a USkinnedMeshComponent → UMeshComponent → UPrimitiveComponent → USceneComponent chain). Casting to `SceneComponent` does succeed at runtime (SkeletalMesh IS a SceneComponent), but the `K2_AttachToComponent` call requires the Parent pin to accept a `SceneComponent*` — which `SceneComponent` cast output would give. So the cast is technically functional, but...

2. **Unnecessary.** `GetComponentByClass` returns `ActorComponent*`. `K2_AttachToComponent`'s `Parent` pin accepts `SceneComponent*`. An autocast or direct typed variable would suffice. The agent should have used `GetMesh()` directly (which returns `USkeletalMeshComponent*`, a concrete typed accessor), or used `GetComponentByClass` with a more specific class and directly connected without cast. The resolver even rewrote `GetMesh` to `GetComponentByClass(ComponentClass=SkeletalMeshComponent)` (line 2170), which introduces the return-type ambiguity that prompted the agent to add the cast.

3. **User's specific complaint confirmed.** The agent was casting `SkeletalMeshComponent → SceneComponent` — casting DOWN the type hierarchy to a less specific type. This is architecturally backwards: you cast upward when you need a more specific interface, not downward to a less specific parent class. The agent did this because `K2_AttachToComponent`'s `Parent` pin expects `SceneComponent*`, and the agent incorrectly thought it needed to cast to match.

**Why did the agent use a cast?** The data wire in attempt 3 (no cast) failed:
```
Phase 4 complete: 3 data connections succeeded, 1 failed
```
The `GetComponentByClass.ReturnValue -> K2_AttachToComponent.self` connection failed because `ActorComponent*` is not compatible with `SceneComponent*`. Rather than realizing `GetComponentByClass` was the wrong tool (should have used `GetMesh()` or a typed version), the agent added a cast to `SceneComponent` to bridge the type gap.

**Root cause:** The `RewriteAccessorCalls` rewrite of `GetMesh` → `GetComponentByClass` returned `ActorComponent*` instead of the more specific `SkeletalMeshComponent*`. If the resolver had kept `GetMesh` or used a typed version, the return type would have been `SkeletalMeshComponent*`, which IS a `SceneComponent*` and would have connected directly without a cast node.

**The cast worked** — compile succeeded on the final attempt — but it's architecturally wrong pattern. The correct fix is: stop rewriting `GetMesh` to `GetComponentByClass`.

---

### Template and Recipe Usage

- **3 × get_template** before first write (20:05:41–20:05:43, 18s before first blueprint.create):
  - `combatfs_arrow_component` / pattern `SetupArrowVariables`
  - `combatfs_arrow_component` / pattern `ArrowStuckInWall`
  - `combatfs_ranged_component` / pattern `CalculateRangedDamage`
- **1 × describe_function** at 20:13:47: `K2_AttachRootComponentTo` on Actor (agent exploring attachment API while stuck on the attach problem)
- **0 × get_recipe**

Template research phase: agent researched relevant arrow component patterns before building — this is the correct "fallback-only" template behavior in action.

---

### Compile Results

| Blueprint | Final Status |
|-----------|-------------|
| BP_Bow | SUCCESS (0 errors, 0 warnings) |
| BP_Arrow | SUCCESS (0 errors, 1 warning — ComponentBoundEvent node added manually has uninitialized props) |
| BP_ThirdPersonCharacter | SUCCESS (final compile 20:16:58, 0 errors, 0 warnings) |

One intermediate FAILED compile (20:14:34): SpawnTransform `by ref` error. Agent caught it, removed nodes, and rebuilt properly.

---

### Self-Correction Quality

**Excellent recovery on the attachment problem.** When K2_AttachComponentTo failed resolution, the agent:
1. Immediately tried `K2_AttachToComponent` (correct) on next attempt
2. Added GetRootComponent step for the bow's root component reference
3. When data wire failed (ActorComponent* → SceneComponent*), added cast node
4. When EventGraph's BeginPlay became orphaned, recognized the problem and moved logic to a separate `InitializeBow` function graph
5. Realized the SpawnTransform literal string was wrong, added a MakeTransform node manually

**Poor recovery on FireArrow function call.** When `blueprint.add_node` with `type="CallFunction"` and `function_name="FireArrow"` failed (line 3098-3100 — FireArrow not found because it's a user function not a library function), the agent tried again with `type="K2Node_CallFunction"` (line 3101), which succeeded via the ghost-node detection path. This is a known limitation: the type alias `CallFunction` doesn't search user-defined functions. The agent worked around it by using the raw K2Node type.

**IA_Fire missing.** Agent correctly fell back to `InputKey(LeftMouseButton)` when IA_Fire was not found. This is clean fallback behavior.

---

### Node-Wipe Activity

**9 × remove_node** on ThirdPersonCharacter EventGraph between 20:11:01 and 20:12:16. These were targeted node removals (nodes 4–9, from failed plan attempt 3 that had partially committed). This is not a wipe — the agent was doing surgical cleanup of the specific rollback-residue nodes. It read the graph twice (at 15 nodes, then at 10 nodes) before removing nodes down to 6, then started rebuilding.

This took approximately 1:15 of the 12:02 total.

---

### Comparison Table

| Run | Time | plan_json rate | Tool success | Notes |
|-----|------|----------------|--------------|-------|
| 08i | 14:55 | 80% (8/10) | 96.6% | Pre-pipeline best; no orphan bug |
| 09m | 10:03 | 60% (9/15) | 86.4% | Single-agent; fixes not firing |
| 09n | 8:14 | 36% (4/11) | ~85% | Worst plan_json; orphan bug severe |
| **09o** | **12:02** | **44.4% (4/9)** | **76.1%** | Orphan bug still fires 1×; dumb cast; all BPs compile |

**09o sits between 09n and 09m on plan_json rate.** The run was longer than 09m/09n because the agent spent significant time in surgical node cleanup and iterative rebuilding of the attachment logic.

Tool success rate of 76.1% is the lowest observed (many connect_pins retries and repeated remove_nodes inflate the failure count; structurally the agent achieved its goal).

---

## Recommendations

1. **EnsurePinNotOrphaned fix is not deployed or not triggering.** The TypesIncompatible-on-exec-pin bug fired again on the same code path (BeginPlay event node reused after rollback of prior attempt). Verify the fix is compiled in and is being called on the correct path (the plan executor's node reuse path, not just node creation path).

2. **Stop rewriting GetMesh to GetComponentByClass.** The `RewriteAccessorCalls` substitution of `GetMesh` → `GetComponentByClass(SkeletalMeshComponent)` downgrades the return type from `SkeletalMeshComponent*` to `ActorComponent*`. This forced the agent to add an unnecessary cast to `SceneComponent`. The resolver should instead keep `GetMesh` as-is (it resolves correctly via the alias map to `GetComponentByClass` with the correct template specialization, or map it to the typed `GetSkeletalMeshComponent` accessor). Alternatively: alias `GetMesh` → `GetSkeletalMeshComponent` which returns `SkeletalMeshComponent*` directly.

3. **The SceneComponent cast was wrong but functional.** The agent cast `SkeletalMeshComponent → SceneComponent` because it needed a SceneComponent* for `K2_AttachToComponent`'s Parent pin. This works at runtime (SkeletalMesh IS a SceneComponent) but is architecturally backwards (casting to a less specific type). This entire detour is eliminated by fixing recommendation 2.

4. **describe_function is being used correctly.** 1 call, targeted, at a decision point (exploring attachment API). Not spam.

5. **DoOnce usage in FireArrow is wrong architecture.** The agent used `do_once` as the terminal node in FireArrow with `exec_after: print`. This means FireArrow can only fire once per game session (DoOnce blocks further calls until Reset is called). The correct pattern is no DoOnce here — it was likely added because the agent confused "ammo depletion stop" with "DoOnce semantics". This is a logic error in the generated Blueprint, not a tooling bug.

6. **InputKey(LeftMouseButton) as fire trigger is a workaround.** The correct pattern for UE5 input is an Enhanced Input Action. The agent tried `IA_Fire` (correct), found it missing, and fell back to legacy InputKey. The KB should note: "if IA_Fire does not exist, create it with `editor.run_python` or `blueprint.create` (DataAsset, UInputAction) before using it in plan_json."

7. **Rate limit hit once (20:06:39) and recovered correctly.** WriteRateLimit fired on the 6th add_variable in quick succession. The 44-second pause before the retry (20:06:39 → 20:07:22) indicates the agent correctly backed off rather than spamming. Rate limit behavior working as designed.

8. **Blueprint.add_node with type="CallFunction" does NOT search user-defined functions.** The agent hit this on `FireArrow`. Using `type="K2Node_CallFunction"` works around it but requires knowing the internal type name. Either: (a) make `type="CallFunction"` search user-defined functions on the Blueprint, or (b) add this to the system prompt knowledge as a known limitation.

9. **No GetForwardVector alias bug this run.** GetActorForwardVector was resolved correctly via alias in FireArrow. This suggests the bug from 09k/09m is either intermittent or was fixed.

10. **Final state: all 3 BPs compile SUCCESS.** BP_Bow (empty actor with components), BP_Arrow (hit + physics logic), BP_ThirdPersonCharacter (InitializeBow + FireArrow + InputKey trigger). System is functional but incomplete (no aiming, no animation integration, DoOnce logic error in FireArrow).
