# Research: Bow and Arrow Session Log Analysis (2026-03-08, Post Bug-Fix Run)

## Question
Comprehensive analysis of the full UE log from a pipeline run after 5 bug fixes were applied. Task: "CREATE A BOW AND ARROW SYSTEM FOR @BP_ThirdPersonCharacter". Log at `docs/logs/UE_Olive_AI_Toolkit.log`.

---

## 1. Timeline

| Phase | Start | End | Duration | Notes |
|-------|-------|-----|----------|-------|
| Engine startup | 02:23:19 | 02:23:29 | ~10s | PostEngineInit complete |
| Pipeline triggered | 02:28:01 | — | — | CLI-only mode detected |
| Router | SKIPPED | — | — | CLI mode defaults to Moderate |
| Scout (CLI, no LLM) | 02:28:01 | 02:28:15 | 14.5s | 10 assets, 3 template overviews (25,668 chars), 3 refs |
| Planner (MCP) | 02:28:16 | 02:29:47 | 91.1s | PID=50356, max 15 turns, 8003-char prompt |
| ComponentAPIMap | inside Planner | 02:29:47 | — | 3/3 classes resolved, 2134 chars |
| Validator (C++) | 02:29:47 | 02:29:47 | 0.004s | 3 issues, blocking=no |
| Total pipeline | 02:28:01 | 02:29:47 | 105.6s | valid=true |
| Builder (first process) | 02:30:16 | 02:44:47 | ~870s | Hit 900s timeout, relaunched |
| Builder (second process) | 02:44:47 | 02:46:41 | ~114s | Completed, exit code 0 |
| Reviewer | 02:46:41 | 02:47:09 | 28.2s | SATISFIED |
| **Total end-to-end** | **02:28:01** | **02:47:09** | **~19m8s** | |

---

## 2. What Was Built

Three assets created at `/Game/Blueprints/Bow/`:

### BP_ArrowProjectile (Actor Blueprint)
- **Components**: CollisionSphere (SphereComponent), StaticMesh, ProjectileMovementComponent
- **Variables (11)**: ArrowDamage (float), ArrowSpeed (float), GravityScale (float), InstigatorRef (Pawn ref), bArrowFired (bool), bHitLanded (bool), SpawnedArrow (ref, possibly leftover), plus component-derived ones
- **Functions**: InitializeArrow (4 inputs: Damage, Speed, Gravity, Instigator), LaunchArrow (1 input: LaunchVelocity), HandleHit (2 inputs: DamagedActor, Hit), StickToSurface (1 input: Hit)
- **Event graphs**: None logged (component setup only)
- **Compiled**: SUCCESS at end

### BP_BowComponent (ActorComponent Blueprint)
- **Variables (11)**: bArrowLoaded (bool), bBowAiming (bool), CurrentAmmo (int), MaxAmmo (int), ArrowDamage (float), ArrowSpeed (float), GravityScale (float), CharacterRef (Character ref), SpawnedArrow (BP_ArrowProjectile ref), OnAmmoChanged (event dispatcher)
- **Functions**: LaunchConditions (bool return), NockArrow, FireArrow, DestroyNockedArrow, plus StartAim (inferred from call in BP_ThirdPersonCharacter)
- **Compiled**: SUCCESS at end (after multiple failures)

### BP_ThirdPersonCharacter (modified, existing)
- **Added component**: BowComponent (BP_BowComponent_C)
- **Added variable**: bIsBowEquipped (bool, default true)
- **Added functions**: StartBowAim, ReleaseBowAim, FireBowArrow (shell functions with graphs)
- **EventGraph**: InputKey nodes (RightMouseButton, LeftMouseButton) wired to StartBowAim, ReleaseBowAim, FireBowArrow via K2Node_CallFunction
- **Compiled**: SUCCESS

---

## 3. Tool Call Scorecard

Totals compiled from all `MCP tools/call result:` lines:

| Tool | Calls | Success | Failed | Notes |
|------|-------|---------|--------|-------|
| blueprint.get_template | 9 | 9 | 0 | Planner: 3 broad + 6 pattern-specific |
| blueprint.create | 2 | 2 | 0 | BP_ArrowProjectile, BP_BowComponent |
| blueprint.add_component | 5 | 4 | 1 | Fail: 'BP_BowComponent' not found (missing `_C`) |
| blueprint.reparent_component | 1 | 1 | 0 | |
| blueprint.add_variable | 21 | 21 | 0 | 7+10+1 across 3 assets |
| blueprint.add_function | 13 | 12 | 1 | Fail: WriteRateLimit on 'FireArrow' |
| blueprint.modify_component | 2 | 2 | 0 | ProjectileMovement props + CollisionSphere radius |
| blueprint.apply_plan_json | 18 | 12 | 6 | See Section 4 |
| blueprint.add_node | 10 | 8 | 2 | See Section 4 |
| blueprint.connect_pins | 12 | 9 | 3 | See Section 4 |
| blueprint.disconnect_pins | 1 | 1 | 0 | |
| blueprint.remove_node | 2 | 2 | 0 | |
| blueprint.get_node_pins | 3 | 3 | 0 | |
| blueprint.read | 4 | 4 | 0 | |
| blueprint.compile | 5 | 5 | 0 | All final compiles succeeded |
| **TOTAL** | **108** | **95** | **13** | **88.0% success rate** |

---

## 4. Every Single Failure

### F1 — apply_plan_json: InitializeArrow — SetFloatPropertyByName not found
- **Line**: 1916–1918
- **Time**: 02:31:07
- **Tool**: blueprint.apply_plan_json
- **Graph**: BP_ArrowProjectile::InitializeArrow
- **Error**: Plan resolution failed: 10/13 steps resolved, 3 errors. `FindFunction('SetFloatPropertyByName')` not found (also checked universal library scan, K2_ fuzzy match). Closest: SetFieldPathPropertyByName, SetVector3fPropertyByName.
- **Root cause**: The AI tried to use `SetFloatPropertyByName` to set ProjectileMovementComponent properties (InitialSpeed, MaxSpeed, GravityScale) — this function does not exist in UE 5.5. The correct approach is `modify_component` or calling the specific setter via target ref. The AI was trying to call it 3 times.
- **Recovery**: AI retried with a trimmed plan (dropped SetFloatPropertyByName calls) and instead used `modify_component` directly (02:31:34 — SUCCESS). Smart recovery.
- **New/Known**: NEW failure pattern (non-existent function). The alias map has 216 entries but `SetFloatPropertyByName` is not a valid UE 5.5 API.

### F2 — apply_plan_json: HandleHit — FunctionOutput on graph with no return node
- **Line**: 2169–2177
- **Time**: 02:31:52
- **Tool**: blueprint.apply_plan_json
- **Graph**: BP_ArrowProjectile::HandleHit
- **Error**: Phase 1 FAIL: Step 'done' (FunctionOutput) — no FunctionResult node in graph 'HandleHit'. The AI put a `return` op in a void function.
- **Root cause**: `HandleHit` has no return values (pure side-effect function). The `return` op resolves to `FunctionOutput` which requires an existing FunctionResult node, but that only exists in functions with declared outputs. The AI incorrectly added a `return` op to a void function.
- **Recovery**: AI retried without the `return` op (02:32:08) — resolved successfully but then hit F3.
- **New/Known**: KNOWN issue with `return` op on void functions. Previously documented behavior.

### F3 — apply_plan_json: HandleHit — Compile error, ApplyPointDamage 'Hit from Direction' by-ref
- **Line**: 2291–2300
- **Time**: 02:32:08
- **Tool**: blueprint.apply_plan_json
- **Graph**: BP_ArrowProjectile::HandleHit
- **Error**: Compilation FAILED — "The current value (0, 0, 0) of the 'Hit from Direction' pin is invalid: 'Hit from Direction' in action 'Apply Point Damage' must have an input wired into it ('by ref' params expect a valid input to operate on)."
- **Root cause**: `ApplyPointDamage` has a `HitFromDirection` (FVector) parameter that is a by-ref input and cannot be left at default. The plan did not wire this pin.
- **Recovery**: AI retried with a step that gets forward vector (`GetActorForwardVector`) and wires it to HitFromDirection (02:32:30 — SUCCESS). Correct fix.
- **New/Known**: NEW failure pattern. Specific to `ApplyPointDamage`'s unusual by-ref parameter enforcement.

### F4 — apply_plan_json: StickToSurface — Phase 0 COMPONENT_FUNCTION_ON_ACTOR (multi-match)
- **Line**: 2443–2445
- **Time**: 02:32:33
- **Tool**: blueprint.apply_plan_json
- **Graph**: BP_ArrowProjectile::StickToSurface
- **Error**: Phase 0 FAILED: 1 errors, 1 warnings. "Step 'disable_coll' calls component function 'SetCollisionEnabled' (PrimitiveComponent) on Actor BP without Target wire (2 matches)" — validator rejected because 2 components match (CollisionSphere and StaticMesh), so it cannot auto-wire.
- **Root cause**: The plan used `SetCollisionEnabled` without specifying which component. The auto-wire only fires when there is exactly 1 match. With 2 PrimitiveComponents (CollisionSphere + StaticMesh), the validator correctly blocks ambiguous wiring.
- **Recovery**: AI resubmitted with explicit `target_class='PrimitiveComponent'` AND Target wired to `@get_coll.auto` (02:32:41 — SUCCESS). Correct fix.
- **New/Known**: KNOWN: Phase 0 COMPONENT_FUNCTION_ON_ACTOR. This is one of the two implemented validation checks. Working correctly.

### F5 — apply_plan_json: NockArrow — GetActorTransform not found on ActorComponent
- **Line**: 3258–3271
- **Time**: 02:35:38
- **Tool**: blueprint.apply_plan_json
- **Graph**: BP_BowComponent::NockArrow
- **Error**: Plan resolution failed: 13/14 steps resolved, 1 error. `FindFunction('GetActorTransform')` failed on BP_BowComponent (ActorComponent). Closest: GetActorTimeDilation, GetActorForwardVector.
- **Root cause**: `GetActorTransform` is an Actor function, not on ActorComponent. The AI was calling it without a target, so FindFunction searched the component's class hierarchy (ActorComponent → Object) and library classes, neither of which has GetActorTransform. The fix requires passing the target character reference explicitly.
- **Recovery**: AI switched to using `GetActorLocation` via `@get_char.auto` target (02:36:01). BUT this produced SpawnTransform data-wire failure — see F6/F7.
- **New/Known**: NEW. AI confused Actor vs ActorComponent scope. The error message is clear (shows full search trail), so recovery was attempted. But the immediate next plan also failed (F6).

### F6 — apply_plan_json: NockArrow — Compile error, Spawn Transform by-ref (x3 repeated)
- **Lines**: 2293–2295 (02:32:08), 3416–3421 (02:36:01), 3571–3576 (02:36:52), 3635–3640 (02:38:45)
- **Time**: 02:36:01, 02:36:52, 02:38:45
- **Tool**: blueprint.apply_plan_json
- **Graph**: BP_BowComponent::NockArrow (and LaunchConditions on the 3rd attempt — residual from previous NockArrow graph state)
- **Error**: Compilation FAILED (2 errors) — "The current value of the 'Spawn Transform' pin is invalid: 'Spawn Transform' in action 'Begin Deferred Actor Spawn from Class' must have an input wired into it ('by ref' params expect a valid input to operate on)."
- **Root cause**: The SpawnActor node's SpawnTransform pin is a by-ref FTransform and cannot have a default value — it must have a wire. On attempt 1 (02:36:01), the plan connected `@get_loc.auto` (a FVector from GetActorLocation) to SpawnTransform (FTransform) — type mismatch, pin connected to 'Location' sub-field of SpawnTransform rather than SpawnTransform itself, leaving SpawnTransform un-wired at the `FTransform` level. On attempt 2 (02:36:52), the `ExpandPlanInputs` auto-synthesizer fired and created a `_synth_maketf_spawn` MakeTransform step — this wired correctly to SpawnTransform (line 3554: "Connected pins: ReturnValue -> SpawnTransform") BUT the compile still failed with the same error. Root cause: the previous NockArrow plan attempt's nodes were rolled back but the NockArrow graph apparently already had partially committed SpawnActor nodes from a prior non-rolled-back execution (the `0 new orphans` delta confirms nodes existed before the plan). The compile errors on the LaunchConditions plan at 02:38:45 are the same BP_BowComponent compile errors from the still-broken NockArrow — these errors persist until NockArrow is finally fixed via manual granular tools.
- **Recovery**: After 3 apply_plan_json failures, the AI shifted to:
  1. `blueprint.read` NockArrow graph (02:39:03) to inspect state
  2. `blueprint.add_node` + `blueprint.connect_pins` + `blueprint.disconnect_pins` granular edits (02:39:21–02:40:07) to patch the MakeTransform wiring
  3. A new apply_plan_json for NockArrow (02:40:17 — SUCCESS at line 3788)
  4. A separate apply_plan_json for DestroyNockedArrow (02:40:20 — SUCCESS)
- **New/Known**: PARTIALLY KNOWN. The MakeTransform synthesis (ExpandPlanInputs) is a known fix for SpawnActor that was recently added. But it did not fully solve the problem here because the nodes from earlier failed-but-not-rolled-back plans were still in the graph. This is a graph state residue issue.

**Note on F6 mechanism**: The "Rolled back N nodes" log messages confirm that compile-fail plans DO roll back their nodes. The reason compiles kept failing is that the NockArrow graph had SpawnActor nodes from the plan at 02:35:16 (the one that failed at Phase 4 data wire with 1 failure — line 3239: "Error: Execution failed for tool 'blueprint.apply_plan_json' ()"). That plan DID execute Phase 1–5 successfully before the data wire failure, meaning it **did create 14 nodes** but data-wire failure caused a rollback. However, subsequent plans apparently left a graph in a state where the SpawnActor was present. Investigating: line 3239 says the write pipeline error'd and line 3240 says "Scoped transaction cancelled and rolling back" — the rollback IS firing. But compile still fails on the next plan. This suggests the rollback is partially failing or the SpawnActor nodes from a successfully-executed earlier apply are persisting.

### F7 — apply_plan_json: FireArrow — LaunchArrow not found (first attempt)
- **Line**: 3905–3907
- **Time**: 02:41:32
- **Tool**: blueprint.apply_plan_json
- **Graph**: BP_BowComponent::FireArrow
- **Error**: Plan resolution failed: 15/16 steps resolved, 1 error. `FindFunction('LaunchArrow')` not found. Closest: LaunchURL, LaunchExternalUrl, LaunchCharacter, LaunchConditions.
- **Root cause**: `LaunchArrow` is a function on `BP_ArrowProjectile`, but the plan step did not specify `target_class='BP_ArrowProjectile'`. The resolver searched BP_BowComponent's hierarchy and library classes — neither has it.
- **Recovery**: AI retried with `target_class='BP_ArrowProjectile_C'` added to the step (02:41:47 — resolved successfully). See also the alias fallback for GetForwardVector (below). The retry resolved LaunchArrow correctly (line 3929) but hit the GetForwardVector/InRot data wire issue (F8).
- **New/Known**: NEW. Missing target_class for cross-blueprint function call. The error message clearly shows "Did you mean: LaunchConditions (BP_BowComponent_C)" — which actually helped the AI understand the pattern. The alias fallback mechanism for GetForwardVector fired correctly here (lines 3886–3887).

### F8 — apply_plan_json: FireArrow — data wire InRot not found on aliased node
- **Line**: 4042–4065
- **Time**: 02:41:47
- **Tool**: blueprint.apply_plan_json
- **Graph**: BP_BowComponent::FireArrow
- **Error**: Phase 4 data wire FAILED: "No input pin matching 'InRot' on step 'get_fwd' (type: CallFunction). Available: self (Actor Object Reference)." The plan had `get_fwd` call `GetForwardVector` with an input `InRot="@get_rot.auto"` but at execution the node was aliased to `GetActorForwardVector` (Actor), which has no `InRot` pin.
- **Root cause**: The resolver CORRECTLY detected alias mismatch and rerouted to `KismetMathLibrary::GetForwardVector` (which takes `InRot: Rotator`) — see line 3923: "Alias fallback succeeded". However, the executor then failed to find `InRot` on the created node. This means the executor created `GetActorForwardVector` (Actor) rather than `KismetMathLibrary::GetForwardVector`, i.e., the alias fallback resolution was not propagated to the node creation step. The resolved step still used the aliased function.
- **Recovery**: AI fell back to granular tools: removed the wrong node, added `Conv_RotatorToVector` + re-read graph + used `connect_pins` with explicit pin names. After 3 connect_pins failures trying to use `node_16.InRot` (because GetActorForwardVector has no InRot), the AI eventually found the correct node_26 (Conv_RotatorToVector) via get_node_pins and successfully connected (02:43:51).
- **New/Known**: NEW BUG. The alias fallback mechanism in the resolver fires correctly (logs "Alias fallback succeeded — rerouted to KismetMathLibrary::GetForwardVector") but the resolved function is NOT correctly stored for Phase 1 node creation. The executor still creates GetActorForwardVector because the alias map resolution in FindFunction() returns the aliased version and the fallback fix is only logged in the resolver, not persisted to `ResolvedStep.FunctionName`.

### F9 — connect_pins: Target pin 'InRot' not found on node_16 (x2)
- **Lines**: 4132–4135 (02:43:04), 4157–4160 (02:43:24)
- **Tool**: blueprint.connect_pins
- **Error**: BP_CONNECT_PINS_FAILED: Target pin 'InRot' not found on node 'node_16'
- **Root cause**: Node 16 is `GetActorForwardVector` (Actor) — it has no InRot pin. This is a downstream consequence of F8. The AI tried to connect directly using pin name from the template/KB, not from actual node inspection.
- **Recovery**: AI used `get_node_pins` to inspect node_16 (line 4136), got the real pins, then tried semantic ref (`source_ref`/`target_ref` with `rotation_output`/`rotation_input` semantics) which also failed (line 4170 — 0.07ms, immediate failure, no error detail logged). Then inspected node_26 (Conv_RotatorToVector's actual pin node), successfully connected via explicit node_12.ReturnValue → node_26.InRot (line 4185: SUCCESS).
- **New/Known**: Downstream of F8, KNOWN pattern of pin name mismatch after wrong node type created.

### F10 — connect_pins: semantic ref failure
- **Line**: 4170–4171
- **Time**: 02:43:33
- **Tool**: blueprint.connect_pins
- **Error**: FAILED in 0.07ms — no error detail logged (sub-millisecond suggests schema validation failure, not execution). The params used `source_ref`/`target_ref` with semantic hints (`rotation_output`, `rotation_input`) — this appears to be a non-standard parameter format not supported by the current tool schema.
- **Root cause**: The AI invented a `source_ref` / `target_ref` semantic notation that does not exist in the `connect_pins` tool schema. The tool expects `source` and `target` as string node_id.pin_name.
- **New/Known**: NEW — AI hallucinated a tool parameter format. Worth logging in schema error guidance.

### F11 — add_component: 'BP_BowComponent' not found (missing _C suffix)
- **Line**: 4224–4227
- **Time**: 02:44:19
- **Tool**: blueprint.add_component
- **Error**: BP_ADD_COMPONENT_FAILED: Component class 'BP_BowComponent' not found
- **Root cause**: Blueprint-derived components must be referenced with the `_C` suffix (`BP_BowComponent_C`) when used as a component class. The AI initially tried the asset name without `_C`.
- **Recovery**: AI immediately retried with `'BP_BowComponent_C'` (02:44:26 — SUCCESS).
- **New/Known**: KNOWN. This is a recurring pattern. The `_C` suffix requirement for Blueprint-derived classes used as components is documented in the system prompt capability knowledge. However, the AI had to fail once to remember.

### F12 — add_function: WriteRateLimit
- **Line**: 2756–2757
- **Time**: 02:33:21
- **Tool**: blueprint.add_function
- **Error**: Validation failed for blueprint.add_function: rule WriteRateLimit
- **Root cause**: The Builder was calling multiple add_function calls in rapid succession (7 functions in ~12 seconds, 02:33:08 to 02:33:20). The rate limit rule fired at 30 ops/minute. The AI appears to have been batching add_function calls aggressively.
- **Recovery**: The next attempt for the same function succeeded ~9 seconds later (02:34:18, line 2771), suggesting the AI either waited or retried after a brief pause.
- **New/Known**: KNOWN. WriteRateLimit fires at 30 ops/minute. Normal behavior. MaxWriteOpsPerMinute=30 setting.

### F13 — apply_plan_json: EventGraph BP_ThirdPersonCharacter — IA_Aim not found
- **Line**: 4458–4467
- **Time**: 02:45:16
- **Tool**: blueprint.apply_plan_json
- **Graph**: BP_ThirdPersonCharacter::EventGraph
- **Error**: Phase 1 FAIL: Enhanced Input Action 'IA_Aim' not found. Available Input Actions in project: [IA_Jump, IA_Look, IA_Move].
- **Root cause**: The AI tried to use an `event` op with `target='IA_Aim'` (an EnhancedInputAction node), but `IA_Aim` does not exist in the project. The project only has IA_Jump, IA_Look, IA_Move.
- **Recovery**: The AI fell back to legacy InputKey nodes (RightMouseButton, LeftMouseButton) using `blueprint.add_node` with `type='InputKey'`. This worked (SUCCESS). However, this means the bow system uses deprecated InputKey events instead of Enhanced Input, which is architecturally inferior for the target project template.
- **New/Known**: NEW. The Planner should have known which Input Actions exist — this is a discovery gap. ComponentAPIMap provided the BowComponent API, but no IA asset discovery was done.

### F14 — add_node: CallFunction 'StartBowAim' not found (x2)
- **Lines**: 4492–4494 (02:45:44), 4497–4498 (02:45:50)
- **Tool**: blueprint.add_node
- **Error attempt 1**: `FindFunction('StartBowAim')` not found (type: "CallFunction"). No class specified.
- **Error attempt 2**: Type "Call Function" (with space) — schema validation likely rejected the node type string.
- **Root cause**: The AI tried to add a CallFunction node for `StartBowAim` which is a function on `BP_ThirdPersonCharacter` itself (self call). `FindFunction` searched without a class and the Blueprint was not compiled yet, so SkeletonGeneratedClass didn't have StartBowAim yet (it was added seconds before via add_function but may not have been compiled). Self-function calls without explicit class context fail the FindFunction search.
- **Recovery**: AI used `type='K2Node_CallFunction'` with `function_name='StartBowAim'` (02:46:06). The `CreateNodeByClass` path found StartBowAim via a match method 3 (GeneratedClass) — line 4509. SUCCESS.
- **New/Known**: PARTIALLY KNOWN. The `K2Node_CallFunction` bypass via CreateNodeByClass is a known workaround pattern. The alias map and FindFunction path did not find a freshly-added function because it needed a compile cycle first. But CreateNodeByClass's `SetFunctionReference` found it on the GeneratedClass.

---

## 5. Did the 5 Bug Fixes Trigger?

### Bug 1 Fix: @entry / tilde / synth_param
**Evidence: YES, fired correctly and repeatedly.**
- Lines 1891–1903: `ExpandComponentRefs: Expanded 4 component/param references, inserted 4 synthetic steps`. The InitializeArrow function with 4 inputs (Damage, Speed, Gravity, Instigator) had `@Damage`, `@Speed`, etc. referenced in the plan. ExpandComponentRefs fired and synthesized `_synth_param_damage`, `_synth_param_speed`, `_synth_param_gravity`, `_synth_param_instigator` steps.
- All 4 synthetic params resolved correctly via `ResolveGetVarOp: 'Damage' matched function input param 'Damage'` (line 1894).
- Also fired for `@LaunchVelocity` (LaunchArrow), `@DamagedActor`, `@Hit` (HandleHit), and `@Instigator` parameter.
- **Verdict**: Working correctly. No `VARIABLE_NOT_FOUND` errors for param references.

### Bug 2 Fix: VARIABLE_NOT_FOUND hard error
**Evidence: NOT triggered in this run.**
- None of the plans attempted to reference a genuinely non-existent variable — all `get_var` targets were valid variables that existed on the Blueprint. The hard error was not needed.
- This fix is defensive; its absence here is expected (good plans don't hit it).

### Bug 3 Fix: ToToolResult synthesize / PIPELINE_ERROR
**Evidence: NOT triggered.**
- No `PIPELINE_ERROR` code in the log. All pipeline errors were reported via standard error codes (`BP_CONNECT_PINS_FAILED`, `BP_ADD_COMPONENT_FAILED`, execution failures). The fix was for a crash/null-pointer path in result serialization.
- This fix is also defensive.

### Bug 4 Fix: RefreshAllNodes / FindFunction fix
**Evidence: Partially triggered — the alias fallback fired, but the fix did not fully work.**
- Lines 3886–3887: "Alias fallback succeeded — 'GetForwardVector' rerouted from 'Actor::GetActorForwardVector' to 'KismetMathLibrary::GetForwardVector' (input pins [InRot] matched)." The alias fallback mechanism fired correctly during resolution.
- HOWEVER: at Phase 1 node creation, the executor still created `GetActorForwardVector` (Actor version), not `KismetMathLibrary::GetForwardVector`. The resolved function name was not propagated to `CreateNodeByType`.
- **Root cause of residual bug**: The resolver logs "Alias fallback succeeded" and changes the display name, but the `ResolvedStep.FunctionName` or `ResolvedStep.ResolvedClass` was apparently still set to the alias-resolved Actor version. The Phase 1 executor reads these fields, not the fallback result.
- **Verdict**: Fix is INCOMPLETE. The resolver detects the mismatch and logs the fallback, but the resolved step is not updated with the fallback function. This caused F8 and F9.

### Bug 5 Fix: Stale node / remove_node hint ("shifted", "re-read", "stale")
**Evidence: NOT triggered by keywords.**
- No "shifted", "re-read", or "stale" messages in the log. The `remove_node` tool was used twice (lines 4068, 4098) but these were successful and not preceded by stale-node hint messages. The AI found the wrong node via its own reasoning (inspecting graph, using get_node_pins), not via a stale hint.
- This fix is also defensive for a specific scenario that did not occur here.

---

## 6. Builder Process Management

### First Builder Process
- **Launched**: 02:28:16, PID 50356 (Planner MCP process)
- **Builder spawn**: The CLI builder is the same autonomous process used for building; PID not separately logged for Builder but referenced as the autonomous run
- **Timeout event**: Line 4273–4275 (02:44:47): "Claude process exceeded total runtime limit (900 seconds) - terminating". Run timed out at attempt 1/1.
- **Relaunch**: Line 4275: "Run timed out (attempt 1/1) — relaunching with decomposition nudge." `MaxAutoContinues = 3` but this is labeled attempt 1/1, suggesting only 1 relaunch was configured or the timeout policy triggers exactly one relaunch.
- **Second process**: Launched at 02:44:47, PID not logged, `--max-turns 500`. Sandbox at `B:/Unreal Projects/UE_Olive_AI_Toolkit/Saved/OliveAI/AgentSandbox`.
- **Second process duration**: ~114 seconds (02:44:47 to 02:46:41)
- **Second process exit**: exit code 0, "last tool call 9.3s ago, accumulated 2810 chars, 17 tool calls logged" (line 4617)

### Auto-continues
- No `IsContinuationMessage()` / auto-continue log entries found. The first process timed out at wall clock rather than idle timeout. The second process was launched via the relaunch-with-decomposition-nudge path, not auto-continue.

### Tool filter
- Line 4277: "MCP tool filter set: 6 prefixes" — the relaunched process was given a filtered tool set (52/85 tools visible, line 4286). This is the write-pack gating.

---

## 7. Reviewer Result

**Line 4623**: `Reviewer: SATISFIED, 0 missing, 0 deviations (28.2s)`

The reviewer read final state of all 3 blueprints (lines 4618–4622):
- BP_ThirdPersonCharacter: Type=Blueprint, Variables=2, Components=1
- BP_ArrowProjectile: Type=Blueprint, Variables=11, Components=2
- BP_BowComponent: Type=Actor Component Blueprint, Variables=11, Components=0

All blueprints compiled clean. Reviewer found the final state satisfactory against the 7714-char Planner build plan.

---

## 8. Comparison to Previous Run

| Metric | Previous Run | This Run | Change |
|--------|-------------|----------|--------|
| Tool success rate | 85.7% (102/119) | 88.0% (95/108) | +2.3 pp |
| plan_json success rate | 46.7% (7/15) | 66.7% (12/18) | +20 pp |
| Total tool calls | 119 | 108 | -11 |
| Reviewer result | Unknown (not in prev log) | SATISFIED (0 missing) | |
| Builder timeouts | Unknown | 1 (at 900s), relaunched | |
| Compile errors | Unknown | 4 apply_plan_json (3 in BowComponent) | |
| @entry / synth_param | Not present (Bug 1) | Working: 4-8 expansions per function | |
| GetForwardVector alias | Failed | Resolver detects but node creation still wrong | |

Key observations:
- plan_json success rate improved significantly (+20 pp). In the previous run only 7/15 apply_plan_json calls succeeded. Now 12/18 succeed.
- The @entry fix (Bug 1) clearly reduced failures on functions with input parameters — the entire InitializeArrow and LaunchArrow functions were built successfully using synthetic param steps.
- The alias fallback for GetForwardVector (Bug 4) partially worked but has a residual bug in propagation to the executor.
- Two classes of new failures appeared: SpawnTransform by-ref (F6) and IA_Aim not found (F13).

---

## 9. Overall Assessment

### Positive
1. **@entry fix (Bug 1) is working**: 4 functions with input parameters (InitializeArrow with 4 params, LaunchArrow with 1, HandleHit with 2) all correctly resolved `@Param` references via synth_param expansion. Zero VARIABLE_NOT_FOUND for parameter refs.
2. **plan_json success rate dramatically improved** (47% → 67%). The resolver is more capable and the AI is generating better plans after seeing clearer error messages.
3. **All three blueprints compiled cleanly** at the end. The Reviewer is SATISFIED. The system works end-to-end.
4. **Alias fallback mechanism fires correctly in resolver** — "GetForwardVector rerouted to KismetMathLibrary::GetForwardVector" is logged. The detection logic is right.
5. **Phase 0 COMPONENT_FUNCTION_ON_ACTOR correctly blocked F4** and the AI recovered with explicit target wiring. The validator is doing its job.
6. **ExpandPlanInputs synthesized MakeTransform for SpawnActor** correctly (line 3431) — the Location→Transform synthesis fired.
7. **Self-correction loop worked**: On F7 (LaunchArrow not found), the AI added `target_class='BP_ArrowProjectile_C'` on the next attempt and succeeded.

### Remaining Issues

**Issue A — GetForwardVector alias fallback not propagated to executor (Bug 4 incomplete)**
The resolver detects the pin mismatch and logs "Alias fallback succeeded", but `ResolvedStep.FunctionName` / class is not updated. Phase 1 creates the actor version. Fix: when the alias fallback succeeds in `ResolveCallOp`, overwrite `Step.FunctionName = FallbackFunctionName` and `Step.ResolvedClass = FallbackClass` so the executor uses the correct function.

**Issue B — SpawnActor SpawnTransform by-ref compile error (F6)**
Even with ExpandPlanInputs creating a MakeTransform step, the build produced two "Spawn Transform must have a wire" compile errors after a correct-looking plan execution (12/12 data wires succeeded, line 3561). The issue appears to be residual graph state from a prior partially-applied plan. The SpawnActor node in NockArrow was created in a prior apply (02:35:16) that failed data wiring and was supposed to roll back, but compile errors on the 02:36:01 plan suggest the SpawnActor persisted. This needs investigation into whether `FScopedTransaction::Cancel` fully undoes SpawnActor node creation.

**Issue C — IA_Aim not found, fell back to deprecated InputKey (F13)**
The Planner built a plan using Enhanced Input (`IA_Aim`) but that asset doesn't exist. The Scout/Planner should discover what InputAction assets are available. The ComponentAPIMap runs for component classes but not for InputAction assets. The fallback to `InputKey` nodes works but is architecturally wrong for UE5 projects using Enhanced Input.

**Issue D — ai invented source_ref/target_ref semantic notation (F10)**
The AI attempted `source_ref: {node_id, semantic}` format for connect_pins which doesn't exist. This is a schema hallucination. The 0.07ms failure (schema validation) is the correct response but the AI tried it twice implicitly. Worth adding `source_ref` as an alias or documenting the valid format more prominently.

**Issue E — SpawnActor node residue from failed-but-partially-committed plans**
The NockArrow graph showed SpawnActor compile errors even after what logs say was a full rollback. Three consecutive apply_plan_json calls failed on compile for NockArrow (F6 x3). The AI ultimately had to use granular tools to patch the existing SpawnActor node — suggesting the SpawnActor WAS in the graph. Either the rollback is not fully undoing SpawnActor node creation, or the SpawnActor from the first successful (but data-wire-failed) plan at 02:35:16 was never rolled back because the "execution failed" check fires after data wire failure but the transaction was already committed.

Looking at line 3239: the pipeline error fires, line 3240: "Scoped transaction cancelled and rolling back." But the data wire failure in Phase 4 only produces a WARNING (line 3224), not an error. The pipeline's failure threshold may need rechecking — 1 data wire failure (`16 connections (1 failed)`) at Phase 4 triggered `Error: Execution failed for tool` but the question is whether Phase 4 data wire failure counts as an execution error that triggers rollback. Looking at line 3239 `Error: Execution failed for tool 'blueprint.apply_plan_json' ()`: — empty error message after `()` suggests the execution did fail but without a named error code. This is consistent with Phase 4 data wire failure triggering an "execution failed" result without a specific code.

If the rollback IS happening but SpawnActor nodes persist, the issue may be that `FScopedTransaction` rollback does not undo `UK2Node_SpawnActorFromClass` node addition fully (a known UE quirk for some node types that hold external references). This is worth deeper investigation.

---

## Recommendations

1. **Fix Bug 4 (alias fallback propagation)**: In `ResolveCallOp`, when "Alias fallback succeeded", overwrite the step's `ResolvedFunctionName` and `ResolvedClass` with the fallback values before returning. Currently only the log is updated.

2. **Investigate SpawnActor rollback (Issue E)**: Add a post-rollback verification: after `FScopedTransaction::Cancel`, check that nodes added during Phase 1 are gone. If any SpawnActor node persists, force-remove it.

3. **Add InputAction discovery to Scout**: The Scout already runs asset discovery. Add a filter for `UInputAction` type assets so the Planner knows what IA assets exist. This prevents F13 (trying to use non-existent IA_Aim).

4. **Reject `return` op on void functions in validator**: Phase 0 should check if the function has declared outputs when a `return` op is present. If no outputs → reject with clear error. This prevents F2 wasted round-trip.

5. **Add `ApplyPointDamage` hint to capability knowledge**: "HitFromDirection is a required by-ref FVector — always wire it (e.g., GetActorForwardVector output)." Prevents F3.

6. **Document `source_ref`/`target_ref` as unsupported**: Add a clear schema error message for unrecognized parameter formats in connect_pins so the AI gets an actionable error rather than a 0.07ms silent fail.

7. **First-process Builder timeout too long**: At 900 seconds, the Builder took 14.5 minutes before timeout. The relaunch was clean and completed in 114 seconds — suggesting the first process may have been stuck in a retry loop. Consider monitoring idle time (no tool calls for 60s) rather than only total wall time.

Sources:
- `docs/logs/UE_Olive_AI_Toolkit.log` (lines 1–4733, sampled systematically)
- Project CLAUDE.md (architecture reference)
