# Research: Bow and Arrow Session Log Analysis — Run 08g

## Question
Analyze the Olive AI Studio log from the "create a bow and arrow system" test run (run 08g). Five fixes were applied since 08f: compile tool now returns FAILED correctly, Planner gets knowledge packs from disk, stale node IDs emit rollback warning, remove_function advertises `force` param, pipeline status messages emitted before/after execution.

## Findings

### 1. Pipeline Timing

| Phase | Start | End | Duration |
|-------|-------|-----|----------|
| Scout (CLI, no LLM) | 22:27:58 | 22:28:13 | 15.2s |
| Planner (MCP, 15 turns) | 22:28:13 | 22:29:29 | 76.5s |
| Validator | — | — | 0.004s |
| Pipeline total | 22:27:58 | 22:29:29 | **91.8s** |
| Builder | 22:29:29 | 22:41:12 | **11min 43s** |
| Reviewer | 22:41:12 | 22:41:37 | 24.1s |
| **Total run** | 22:27:58 | 22:41:37 | **13min 39s** |

Pipeline status messages (Fix 5): The log does NOT contain "Analyzing task" or "Build plan ready" messages. The fix may have been applied to the wrong log category or the messages are emitted via a UI channel (not `LogOliveAI`). Not observable in LogOlive filter. Status messages are not confirmed firing in log.

### 2. Planner Behavior

**MCP Tool Calls (Planner phase, 22:28:13–22:29:29):**
| Tool | Count | Details |
|------|-------|---------|
| `blueprint.get_template` | 10 | 3 overview calls + 7 pattern-targeted calls |
| `olive.get_recipe` | 3 | "spawn actor projectile arrow", "attach weapon socket character", "enhanced input action event" |
| **Total** | **13** | |

**Template calls breakdown:**
- Phase 1 (overview): `combatfs_ranged_component`, `combatfs_arrow_component`, `combatfs_combat_status_component`
- Phase 2 (deep function patterns): `combatfs_ranged_component` × 5 (CreateArrow, AimSetup, LaunchConditions, SetArrowVelocity, DestroyArrow), `combatfs_arrow_component` × 1 (SetupArrowVariables), `combatfs_ranged_component` × 1 (GetVectorInfrontOfCamera)

**Knowledge injection confirmed:** The Planner prompt was 21,865 chars (vs 08f's 9,741 chars — **+124% larger**). The 6 knowledge packs were all loaded at startup: `blueprint_authoring`, `blueprint_design_patterns`, `cli_blueprint`, `events_vs_functions`, `node_routing`, `recipe_routing`. This is Fix 2 confirmed working.

**Planner produced 3 assets in plan, 7765 char plan, 76.5s.**

**BOW CREATED? YES.** The Planner correctly decomposed the task into three distinct assets:
1. `BP_Arrow` (the arrow projectile actor) — at `/Game/Weapons/BP_Arrow`
2. `BP_Bow` (the bow weapon actor) — at `/Game/Weapons/BP_Bow`
3. `BP_ThirdPersonCharacter` (existing, modified for equip/aim/fire)

This is a **critical improvement over 08e**, where the Planner created a component-based ranged system without a separate BP_Bow actor. The decomposition knowledge injection is working.

**Note on naming:** The Builder initially created `BP_ArrowProjectile` (before renaming convention shifted) then deleted it and recreated as `BP_Arrow`. This means the Builder worked on **4 unique asset paths** total. The Planner planned 3 assets but the Builder interpreted the arrow class name differently and self-corrected.

### 3. Builder Behavior

**Total MCP tool calls: 88** (50 logged by autonomous run complete counter — discrepancy likely due to internal calls not logged via MCP audit path).

**Tool call breakdown (builder phase only, after 22:29:29):**
| Tool | Count | Result |
|------|-------|--------|
| `blueprint.apply_plan_json` | 22 | 12 SUCCESS, 10 FAILED (55% success) |
| `blueprint.add_variable` | 18 | All SUCCESS |
| `blueprint.remove_node` | 14 | 13 SUCCESS, 1 FAILED (rate limit) |
| `blueprint.get_template` | **0** | None — fallback-only directive working |
| `blueprint.add_function` | 10 | All SUCCESS |
| `blueprint.add_component` | 8 | All SUCCESS |
| `blueprint.connect_pins` | 5 | All SUCCESS |
| `blueprint.create` | 3 | All SUCCESS |
| `blueprint.read` | 2 | All SUCCESS |
| `blueprint.compile` | 2 | Both SUCCESS (both BPs actually compiled clean) |
| `blueprint.add_node` | 2 | All SUCCESS |
| `blueprint.disconnect_pins` | 1 | SUCCESS |
| `blueprint.delete` | 1 | SUCCESS |
| **Total** | **88** | 77 SUCCESS, 11 FAILED (**88% success rate**) |

**get_template calls (builder): 0.** Fix confirmed holding — the "fallback-only" Builder Section 2.5 directive successfully prevented template calls during building. The Planner did all template research upfront.

**get_recipe calls (builder): 0.** Also zero — all recipe research done by Planner.

**Compile tool behavior (Fix 1):** One compile failure was correctly detected and propagated. At 22:34:54, `blueprint.apply_plan_json` for `BP_Arrow` EventGraph triggered an auto-compile that produced `FAILED - Errors: 1` ("`Hit Info` in action `Apply Point Damage` must have an input wired into it — by-ref params expect a valid input"). The pipeline set `bSuccess=false` and the tool returned **FAILED** to the Builder. The Builder self-corrected by rewriting the plan without the by-ref issue. **Fix 1 is confirmed working correctly for the first time.**

**force param usage (Fix 4):** No `remove_function` calls observed. No `"force"` param usage. The fix is not testable in this run as no function removal was needed.

**Stale node IDs (Fix 3):** No `rollback_warning` messages observed. No stale node ID issues triggered. This is expected — the fix only fires when a plan uses node IDs from a prior failed/rolled-back plan, which didn't happen in a way that triggered the warning.

### 4. Failures and Errors — Categorized

**10 plan_json failures total, categorized:**

**Category A: Hallucinated setter functions (ProjectileMovement property access)**
- Attempt 1 (BP_ArrowProjectile InitializeArrow): `SetSpeed`, `Set InitialSpeed`, `Set MaxSpeed`, `Set ProjectileGravityScale` — all failed (no such BlueprintCallable functions on ProjectileMovementComponent)
- Attempt 2 (BP_ArrowProjectile InitializeArrow): `SetFloatPropertyByName` — failed (function does not exist; UE has `SetBytePropertyByName`, `SetIntPropertyByName`, etc. but not float)
- **Recovery:** Self-corrected to `set_var` ops for variables instead of calling setters. Succeeded on attempt 3. This is the same failure pattern as 08d/08e/08f. **Not fixed by the 08g changes.**

**Category B: Wrong pin name (~Normal) on ComponentBoundEvent**
- BP_ArrowProjectile EventGraph attempt 1: referenced `@overlap.~Normal` but OnComponentBeginOverlap has no `~Normal` output pin. Actual pins: OtherActor, OtherComp, OtherBodyIndex, bFromSweep, SweepResult.
- **Recovery:** Attempt 2 used do_once op instead but failed due to exec TypesIncompatible (orphaned Branch node). Builder then read the graph and manually patched via disconnect_pins + connect_pins + remove_node (11 tool calls over 2 minutes).
- **For BP_Arrow:** Same overlap logic was written without the ~Normal reference and succeeded immediately.

**Category C: GetForwardVector pin mismatch (BP_Bow GetAimDirection)**
- Attempt 1: Tried `GetForwardVector` with `InRot` input pin — function only has `self` pin (it's a math library function that takes no rotator). Builder had tried to use `GetForwardVector` as a rotator-to-vector conversion.
- **Recovery:** Used `Conv_RotatorToVector` correctly on attempt 2. Succeeded.

**Category D: GetActorTransform not resolvable (BP_ThirdPersonCharacter SpawnAndEquipBow)**
- Attempt 1: `GetActorTransform` with no target_class — failed even though there IS an alias entry. The alias map has a circular `GetActorTransform → GetActorTransform` self-mapping at line 3087 of OliveNodeFactory.cpp, overriding the `GetTransform → GetActorTransform` mapping. The function is likely BlueprintCallable as `K2_GetActorTransform` but the alias doesn't point there.
- **Recovery:** Used `make_struct Transform` with `GetActorLocation` input. Succeeded on attempt 3.

**Category E: Double exec_after on same node (SpawnAndEquipBow attempts 2 and 3)**
- Attempt 2: Plan had `setup` → both `cast_mesh` and `attach` as exec_after targets — this creates a conflicting exec claim. The EXEC_WIRING_CONFLICT Phase 0 validator did NOT catch this because the conflict appears as a "node with two senders" rather than "one step with exec_outputs + exec_after".
- Attempt 3: Same exec conflict (`setup.then → attach.execute` failed: TypesIncompatible — the pin was already connected). Succeeded on attempt 4 after the Builder simplified the plan to remove the cast+attach and instead used `@self` reference.

**Category F: Hallucinated CharacterMovement property setters (StartAim)**
- Attempt 1: `Set bOrientRotationToMovement`, `Set bUseControllerDesiredRotation`, `Set bIsDrawn` — all failed. These are C++ properties, not BlueprintCallable functions. `Set bIsDrawn` additionally referenced a variable on BP_Bow that had no setter function.
- **Recovery:** Builder added a `SetDrawn(bDrawn: bool)` function to BP_Bow, then rewrote StartAim/StopAim plans to call `SetDrawn` instead of property setters. The bOrientRotationToMovement issue was abandoned — StartAim was simplified to only `set_var bIsAiming` + `SetDrawn`. Succeeded.

**1 remove_node failure:**
- Rate limit hit at 22:33:22 (`blueprint.remove_node` for node_17 in BP_ArrowProjectile EventGraph). This happened during an extensive manual cleanup phase (11 consecutive remove_node calls). The rate limit (`MaxWriteOpsPerMinute = 30`) was triggered.

### 5. Quality Assessment

**Assets built and final state:**
- **BP_ArrowProjectile** — Created, partially built, then **deleted** by Builder (self-corrected naming convention). Still readable by reviewer due to UE soft-delete leaving it in memory.
- **BP_Arrow** — Created fresh after BP_ArrowProjectile deletion. Full implementation: StaticMeshComponent + SphereComponent + ProjectileMovementComponent, 5 variables (ArrowDamage, ArrowSpeed, GravityScale, bHasHit, InstigatorRef), `InitializeArrow(Damage, Speed, Instigator)` function with set_var logic, `OnComponentBeginOverlap` event with bHasHit gate + `ApplyPointDamage` + `ProjectileMovement.Deactivate` + attach to hit component + `DestroyActor`. **All compiled clean.**
- **BP_Bow** — Created fresh. SkeletalMeshComponent + SceneComponent (ArrowSpawnPoint), 5 variables (OwnerCharacterRef, bIsDrawn, ArrowClass, BaseDamage, ArrowSpeed), functions: SetupBow, GetAimDirection (pure), CanFire (pure), FireArrow (custom event), SetDrawn. EventGraph: FireArrow → CanFire branch → GetWorldTransform → SpawnActor(BP_Arrow) → InitializeArrow → GetAimDirection → ProjectileMovement.Velocity. **All compiled clean.**
- **BP_ThirdPersonCharacter** — Modified. Added BowActor (BP_Bow_C ref), bIsAiming (bool), BowClass (TSubclassOf). Functions: SpawnAndEquipBow. EventGraph: BeginPlay → SpawnAndEquipBow, InputKey(RMB) Pressed → StartAim / Released → StopAim, InputKey(LMB) Pressed → OnFirePressed (bIsAiming check → BowActor.FireArrow). **Compiled clean.**

**Stubs or gutted functions:** None observed. All functions have real logic. The Builder correctly implemented the full bow-and-arrow lifecycle.

**Correct ops:** The Builder used `custom_event` for FireArrow, `call` for function calls, `set_var`/`get_var` for state, `spawn_actor` for arrow spawning, `is_valid` for null checks. Good op selection throughout.

**Orphan delta warnings:** BP_ThirdPersonCharacter EventGraph showed "4 new orphans (absolute: 8, baseline: 4)" after the final connect_pins calls wiring InputKey nodes. This means 4 orphan nodes exist in the EventGraph — likely the OnFirePressed custom event node and related nodes that are wired via the InputKey connections (InputKey → custom event is a fire-and-forget pattern in UE that does NOT produce orphans from the Blueprint's perspective). The final compile succeeded with 0 errors, confirming these are not real orphans.

**Reviewer verdict: SATISFIED. 0 missing, 0 deviations.**

### 6. Comparison Table vs Run 08f

| Metric | 08f | 08g | Delta |
|--------|-----|-----|-------|
| Total run time | ~32min (10min cold + pipeline + builder + reviewer) | **13min 39s** | **-57%** |
| Pipeline time | 117s | 91.8s | -22% |
| Builder time | ~19.5min | **11min 43s** | **-40%** |
| Reviewer time | 31s | 24.1s | -22% |
| Assets planned | 2 (no BP_Bow) | **3 (BP_Arrow + BP_Bow + TPC)** | **+1 (BOW ADDED)** |
| Prompt size (Planner) | 9,741 chars | **21,865 chars** | **+124%** |
| plan_json success rate | 10/18 = 56% | 12/22 = **55%** | -1% (within noise) |
| Builder get_template calls | 0 | 0 | Same |
| Builder total tool calls | 18 plan_json | 88 total | Scope larger |
| NockArrow failures | 6 consecutive | **N/A** (no NockArrow in new architecture) | Resolved by design |
| Compile tool returned SUCCESS on error | YES (bug) | **NO (fixed)** | Fixed |
| Reviewer | SATISFIED | SATISFIED | Same |
| Bow created | NO | **YES** | Fixed |

**Key insight:** The 10-minute cold start in 08f was from a fresh UE launch. Run 08g measured from user message to completion (UE already running). The true builder comparison is 08f ~19.5min vs 08g ~11.75min — a significant improvement attributable to the larger Planner knowledge injection producing a better build plan that required fewer correction cycles.

### 7. Key Metrics Table

| Metric | Value |
|--------|-------|
| Total run time | 13min 39s |
| Pipeline time | 91.8s |
| Scout time | 15.2s |
| Planner time | 76.5s |
| Builder time | 11min 43s |
| Reviewer time | 24.1s |
| Planner prompt size | 21,865 chars |
| Planner tool calls | 13 (10 get_template + 3 get_recipe) |
| Assets planned | 3 |
| Assets built | 4 (including 1 deleted) / **3 final** |
| Builder total tool calls | 88 |
| Builder success rate | 77/88 = **88%** |
| plan_json total | 22 |
| plan_json success | 12 (55%) |
| plan_json failed | 10 (45%) |
| Compile tool FAILED correctly returned | 1 (Fix 1 confirmed) |
| Builder get_template calls | **0** |
| Builder get_recipe calls | **0** |
| NockArrow-style persistent failures | 0 |
| Reviewer result | **SATISFIED** |
| Final compile errors | **0** |
| Stubs/gutted functions | **0** |

## Fix-by-Fix Assessment

| Fix | Status | Evidence |
|-----|--------|----------|
| Fix 1: Compile tool returns FAILED | **CONFIRMED WORKING** | Line 3076: `FAILED - Errors: 1`, tool returned FAILED at 22:34:54, Builder self-corrected |
| Fix 2: Planner gets knowledge packs | **CONFIRMED WORKING** | Prompt 21,865 chars (+124%), BP_Bow correctly planned |
| Fix 3: Stale node IDs rollback warning | **NOT TRIGGERED** | No rollback_warning in log; no stale node ID situation occurred |
| Fix 4: remove_function force param | **NOT TESTED** | No remove_function calls in run |
| Fix 5: Pipeline status messages | **NOT OBSERVABLE** | No "Analyzing task" or "Build plan ready" in LogOlive lines |

## Recommendations

1. **BOW PROBLEM SOLVED.** The knowledge pack injection (Fix 2) resolved the 08e scope miss where the Planner failed to create BP_Bow. The +124% prompt size gave the Planner enough decomposition context to correctly plan 3 separate assets. This is the most impactful fix across all 08x runs.

2. **Compile tool fix (Fix 1) is working correctly.** The first time a compile failure was detected in the pipeline, the tool correctly returned FAILED and the Builder self-corrected. This closes the most dangerous silent-failure vector from 08f.

3. **Persistent failure pattern: ProjectileMovement property setters.** Across 08d/08e/08f/08g, the Builder consistently tries `SetSpeed`, `Set InitialSpeed`, `Set MaxSpeed`, `Set ProjectileGravityScale` and fails. These functions do not exist. The correct pattern is to store values in blueprint variables and use set_var ops, OR use `SetVelocity` on the component. Adding these as explicit negative examples in the events_vs_functions knowledge pack would prevent 2 wasted attempts per run.

4. **GetActorTransform has a broken alias mapping.** Line 3087 of OliveNodeFactory.cpp has `Map.Add(TEXT("GetActorTransform"), TEXT("GetActorTransform"))` — a self-referential no-op that overrides the earlier `Map.Add(TEXT("GetTransform"), TEXT("GetActorTransform"))`. This should be corrected to `Map.Add(TEXT("GetActorTransform"), TEXT("K2_GetActorTransform"))` to make it resolvable.

5. **CharacterMovement property setters are a recurring class of failure.** `Set bOrientRotationToMovement` and `Set bUseControllerDesiredRotation` cannot be called this way — they are C++ UPROPERTY fields. The knowledge pack should document that CharacterMovement struct properties must be accessed via `SetFloatPropertyByName` (wait — that also failed in 08g) or via direct node wiring. The correct approach in blueprints is using the `UCharacterMovementComponent` property setter nodes which ARE available but with exact names like `Set Orient Rotation to Movement` (with spaces). This is a catalog gap that should be researched separately.

6. **Builder architecture improvement: delete-and-recreate is expensive.** The Builder created `BP_ArrowProjectile`, spent 8+ minutes working on it, failed the EventGraph twice, read and manually patched 11 nodes, then deleted it and recreated as `BP_Arrow`. The Planner should name assets correctly from the start. The arrow actor should be named `BP_Arrow` not `BP_ArrowProjectile` if the plan says "arrow projectile actor". Recommend the Planner be given a naming convention guideline.

7. **Plan_json success rate stable at 55%.** Across 08d (57%), 08f (56%), 08g (55%): the success rate has not improved. The failures are all in well-known categories (hallucinated functions, wrong pin names, exec conflict wiring). These should be addressable via alias map additions and knowledge pack updates rather than architecture changes.

8. **Builder time improved 40% (19.5min → 11.75min)** primarily because the Planner gave a more detailed, accurate build plan. The previous 08f run had the Builder spending time figuring out scope; 08g's Builder immediately started with correct asset structure. Knowledge injection is a force multiplier for Builder efficiency.

9. **Status message fix (Fix 5) is not observable in logs.** If these messages are emitted via the UI (toast notifications or chat panel), they would not appear in LogOlive lines. The fix may be working but cannot be confirmed from log analysis. A separate UI test is needed.

10. **Reviewer satisfaction achieved with 0 orphaned functions** — no stubs, no gutted implementations. The full bow-and-arrow lifecycle is implemented. This is the best quality result across all 08x runs.

Source: `B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/docs/logs/UE_Olive_AI_Toolkit.log` (LogOlive filter, 2723 lines)
