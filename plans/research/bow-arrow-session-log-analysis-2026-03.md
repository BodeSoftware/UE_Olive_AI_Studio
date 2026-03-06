# Research: Bow-and-Arrow Session Log Analysis (2026-03-06)

## Question
Analyze the autonomous agent run log from 2026-03-06 for the bow/arrow Blueprint task. Assess discovery,
decomposition, template usage, write operations, error handling, rate limiting, and overall quality.

---

## Findings

### 1. Task Given

Exact user message (from line 1752):
> "create a bow and arrow system that @BP_ThirdPersonCharacter can equip"

Auto-snapshot label (line 1748): `Pre-autonomous: create a bow and arrow system that @BP_ThirdPersonCharacter `

The `@BP_ThirdPersonCharacter` mention triggered pre-injection of that asset's state into the
initial prompt, plus a keyword search for related assets.

---

### 2. Discovery Pass

Discovery ran before the main agent launch, using `olive.search_community_blueprints`.

**First pass (lines 1756–1792):**
- 5 queries, 5 community DB searches, 8 results found, elapsed 13.7s
- LLM generated queries: `bow arrow`, `ranged weapon equip`, `projectile system`,
  `ammo component`, `character weapon socket`
- Queries were coherent and domain-specific — no noise

**Second pass (timeout recovery, lines 2601–2615):**
- 4 queries, elapsed 19.5s, 8 results
- BUT the queries were garbage: `## continuation`, `of previous`, `task`, `### original`,
  `task create bow`
- The continuation context injected into the second discovery pass was raw Markdown from the
  decomposition nudge prompt, not a real user message. The utility model extracted Markdown
  headers as search queries. This is a known failure mode for the continuation path.
- Two of the four queries returned immediate failures (0.2ms, 0.16ms) — likely exact-match
  misses on non-word tokens.

**`LogJson` warnings during discovery (lines 1756–1784):**
`Field parent_class was not found` and `Field function_count was not found` printed for every
community DB result. This is a benign schema mismatch (community DB schema does not match the
template loader's expected fields) but it's noisy. Appears 6+ times per run.

---

### 3. Project Asset Injection

Line 1755:
> `Injected 10 related assets from keyword search (keywords: bow, arrow, bp_thirdpersoncharacter, can, equip)`

Keywords extracted from the user message. Ten assets from the project index were injected into
the initial context. No detail is logged on which assets these were, but the keywords are good —
`bow`, `arrow`, and the mentioned character were all extracted.

Second run injection (line 2605):
> `Injected 10 related assets from keyword search (keywords: continuation, previous, task, original, task\ncreate)`

Garbage keywords again from the continuation nudge — same problem as the discovery queries above.

---

### 4. Asset Decomposition

The agent chose to create three Blueprints:
1. `/Game/BowAndArrow/BP_Arrow` — projectile actor (created at 13:40:47)
2. `/Game/BowAndArrow/BP_Bow` — weapon actor (created at 13:52:00)
3. Modified `/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter` — equip integration

No Olive decomposition system was used (this is not the multi-asset decomposition tool); the agent
decided on the structure independently based on the task prompt and template research.

The decomposition was sensible: Arrow as a self-contained projectile, Bow as the holder/firer,
Character as the integration point.

---

### 5. Template Usage

**`blueprint.get_template` was called three times (lines 1808–1819), all on library templates:**

| Call | Template ID | Pattern |
|------|-------------|---------|
| 1 | `combatfs_ranged_component` | `CreateArrow` |
| 2 | `combatfs_arrow_component` | `ArrowStuckInWall` |
| 3 | `combatfs_combat_status_component` | `AttachWeaponToSocket` |

All three were called immediately after the first `tools/list` in run 1 (within ~1 second of each
other, at 13:39:03–13:39:04). The agent read all three in parallel before doing any writes — this
is excellent pre-read behavior.

`blueprint.list_templates` was NOT called. The agent went directly to `get_template` by ID,
which means it recognized the relevant templates from the catalog block already in its system
prompt.

**What the agent learned from templates:**
- `combatfs_arrow_component.ArrowStuckInWall` — collision handling, stopping movement, the
  "bIsStuck" guard pattern
- `combatfs_ranged_component.CreateArrow` — SpawnActor + damage injection pattern
- `combatfs_combat_status_component.AttachWeaponToSocket` — attach-to-socket flow

This is clearly visible in the resulting plan_json calls: `bIsStuck` variable, `CollisionSphere`
hit event, `StopMovementImmediately`, `ApplyDamage`, and the `hand_r` socket attach all mirror
what the templates showed. Template usage was direct and effective.

---

### 6. Write Operations Summary

**BP_Arrow (13:40:47 – 13:51:02):**
- `blueprint.create` — Actor parent
- `blueprint.add_component` x3 — StaticMeshComponent (ArrowMesh), SphereComponent (CollisionSphere),
  ProjectileMovementComponent (ProjectileMovement)
- `blueprint.add_variable` x3 — Damage (float, 25.0), bIsStuck (bool, false), then Damage again
  with expose_on_spawn=true (upsert)
- `blueprint.add_function` — SetDamage(NewDamage) -> bSuccess
- `blueprint.modify_component` x2 — ProjectileMovement (InitialSpeed=3000, MaxSpeed=3000,
  GravityScale=0.5, bRotationFollowsVelocity), CollisionSphere (SphereRadius=8, NotifyRigidBodyCollision=true)
- `blueprint.compile` — success at 13:44:23
- Granular node + pin wiring for SetDamage function (troubled — see Section 8)
- `blueprint.apply_plan_json` for SetDamage graph — success after 3 failed attempts
- `blueprint.apply_plan_json` for EventGraph (OnComponentHit logic) — success on second attempt
  (first rejected by Phase 0 COMPONENT_FUNCTION_ON_ACTOR validator)
- `blueprint.compile` — success at 13:45:26

**BP_Bow (13:52:00 – 13:53:23):**
- `blueprint.create` — Actor parent
- `blueprint.add_component` x2 — StaticMeshComponent (BowMesh), ArrowComponent (ArrowSpawnPoint)
- `blueprint.add_variable` x4 — Damage (float), bIsDrawing (bool), DrawStartTime (float),
  MaxFullDrawTime (float, 1.5, edit_anywhere), OwnerCharacter (Character*)
- `blueprint.add_function` x2 — StartDraw() -> bSuccess, FireArrow() -> bSuccess
- `blueprint.apply_plan_json` for StartDraw — success (4 steps: set bIsDrawing, GetGameTimeSinceCreation,
  set DrawStartTime, return)
- `blueprint.set_pin_default` — set FunctionResult.bSuccess = true
- `blueprint.apply_plan_json` for FireArrow — FAILED on last attempt (1 data wire failure: 'Damage'
  not found as a SpawnActor pin)

**BP_ThirdPersonCharacter integration (13:56:50 – 14:02:43, run 2):**
- `blueprint.read` x3 — summary, components, variables (all reading 0 components/variables — suspicious)
- `blueprint.read` x2 — BP_Bow graphs StartDraw and FireArrow (to review what was built)
- `blueprint.read` — BP_ThirdPersonCharacter EventGraph (3 nodes, pre-edit state)
- `blueprint.add_variable` — EquippedBow (BP_Bow_C*, category Bow)
- `blueprint.apply_plan_json` x2 — both failed (wrong Mesh ref, then orphaned spawn node)
- Manual granular repair: `blueprint.remove_node` x4, `blueprint.add_node` (Cast to SkeletalMesh),
  `blueprint.connect_pins` x5, `blueprint.disconnect_pins` x2
- `blueprint.apply_plan_json` x2 — CE_StartDraw custom event + IsValid + cast + call StartDraw (success),
  CE_FireArrow custom event + cast + call FireArrow (success)
- `blueprint.add_node` x3 — InputKey (RightMouseButton), K2Node_CallFunction (CE_StartDraw),
  K2Node_CallFunction (CE_FireArrow)
- `blueprint.connect_pins` x2 — Pressed/Released to CE calls
- `blueprint.compile` — success at 14:02:43

**Total tool calls (run 2 final summary line 3314):** 39 tool calls logged in the second run.
First run had approximately 60+ calls based on log range. Combined session: ~100 tool calls.

---

### 7. Rate Limiting

**No RATE_LIMITED errors occurred.** No rate limit mentions anywhere in the log. The agent
ran at Claude Code's natural pace — individual tool calls take 0.3ms–30ms on the plugin side.
Large gaps between tool calls are thinking time on the LLM side, not throttling.

---

### 8. Errors and Failures

#### SetDamage function wiring — persistent connect_pins failure (lines 1961–2011)

Three consecutive failures of `blueprint.connect_pins` for `node_0.then -> node_2.execute`
in the SetDamage function graph:
- Attempt 1 (13:44:54): "Execution failed ()" — blank error message, no code
- Attempt 2 (13:45:15): Same
- Attempt 3 (13:45:47): Same, with `source_ref/target_ref` object format tried instead of string
- Then a different attempt: `node_0.NewDamage -> node_2.Damage` failed with
  `BP_CONNECT_PINS_FAILED: Source pin 'NewDamage' not found on node 'node_0'`

The agent then called `blueprint.get_node_pins` on node_0 and node_2 to discover actual pin
names. node_1 (which was the FunctionEntry node) failed with "FAILED" (node not found). The
agent eventually gave up trying to wire the SetDamage function with granular tools and instead
used `blueprint.apply_plan_json` which succeeded cleanly. The blank error messages on the
exec-pin connect failures are a bug: the write pipeline is returning empty error messages
for some pin connection failures.

**Key insight:** The agent correctly gave up on granular wiring after 3 failures and switched to
plan_json. Good self-recovery behavior.

#### blueprint.remove_function blocked (lines 2041–2049)

The agent tried to remove SetDamage twice (`blueprint.remove_function`) but was blocked because
the function already had 3 nodes of graph logic. Correct safety behavior. The agent eventually
bypassed this by leaving the function in place and just re-wiring it with plan_json.

#### FireArrow data wire failure — SpawnActor Damage pin (line 2573)

The plan tried to wire `calc_dmg.ReturnValue -> spawn_arrow.Damage` but `SpawnActor` has no
`Damage` pin (SpawnActor only exposes class-agnostic pins: Class, SpawnTransform,
CollisionHandlingOverride, Owner). The damage value needs to be set after spawn via
`arrow->SetDamage(...)`. This is a conceptual error in the plan — the agent was trying to pass
the damage at spawn time via a pin that doesn't exist.

This failure caused the entire FireArrow plan to rollback (1 data wire failure triggers full
rollback per current pipeline behavior). However the graph was still created without the Damage
pass-through — the agent appears to have accepted this and moved on without attempting to fix it.

**Missing fix:** FireArrow in BP_Bow does NOT call `SetDamage` on the spawned arrow. The
computed `calc_dmg` float value is never wired to the arrow. This is a silent functional gap.

#### Phase 0 validator — COMPONENT_FUNCTION_ON_ACTOR (line 2126)

First EventGraph plan for BP_Arrow: `SetCollisionEnabled` on PrimitiveComponent rejected because
no Target wire for the Actor BP. The agent correctly fixed this by adding a `get_sphere` step in
the retry plan (line 2159), which passed Phase 0.

#### apply_plan_json x2 failures on BP_ThirdPersonCharacter BeginPlay (lines 2762–2877)

Attempt 1: `@Mesh` bare ref in `attach.Parent` — failed with "Invalid @ref format: '@Mesh'.
Expected '@stepId.pinHint'". Partial execution left nodes in the graph; transaction rolled back.

Attempt 2: Added `get_mesh` step (GetMesh -> auto-rewrote to GetComponentByClass), but then
`begin.then -> spawn_bow.execute` failed with "Cannot connect Exec to Exec: TypesIncompatible"
— this is strange since both are exec pins. The orphan SpawnActor node was flagged in Phase 5.5.
Again rolled back.

After two apply_plan_json failures, the graph had accumulated 14 nodes from failed+rolled-back
plans plus leftover nodes from the initial snapshot state. The agent pivoted to manual cleanup:
reading the graph, removing stale nodes individually, adding a Cast node manually, then wiring
manually with connect_pins. This eventually succeeded.

#### Silent connect_pins failures (empty error strings)

Multiple occurrences of `Error: Execution failed for tool 'blueprint.connect_pins' ():` with no
error code or message. This is a bug in the write pipeline — it's rolling back but not propagating
the failure reason from the graph writer to the pipeline result. The agent has no useful error
message to act on. Observed at lines 1968, 1982, 2008, 2985.

---

### 9. Asset Switching

No suspicious asset switching detected. The agent worked on BP_Arrow completely, then BP_Bow,
then BP_ThirdPersonCharacter — clean sequential order. No switching mid-work on a broken asset.

The "soft nudge" feature (warning when switching with unresolved errors) did not fire because the
agent did not leave errors unresolved before switching — it compiled both BP_Arrow and BP_Bow
successfully before moving on.

---

### 10. Completion

**What was completed:**

- BP_Arrow: Fully built and compiling. Components, variables, SetDamage function, EventGraph with
  OnComponentHit logic (IsStuck guard, ApplyDamage, StopMovementImmediately, SetCollisionEnabled).
  Compiled clean (0 errors, 0 warnings).

- BP_Bow: Mostly built and compiling. Components, variables, StartDraw function (fully wired),
  FireArrow function (built but missing the damage pass-through to the spawned arrow).
  Compiled clean.

- BP_ThirdPersonCharacter: Integration added. EquippedBow variable, BeginPlay spawns and attaches
  bow to `hand_r` socket, two custom events (CE_StartDraw, CE_FireArrow) wired to InputKey
  RightMouseButton Pressed/Released. Final compile succeeded.

**What is missing / incorrect:**

- FireArrow does not pass the calculated damage to the spawned arrow. The `calc_dmg` node result
  (draw-time-scaled damage) is computed but never wired anywhere after SpawnActor. The BP_Arrow
  default Damage=25 will always be used regardless of draw time.
- SetDamage in BP_Arrow has an unresolved FunctionResult pin warning (bSuccess never wired from
  the VariableSet node's output — it's hardcoded to `true` via set_pin_default, which is acceptable).
- StartDraw in BP_Bow has the same FunctionResult bSuccess warning — also hardcoded to true via
  set_pin_default.
- No aiming/trajectory system was added. The arrow fires from the `ArrowSpawnPoint` in whatever
  direction it happens to face. No aim direction is computed.

**Run outcome (line 3315):** `outcome=0` (Completed). Brain state: WorkerActive -> Completed -> Idle.

---

### 11. Timing

| Timestamp | Event |
|-----------|-------|
| 13:33:07 | Engine startup |
| 13:38:15 | Task submitted, auto-snapshot taken |
| 13:38:15–28 | Discovery pass (13.7s) |
| 13:38:29 | Claude Code run 1 launched (stdin: 3385 chars) |
| 13:38:58 | First tool call (30s after launch — LLM thinking) |
| 13:39:03–04 | 3x get_template calls (parallel, library templates) |
| 13:39:04–40:47 | ~97s gap — agent planning BP_Arrow design |
| 13:40:47 | First create (BP_Arrow) |
| 13:41:09–44:23 | BP_Arrow variable/function/component work |
| 13:44:23 | BP_Arrow compile success |
| 13:44:36–49:55 | SetDamage wiring struggles + apply_plan_json for both graphs |
| 13:51:02–52:00 | ~58s gap — agent planning BP_Bow design |
| 13:52:00 | BP_Bow create |
| 13:52:06–53:23 | BP_Bow work — StartDraw success, FireArrow failure |
| 13:53:29 | **TIMEOUT** — run 1 killed after 900s total runtime |
| 13:53:29–48 | Discovery pass 2 (garbage queries, 19.5s) |
| 13:53:48 | Claude Code run 2 launched (stdin: 5428 chars — larger continuation prompt) |
| 13:54:00 | Run 2 first tool call (12s thinking) |
| 13:55:10–56:50 | ~100s gap — agent reviewing BP_Bow and planning character integration |
| 13:56:50 | EquippedBow variable added to BP_ThirdPersonCharacter |
| 13:56:59–57:35 | Two apply_plan_json failures for BeginPlay |
| 13:57:55–14:01:14 | Manual node surgery (read, remove x4, add_node, connect x5, disconnect x2) |
| 14:01:33 | compile success (BP_ThirdPersonCharacter) |
| 14:01:45–02:05 | Two plan_json calls for CE_StartDraw / CE_FireArrow — both succeed |
| 14:02:22–39 | InputKey + connect to custom events |
| 14:02:43 | Final compile success |
| 14:03:00 | Run 2 complete, exit code 0 |

**Notable large gaps:**
- 13:39:04 -> 13:40:47: 97s. Agent was reading 3 templates and deciding on BP_Arrow structure.
  This is expected, not a stuck state.
- 13:51:02 -> 13:52:00: 58s. Planning BP_Bow after finishing Arrow.
- 13:55:10 -> 13:56:50: 100s. Reviewing built graphs + planning character integration. The agent
  read BP_Bow.StartDraw, BP_Bow.FireArrow, and BP_ThirdPersonCharacter.EventGraph before writing.

Total wall time: 13:38:15 to 14:03:00 = **24 minutes 45 seconds** for the full task.

---

### 12. Overall Quality Assessment

**Strengths:**

1. **Template usage was excellent.** The agent went straight to three specific combatfs library
   templates in parallel and demonstrably used what it found (bIsStuck pattern, ArrowStuckInWall
   hit handling, socket attachment). The library template system is providing real value.

2. **plan_json plans were well-structured.** The FireArrow plan (16 steps, all resolved, pure
   collapse, exec auto-fix) shows sophisticated understanding of the plan vocabulary. The OnComponentHit
   plan (10 steps) was also clean after one validator rejection.

3. **Self-recovery on tool failures.** Three connect_pins failures in a row → switched to plan_json.
   Two apply_plan_json failures on BeginPlay → switched to granular manual surgery. Both recoveries
   were appropriate.

4. **Read-before-write in run 2.** After timeout/relaunch, the agent re-read BP_Bow's graphs and
   BP_ThirdPersonCharacter before touching them. Good.

5. **Correct resolver behavior.** Resolver auto-synthesized MakeTransform for spawn, expanded
   bare @ProjectileMovement and @ArrowSpawnPoint component refs, mapped BeginPlay ->
   ReceiveBeginPlay, applied float alias Subtract_FloatFloat -> Subtract_DoubleDouble, etc.
   All worked correctly.

6. **Final compile clean.** All three Blueprints compiling with 0 errors, 0 warnings at the end.

**Weaknesses:**

1. **Damage not passed to arrow.** The draw-time-scaled damage calculation in FireArrow is dead
   code — it never reaches the spawned BP_Arrow. This is the most significant functional gap.
   The agent should have added a `call SetDamage` step after `spawn_arrow` in the FireArrow plan.

2. **First discovery pass garbage queries on timeout.** The continuation nudge prompt being fed
   to the utility model as-is causes Markdown headers to become search queries. This should be
   filtered.

3. **Silent connect_pins errors.** Four empty-error-message failures give the agent no information
   to debug with. The pipeline should always propagate an error code from the graph writer.

4. **Total runtime 900s to complete BP_Arrow + BP_Bow + integration.** This is long for a
   ~3-Blueprint system. The 97s pre-build planning gap for BP_Arrow (after template reads) is
   likely model thinking time and may be acceptable, but it inflates wall time significantly.

5. **BeginPlay approach messy.** Two failed apply_plan_json calls left ghost nodes in the graph
   that required manual removal. The @Mesh bare ref is a usage error, but the "Exec to Exec:
   TypesIncompatible" on the second attempt is unexplained — both source and target should have
   been exec pins from a BeginPlay event to a SpawnActor node.

6. **No aiming system.** The arrows fire from ArrowSpawnPoint in its local forward direction.
   No aim vector calculation, no character rotation alignment.

---

## Recommendations

1. **Fix blank connect_pins error propagation.** The `Error: Execution failed for tool 'blueprint.connect_pins' ():`
   lines with empty messages should surface the actual failure from OliveGraphWriter. The agent
   wasted 3 retries with no information. This is a priority bug.

2. **Fix continuation discovery queries.** When the nudge prompt contains Markdown, the utility
   model receives it verbatim and extracts headings as queries. Either: strip Markdown from the
   message before passing to the utility model, or pass a structured field ("original_task": ...,
   "continuation": true) instead of raw text.

3. **The template system is working.** The agent found and used three precise library templates
   without needing `list_templates`. The catalog injection into the system prompt is sufficient
   for an agent to go directly to `get_template` by ID. No changes needed here.

4. **The Phase 0 validator is catching real errors.** The COMPONENT_FUNCTION_ON_ACTOR catch on
   SetCollisionEnabled was correct and the agent fixed it in one retry. The validator is providing
   real value.

5. **Consider SpawnActor expose_on_spawn documentation.** The agent tried to pass `Damage` to
   SpawnActor directly — this requires `Damage` to be marked `ExposeOnSpawn=true` on the target
   Blueprint variable. The agent did set `expose_on_spawn=true` on the Arrow's Damage variable
   (line 1912), which should have created a SpawnActor pin. The fact it didn't succeed (pin not
   found) suggests either: (a) `expose_on_spawn` wasn't persisted before the SpawnActor node was
   created in FireArrow, or (b) the SpawnActor node's exposed pins aren't being picked up by the
   plan executor. This warrants deeper investigation.

6. **900s timeout is reasonable for a 3-Blueprint task.** The run would have fit in one session
   with ~2 minutes to spare if not for the prolonged planning gaps. The timeout should not be
   lowered below 900s for tasks of this complexity. The second run completing in ~9 minutes
   suggests the timeout recovery worked correctly.

Source: `B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/docs/logs/UE_Olive_AI_Toolkit.log`
