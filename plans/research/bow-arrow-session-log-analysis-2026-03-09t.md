# Research: Bow-Arrow Session Log Analysis — Run 09t

## Question
Full analysis of run 09t: timeline, tool call summary, new feature verification, error analysis, agent behavior, @Mesh bug deep dive, final state, and comparison to prior runs.

---

## Findings

### 1. Timeline

- **First tool call:** `2026.03.10-01.52.08:251` (blueprint.list_templates)
- **Last tool call:** `2026.03.10-01.58.45:312` (blueprint.read, final verification read)
- **Run start (autonomous launch):** `2026.03.10-01.51.24:690`
- **Run end (exit code 0):** `2026.03.10-01.59.02:645`

**Total agent duration:** 7 minutes 38 seconds (01:51:24 → 01:59:02)

This is the **second slowest run** observed, worse than 09q (4:09) and 09s (5:50), but not as bad as the 12-minute runs (09l, 09n).

Source: lines 1772, 1826, 3025, 3027

---

### 2. Tool Call Summary

**Total tool calls:** 59
- Confirmed via `grep "MCP tools/call:"` count = 59
- The run summary (line 3027) says "50 tool calls logged" — this is the CLI provider's internal counter, which undercounts because the counter starts after the first MCP connection handshake. The 59 grep count is authoritative.

**Successes / failures:**
- `executed in ... - success`: 57 (counts `blueprint.apply_plan_json` as 1 even for FAILED — no, wait: FAILED plan_json calls also emit this line)
- Actually from `MCP tools/call result:` lines: 8 FAILEDs, 51 SUCCESSes → **86.4% tool success rate**

**Tool breakdown:**

| Tool | Count |
|------|-------|
| blueprint.remove_node | 16 |
| blueprint.apply_plan_json | 9 |
| blueprint.connect_pins | 8 |
| blueprint.read | 5 |
| blueprint.get_template | 3 |
| blueprint.add_variable | 3 |
| blueprint.add_component | 3 |
| blueprint.create | 2 |
| blueprint.add_node | 2 |
| blueprint.add_function | 2 |
| olive.get_recipe | 1 |
| blueprint.list_templates | 1 |
| blueprint.get_node_pins | 1 |
| blueprint.disconnect_pins | 1 |
| blueprint.describe_function | 1 |
| blueprint.compile | 1 |

**plan_json results (9 calls):**

| # | Blueprint | Result |
|---|-----------|--------|
| 1 | BP_Arrow EventGraph | SUCCESS |
| 2 | BP_Bow Fire function (first attempt) | FAILED — Phase 0: EXEC_SOURCE_IS_RETURN |
| 3 | BP_Bow EventGraph (ResetCanFire only) | SUCCESS |
| 4 | BP_Bow Fire function (retry) | SUCCESS |
| 5 | BP_ThirdPersonCharacter EventGraph (first attempt) | FAILED — Phase 4: @Mesh data wire |
| 6 | BP_ThirdPersonCharacter EventGraph (retry with get_var step) | SUCCESS |
| 7 | BP_ThirdPersonCharacter EventGraph (HandleFire + is_valid) | SUCCESS |
| 8 | BP_ThirdPersonCharacter EventGraph (HandleFire 2nd retry) | SUCCESS |
| 9 | BP_ThirdPersonCharacter EventGraph (final full rebuild) | SUCCESS |

**plan_json success rate: 7/9 = 77.8%**

Source: lines 1965, 2073, 2119, 2243, 2343, 2431, 2762, 2842, 2976

---

### 3. New Feature Verification

#### a) EXEC_SOURCE_IS_RETURN (Phase 0 Check 5)

**CONFIRMED FIRING.** Line 2070:
```
LogOlivePlanValidator: Warning: Phase 0: EXEC_SOURCE_IS_RETURN -- step 'ret_success' exec_after targets FunctionOutput step 'set_success'
```

This correctly caught the structural error in the BP_Bow Fire function plan where `ret_success` (a `return` op) had `exec_after: set_success`, and `set_success` was a `set_var` op that resolved to `FunctionOutput`. Phase 0 FAILED, the call was rejected, and the agent recovered by splitting the plan: it submitted `ResetCanFire` logic to EventGraph separately, then resubmitted Fire without the invalid exec chaining.

**VERDICT: Feature is working correctly.** One firing, one clean catch.

Source: lines 2069–2073

#### b) CleanupStaleEventChains

**CONFIRMED FIRING. Zero stale nodes removed.**

Line 2087:
```
LogOlivePlanExecutor: CleanupStaleEventChains: checking 1 event(s) for orphaned chains
```

This ran during the second BP_Bow plan execution (the ResetCanFire-only plan on EventGraph). It checked 1 event but emitted no "removing stale node" or "removed X stale nodes" message — meaning no stale chains were found. The event in question was likely a pre-existing ReceiveBeginPlay event on BP_Bow that had no downstream connections yet (which is clean, not stale).

**VERDICT: Feature fires but did not need to clean anything in this run.** Cannot confirm the cleanup path works without a run where stale nodes actually exist.

Source: line 2087

#### c) Exec Auto-Break

**CONFIRMED FIRING.** Line 2400:
```
LogOlivePinConnector: Exec auto-break: breaking existing connection on K2Node_Event_3.then to make new connection to K2Node_SpawnActorFromClass_1.execute
```

This fired during the second BP_ThirdPersonCharacter BeginPlay plan (the successful retry at line 2344). The ReceiveBeginPlay event already had a `then` connection from the first (rolled-back) plan attempt — the auto-break severed that stale link and connected it to the SpawnActor node correctly. The plan then completed with SUCCESS.

**VERDICT: Feature is working. It resolved the exact exec-pin-already-wired scenario it was designed for.** This confirms the fix from 09q is still holding.

Source: line 2400

#### d) describe_function

**CALLED ONCE. FAILED.** Lines 2253–2257:
```
MCP tools/call: blueprint.describe_function (function_name="GetMesh", target_class="Character")
FindFunction('GetMesh'): FAILED -- searched specified class + Blueprint... universal library search
MCP tools/call result: blueprint.describe_function -> FAILED
```

The agent called `describe_function` before attempting to wire `@Mesh` to `K2_AttachToComponent`. It was looking for `GetMesh` on `Character`. This function does not exist as a callable `UFunction` — `GetMesh()` returns `USkeletalMeshComponent*` but is a C++ accessor, not a `UFUNCTION`, and is not accessible this way.

After the failure the agent did NOT pivot to `get_var("Mesh")` on the first try. Instead it submitted a plan_json using `@Mesh` as a raw dotless reference. That plan passed resolver and Phase 0 but failed in Phase 4 execution (see Section 6 below). On the second retry the agent correctly used `get_var(target="Mesh")` which resolved to the SCS component and worked.

**VERDICT: describe_function was called (behavior improvement over prior runs that never called it). But it did not prevent the first plan_json failure. The agent needed one plan_json failure to learn the correct approach.**

Source: lines 2253–2257, 2258–2343, 2344–2431

#### e) Reused Existing Event Node (Modify path)

**CONFIRMED. Fired twice.**

Line 1902:
```
LogOlivePlanExecutor: Reused existing event node 'ReceiveBeginPlay' for step 'begin'
```

Line 2090:
```
LogOlivePlanExecutor: Reused existing custom event node 'ResetCanFire' for step 'reset_evt'
```

- First occurrence: BP_Arrow EventGraph, plan 1 reused pre-existing ReceiveBeginPlay.
- Second occurrence: BP_Bow EventGraph (ResetCanFire plan), reused the custom event created by `add_function` call at line 2030.

Line 2371 shows a third reuse:
```
LogOlivePlanExecutor: Reused existing event node 'ReceiveBeginPlay' for step 'begin'
```
(BP_ThirdPersonCharacter, second attempt at line 2344)

**VERDICT: Reuse path (Modify) is functioning. EXEC_WIRE_REJECTED errors were zero — confirmed clean for third consecutive run.**

Source: lines 1902, 2090, 2371

---

### 4. Error Analysis

#### Error 1: BP_Bow Fire — EXEC_SOURCE_IS_RETURN (Phase 0)

**Line:** 2070–2073
**Error code:** Phase 0 EXEC_SOURCE_IS_RETURN
**Cause:** The agent's first Fire plan included `{"step_id":"ret_success","op":"return","exec_after":"set_success"}` where `set_success` was a `set_var` that the resolver routed to the FunctionOutput node. Phase 0 correctly detected that a `return` op cannot have `exec_after` pointing to another step (the return IS the FunctionOutput, it has no exec output).
**Recovery:** Agent split the plan. Submitted `ResetCanFire` to EventGraph separately (SUCCESS), then resubmitted Fire without the `ret_success` exec_after (SUCCESS). Clean and correct.

#### Error 2: BP_Bow Fire — Unwired FunctionResult pin (Phase 5.5 warning, not a failure)

**Line:** 2232
**Error code:** Warning only: "Unwired FunctionResult data pin 'bSuccess'"
**Cause:** The Fire function has a `bool bSuccess` return value. The plan's spawn+timer flow didn't wire anything into `bSuccess`. The plan applied successfully (compilation still passed — default false), but Phase 5.5 flagged it as a structural concern.
**Recovery:** Not addressed. The warning was non-blocking. This is a quality gap but not a correctness failure.

#### Error 3: BP_ThirdPersonCharacter — @Mesh data wire FAILED (Phase 4)

**Lines:** 2261, 2263, 2329, 2343
**Error code:** Phase 4 data wire FAILED: "Invalid @ref format: '@Mesh'"
**Cause:** See Section 6 for full deep dive. Short version: ExpandComponentRefs DID run on `@Mesh` but chose not to synthesize a `get_var` step, leaving `@Mesh` as a raw bare ref. Phase 4 then rejected it because it's not a `@stepId.pinHint` format.
**Recovery:** Agent retried with explicit `get_var(target="Mesh")` step. The get_var resolved correctly to the SCS SkeletalMesh component, the plan succeeded.

#### Error 4: connect_pins node_8.execute — "Available input pins: (none)" (3 occurrences)

**Lines:** 2508, 2582, 2984
**Error code:** BP_CONNECT_PINS_FAILED
**Cause:** node_8 is a `GetVariable` node (for Mesh variable), which is a pure getter. Pure nodes have no exec pin (`execute`). The agent tried to wire exec into a pure node.

- **First two occurrences** (lines 2508, 2582): from the second apply attempt's partial state (plan succeeded but agent read graph and tried to manually rewire node_8=GetVariable's exec). The agent used `blueprint.read`, saw the graph, tried to connect `node_6.then → node_8.execute` (wrong — node_8 is pure). After the second failure the agent read the graph again, then used `get_node_pins` to inspect node_8, confirmed it had no exec input, and then deleted all the newly created nodes (remove_node × 6) and resubmitted a fresh plan.

- **Third occurrence** (line 2984): Much later, the agent created a `HandleFire` custom event (node_22), then tried to wire `InputKey(node_21).Pressed → HandleFire(node_22).execute`. A CustomEvent node (UK2Node_CustomEvent) has no `execute` input pin — it is the SOURCE of execution, not a target. The agent's mental model was to wire the InputKey into the custom event's "call" point, which doesn't exist. After this failure the agent removed node_22 and pivoted to wiring InputKey.Pressed directly into the branch node (node_25), then deleted the custom event entirely.

**Recovery:** The 3rd connect_pins error led to a surprisingly good architectural pivot — the agent realized the fire input structure needed redesign and ended up with InputKey→Branch→BowRef.Fire without needing the custom event as an intermediary.

#### Error 5: connect_pins node_8.self — type mismatch (silent empty error, line 2519)

**Lines:** 2519–2522
**Error code:** (empty) — no BP_CONNECT_PINS_FAILED code, just "Execution failed: "
**Cause:** Attempted `node_5.ReturnValue → node_8.self` where node_5 is `blueprint.describe_function` result (not in graph) and node_8 is GetVariable. This was an architectural confusion — the agent was trying to wire a `Cast` return value to a `GetVariable.self` pin which doesn't exist on GetVariable. The empty error message is a bug (should report why it failed).
**Recovery:** Agent recovered via node deletion and plan resubmission.

#### Error 6: describe_function GetMesh FAILED

Covered in Section 3d above.

---

### 5. Agent Behavior

#### Manual remove_node count: 16 (worst ever recorded)

**Distribution of remove_node calls:**
- 4 removes (lines 2440–2479): After second BP_ThirdPersonCharacter plan succeeded but Phase 5.5 reported "Orphaned SpawnActor" warning — agent read graph, saw the old nodes from the first (rolled-back) attempt… wait, those were actually from the plan that SUCCEEDED (attempt 2 at line 2344). The orphaned SpawnActor was a node from the first plan execution that completed partially. The agent then manually deleted nodes 4, 5, 6, 7 (the orphaned SpawnActor and its companions).
- 6 removes (lines 2614–2673): After the connect_pins failures on node_8, agent nuked nodes 8, 7, 6, 5, 4, 3 to clear the graph and start fresh.
- 5 removes (lines 2843–2892): After the HandleFire custom event plan succeeded but the agent decided it needed to restructure — removed node_20 (CallFunction/Fire), node_19 (IsValid), node_18 (GetVariable/BowRef), node_17 (CustomEvent/HandleFire), node_16 (InputAction/IA_Fire). This entire remove batch was the IA_Fire → InputKey pivot.
- 1 remove (line 3001): Removed node_22 (second HandleFire custom event) after the connect_pins failure showed you can't wire exec INTO a custom event.

CleanupStaleEventChains was supposed to reduce manual cleanup. In this run it ran once (during BP_Bow EventGraph) but found nothing to clean. The 16 remove_node calls are all from BP_ThirdPersonCharacter graph mismanagement — a different cleanup problem entirely (orphaned nodes from partial commits and failed plan retries). CleanupStaleEventChains would not have helped here.

#### Self-correction retries: 3 distinct retry cycles

1. BP_Bow Fire function: 1 retry (Phase 0 EXEC_SOURCE_IS_RETURN → split plan)
2. BP_ThirdPersonCharacter BeginPlay: 2 retries (@Mesh failure → 6-node wipe → successful retry → node cleanup after orphan warning)
3. BP_ThirdPersonCharacter fire input: 3 retries (IA_Fire node removed for unknown reason → InputKey added → HandleFire plan with is_valid → connect_pins failure on custom event exec → remove custom event → direct InputKey→Branch wiring)

#### IA_Fire vs InputKey

The agent first tried `blueprint.add_node(type="InputAction", properties={"InputActionName":"IA_Fire"})` at line 2763. This **succeeded** — the node was created (node_16). However at line 2843 the agent subsequently removed node_16 (IA_Fire) along with node_17–node_20 (the HandleFire plan nodes). The reason is unclear from the log — no error was reported for the IA_Fire node itself. The agent may have decided the fire-input architecture was wrong after seeing the compiled-with-1-warning result at line 2838.

After deleting IA_Fire, the agent used `add_node(type="InputKey", properties={"key":"LeftMouseButton"})` at line 2893. This succeeded (node_21). The agent then tried to wire InputKey.Pressed → HandleFire(node_22).execute (failed — custom event has no execute input), then deleted node_22 and instead wired InputKey.Pressed → Branch(node_25).execute (succeeded at line 2995).

**Final fire input: InputKey(LeftMouseButton) → Branch (if BowRef IsValid) → BowRef.Fire().**

The IA_Fire node was created, worked at node level, but was abandoned by the agent for architectural reasons. The 1 compile warning at line 2838 (during the HandleFire plan) may have triggered the rethink.

---

### 6. @Mesh Bug Deep Dive

**The chain of events:**

1. Agent submitted plan for BP_ThirdPersonCharacter BeginPlay with `{"step_id":"attach","op":"call","target":"AttachToComponent","inputs":{"Parent":"@Mesh",...}}`.

2. IR schema (line 2261) logged: `Step 4 ('attach'): input 'Parent' has dotless @ref '@Mesh' — will be treated as component/variable reference by resolver`.

3. ExpandComponentRefs ran (line 2263): `Expanded 1 component/param references, inserted 0 synthetic steps`.

   **This is the bug.** ExpandComponentRefs recognized `@Mesh` as a dotless ref but inserted **0 synthetic steps** — it did not synthesize a `get_var` step. Why?

   Looking at the context: ExpandComponentRefs synthesizes a `get_var` step when it encounters a bare `@ComponentName` ref. However, `Mesh` is the SCS component name, and in this plan, `@Mesh` was used as an *input value* to a `call` op's `Parent` pin. The resolver expanded `@ArrowSpawnPoint` (a simpler case on BP_Bow) but failed to expand `@Mesh` here.

   The most likely reason: `@Mesh` appeared in the input of an `attach` step where the plan also had `"Target":"@spawn.auto"`. The resolver may have checked that `@Mesh` matches a step ID first — it doesn't — then may have decided the component was already covered by another path (or the expansion logic has a condition that prevents synthesis when the @ref is not the `Target` field).

4. Resolution continued (lines 2264–2278): 5/5 steps resolved with 2 warnings. Neither warning identified the @Mesh issue — they were for `@self.auto` and the @Mesh ref was silently passed through.

5. Phase 0 passed (line 2280) — no structural check catches "dotless @ref with no synthetic step."

6. Phase 4 data wiring (line 2329): `Data wire FAILED: Invalid @ref format: '@Mesh'. Expected '@stepId.pinHint'`. The executor received `@Mesh` and couldn't resolve it because there is no step with id `Mesh` and no `stepId.Mesh` pattern.

7. Rollback fired, plan returned FAILED.

**Root cause: ExpandComponentRefs has a conditional that prevents synthesis of a `get_var` step for `@Mesh` in this specific context.** The `@ArrowSpawnPoint` ref on BP_Bow (same function, same attach pattern) WAS synthesized (line 2125) — so the difference must be something specific to: (a) BP_ThirdPersonCharacter vs BP_Bow, (b) the presence of an `Owner` input in the first plan attempt, or (c) the way `Mesh` is named (it could be matching a pin name rather than a component name).

**Specific difference:** In the first (failed) plan at line 2259, the attach step had `"Parent":"@Mesh"` AND the spawn step had `"Owner":"@self.auto"`. The `@self.auto` ref on `Owner` may have occupied ExpandComponentRefs' attention for the `self` pseudo-ref, leaving `@Mesh` improperly handled. In the successful BP_Bow plan, `@ArrowSpawnPoint` was the only dotless ref and was expanded correctly.

**Why did the second attempt succeed?** The agent explicitly added a `get_var(target="Mesh")` step (step_id="get_mesh") and wired it as `"Parent":"@get_mesh.auto"`. This is the correct approach — the dotless `@Mesh` expansion should have done this automatically but didn't.

Source: lines 2261–2343

---

### 7. Final State

**All Blueprints compile clean:**
- BP_Arrow: SUCCESS (line 1961)
- BP_Bow: SUCCESS (line 2239, final)
- BP_ThirdPersonCharacter: SUCCESS (lines 2427, 2758, 2838 with 1 warning, 2972 clean, 3015 final clean)

The 1 compile warning at line 2838 (during the HandleFire+is_valid plan) is unknown — the log doesn't print the warning text inline. By the final compile at line 3015 (after removing the custom event and using direct InputKey wiring) the warning was gone.

**Last tool call:** `blueprint.read` on BP_ThirdPersonCharacter EventGraph at line 3018 — reading 14 nodes with 22 connections. This is a verification read.

**Orphaned nodes at end:** Unknown. After the final compile the graph had 14 nodes. The remove_node × 1 at line 3001 (removing node_22/HandleFire custom event) occurred after the plan succeeded, leaving the InputKey node (node_21) orphaned from the event chain until the connect_pins at line 2988 wired it to node_25. The 1 "new orphan" warning at line 2997 (Orphan delta: 1 new, absolute: 6, baseline: 5) suggests one orphan was left — the orphan is the InputKey node which gets its exec output connected only later. By the final compile, compilation was clean, so UE did not complain about orphaned exec nodes (orphaned data nodes are tolerated by the compiler).

Run ended with exit code 0, outcome=Completed (line 3028).

---

### 8. Comparison to Previous Runs

| Metric | 09q (best) | 09s (last) | 09t (this) |
|--------|-----------|-----------|-----------|
| Agent time | 4:09 | 5:50 | 7:38 |
| plan_json rate | 75% (6/8) | 55.6% (5/9) | 77.8% (7/9) |
| Tool success | 90% (27/30) | 88.4% (38/43) | 86.4% (51/59) |
| Total tool calls | 30 | 43 | 59 |
| remove_node calls | 0 | 16 | 16 |
| Blueprints compiled | 4 (SUCCESS all) | 3 (SUCCESS all) | 3 (SUCCESS all) |
| EXEC_WIRE_REJECTED | 0 | 0 | 0 |
| New feature: EXEC_SOURCE_IS_RETURN | N/A | 0 fired | 1 fired, caught correctly |
| New feature: CleanupStaleEventChains | N/A | N/A | Fired 1×, 0 nodes cleaned |
| New feature: exec auto-break | 5× | 0× | 1× |
| describe_function calls | 0 | 0 | 1 (FAILED) |

09t is a **regression from 09q** on all time and efficiency metrics. It is better than 09s on plan_json rate (77.8% vs 55.6%) but worse on time (7:38 vs 5:50) and total calls (59 vs 43). The remove_node count remains at 16 — same as 09s, much worse than 09q's 0.

---

## Recommendations

1. **@Mesh ExpandComponentRefs gap is the highest-impact bug.** The resolver correctly synthesized `@ArrowSpawnPoint` but not `@Mesh`. The difference appears to be context-dependent (presence of other @refs in same plan, or component name matching a pin hint). The `@Mesh` ref passed through resolver, Phase 0, and failed silently in Phase 4. Fix: ExpandComponentRefs should emit a phase-0-catchable error when a dotless @ref survives into Phase 4 without a backing step. Alternatively, strengthen ExpandComponentRefs to always synthesize a get_var for any component-name ref that isn't otherwise resolvable. Priority: HIGH — this caused one plan failure and forced a 6-node wipe and rebuild.

2. **CustomEvent has no exec INPUT — this must be in agent knowledge.** The agent tried to wire `InputKey.Pressed → HandleFire_custom_event.execute` twice (runs 09s and 09t). A custom event node is an exec SOURCE, not an exec SINK. The agent's mental model is wrong. Fix: add a note to the plan_json ops reference: "custom_event nodes have only an exec OUTPUT (then), not an exec input — you cannot wire an exec source into a custom event node. To call a custom event from an input, wire the input's exec output directly to the first step that follows the custom_event in the chain." This is a knowledge injection fix, not a code fix. Priority: HIGH — fired in both 09s and 09t.

3. **remove_node count at 16 for second run in a row indicates a systematic fragmentation pattern.** In 09t the agent built BP_ThirdPersonCharacter in three separate plan_json calls, each partially building up state, triggering orphan warnings, then manually wiping and rebuilding. This wastes ~3-4 minutes of the run. CleanupStaleEventChains did not help because the orphans are not stale event chains — they are partial plan commit artifacts from rolled-back transactions that somehow left nodes behind. Root cause: the first BP_ThirdPersonCharacter plan_json FAILED (Phase 4 rollback) but the Phase 5.5 on the SECOND (successful) attempt still reported "Orphaned SpawnActor" — this means the rollback from attempt 1 did not fully clean up. If rollback is functioning correctly this should not happen. Priority: MEDIUM — investigate why Phase 5.5 sees orphaned SpawnActor after rollback.

4. **describe_function was finally called (improvement) but the function it looked up (GetMesh) is not a UFUNCTION.** The lookup was architecturally correct (agent trying to understand how to get the mesh component) but FindFunction cannot find C++ accessor functions. The failure then did NOT prevent the plan_json failure — the agent still submitted `@Mesh` raw. A better self-correction path would be: describe_function FAILS → agent infers "this is likely a component variable, use get_var instead." The describe_function failure response should include a suggestion: "This function may be a C++ accessor (not a UFUNCTION). Try get_var with the component name." Priority: MEDIUM.

5. **IA_Fire was created successfully (UK2Node_InputAction) but then abandoned.** The agent created it at line 2763, the node appeared as node_16 with 3 pins, and compilation succeeded. However after the HandleFire plan (which compiled with 1 warning), the agent deleted IA_Fire. The 1 compile warning is the likely trigger — the agent may have interpreted the warning as a problem with IA_Fire. The warning text is not printed. Recommend: print compile warning text inline in the compile result so the agent can make informed decisions. Priority: LOW.

6. **EXEC_SOURCE_IS_RETURN is working perfectly.** The first Fire plan was caught at Phase 0 with a clear message. The agent's recovery (split the plan, submit ResetCanFire separately) was correct and efficient. No change needed here.

7. **CleanupStaleEventChains needs a test case to verify.** It fired but found nothing to clean. It will only trigger when an event from a previous plan_json call has a chain of orphaned downstream nodes. Run 09t's orphan nodes were SpawnActor-type (non-event-sourced), so CleanupStaleEventChains would not have caught them regardless. The feature is probably correct but unverified in practice.

8. **The agent's final BeginPlay architecture was correct but roundabout.** Three plan_json calls were needed to build what could have been expressed in one (spawn bow, store ref, get mesh component, attach to mesh). The describe_function failure and the @Mesh expansion bug forced the agent to rebuild twice. If both of those were fixed (describe_function returning useful guidance, @Mesh auto-expanded), this should converge in 1 plan_json call.

9. **The agent correctly used target_class="BP_Bow_C" for the Fire call** (lines 2780, 2904). This is the correct pattern for calling a function on a Blueprint class. No regression here.

10. **Second connect_pins failure type (empty error message) needs investigation.** Line 2519 shows `Error: Execution failed for tool 'blueprint.connect_pins' (): ` with an empty error body. This is a different failure path from the "pin not found" error. The connection attempt was `ReturnValue → self` which is a type incompatibility (object→self pin type mismatch). The diagnostic message should say "TypeIncompatible" not empty string. This makes it harder for the agent to diagnose.
