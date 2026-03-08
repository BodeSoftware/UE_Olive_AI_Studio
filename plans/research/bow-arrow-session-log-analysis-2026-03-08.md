# Research: Bow and Arrow Session Log Analysis (2026-03-08)

## Question
Comprehensive analysis of a complete agent pipeline run that built a bow and arrow system, covering timeline, tool scorecard, all failures, auto-continue, and Reviewer outcome.

## Session Overview

**Task:** "create a bow and arrow system for @BP_ThirdPersonCharacter"
**Session start (user message):** `2026.03.08-00.14.46` (19:14:46 local, same date)
**Session end (Brain idle):** `2026.03.08-00.40.08`
**Total wall clock:** ~25m 22s

**Assets produced:**
- `/Game/BowSystem/BP_Arrow` (new Actor Blueprint)
  - Components: `ArrowMesh` (StaticMeshComponent), `CollisionSphere` (SphereComponent), `ProjectileMovement` (ProjectileMovementComponent)
  - Variables: `Damage` (float=25), `bHasHit` (bool=false), `InstigatorRef` (Actor ref)
  - Functions: `InitializeArrow(Instigator, LaunchVelocity)`, `DestroyAfterDelay` (custom event)
- `/Game/BowSystem/BP_Bow` (new Actor Blueprint)
  - Components: `BowMesh` (StaticMeshComponent)
  - Variables (from Reviewer read at end): 2 variables, 1 component
  - Functions: `GetArrowSpawnTransform` (pure, returns Transform)
- `/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter` (existing, modified)
  - Variables added: `BowActor` (Actor ref), `bIsAiming` (bool), `bArrowLoaded` (bool), `ArrowSpeed` (float=3000), `ArrowCount` (int=20), `DefaultMaxWalkSpeed` (float=500)
  - Functions added: `EquipBow`, `StartAim`, `StopAim`, `FireArrow`
  - EventGraph: BeginPlay chain (EquipBow → set ArrowCount/ArrowSpeed), InputKey (RMB/LMB) nodes, custom events (OnAimStart, OnAimStop, OnFirePressed)

---

## 1. Timeline

| Stage | Start | End | Duration |
|---|---|---|---|
| **Engine boot + build** | `00.13.12` | `00.13.54` | ~42s |
| **Plugin startup** | `00.13.52` | `00.13.54` | ~2s |
| **User message received** | `00.14.46` | — | — |
| **Pipeline start** | `00.14.46:915` | — | — |
| **Scout (discovery queries)** | `00.14.46:915` | `00.14.56:372` | **9.46s** (5 community BP searches) |
| **Scout (full, incl. templates)** | `00.14.46:915` | `00.15.01:092` | **14.2s** |
| **Planner (MCP, PID=31140)** | `00.15.01:473` | `00.16.25:590` | **84.5s** |
| **ComponentAPIMap build** | (inside Planner end) | `00.16.25:590` | resolved 3/3 classes, 2134 chars |
| **Pipeline validation** | `00.16.25:590` | `00.16.25:593` | **0.003s** |
| **CLI pipeline complete** | — | `00.16.25:593` | **98.7s total** |
| **Builder (CLI process 1)** | `00.16.25` (inferred) | `00.31.26:038` (timeout) | **~15m 0s** |
| **Builder (CLI process 2, relaunch)** | `00.31.26:259` | `00.39.17:636` | **~7m 51s** |
| **Builder total** | — | — | **~22m 51s** |
| **Reviewer** | `00.39.17:637` | `00.40.08:755` | **51.1s** |
| **Brain → Idle** | `00.40.08:755` | — | — |

---

## 2. Scout Phase

**Line 1776:** CLI-only mode detected, 2-call pipeline (Scout + Planner).

**Discovery queries (community blueprints DB):** 5 searches via `olive.search_community_blueprints`
- Line 1797: first query, 71.25ms
- Line 1810: second, 260.28ms
- Line 1811: third, 164.89ms
- Line 1812: fourth, 89.13ms
- Line 1813: fifth, 80.75ms
- Total discovery: 8 entries found (line 1814)

**JSON warnings (lines 1783-1793, 1798-1809):** `parent_class` and `function_count` fields not found in community blueprint search results — these are null fields in the DB schema. Not a failure, just schema gaps in the community blueprint data.

**Template results (line 1815):**
- 3 template overviews loaded
- 24,701 chars of template overview data
- 3 template references

**Full Scout result (line 1816):** 10 assets discovered, 1415 chars discovery text, 24,701 chars template overviews, 3 refs, 14.2s.

---

## 3. Planner Phase

**Launch:** Line 1820 — 8,142 char prompt, max 15 turns, PID=31140.
**Tool filter:** 3 prefixes only (line 1827: returning 3/85 tools). This means the Planner had a highly restricted tool view.

**Planner tool calls — all `blueprint.get_template`:**

| # | Time | template_id | pattern arg | Result |
|---|---|---|---|---|
| 1 | `00.15.11` | `combatfs_ranged_component` | (none, full) | SUCCESS |
| 2 | `00.15.12` | `combatfs_combat_status_component` | (none, full) | SUCCESS |
| 3 | `00.15.18` | `combatfs_ranged_component` | `CreateArrow` | SUCCESS |
| 4 | `00.15.18` | `combatfs_ranged_component` | `AimSetup` | SUCCESS |
| 5 | `00.15.19` | `combatfs_ranged_component` | `LaunchConditions` | SUCCESS |
| 6 | `00.15.20` | `combatfs_ranged_component` | `SetArrowVelocity` | SUCCESS |
| 7 | `00.15.20` | `combatfs_ranged_component` | `DestroyArrow` | SUCCESS |
| 8 | `00.15.21` | `combatfs_ranged_component` | `GetVectorInfrontOfCamera` | SUCCESS |
| 9 | `00.15.27` | `combatfs_ranged_component` | `FireArrowGroup` | SUCCESS |
| 10 | `00.15.27` | `combatfs_ranged_component` | `ResetAllVariables` | SUCCESS |
| 11 | `00.15.28` | `combatfs_combat_status_component` | `DrawWeaponSetup` | SUCCESS |

**Total Planner tool calls: 11** — all successful, all `blueprint.get_template`.

**ComponentAPIMap (line 1876):** Built after Planner finished — 3/3 component classes resolved, 2134 chars.

**Planner output:** 6,617 char plan, 3 assets, 84.5s total.

**Validator:** 0 issues, blocking=no, 0.003s (line 1879).

---

## 4. Builder Phase

### Process Management

**CLI Process 1 (Planner's PID=31140 → Builder launch):**
- Launched with 52/85 tools visible (6 prefixes, line 2597)
- Ran from ~00.16.25 until `00.31.26:038` — **exactly 900 seconds (15 minutes)**
- **Terminated by the runtime timeout limit** (line 2584: "Claude process exceeded total runtime limit (900 seconds) - terminating")

**Auto-continue triggered:** Line 2586: "Run timed out (attempt 1/1) — relaunching with decomposition nudge"

**CLI Process 2 (relaunch):**
- Line 2589: new process launched with `--max-turns 500`
- Line 2592: 2,766 chars stdin prompt
- MCP client re-initialized (line 2594-2596)
- Returned 52/85 tools (line 2597)
- Ran from `00.31.26` until `00.39.17:636` (~7m 51s)
- Exit code 0 (line 2682)
- **50 tool calls logged total** (line 2682)

**Additional Builder tool calls before timeout (in Process 1):**
The process also made 4 more `blueprint.get_template` calls before starting actual writes (lines 1895-1910, at 00.16.39-00.16.40) — it re-fetched 4 templates to refresh its context before building.

---

### Builder Tool Call Scorecard (Processes 1 + 2 combined)

| Tool | Success | Fail | Notes |
|---|---|---|---|
| `blueprint.get_template` | 15 | 0 | 11 in Planner, 4 re-fetches at Builder start |
| `blueprint.create` | 2 | 0 | BP_Arrow, BP_Bow |
| `blueprint.add_component` | 4 | 0 | ArrowMesh, CollisionSphere, ProjectileMovement, BowMesh |
| `blueprint.add_variable` | 9 | 0 | 3 on BP_Arrow, 6 on BP_ThirdPersonCharacter |
| `blueprint.modify_component` | 3 | 0 | ProjectileMovement×2, CollisionSphere |
| `blueprint.add_function` | 7 | 0 | 2 on BP_Arrow, 1 on BP_Bow, 4 on BP_ThirdPersonCharacter |
| `blueprint.apply_plan_json` | 7 | **8** | See failure detail below |
| `blueprint.connect_pins` | 15 | **4** | See failure detail below |
| `blueprint.remove_node` | 11 | **1** | node not found |
| `blueprint.read` | 8 | 0 | graph inspection |
| `blueprint.get_node_pins` | 4 | **1** | node_2 not found |
| `blueprint.add_node` | 7 | **3** | 1× wrong property name, 2× function not found |
| `blueprint.disconnect_pins` | 1 | 0 | |
| `blueprint.compile` | 4 | 0 | All compiled successfully |
| `olive.search_community_blueprints` | 5 | 0 | Scout only |
| **TOTALS** | **102** | **17** | **85.7% success rate** |

---

### Every Failure with Line Number, Error, and Root Cause

#### F1 — `blueprint.apply_plan_json` — line 2091
**Time:** `00.24.24`
**Target:** `BP_Arrow / InitializeArrow`
**Plan:** `set_var InstigatorRef` from `@entry.~Instigator`
**Error (internal):** Data wire FAILED — synthetic get_var for `~Instigator` produced a GetVariable node with no output pins (variable `~Instigator` not found on BP_Arrow)
**Root cause:** The AI used `@entry.~Instigator` to reference a function parameter, but the resolver synthesized a `get_var` step for `~Instigator` (the tilde-prefixed param alias) which then failed to find that variable on the blueprint. The result was a GetVariable node with 0 output pins, so the data wire to InstigatorRef failed. WritePipeline error message was blank (empty error code string).
**Recovery:** Builder read the graph, tried `connect_pins` directly (F2), then got node pins (F3), then removed the ghost node and re-wired.

#### F2 — `blueprint.connect_pins` — line 2110
**Time:** `00.25.01`
**Target:** `BP_Arrow / InitializeArrow / node_0.Instigator -> node_2.InstigatorRef`
**Error:** `BP_CONNECT_PINS_FAILED: Source pin 'Instigator' not found on node 'node_0'`
**Root cause:** node_0 was the GetVariable node left from the failed plan — it exposed no pin named `Instigator` (it's a variable getter, not a function entry node). The AI mis-read the node structure. Node IDs also renumbered after the rollback since node_2 may not have existed.

#### F3 — `blueprint.get_node_pins` — line 2128
**Time:** `00.25.15`
**Target:** `BP_Arrow / InitializeArrow / node_2`
**Error:** FAILED (no message in log, 0.21ms)
**Root cause:** node_2 did not exist. After the plan rollback in F1, the graph had fewer nodes. The AI was probing nodes in order (node_0, node_1, node_2) and node_2 was out of range.

#### F4 — `blueprint.apply_plan_json` — line 2580
**Time:** `00.31.15`
**Target:** `BP_ThirdPersonCharacter / EquipBow` (6-step plan: spawn BP_Bow, store, get mesh, attach)
**Internal error:** Phase 4 data wire FAILED — 1 failed connection (the attachment parent)
**Root cause:** The plan had `@get_mesh.auto` feeding into `K2_AttachToComponent.Parent`, but the GetComponentByClass node returns a `USceneComponent` while `K2_AttachToComponent` expects a typed component. The auto-wire could not reconcile the generic output. Additionally, the executor reported "1 failed" data connection and the WritePipeline triggered a rollback because the failure threshold was exceeded.

#### F5 — `blueprint.apply_plan_json` — line 2616
**Time:** `00.31.45`
**Target:** `BP_ThirdPersonCharacter / StartAim` (3-step plan)
**Error:** Plan resolution failed 2/3 steps — `SetMaxWalkSpeed` not found
**Root cause:** `SetMaxWalkSpeed` is a property-setter on `UCharacterMovementComponent`, not a BlueprintCallable function. It does not appear as a UFunction anywhere in the search chain. FindFunction exhausted all 13+ classes and the universal library scan. The function simply does not exist as a callable node — the property `MaxWalkSpeed` must be set via `set_var` on the component object, not a function call.

#### F6 — `blueprint.apply_plan_json` — line 2632
**Time:** `00.32.08`
**Target:** `BP_ThirdPersonCharacter / StartAim` (retry with `target_class="CharacterMovementComponent"`)
**Error:** Plan resolution failed — same failure, now explicitly specifying target_class
**Root cause:** Same as F5. Even with `target_class="CharacterMovementComponent"` the function is not found because it is not a UFunction/BlueprintCallable. The suggestion to add `target_class` did not help.

#### F7 — `blueprint.apply_plan_json` — line 2701
**Time:** `00.32.27`
**Target:** `BP_ThirdPersonCharacter / StartAim` (4-step plan: switched `call` to `set_var MaxWalkSpeed`)
**Error:** Execution FAILED — Phase 4 data wire FAILED + Phase 5 defaults FAILED
**Internal detail:** `set_var MaxWalkSpeed` created a SetVariable node, but the variable `MaxWalkSpeed` does not exist on `BP_ThirdPersonCharacter`. The resolver warned about this (line 2644) but still resolved the step (warning, not error). The SetVariable node for MaxWalkSpeed had no `Target` input pin (it's not a component accessor), so the data wire for `@get_move.auto -> Target` failed (line 2688). A default value also failed.
**Root cause:** The AI tried to `set_var` a component's property by name as if it were a BP variable. `MaxWalkSpeed` is on `CharacterMovementComponent`, not directly accessible as a BP variable.

#### F8 — `blueprint.apply_plan_json` — line 3138
**Time:** `00.33.59`
**Target:** `BP_ThirdPersonCharacter / FireArrow` (large 15-step plan)
**Error:** Execution FAILED — Phase 4 had 2 failed connections
**Internal details:**
- Wire FAILED: `No input pin matching 'Speed' on step 'init'` — the `InitializeArrow` function call node does not have a pin named `Speed`; actual pins are `Instigator` and `LaunchVelocity` (line 3123)
- A second data wire also failed (line 3126: 9 succeeded, 2 failed)
**Root cause:** The AI's plan referenced a pin called `Speed` on `InitializeArrow`, but the function signature has `LaunchVelocity` (a Vector). This was a pin name mismatch. The large plan (15 steps, 15 connections) was abandoned and the Builder switched to granular `add_node` + `connect_pins` calls.

#### F9 — `blueprint.add_node` — line 3157
**Time:** `00.34.53`
**Target:** `BP_ThirdPersonCharacter / FireArrow`
**Params:** `{"type":"Cast","properties":{"TargetType":"BP_Bow"}}`
**Error:** `BP_ADD_NODE_FAILED: Failed to create node: Cast node requires 'target_class' property`
**Root cause:** The AI used `TargetType` instead of `target_class` as the property name for the Cast node. Correct key is `target_class`.

#### F10 — `blueprint.connect_pins` — line 3225
**Time:** `00.35.43`
**Target:** `BP_ThirdPersonCharacter / FireArrow / node_7.then -> node_32.execute`
**Error:** Execution failed (blank error message)
**Root cause:** node_7 was a `GetComponentByClass` call node, which is a **pure node** (no exec pins). Pure nodes in UE Blueprint have no `then` exec output pin. Connecting `node_7.then` to anything is impossible. The AI later worked around this by using `disconnect_pins`, `remove_node`, and inserting a `Conv_RotatorToVector` node in the exec chain.

#### F11 — `blueprint.connect_pins` — line 2872
**Time:** `00.33.31`
**Target:** `BP_ThirdPersonCharacter / StartAim / node_0.then -> node_5.execute`
**Error:** `BP_CONNECT_PINS_FAILED: Target node 'node_5' not found`
**Root cause:** The AI removed several nodes (node_1 through node_4 in StartAim) and then tried to connect to node_5. After the removals, node IDs had shifted — node_5 did not exist or corresponded to a different node than expected. The AI then re-read the graph and used a fresh approach.

#### F12 — `blueprint.remove_node` — line 2910
**Time:** `00.33.45`
**Target:** `BP_ThirdPersonCharacter / StartAim / node_3`
**Error:** `BP_REMOVE_NODE_FAILED: Node 'node_3' not found in graph 'StartAim'`
**Root cause:** The AI had already removed nodes sequentially (node_1, node_2, node_3). After each removal, the graph's remaining nodes do not get renumbered — but node_3 was already gone by this point. The AI issued one remove too many.

#### F13 — `blueprint.add_node` — line 3516
**Time:** `00.38.11`
**Target:** `BP_ThirdPersonCharacter / EventGraph`
**Params:** `{"type":"CallFunction","properties":{"function_name":"StartAim"}}`
**Error:** FindFunction failed — `StartAim` not found
**Root cause (line 3514):** `StartAim` is a function on `BP_ThirdPersonCharacter` itself, but the `add_node` call did not include the `target_class` / class disambiguation. The universal library search cannot find it because it is on the current Blueprint's generated class, not a library. The Builder should have used `apply_plan_json` with `op="call"` which does resolve Blueprint-local functions via the resolver's FunctionGraph scan, or specified `target_class`.

#### F14 — `blueprint.add_node` — line 3520
**Time:** `00.38.17`
**Target:** `BP_ThirdPersonCharacter / EventGraph`
**Params:** `{"type":"Call Function","properties":{"function_name":"StartAim"}}` (space in type name)
**Error:** FAILED (3.40ms, no message logged)
**Root cause:** The type string "Call Function" (with a space) is not a recognized node type. Correct is `"CallFunction"`. The AI tried a variant after F13 failed.

#### F15 — `blueprint.connect_pins` — line 3419
**Time:** `00.37.48`
**Target:** `BP_ThirdPersonCharacter / EquipBow / node_5.ReturnValue -> node_6.Parent`
**Error:** Execution failed (blank error message)
**Root cause:** `node_5` was a `GetComponentByClass` call returning a `UActorComponent` reference. `node_6` was `K2_AttachToComponent` which expects its `Parent` input to be a `USceneComponent`. The types are incompatible and no autocast exists for this pair. The Builder then used `apply_plan_json` to rebuild the EquipBow graph cleanly (F4's recovery).

---

### Self-Correction Pattern

No explicit `FOliveSelfCorrectionPolicy` cycles were logged (no "self-correct" or "correction" entries). The Builder used its own internal LLM reasoning to retry after each failure — reading graphs, probing node pins, and reformulating plans. The auto-continue at `00.31.26` was triggered by the 900-second runtime timeout, not by a correction policy evaluation.

---

## 5. Reviewer Phase

**Start:** `00.39.17:637` (immediately after Builder exit code 0)
**End:** `00.40.08:755`
**Duration:** 51.1s

**Final asset state read before Reviewer (lines 3683-3688):**
- `BP_ThirdPersonCharacter`: 6 variables, 0 components (component reader returned 0 — likely BP_ThirdPersonCharacter doesn't expose SCS components this way)
- `BP_Arrow`: 7 variables, 2 components (StaticMeshComponent + SphereComponent; ProjectileMovement not counted or ComponentReader filtered it)
- `BP_Bow`: 2 variables, 1 component (BowMesh)

**Result (line 3689):** `Reviewer: SATISFIED, 0 missing, 0 deviations (51.1s)`

**Brain outcome (line 3690):** `outcome=0` (success), state `WorkerActive -> Completed -> Idle`.

---

## 6. Notable Observations

### Template system worked as designed
The Planner made 11 targeted `blueprint.get_template` calls — 2 initial full reads of `combatfs_ranged_component` and `combatfs_combat_status_component`, then 9 pattern-specific fetches for individual functions. The Builder re-fetched 4 of the same functions at the start of Process 1. This is the expected 3-tier flow (catalog → overview → per-function).

### The 900s timeout is the primary bottleneck
The first Builder process ran for exactly 900 seconds before being killed. The total Builder work (both processes) was ~22m 51s for building 3 blueprints with substantial graph logic. This is plausible given the number of retries on `StartAim`/`FireArrow`, but the hard cutoff at 900s forced a relaunch that lost partial context.

### `SetMaxWalkSpeed` is a systemic alias gap (F5, F6, F7)
The AI tried 3 different approaches to change walk speed during aiming (as a function call, as a function call with explicit class, as a set_var). All failed. The Builder eventually gave up on the movement-speed-reduction feature and only set `bIsAiming=true` + `bArrowLoaded=true` in StartAim (lines 2702-2758). This means the StartAim behavior is incomplete relative to the original plan intent — but the Reviewer still reported SATISFIED.

### Empty error messages on `connect_pins` failures (F10, F15)
Two `connect_pins` failures produced blank error codes in the pipeline log (`Error: Execution failed for tool 'blueprint.connect_pins' ():`). The wiring diagnostic system has a missing case for pin type incompatibility when `TryCreateConnection` returns a non-MAKE response code other than the explicit ones checked.

### Node ID instability after removals (F11, F12)
The Builder repeatedly confused itself by removing nodes and then referencing stale node IDs. After each `remove_node`, the remaining IDs do not change, but the AI's mental model of which nodes exist became inconsistent. The pattern of "read graph → remove a batch of nodes → try to connect using IDs from the pre-removal read" is fragile.

### `olive.search_community_blueprints` JSON schema warnings
The community blueprints DB has null `parent_class` and `function_count` fields in some entries. The Scout's deserialization emits LogJson warnings for each null field. This is noise — not a failure — but the repeated warnings (12 total) indicate the DB schema is partially populated.

---

## Recommendations

1. **`SetMaxWalkSpeed` alias gap.** Add `SetMaxWalkSpeed` → property access via component to the alias map or add resolver intelligence to detect component property setters. Currently the AI tries 3+ approaches and fails all. The correct Blueprint node is a "Set Max Walk Speed" setter that UE auto-generates as a property accessor on `CharacterMovementComponent`. The FindFunction search should also scan `TFieldIterator<FProperty>` on component classes for settable properties, not just UFunctions.

2. **Empty `connect_pins` error messages.** When `TryCreateConnection` returns `CONNECT_RESPONSE_DISALLOW`, the wiring diagnostic has no handler and the pipeline logs a blank error code. The error string should always contain the reason code. Investigate `OlivePinConnector`'s error path when `CanSafeConnect` passes but `TryCreateConnection` still fails.

3. **900s timeout is too tight for 3-asset builds.** The first Builder process was terminated at exactly 900 seconds with significant work remaining (only BP_Arrow and BP_Bow were complete; BP_ThirdPersonCharacter was partially done). Consider increasing `MaxRuntime` for multi-asset plans above a complexity threshold, or allow the pipeline to detect plan scope and scale the timeout.

4. **Node ID stability guidance for the Builder.** After bulk `remove_node` calls, the Builder should re-read the graph before making connections based on node IDs. Consider adding a warning to `remove_node` results that says "graph was modified — re-read recommended before further edits."

5. **`@entry.~Param` alias in plan_json (F1).** The resolver synthesizes a `get_var` step for tilde-prefixed params which then fails when the variable doesn't exist by that name. The `~Instigator` param alias needs to generate a FunctionEntry output pin reference, not a GetVariable node. This was a resolver bug that caused a cascade of downstream failures.

6. **`add_node` with `type="CallFunction"` for Blueprint-local functions.** The `add_node` tool does not search the Blueprint's own FunctionGraphs, only the FindFunction chain. When `type="CallFunction"` is used for a self-function, `target_class` must be specified or the tool fails. The error message should say "For blueprint-local functions, use `apply_plan_json` with op='call', or specify `target_class`." Also, `"Call Function"` (with space) silently fails with no error message — add a normalization step.

7. **Reviewer duration (51s) is long.** The Reviewer read 3 Blueprint summaries and took 51 seconds. Most of that is LLM inference time. This is acceptable for a satisfied result but should be noted as a fixed overhead per run regardless of build size.

Sources: Internal log file at `docs/logs/UE_Olive_AI_Toolkit.log` (3791 lines), session 2026.03.08.
