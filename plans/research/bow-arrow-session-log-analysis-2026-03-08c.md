# Research: Bow-Arrow Session Log Analysis (2026-03-08c)

## Question

Analyze the session log at `docs/logs/UE_Olive_AI_Toolkit.log` for the post-fix test run (2026-03-08). This run followed three prompt changes (verify-first directive, compile-per-function, auto-continue reduction to 2) and two C++ fixes (Fix A: alias skip on re-entry, Fix B: data wire rollback). The user reports a regression in output quality. Answer six specific diagnostic questions, provide a full failure inventory, and give a brutally honest quality assessment.

Source log: `docs/logs/UE_Olive_AI_Toolkit.log` (4682 lines)

---

## Diagnostic Questions

### Q1: Did the Builder follow the "verify pin names first" directive?

**Answer: Partially — the directive worked where the catalog had entries, and failed silently where it did not.**

The Builder called `blueprint.get_template` 4 additional times immediately after launch (lines 1911–1926: CreateArrow, SetArrowVelocity, LaunchConditions, ArrowStuckInWall). This is verify-first behavior and it worked correctly for those functions.

The Builder also called `blueprint.describe_node_type` for specific node types at the moment it needed them:
- `describe_node_type("K2_SetVariableByName")` — succeeded
- `describe_node_type("SetFieldOfView")` — FAILED (not in catalog)
- `describe_node_type("Set Field Of View")` — FAILED (not in catalog)
- `describe_node_type("K2_AttachToComponent")` — FAILED (not in catalog)

The two failed `describe_node_type` calls are the proximate cause of the two most damaging failures in this run (StartAim degradation and NockArrow rollback). The directive is logically sound but the catalog coverage is inadequate. The Builder had no recourse when the catalog returned nothing — it guessed, guessed wrong, and either failed (NockArrow) or catastrophically simplified (StartAim).

**Verdict: Directive fires. Catalog coverage is the bottleneck.**

---

### Q2: Did compile-per-function fire?

**Answer: YES — confirmed working.**

Four intermediate compiles fired at logical boundaries:
- After BP_ArrowProjectile structure (variables, components, function signatures): line ~2053
- After BP_ThirdPersonCharacter function signatures (EquipBow, UnequipBow stubs): line ~3184
- After ThirdPersonCharacter function graphs built: line ~3536 (just before timeout)
- Final compile after EventGraph wiring: line ~4494

This is a material improvement over previous runs where compile only happened at end. The intermediate compiles caught the persistent ApplyPointDamage compile error early (lines 2278–2350) and allowed the Builder to retry with a different approach rather than discovering the error at the very end.

**Verdict: Working as designed. No regression here.**

---

### Q3: Did Fix B (data wire rollback) trigger?

**Answer: YES — triggered twice.**

- **First trigger**: Line 2265 — `"Phase 4: 1 data wire(s) failed — marking for rollback"` on ApplyHitDamage. Root cause: BreakHitResult output pin for ImpactNormal is named `~Normal` (tilde prefix) and the data wirer could not match it. Builder retried 3 times, eventually switching from ApplyPointDamage to the simpler ApplyDamage which doesn't need the ImpactNormal pin.

- **Second trigger**: Line 3912 — `"Phase 4: 1 data wire(s) failed — marking for rollback"` on NockArrow. Root cause: K2_AttachToComponent target pin expected SceneComponent; Builder provided GetComponentByClass result (UActorComponent) — type mismatch. Builder added a `cast` op on the second attempt and succeeded.

Fix B is working correctly. The rollbacks forced retries that ultimately produced working output. The cost was approximately 2.5 additional minutes and some plan simplification.

**Verdict: Working as designed. Two successful rollback+retry cycles.**

---

### Q4: Did Fix A (alias skip) work?

**Answer: INDETERMINATE — cannot confirm from log output.**

The log contains frequent "alias resolved" lines (meaning the alias map IS being used throughout the run). There are no log lines matching "_resolved" suffix or "bSkipAliasMap" anywhere in the 4682-line log. Either:

1. The fix was implemented without a dedicated log line, so there is nothing to observe.
2. The specific code path that Fix A targets (alias re-resolution on retry) was not triggered in this run.
3. The fix is present but the test scenario didn't exercise it.

No alias-related regression was observed either. The alias map appears to have functioned normally throughout.

**Verdict: Cannot confirm or deny. Add a log line at the skip point to make this observable.**

---

### Q5: Did the 120s idle timeout trigger prematurely?

**Answer: NO — the idle timeout did not fire. What fired was the 900s total runtime limit.**

Line 3536: `"LogOliveCLIProvider: Warning: Claude process exceeded total runtime limit (900 seconds) - terminating"`

The Builder was killed WHILE ACTIVELY WORKING — it had just completed a `blueprint.connect_pins` call for StartAim. This is a wall-clock kill, not an idle kill. The Builder was not idle; it was building.

The 900s limit is 15 minutes. This run involved building two complex Blueprints (BP_ArrowProjectile with 5 functions + event graph, BP_ThirdPersonCharacter with 6 functions + event graph). With multiple plan_json retries (5 failed attempts across the run costing ~2.5 minutes each) and the Scout+Planner overhead (108s), the Builder had approximately 792 seconds of budget after pipeline completion. That was not enough.

**Verdict: 900s limit is too short for 2-Blueprint tasks with retries. The idle timeout is irrelevant here — wall-clock was the constraint.**

---

### Q6: How many auto-continues happened?

**Answer: 1 of 2 max auto-continues fired.**

Line 3559: `"Run timed out (attempt 1/2) — relaunching with decomposition nudge"`
Line 3565: `"Stdin content delivered: 2892 chars"`

The continuation prompt was 2892 characters — very sparse compared to what would be needed to resume a complex multi-Blueprint task. The Builder restarted and completed:
- ReleaseAim (4 nodes, SUCCESS)
- DestroyArrow (4 nodes + Branch fix, SUCCESS)
- NockArrow (2 attempts — first rollback, second SUCCESS with cast op)
- FireArrow (9 nodes, SUCCESS)
- EventGraph InputKey wiring (12 nodes + 4 connect_pins, SUCCESS)

However, the continuation **completely skipped fixing StartAim** — the function that had been catastrophically simplified to 4 nodes (get bBowEquipped → branch → set bIsAiming → print string) immediately before the timeout. The full StartAim was supposed to set camera FOV, character movement speed, and orient rotation — all dropped.

**Verdict: Auto-continue fired once, worked structurally, but the sparse continuation prompt did not include enough context for the Builder to know StartAim needed repair.**

---

## Full Failure Inventory

| # | Line | Tool | Failure | Root Cause | Resolution |
|---|------|------|---------|------------|------------|
| 1 | 2043 | `blueprint.modify_component` | "No properties were successfully modified" for `bNotifyRigidBodyCollision` | Property name does not match SphereComponent's reflection-exposed name | Builder moved on; property not set |
| 2 | 2265 | `blueprint.apply_plan_json` (ApplyHitDamage) | Data wire rollback: `'~Normal'` pin not found on BreakHitResult | BreakHitResult pin name includes tilde prefix; plan wirer does exact/normalized match | Builder retried 3x; simplified to ApplyDamage without ImpactNormal |
| 3 | ~2278 | `blueprint.apply_plan_json` (ApplyHitDamage v2) | Compile failure: ApplyPointDamage `HitFromDirection` pin issue | ApplyPointDamage function signature mismatch (possibly wrong target class) | Retry → switched to ApplyDamage |
| 4 | ~2350 | `blueprint.apply_plan_json` (ApplyHitDamage v3) | Compile failure again on ApplyPointDamage | Same as above | Final retry with ApplyDamage succeeded |
| 5 | 2550–2560 | `blueprint.connect_pins` | "Source pin 'ImpactNormal' not found on node 'node_5'" | Builder tried to wire ImpactNormal directly after rollback instead of using ~Normal | Builder abandoned manual wiring, used plan_json |
| 6 | 2694, 2713, 2729 | `blueprint.connect_pins` × 3 | EMPTY error string | Diagnostic text completely missing — no failure reason surfaced | Builder abandoned approach, used plan_json |
| 7 | 2880–2913 | `blueprint.add_node` (ComponentBoundEvent) | Node created but 0/2 properties set via reflection; Builder deleted it | `component_name` and `delegate_name` are NOT UPROPERTYs on UK2Node_ComponentBoundEvent | Builder correctly pivoted to `event` op in plan_json which uses InitializeComponentBoundEventParams directly |
| 8 | 3149–3167 | `blueprint.add_function` × 2 | WriteRateLimit validation triggered | Two rapid-fire add_function calls hit rate limiter | Builder waited, retried successfully |
| 9 | 3312–3341 | `blueprint.apply_plan_json` (StartAim v1) | 3 resolution errors — SetFieldOfView found on wrong class | `describe_node_type("SetFieldOfView")` had already failed; Builder guessed BlueprintCameraPoseFunctionLibrary | Retry |
| 10 | 3349–3355 | `blueprint.describe_node_type` × 2 | FAILED for "SetFieldOfView" and "Set Field Of View" | Not in catalog | No recourse |
| 11 | 3357–3389 | `blueprint.apply_plan_json` (StartAim v2) | 2 errors: `bOrientRotationToMovement` on wrong class, `MaxWalkSpeed` wrong class | Resolver found CameraComponent for FOV (correct) but CharacterMovement component functions not resolved | Retry |
| 12 | 3393–3465 | `blueprint.apply_plan_json` (StartAim v3) | "SUCCESS" but plan was gutted to: get bBowEquipped → branch → set bIsAiming → print | Builder gave up on FOV/movement, went minimal | No retry — Builder accepted degraded output |
| 13 | 3536 | Builder process | 900s total runtime kill — Builder terminated mid-execution | Wall-clock limit too short for 2-Blueprint task with retries | Auto-continue fired (attempt 1/2) |
| 14 | 3819–3822 | `blueprint.describe_node_type` | FAILED for "K2_AttachToComponent" | Not in catalog | No recourse |
| 15 | 3823–3924 | `blueprint.apply_plan_json` (NockArrow v1) | Data wire rollback: GetComponentByClass→SceneComponent type mismatch | No pin info from describe_node_type; Builder guessed wrong type | Retry with cast op — succeeded |
| 16 | 4309–4334 | `blueprint.add_node` ("CallFunction"/"Call Function"/"K2Node_CallFunction") × 3 | GHOST_NODE_PREVENTED or unknown type | add_node cannot create K2Node_CallFunction without a valid function reference via reflection | Builder pivoted to apply_plan_json custom_event+call pattern |

---

## Scorecard

### Timeline

| Phase | Duration |
|-------|----------|
| Scout (CLI, no LLM) | 14.3s |
| Planner (MCP, 3 tools) | 94.0s |
| Pipeline total | 108.3s |
| Builder Phase 1 (BP_ArrowProjectile structure + graphs) | ~8 min |
| Builder Phase 2 (BP_ThirdPersonCharacter structure + graphs) | ~7 min — killed at 900s |
| Auto-continue (NockArrow, FireArrow, EventGraph) | ~6 min |
| **Total wall clock** | **~36 min** |

Previous run (2026-03-08b): ~19 min. This run took nearly 2x as long.

The extra 17 minutes breaks down as:
- Planner phase: +45s over typical (12 get_template calls)
- ApplyHitDamage retries × 3: ~4 min
- StartAim retries × 2 + simplified result: ~3 min
- NockArrow rollback + retry: ~2 min
- 900s kill + restart overhead: ~3 min
- Total retry overhead: ~12 of the extra 17 minutes

### Tool Call Counts

| Tool | Calls | Successes | Failures | Success Rate |
|------|-------|-----------|----------|-------------|
| `blueprint.apply_plan_json` | ~15 | ~10 | ~5 | 67% |
| `blueprint.connect_pins` | ~12 | ~9 | ~3 (+ 3 empty errors) | ~50% |
| `blueprint.add_node` | ~6 | ~3 | ~3 | 50% |
| `blueprint.describe_node_type` | ~5 | ~2 | ~3 | 40% |
| `blueprint.get_template` | ~16 | ~16 | 0 | 100% |
| `blueprint.modify_component` | 1 | 0 | 1 | 0% |
| `blueprint.add_function` | ~8 | ~6 | ~2 (rate limit) | 75% |
| `blueprint.read_graph` / `blueprint.read_blueprint` | ~10 | ~10 | 0 | 100% |
| **Overall (all tools)** | **~73** | **~56** | **~17** | **~77%** |

Previous run (2026-03-08b): 88% overall tool success. This run regressed to ~77%.

### plan_json Success Rate

| Metric | This Run | Previous Run (08b) |
|--------|----------|-------------------|
| Total apply_plan_json attempts | ~15 | ~12 |
| Successes | ~10 | ~10 |
| Failures (rollback + compile) | ~5 | ~2 |
| Success rate | **67%** | **83%** |

The success rate dropped significantly. Three of the five failures were related to the two catalog gaps (SetFieldOfView, K2_AttachToComponent) and one was the ~Normal tilde pin issue.

### Quality Assessment

**BP_ArrowProjectile**: 80% complete. Structure, components, SetupArrow, and EventGraph handling are correct. ApplyHitDamage was simplified (no ImpactNormal usage) — functional but loses damage direction information. modify_component for bNotifyRigidBodyCollision was silently skipped.

**BP_ThirdPersonCharacter**: 60% complete. EquipBow, UnequipBow, ReleaseAim, DestroyArrow, NockArrow, FireArrow all built correctly. **StartAim is the critical failure** — supposed to: set camera FOV, adjust movement speed, enable strafe rotation. Delivered: check bBowEquipped, branch, set bIsAiming=true, print. The camera and movement system integration is completely missing. The EventGraph InputKey wiring is structurally correct.

**Overall quality: REGRESSION vs previous run.** The previous run (08b) had StartAim with partial FOV logic. This run dropped it entirely.

---

## Root Cause Analysis

### Primary Regression: Catalog Coverage Gaps

The verify-first directive is architecturally correct but operationally toothless when the node catalog doesn't have the requested node type. `SetFieldOfView` and `K2_AttachToComponent` are both common Blueprint nodes. The Builder had no alternative after `describe_node_type` failed — it had to guess, and it guessed wrong twice.

This is the most fixable problem. The node catalog needs entries for at minimum:
- `K2_AttachToComponent` / `AttachToComponent` (SceneComponent.h)
- `SetFieldOfView` (CameraComponent.h) — or more accurately, this is `UCameraComponent::SetFieldOfView`
- `K2_AttachComponentToComponent`
- `SetRelativeLocation`, `SetWorldLocation`, `SetRelativeRotation` (commonly misresolved)

### Secondary Regression: ~Normal Tilde Pin Name

BreakHitResult exposes `ImpactNormal` but the internal pin name in the Blueprint graph is `~Normal`. This is a UE internal quirk. The plan wirer does exact match + normalized match (spaces→underscores, lowercase) but neither matches `~Normal` from `ImpactNormal`. The catalog should include BreakHitResult with correct pin names, OR the data wirer should be updated to strip/handle the tilde prefix.

### Tertiary Regression: 900s Budget Too Short

Two complex Blueprints with retry overhead cannot complete in 15 minutes. The Scout+Planner take 108s of that. With 5 failed plan attempts at roughly 30–60s each, the Builder has ~600s of effective budget for actual building — which covers about 10–12 successful plan_json calls. This run needed ~20.

Options: raise the limit to 1500–1800s, or decompose complex tasks so the pipeline runs separately per Blueprint.

### connect_pins Empty Error (Ongoing)

Three `connect_pins` calls returned empty error strings (lines 2694, 2713, 2729). This bug was documented in the previous run analysis. It has not been fixed. The Builder had no diagnostic information and abandoned those wiring attempts.

### Continuation Prompt Sparsity

The 2892-character continuation prompt is insufficient for a complex resume. It doesn't tell the Builder what was completed, what was degraded, or what remains. The Builder restarted blind and happened to find the right next steps via read_blueprint calls, but it never went back to fix StartAim.

The continuation prompt should include: (1) a list of completed functions with status, (2) a specific note on StartAim's degraded state and what was supposed to be there, (3) the remaining work list.

---

## Comparison to Previous Run (2026-03-08b)

| Metric | 2026-03-08b | 2026-03-08c | Delta |
|--------|-------------|-------------|-------|
| Total wall clock | ~19 min | ~36 min | +17 min (-89%) |
| Tool success rate | 88% | ~77% | -11pp |
| plan_json success rate | 67% | 67% | 0 |
| Auto-continues used | 0 | 1 | +1 |
| Blueprints fully complete | 2 (partial) | 1.6 (partial) | -0.4 |
| StartAim quality | Partial (FOV partially attempted) | Critical regression (FOV/movement dropped) | Worse |
| Fix B triggers | 0 | 2 | +2 (working) |
| Fix A observable | N/A | Cannot confirm | — |
| 900s timeout hit | No | Yes | Regression |

The previous run (08b) was documented as "88% tool success, 67% plan_json success." This run is 77% tool success and 67% plan_json success. The plan_json rate is identical; the tool success rate dropped because of the additional `describe_node_type` failures, `connect_pins` empty errors, `add_node` rejections, and the `modify_component` failure.

---

## Recommendations

1. **Expand node catalog coverage immediately.** The two verify-first failures (SetFieldOfView, K2_AttachToComponent) caused the primary quality regression. Add at minimum: all SceneComponent attachment nodes, CameraComponent setter nodes, and BreakHitResult with correct pin names including the tilde-prefixed ones. The catalog is the directive's only enforcement mechanism.

2. **Fix the ~Normal tilde pin matching in the data wirer.** BreakHitResult's `ImpactNormal` output pin has the internal name `~Normal`. The plan wirer should normalize `~` prefix out of pin names when doing matching, or the catalog entry for BreakHitResult should document the real pin names the AI should use in plans.

3. **Fix the `connect_pins` empty error bug.** This was documented in 08b and is still broken. Three more empty-error failures in this run. The diagnostic path is not surfacing the failure reason. This is a low-trust signal for the Builder — silent failures with no reason force it to abandon approaches blindly.

4. **Raise the 900s total runtime limit or make it task-scale-aware.** A 2-Blueprint task with a 108s pipeline overhead and retry budget cannot complete in 900s. Set to 1800s for tasks identified as multi-Blueprint, or let the Planner report expected complexity so the limit scales.

5. **Enrich the auto-continue prompt.** The 2892-char continuation prompt is blind restart. It should include: completed-functions list with quality flags (degraded/skipped), degraded-function details (what was planned vs what was built), and explicit next-steps. The Builder restarted and made reasonable choices but skipped the critical StartAim repair because nothing told it StartAim was broken.

6. **Add Fix A logging.** Cannot verify Fix A worked or was triggered. Add a single `UE_LOG(LogOlivePlanResolver, Verbose, "Alias map skipped on retry for step '%s'", ...)` line so the next test run can confirm.

7. **Investigate `bNotifyRigidBodyCollision` property access.** `blueprint.modify_component` failed silently. The property likely exists on `UPrimitiveComponent` but may not be reflected under that exact name or may require a different access path (e.g., it's part of a `FBodyInstance` struct, not a direct UPROPERTY on SphereComponent). Determine the correct access pattern and update the tool or add to the alias map.

8. **Consider catalog-miss fallback in the Builder prompt.** When `describe_node_type` returns empty/not-found, the Builder currently guesses. The system prompt should explicitly instruct: "If describe_node_type fails, use `project.search` with the function name to find which class owns it before attempting plan_json." This adds one extra tool call but avoids the guess-wrong → 3-retry-then-simplify pattern.
