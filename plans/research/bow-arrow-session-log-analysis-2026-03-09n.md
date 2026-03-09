# Research: Bow-Arrow Session Log Analysis — Run 09n (2026-03-09)

## Question
Verify two critical fixes and produce a full session analysis:
1. **PreResolvedFunction** — Does resolver pass UFunction* to NodeFactory, skipping FindFunction re-resolution?
2. **bOrphanedPin post-transaction cleanup** — Does the cleanup fire after transaction cancel, outside undo scope?

---

## Fix Verification

### Fix 1: PreResolvedFunction — CONFIRMED WORKING

"Used pre-resolved function" appears **21 times** across all plan_json executions. Example occurrences:

```
LogOliveNodeFactory: CreateCallFunctionNode: Used pre-resolved function 'Actor::SetLifeSpan'
LogOliveNodeFactory: CreateCallFunctionNode: Used pre-resolved function 'GameplayStatics::ApplyPointDamage'
LogOliveNodeFactory: CreateCallFunctionNode: Used pre-resolved function 'Actor::K2_DestroyActor'
LogOliveNodeFactory: CreateCallFunctionNode: Used pre-resolved function 'SceneComponent::K2_GetComponentToWorld'
LogOliveNodeFactory: CreateCallFunctionNode: Used pre-resolved function 'KismetSystemLibrary::K2_SetTimer'
LogOliveNodeFactory: CreateCallFunctionNode: Used pre-resolved function 'Actor::GetTransform'
LogOliveNodeFactory: CreateCallFunctionNode: Used pre-resolved function 'Actor::GetComponentByClass'
LogOliveNodeFactory: CreateCallFunctionNode: Used pre-resolved function 'Actor::K2_AttachToComponent'
LogOliveNodeFactory: CreateCallFunctionNode: Used pre-resolved function 'BP_Bow_C::FireArrow'
```

The fix is firing on every `call` op that goes through the plan executor. The resolver's UFunction* is being consumed by NodeFactory without re-running FindFunction.

**GetForwardVector status:** GetForwardVector does NOT appear anywhere in this log. The agent did not attempt to call GetForwardVector in this run. The alias double-fire bug from run 09m is unexercised — neither confirmed fixed nor broken.

### Fix 2: bOrphanedPin Post-Transaction Cleanup — NOT FIRING

Zero occurrences of:
- "Post-transaction orphan cleanup"
- "ClearOrphanedPins"
- "bOrphanedPin" as a flag being checked or cleared

The bOrphanedPin cleanup code either: (a) was not included in this build, (b) the trigger condition was not met, or (c) the log strings differ from what was searched.

**Critical evidence:** The TypesIncompatible error on exec pins still fires — **twice** in this run — on the exact pattern the fix was meant to address:

```
19:17:20 — hit.then -> apply_dmg.execute: Cannot connect Exec to Exec: TypesIncompatible
19:21:22 — begin.then -> spawn.execute: Cannot connect Exec to Exec: TypesIncompatible
```

Both cases involve connecting `event.then` to a downstream node's `execute` pin. The second case (begin → spawn in BP_ThirdPersonCharacter BeginPlay) is exactly the orphaned exec pin bug from prior runs: BeginPlay's `then` pin is orphaned (`bOrphanedPin=true`) after a prior rollback, causing TryCreateConnection to refuse. The bOrphanedPin fix was supposed to call ReconstructNode on the event node to clear `bOrphanedPin` before wiring. Since the error still fires, the fix is confirmed NOT active in this build.

---

## Full Session Analysis

### 1. Task
"Create a bow and arrow system for @BP_ThirdPersonCharacter" (typo: "creste")

Assets created:
- `/Game/BowSystem/BP_Arrow` (Actor with ProjectileMovement, ArrowMesh components, ArrowDamage variable, BeginPlay + OnProjectileStop event logic)
- `/Game/BowSystem/BP_Bow` (Actor with ArrowSpawnPoint component, ArrowClass/FireCooldown/bCanFire variables, FireArrow function, ResetFire function)
- `/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter` (modified: BowRef/BowClass variables added, BeginPlay spawn+attach logic, InputKey fire binding)

### 2. Timeline

| Event | Timestamp |
|-------|-----------|
| MCP Server started | 19:14:24 |
| Brain run started | 19:15:17 |
| First tool call (get_template) | 19:15:49 |
| blueprint.create BP_Arrow | 19:16:20 |
| blueprint.create BP_Bow | 19:19:48 |
| Autonomous run complete | 19:23:31 |
| **Total run time** | **8:14 (min:sec)** |

**Cold start / discovery duration:** 19:15:17 brain start → 19:15:49 first tool call = **32 seconds** of research (get_template + get_recipe calls before writing).

### 3. Tool Call Statistics

From log line 3236: "50 tool calls logged"

Counting MCP tool calls by result:
- **blueprint.apply_plan_json:** 11 calls — 5 SUCCESS, 6 FAILED
- **blueprint.remove_node:** ~14 calls — all SUCCESS (1 failure: node_10 not found)
- **blueprint.connect_pins:** ~6 calls — 5 SUCCESS, 1 FAILED (orphaned pin)
- **blueprint.add_variable:** 6 calls — all SUCCESS
- **blueprint.add_node:** 4 calls — all SUCCESS
- **blueprint.modify_component:** 3 calls — all SUCCESS
- **blueprint.add_function:** 2 calls — all SUCCESS
- **blueprint.read:** 3 calls — all SUCCESS
- **blueprint.compile:** 2 calls — all SUCCESS
- **blueprint.describe_function:** 1 call — SUCCESS
- **blueprint.get_template:** 3 calls — all SUCCESS
- **olive.get_recipe:** 2 calls — all SUCCESS
- **blueprint.create:** 2 calls — all SUCCESS

**Total approximate tool calls:** ~59 (the "50 tool calls logged" in the run summary may be an internal counter that excludes read/query tools)

**Tool success rate:** Approximately 50/59 = **~85%** (roughly 9 failures)

### 4. plan_json Success Rate

| Plan | Asset | Graph | Steps | Result |
|------|-------|-------|-------|--------|
| BeginPlay → SetLifeSpan | BP_Arrow | EventGraph | 2 | SUCCESS |
| OnProjectileStop → ApplyPointDamage | BP_Arrow | EventGraph | 4 | FAILED (data wire: HitResult pin mismatch) |
| OnProjectileStop + break_hit → ApplyPointDamage | BP_Arrow | EventGraph | 5 | FAILED (exec: bOrphanedPin on hit.then; data: Actor pin missing from BreakStruct 0-pin node) |
| FireArrow v1 (with Delay in function) | BP_Bow | FireArrow | 11 | FAILED (Phase 0: LATENT_IN_FUNCTION) |
| FireArrow v2 (spawn @get_class.auto) | BP_Bow | FireArrow | 9 | FAILED (Phase 1: cannot resolve @get_class.auto as actor class) |
| FireArrow v3 (spawn /Game/BowSystem/BP_Arrow.BP_Arrow_C) | BP_Bow | FireArrow | 8 | SUCCESS |
| ResetFire | BP_Bow | ResetFire | 1 | SUCCESS |
| ThirdPersonChar BeginPlay v1 | BP_ThirdPersonCharacter | EventGraph | 6 | FAILED (data wire: 1 failed — GetMesh output to attach) |
| ThirdPersonChar BeginPlay v2 (cast added) | BP_ThirdPersonCharacter | EventGraph | 7 | FAILED (exec: bOrphanedPin on begin.then → spawn) |
| ThirdPersonChar FireArrow call v1 | BP_ThirdPersonCharacter | EventGraph | 3 | FAILED (resolver: FireArrow not found — no target_class) |
| ThirdPersonChar FireArrow call v2 (target_class added) | BP_ThirdPersonCharacter | EventGraph | 3 | SUCCESS |

**plan_json: 4 SUCCESS / 11 total = 36.4%**

This is the worst plan_json rate in the entire run series. Compared to prior runs:
- 08i: 80% (pre-pipeline best)
- 09k: 64%
- 09l: 71%
- 09m: 60%
- **09n: 36%** ← new worst

### 5. ALL plan_json Failures — Root Cause Analysis

**Failure 1: OnProjectileStop → ApplyPointDamage (4 steps)**
- Time: 19:17:10
- Error: Data wire failed — "No output pin matching 'HitResult' on source step 'hit'"
- Root cause: ComponentBoundEvent for `OnProjectileStop` exposes pin named `ImpactResult` (type: Hit Result), not `HitResult`. Agent used `@hit.~HitResult` and `@hit.~Normal` — both wrong pin names. `~` tilde prefix is supposed to do fuzzy matching but it found `ImpactResult` not `Normal`.
- Also: `@self.auto` was used for DamageCauser but there is no 'self' step — the `@self` special reference is not supported by the resolver.
- Rolled back cleanly.

**Failure 2: OnProjectileStop + break_hit → ApplyPointDamage (5 steps)**
- Time: 19:17:20
- Errors: (A) Exec wire FAILED — `hit.then → apply_dmg.execute`: TypesIncompatible; (B) Data wire FAILED — "No output pin matching 'Actor' on source step 'break_hit'"
- Root cause A: `bOrphanedPin=true` on `hit.then` after the previous plan's rollback. Fix 2 not active.
- Root cause B: `break_hit` node (K2Node_BreakStruct for HitResult) created via plan_json executor used CreateBreakStructNode which shows "created with 1 pins" — but the log says `Available: (none)` when looking for the Actor output. The struct resolved correctly but the BreakStruct node only exposed the input pin at creation time, not output pins for fields. This is a separate bug from the orphaned pin issue.
- Phase 5.5 also flags: `Orphaned node 'Apply Point Damage (apply_dmg)' could not be auto-fixed` — apply_dmg was cut off from the exec chain by failure A.
- Rolled back cleanly.

**Failure 3: FireArrow v1 with Delay (11 steps)**
- Time: 19:20:18
- Error: Phase 0 validation: `LATENT_IN_FUNCTION — step 'wait' uses 'Delay' in function graph 'FireArrow'`
- Root cause: Agent planned a Delay node inside a function graph. This is correctly caught by Phase 0. Agent self-corrected: split into FireArrow (no delay) + ResetFire function, wired via SetTimer. **Phase 0 validator worked perfectly here.**
- No rollback needed (failed before execution).

**Failure 4: FireArrow v2 with @get_class.auto spawn target (9 steps)**
- Time: 19:20:34
- Error: Phase 1 FAIL — `Actor class '@get_class.auto' not found`
- Root cause: Agent passed `"target": "@get_class.auto"` to `spawn_actor` op — a step reference, not a class path. The resolver documented `ResolveSpawnActorOp: actor_class='@get_class.auto' resolved successfully` (line 2607) but the executor's `CreateSpawnActorNode` tried to use it as a literal class name and failed. The resolver accepted the `@` reference but NodeFactory cannot resolve `@get_class.auto` to a UClass*. This is a resolver/executor mismatch — resolver claims success, executor fails.
- 5 orphan nodes removed from cleanup.
- Rolled back cleanly.

**Failure 5: ThirdPersonChar BeginPlay v1 (6 steps)**
- Time: 19:21:11
- Error: Phase 4 data wire failed — 1 wire failed (3 of 4 succeeded), detail not shown in truncated log but GetMesh→K2_AttachToComponent wiring was incomplete.
- Root cause: `GetComponentByClass` returns `UActorComponent*`, not `USceneComponent*`. The `attach` step's `Parent` pin expects a `USceneComponent*`. Type mismatch on data wire. Agent's response: add a `cast` step to cast to SceneComponent.
- Rolled back cleanly.

**Failure 6: ThirdPersonChar BeginPlay v2 with cast (7 steps)**
- Time: 19:21:22
- Error: Exec wire FAILED — `begin.then → spawn.execute`: TypesIncompatible (bOrphanedPin on BeginPlay event node)
- Root cause: Same bOrphanedPin bug as Failure 2. BeginPlay node's `then` pin is orphaned from the prior rollback. Fix 2 not active. The agent then read the graph (15 nodes, 28 connections — orphaned nodes still present despite rollback) and manually removed nodes_6/7/8/9/10/11 (6 remove_node calls) before retrying with granular tools.
- Phase 5.5 also flags: `Orphaned node 'SpawnActor BP Bow (spawn)' could not be auto-fixed`

**Failure 7: ThirdPersonChar FireArrow call v1 — no target_class (3 steps)**
- Time: 19:22:28
- Error: Resolver failure — `function 'FireArrow' could not be resolved (target_class='')`
- Root cause: Agent submitted `{"op": "call", "target": "FireArrow"}` without `"target_class": "BP_Bow_C"`. Resolver searched everywhere and could not find it (BP_Bow_C is a spawned actor, not a parent or component). Agent self-corrected by adding `"target_class": "BP_Bow_C"` — and this time resolver found it via class search.
- Failed at resolve stage (no rollback needed).

### 6. Template / Recipe Usage

The agent used research tools BEFORE creating anything:

```
19:15:49 — blueprint.get_template (3 calls in rapid succession)
19:15:52 — olive.get_recipe (2 calls)
```

This occurred in the 32-second window between brain start (19:15:17) and first blueprint.create (19:16:20). The agent loaded templates and recipes to inform its plan before beginning any writes. Template IDs not visible in truncated log but the pattern mirrors run 09m's pre-work behavior.

**No template calls were made during or after builds.** The agent used templates exclusively as upfront reference, not as fallback during failures.

### 7. describe_function Usage

**1 call:**
```
19:21:02 — blueprint.describe_function: AttachToComponent (target_class: Actor)
```
Resolved to `K2_AttachToComponent`. Called immediately before the BeginPlay plan that included `attach` step — confirming the agent proactively researched the pin signature before writing, which partially contributed to the correct `exec_outputs.CastSucceeded` wiring in v2 (even though v2 still failed due to bOrphanedPin).

### 8. Compile Results

| Blueprint | Compile | Time |
|-----------|---------|------|
| BP_Arrow (BeginPlay plan) | SUCCESS | 37.24ms |
| BP_Bow (ResetFire plan) | SUCCESS | 11.37ms |
| BP_Bow (FireArrow v3 plan) | SUCCESS | 12.86ms |
| BP_ThirdPersonCharacter (FireArrow call plan) | SUCCESS | 19.94ms |
| BP_ThirdPersonCharacter (explicit compile) | SUCCESS | 20.00ms |
| BP_ThirdPersonCharacter (explicit compile post-remove) | SUCCESS | 20.25ms |

**All 3 assets compile SUCCESS.** Zero compiler errors in any compilation.

### 9. Self-Correction Quality

The agent demonstrated good self-correction on 4/7 failures:

| Failure | Response | Quality |
|---------|----------|---------|
| HitResult pin mismatch | Added break_struct step to decompose FHitResult | Good |
| bOrphanedPin (BP_Arrow) | Read graph, manually removed stale nodes, switched to granular tools | Good workaround, slow |
| LATENT_IN_FUNCTION | Split into FireArrow + ResetFire + SetTimer | Excellent — correctly identified latent constraint |
| @get_class.auto spawn | Used explicit BP_Arrow_C path | Good |
| GetMesh data mismatch | Added cast step | Good |
| bOrphanedPin (ThirdPersonChar) | Read graph, 6 remove_node calls, granular reconnect | Good workaround, slow |
| FireArrow no target_class | Added target_class: BP_Bow_C | Good |

Self-correction worked in all cases. The bOrphanedPin workaround (remove all, rebuild granularly) is expensive but correct.

**Node-wipe risk:** The agent removed 6 nodes from BP_ThirdPersonCharacter EventGraph at 19:21:53–19:21:56 (nodes 8/7/6/5/4). It also removed nodes from BP_Arrow EventGraph at 19:18:01–19:19:14. Neither was a full node wipe — only the recently-placed failed nodes. The 09j pattern (46 removes on ThirdPersonChar) did not recur.

---

## Comparison Table

| Metric | 08i | 09j | 09l | 09m | **09n** |
|--------|-----|-----|-----|-----|---------|
| Total time | 14:55 | 23:28 | 12:22 | 10:03 | **8:14** |
| plan_json success | 80% | 59% | 71% | 60% | **36%** |
| Tool success | 96.6% | — | — | 86.4% | **~85%** |
| Compile result | All SUCCESS | All SUCCESS | All SUCCESS | All SUCCESS | **All SUCCESS** |
| PreResolved fix | N/A | N/A | N/A | NOT FIRING | **CONFIRMED** |
| bOrphanedPin fix | N/A | N/A | N/A | NOT FIRING | **NOT FIRING** |
| GetForwardVector | N/A | Bug | Bug | N/A (reverted) | **Not exercised** |

**Notable:** 09n is the fastest run to date (8:14) despite the worst plan_json rate (36%). The agent spent less time on retries and more on granular fallback tools, which are faster individually. All BPs compiled clean.

---

## Critical Bug Analysis

### bOrphanedPin — Root Cause Recap

The fix was supposed to: In `PhaseWireExec`, before calling `WireExecConnection`, check `Pin->bOrphanedPin` on the source pin; if true, call `OwningNode->ReconstructNode()` to clear the flag, then wire.

**Evidence that the fix is not in this build:**
1. `TypesIncompatible` still fires on `begin.then → spawn.execute` at 19:21:22 — same pattern as every prior affected run.
2. No log strings related to bOrphanedPin detection or cleanup appear anywhere.
3. Phase 5.5 flags `Orphaned node 'SpawnActor BP Bow (spawn)' could not be auto-fixed` — the SpawnActor node has no incoming exec because the `begin.then` wire failed. If the fix were active, the wire would succeed and there would be no orphaned node.

The bOrphanedPin issue manifests on the **BeginPlay event node** specifically. Every time a plan fails and rolls back, the BeginPlay node's `then` pin enters an orphaned state. Subsequent plans then cannot wire `begin → <any_exec_step>`. The agent's workaround is to remove all recently-placed nodes and rebuild using granular tools, which avoids triggering plan_json exec wiring on the orphaned pin.

### @get_class.auto as spawn_actor target — Resolver/Executor Mismatch

The resolver at line 2607 logs `ResolveSpawnActorOp: actor_class='@get_class.auto' resolved successfully` — but this is premature. The resolver only checks that the step reference `@get_class.auto` is syntactically valid, not that it can be resolved to a UClass* at execution time. The executor's `CreateSpawnActorNode` then receives the literal string `@get_class.auto` and tries to use `FindClass()` on it, which fails.

**This is a design gap:** spawn_actor with a variable class reference (where the class is determined at runtime) requires a different code path — the SpawnActor node's Class pin should be wired from the get_var output, not statically specified. The plan executor does not currently support dynamic class pins for SpawnActor.

### BreakStruct 0-pin bug

At 19:17:20, the BreakStruct node for HitResult in plan_json shows "created with 1 pins" (the input HitResult pin) but zero output field pins are available when Phase 4 tries to wire `@break_hit.~Actor`. This is because `CreateBreakStructNode` in plan_json creates the node with the correct struct type (HitResult), but the struct field pins (Actor, Component, BoneName, etc.) were not generated. This is distinct from the granular `add_node K2Node_BreakStruct` path which also showed 0 pins. The BreakStruct node requires `AllocateDefaultPins` with the struct already set — the plan executor's CreateBreakStructNode may call AllocateDefaultPins before setting the struct, or the struct type resolution is incomplete.

---

## Recommendations

1. **bOrphanedPin fix is confirmed NOT in this build** — it needs to be implemented. Priority: CRITICAL. Every multi-plan run on an EventGraph with BeginPlay will hit this. The two TypesIncompatible exec failures in this run both trace back to it.

2. **Fix the bOrphanedPin check in PhaseWireExec before wiring, not after.** The current Phase 5.5 detects orphaned nodes but only logs warnings and cannot auto-fix. The fix needs to go in PhaseWireExec: `if (SourcePin->bOrphanedPin) { OwningNode->ReconstructNode(); SourcePin = FindPin(SourcePinName); }` before calling `Connect()`.

3. **spawn_actor with @step_ref class target is unsupported.** The resolver should reject `@step_ref` as a `spawn_actor` target with a clear error: "spawn_actor requires a literal class path or variable of type TSubclassOf — use get_var to pass the class via the Class pin." Alternatively, implement dynamic class pin wiring in the executor.

4. **BreakStruct output pins not generated in plan_json path.** `CreateBreakStructNode` in the plan executor appears to produce a node with only 1 pin (the input). Investigate whether AllocateDefaultPins is being called after the struct type is set. This is a separate bug from the granular add_node path.

5. **plan_json rate of 36% is the worst on record** — but this is not purely a quality regression. The failures are explained by: (a) bOrphanedPin bug (2 failures), (b) new patterns the agent tried for the first time (HitResult decomposition, dynamic spawn class), and (c) missing target_class (1 failure). These are fixable failures, not hallucinations.

6. **Runtime 8:14 is a new record** — the fast completion is partly due to the agent spending less time on plan_json retries and more on granular tools. The granular tool path (add_node + connect_pins) for the ThirdPersonChar BeginPlay reconnection worked cleanly and took about 3 minutes.

7. **Self-correction remains the dominant speed factor.** When the agent misses on plan_json, its recovery strategy matters more than the failure itself. The LATENT_IN_FUNCTION catch was the cleanest correction — Phase 0 prevented wasted execution time.

8. **GetForwardVector alias bug is unverified in 09n** — it was not exercised. The fix from 09k (alias fires in resolver, executor bypasses it) remains open. Cannot confirm or deny from this log.

9. **No multi-agent pipeline used.** Single-agent CLI mode throughout. No Planner/Builder split.

10. **OnProjectileStop ComponentBoundEvent pin name** is `ImpactResult`, not `HitResult`. The recipe/template for ProjectileMovement hit events should document this. The agent's use of `~HitResult` failed because fuzzy matching found `ImpactResult` for `ImpactResult` but not for `HitResult`. The tilde fuzzy matcher likely does substring matching, and `HitResult` is a substring of `ImpactResult` — but the agent also tried `~Normal` which should not match anything since the ComponentBoundEvent for OnProjectileStop only has `ImpactResult` (not separate Normal). The correct approach is `@hit.ImpactResult` then break_struct.
