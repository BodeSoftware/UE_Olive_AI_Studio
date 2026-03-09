# Research: Bow & Arrow Session Log Analysis — 2026-03-09m

## Question
Analyze the first post-pipeline-revert run. Is there any pipeline activity? What is the total time, plan_json rate, and tool success rate compared to prior runs?

---

## Findings

### 1. What Was the Task?

Task: "create a bow and arrow system for @BP_ThirdPersonCharacter"

Assets created/modified:
- `/Game/Blueprints/Weapons/BP_Arrow` (created via `blueprint.create` with template)
- `/Game/Blueprints/Weapons/BP_Bow` (created via `blueprint.create`, built granularly)
- `/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter` (modified — variables, functions, EventGraph wiring)

---

### 2. Pipeline vs Single-Agent — CONFIRMED ABSENT

Zero pipeline log entries found. Searched for:
- `FOliveAgentPipeline` — not found
- `Router`, `Scout`, `Architect`, `Reviewer` — not found (only `Architect` as a false positive in the word "Architecture" in a config log)
- `Build Plan`, `FormatForPromptInjection` — not found

**The pipeline revert is confirmed. No multi-agent pipeline activity occurred.**

---

### 3. Discovery Pass

Discovery pass DID fire — it is part of the CLI provider pre-launch step, not the multi-agent pipeline.

```
[18.43.15] LogOliveAI: Launching autonomous run (Claude Code CLI)
[18.43.15] LogOliveCLIProvider: Injected 10 related assets from keyword search (keywords: bow, arrow, bp_thirdpersoncharacter)
[18.43.26] LogOliveUtilityModel: Discovery pass: 8 entries found, 5 queries, 11.06s elapsed
[18.43.26] LogOliveCLIProvider: Discovery pass: 8 results in 11.1s (LLM=yes, queries: bow weapon; ranged combat component; arrow projectile; aiming trace; ammo inventory)
[18.43.26] LogOliveCLIProvider: Launching autonomous CLI with args: --print --output-format stream-json...
```

Discovery pass: **11.1 seconds**, LLM-powered, 5 queries, 8 results.

The CLI did NOT launch until discovery was complete (18.43.26 vs request at 18.43.15).

---

### 4. Asset Decomposition

The agent did NOT log an explicit "ASSETS:" block. However, its first actions after launch were a burst of research tool calls (list_templates, get_recipe, 4× get_template), then immediately `blueprint.create` for BP_Arrow. This suggests it decomposed internally and proceeded directly to build — no explicit pre-announce of planned assets observed in this log.

---

### 5. Timeline

| Event | Timestamp | Elapsed |
|---|---|---|
| UE editor open (log start) | 14:40:26 | — |
| Plugin initialized, MCP server up | 18:41:09 | — |
| User message received | ~18:43:14 | 0s |
| Auto-snapshot created | 18:43:15 | 1s |
| Discovery pass completes | 18:43:26 | +11.1s |
| CLI agent launches | 18:43:26 | +11.1s |
| First tool call (blueprint.list_templates) | 18:43:44 | +29s from user message |
| Last tool call (blueprint.compile) | 18:52:57 | — |
| Autonomous run complete | 18:53:17 | — |
| **Total agent runtime** | 18:43:14–18:53:17 | **~10:03 (603s)** |
| **Discovery + CLI total** | +11.1s discovery + ~9:29 CLI | **~10:03** |

**Wall-clock breakdown:**
- Discovery pass: 11.1s
- CLI launch delay (discovery complete → first tool): 18s (agent thinking before first tool)
- First tool to last tool: 18:43:44–18:52:57 = ~9:13 of actual tool activity
- Last tool to exit: 20.1s idle (normal graceful exit)
- Total from user message to run complete: **~10:03**

Note: The 18s pre-tool gap is the agent reading its context and planning. In pipeline runs, this was replaced by the Planner agent outputting a pre-plan. In single-agent mode, the agent does this silently in its thinking window.

---

### 6. Tool Call Statistics

Total tool calls (from `executed in` log lines): **73 logged**, exit says "50 tool calls logged" (the exit count likely excludes pre-launch discovery tools).

| Tool | Count | Successes | Failures |
|---|---|---|---|
| blueprint.apply_plan_json | 15 | 9 | 6 |
| blueprint.remove_node | 21 | 20 | 1 |
| blueprint.add_node | 3 | 1 | 2 |
| blueprint.add_function | 4 | 4 | 0 |
| blueprint.add_variable | 5 | 5 | 0 |
| blueprint.add_component | 3 | 3 | 0 |
| blueprint.compile | 3 | 3 | 0 |
| blueprint.create | 2 | 2 | 0 |
| blueprint.reparent_component | 2 | 2 | 0 |
| blueprint.read | 3 | 3 | 0 |
| blueprint.connect_pins | 1 | 1 | 0 |
| blueprint.disconnect_pins | 2 | 0 | 2 |
| blueprint.get_node_pins | 2 | 2 | 0 |
| blueprint.list_templates | 1 | 1 | 0 |
| blueprint.get_template | 4 | 4 | 0 |
| olive.get_recipe | 1 | 1 | 0 |
| olive.search_community_blueprints | 5 | 5 | 0 |
| **Total** | **81** | **70** | **11** |

**Tool success rate: 70/81 = 86.4%**

Failure breakdown:
- apply_plan_json: 6 failures (dominant)
- remove_node: 1 failure (stale node_id after removals)
- add_node: 2 failures (wrong node type string)
- disconnect_pins: 2 failures

---

### 7. plan_json Success Rate

Total apply_plan_json calls: **15**
Successes: **9**
Failures: **6**

**plan_json success rate: 9/15 = 60.0%**

Calls by asset:
| Asset | Graph | Result |
|---|---|---|
| BP_Arrow | EventGraph | SUCCESS (OnComponentBeginOverlap + SetLifeSpan) |
| BP_Arrow | EventGraph | SUCCESS (BeginPlay → SetLifeSpan) |
| BP_Bow | Fire | FAIL (set_var return value wiring) |
| BP_Bow | Fire | SUCCESS (retry, removed return output path) |
| BP_Bow | ResetCanFire | SUCCESS |
| BP_ThirdPersonCharacter | EventGraph | FAIL (Phase 0: COMPONENT_FUNCTION_ON_ACTOR — AttachToComponent without Target) |
| BP_ThirdPersonCharacter | EventGraph | FAIL (data wire failed: 1 connection — attach data pin) |
| BP_ThirdPersonCharacter | EventGraph | FAIL (bOrphanedPin: TypesIncompatible on BeginPlay exec) |
| BP_ThirdPersonCharacter | SetupBow | SUCCESS (moved spawn logic to dedicated function) |
| BP_ThirdPersonCharacter | FireBow | FAIL (GetForwardVector 'InRot' pin not found — alias rerouted but node lacks that pin) |
| BP_ThirdPersonCharacter | FireBow | SUCCESS (retry: removed do_once/is_valid op confusion, cleaner plan) |
| BP_ThirdPersonCharacter | EventGraph | FAIL (IA_Shoot not found — asset does not exist) |
| BP_ThirdPersonCharacter | EventGraph | SUCCESS (pivoted to BeginPlay → SetupBow call) |
| BP_ThirdPersonCharacter | EventGraph | SUCCESS (custom_event CE_FireBow → FireBow call) |
| BP_ThirdPersonCharacter | EventGraph | SUCCESS (final connect_pins + compile) |

---

### 8. ALL plan_json Failures — Root Cause Analysis

**Failure 1 — BP_Bow/Fire (18:45:28)**
- Error: Two `set_var` steps targeting function output `bSuccess` used as exec source and data output. `FunctionOutput` nodes have no exec output pin and no data output pin — they are write-only.
- Root cause: Agent modeled a function with a bool return value and tried to wire `set_var bSuccess` → `ret_ok` exec chain, then use `@init_fail.auto` / `@set_ok.auto` as bool outputs. `set_var` resolves to `FunctionOutput` node which has no output pins.
- Fix: Agent on retry removed the `bSuccess` return paths and simplified to a single spawn path.

**Failure 2 — BP_ThirdPersonCharacter/EventGraph, attempt 1 (18:46:31)**
- Error: Phase 0 `COMPONENT_FUNCTION_ON_ACTOR` — `K2_AttachToComponent` (SceneComponent method) called on Actor BP with no Target wired, 0 unambiguous matches found.
- Root cause: Agent called AttachToComponent without wiring a target component. The auto-Target injection requires exactly one unambiguous match; 0 components matched the SceneComponent target.
- Fix: Agent on retry added GetRootComponent and cast steps to provide the Target.

**Failure 3 — BP_ThirdPersonCharacter/EventGraph, attempt 2 (18:46:47)**
- Error: Phase 4 data wire failed — 1 connection (unspecified pin mismatch). Phase 5.5: orphaned SpawnActor node.
- Root cause: SpawnActor was correctly exec-chained but one data connection for attach failed. The orphan detection then flagged the SpawnActor as disconnected from exec flow (the attach failed, so SpawnActor had no downstream exec chain connection).
- Fix: Agent added a `cast` step for the SkeletalMesh and restructured.

**Failure 4 — BP_ThirdPersonCharacter/EventGraph, attempt 3 (18:46:56)**
- Error: Phase 3 exec wire failed: "begin.then -> spawn_bow.execute: TypesIncompatible" — bOrphanedPin on BeginPlay exec out pin.
- Root cause: The persistent orphaned exec pin bug. BeginPlay's `then` pin had `bOrphanedPin=true` from a prior rollback. The wiring diagnostic misfires as TypesIncompatible.
- Note: The graph had 25 nodes / 49 connections when the agent read it at 18:47:32, indicating multiple prior rollback cycles had left detritus that accumulated.
- Fix: Agent pivoted — moved spawn logic to a new `SetupBow` function graph, bypassing the polluted EventGraph.

**Failure 5 — BP_ThirdPersonCharacter/FireBow, attempt 1 (18:49:39)**
- Error: Data wire failed: no input pin 'InRot' on step 'fwd_vec' (GetForwardVector / GetActorForwardVector). Available pins: only 'self' (Actor Object Reference).
- Root cause: Alias reroute conflict. The agent used `GetForwardVector` with an `InRot` input, intending `KismetMathLibrary::GetForwardVector(FRotator InRot)`. The resolver correctly detected the alias-fallback at resolve-time (`ResolveCallOp: Alias fallback succeeded -- rerouted to KismetMathLibrary::GetForwardVector`). However, the **node factory** at execution time still used `GetActorForwardVector` (the primary alias target), not the library version. So the node had `self` (Actor) as its only pin, not `InRot` (Rotator). Resolver and executor are out of sync on the alias fallback path.
- Fix: Agent on retry dropped `GetForwardVector`/InRot usage and restructured without it, using GetControlRotation result directly in MakeTransform.

**Failure 6 — BP_ThirdPersonCharacter/EventGraph, IA_Shoot (18:49:57)**
- Error: Phase 1 FAIL — `Enhanced Input Action 'IA_Shoot' not found. Available: [IA_Jump, IA_Look, IA_Move]`.
- Root cause: Asset does not exist. The agent attempted to wire `IA_Shoot` but the project only has IA_Jump/Look/Move. This is expected — the bow system needs a new input action that wasn't created.
- Fix: Agent pivoted to InputKey node (LeftMouseButton) + custom_event approach, successfully.

---

### 9. describe_function / describe_node_type

Zero calls to `blueprint.describe_function` or `blueprint.describe_node_type` in the entire run. The agent relied entirely on its built-in knowledge, templates, and error feedback for node resolution.

---

### 10. Template Tool Usage

The agent used template tools aggressively and correctly in the **first 8 minutes**:

| Time | Tool | Query / ID |
|---|---|---|
| 18:43:44 | blueprint.list_templates | "bow arrow projectile ranged" |
| 18:43:45 | olive.get_recipe | "spawn projectile actor movement" |
| 18:43:50 | blueprint.get_template | "projectile" |
| 18:43:50 | blueprint.get_template | "projectile_patterns" |
| 18:43:51 | blueprint.get_template | "gun" |
| 18:43:51 | blueprint.get_template | "combatfs_ranged_component" (pattern="CreateArrow") |
| 18:44:12 | blueprint.create | ...template_id="projectile", preset="Arrow" |

The agent:
- Listed templates with a relevant query first
- Retrieved 4 templates in parallel before building
- Immediately applied the factory template to create BP_Arrow with correct components (CollisionSphere, ArrowMesh, ProjectileMovement + ArrowDamage variable)
- No template calls after the initial burst — the agent used them for planning, then worked from its plan

---

### 11. Compile Results

All final compilations were SUCCESS.

| Blueprint | Final Compile Result |
|---|---|
| BP_Arrow (factory template) | SUCCESS (auto-compiled by template apply) |
| BP_Arrow (plan_json) | SUCCESS |
| BP_Bow | SUCCESS (explicit compile at 18:45:11) |
| BP_ThirdPersonCharacter SetupBow | SUCCESS |
| BP_ThirdPersonCharacter FireBow | SUCCESS |
| BP_ThirdPersonCharacter EventGraph | SUCCESS (18:52:27) |
| BP_ThirdPersonCharacter final | SUCCESS (18:52:57, Errors: 0, Warnings: 0) |

12 total compilation events, all SUCCESS. Zero compile failures.

---

### 12. Self-Correction / Error Recovery

The agent's error handling was methodical and notably good compared to pipeline runs:

**BP_Bow/Fire failures (1 retry):**
- Recognized that `set_var` targeting a return value cannot be used as an exec source or data output
- Simplified plan: dropped the dual-return-path structure, got it right on attempt 2

**BP_ThirdPersonCharacter/EventGraph failures (3 retries, then pivot):**
- Attempt 1: Phase 0 caught missing Target — agent added GetRootComponent
- Attempt 2: Data wire failed — agent added cast step for SkeletalMesh
- Attempt 3: bOrphanedPin TypesIncompatible — agent read the graph (25 nodes, 49 connections), saw the mess, made a strategic decision to pivot: created a `SetupBow` function, moved all spawn/attach logic there. This was the correct architectural move. The EventGraph stayed clean.
- Did NOT do the destructive node-wipe behavior seen in run 09j. Instead pivoted to a function decomposition strategy.

**BP_ThirdPersonCharacter/FireBow failures (1 retry):**
- GetForwardVector 'InRot' pin mismatch — agent dropped that step and restructured without it
- Clean recovery in one attempt

**IA_Shoot missing:**
- Tried `IA_Shoot`, got clear error with available list
- Did NOT create the asset (would require a new IA)
- Pivoted to InputKey (LeftMouseButton) + custom_event approach
- Then connected InputKey.Pressed → FireBow via connect_pins

**remove_node phase (18:50:51–18:52:09):**
- Agent removed ~18 nodes from EventGraph (cleanup of the orphaned/failed prior attempts)
- One `remove_node` failed (node_6 not found — already removed or wrong ID)
- Then proceeded cleanly with a simplified plan

---

### 13. Comparison Table

| Run | Architecture | Total Time | plan_json Rate | Tool Success Rate | Notes |
|---|---|---|---|---|---|
| 08i | Single-agent (no pipeline) | 14:55 | 80% (pre-pipeline best) | 96.6% | Best historical single-agent |
| 09j | Multi-agent pipeline | 23:28 | 59.3% | 89.7% | Pipeline worst — node-wipe, regression |
| 09k | Multi-agent pipeline | 14:53 | 64.3% | 82.4% | Pipeline, CLI-only path |
| 09l | Multi-agent pipeline | 12:22 | 71.4% | 90.0% | Pipeline best |
| **09m** | **Single-agent (reverted)** | **10:03** | **60.0%** | **86.4%** | **Post-revert first run** |

---

## Recommendations

### Positive Signals

1. **Speed: 10:03 total — the fastest run to date.** Beats both the pipeline best (09l: 12:22) and the pre-pipeline best (08i: 14:55) by significant margins. The single-agent model is faster because it has no inter-agent handoff latency, no Planner call, no Reviewer call.

2. **No pipeline overhead.** The pipeline was adding at minimum ~1:30–2:00 of Planner + Reviewer time even in its best configuration (09l). That overhead is now gone.

3. **Strategic recovery.** The agent pivoted to SetupBow function without prompting, which was architecturally sound. It avoided the 09j node-wipe disaster. This suggests the single-agent has better situational awareness about when to decompose vs. retry.

4. **Template usage excellent.** The agent used list_templates → 4× get_template before building anything. The combatfs_ranged_component library template with pattern="CreateArrow" was fetched — showing the agent correctly used the library. This matches or exceeds pipeline Planner template usage (09l: 10 get_template).

5. **All BPs compile SUCCESS with zero errors.** Output quality is maintained.

### Problem Areas

6. **plan_json rate regression: 60% vs 08i's 80%.** This is the main concern. The 6 failures break down as:
   - 1× FunctionOutput pin confusion (novel failure, not seen before)
   - 1× Phase 0 COMPONENT_FUNCTION_ON_ACTOR (known, recurring)
   - 1× data wire fail + orphaned node (known)
   - 1× bOrphanedPin TypesIncompatible (known, recurring bug)
   - 1× GetForwardVector alias/executor mismatch (novel)
   - 1× IA_Shoot missing asset (expected, not a quality failure)

   Excluding the IA_Shoot missing-asset failure (unavoidable), the real plan_json quality is 9/14 = **64.3%** — slightly better than 09k (64.3%) but still below 08i.

7. **CRITICAL BUG CONFIRMED: GetForwardVector alias fallback mismatch.** Resolver correctly detects that `GetForwardVector(InRot)` should route to `KismetMathLibrary::GetForwardVector`. But the node factory still creates `GetActorForwardVector` (the primary alias). The resolver's fallback decision is lost when it reaches the executor. This causes a guaranteed data wire failure. This was flagged in 09k analysis but the executor still does not honor the fallback result. Needs a fix: resolver must store the fallback-resolved UFunction* and pass it through so the factory uses it.

8. **CRITICAL BUG CONFIRMED: bOrphanedPin TypesIncompatible.** The exec pin `bOrphanedPin=true` bug continues to recur after rollbacks in EventGraphs with existing nodes. Every time a plan_json rolls back mid-execution without calling `Modify()` on the event node first, the orphan state persists and makes subsequent plans fail with misleading errors. The agent's mitigation (pivot to a function graph) works but costs multiple failed attempts and extra time.

9. **remove_node cleanup overhead.** The agent spent ~1:20 (18:50:51–18:52:09) removing 18 orphaned nodes one-by-one via separate tool calls. This is a consequence of multiple plan rollbacks leaving visual clutter even though transactions are cancelled. A `blueprint.clear_orphans` tool or a "clear graph and rebuild" approach would save significant time.

10. **18s pre-tool thinking gap.** After the CLI launches, there is an 18-second gap before the first tool call. In the pipeline, the Planner filled this with a structured plan output. In single-agent, this is silent LLM thinking. Not a problem, but worth noting — the agent appears to be reading a large context before acting.

### Architecture Verdict

The revert to single-agent is validated. The run is faster, output quality is the same (all compile SUCCESS), and the agent's behavior is more coherent (no node-wipe regression, clean function decomposition). The plan_json rate is lower than the pre-pipeline best (08i: 80%) but the comparison may be unfair — 08i was measured before the bOrphanedPin bug was as prevalent, and may have had a simpler test task.

The two critical bugs (GetForwardVector alias executor mismatch, bOrphanedPin after rollback) are the highest-priority fixes for improving plan_json rate.
