# Research: Log Failure Analysis — Where Tools Block the AI

## Question

The user asserts the AI repeatedly tried correct approaches that Olive's validation layer rejected. This report examines the session logs from 2026-02-27 to catalog every tool failure, classify the root cause, and determine whether the rejection was Olive's fault (a false negative) or legitimately the AI's fault.

**Key question:** What specific operations does Olive's validation layer reject that UE5 would have handled (either successfully or with a useful compile error)?

---

## Sessions Analyzed

| Log File | Date/Time | Total Tool Calls | Failures | Failure Rate |
|---|---|---|---|---|
| `UE_Olive_AI_Toolkit.log` | 2026-02-28 01:03 | 59 | 12 | 20% |
| `backup-2026.02.27-22.49.58.log` | 2026-02-27 22:12 | 43 | 14 | 33% |
| `backup-2026.02.27-20.44.38.log` | 2026-02-27 20:05 | 58 | 9 | 16% |
| `backup-2026.02.27-18.43.16.log` | 2026-02-27 ~18:38 | 11 | 0 | 0% |

The sessions with complex cross-blueprint work (pickup/interact system) show the highest failure rates.

---

## Findings

### Failure Pattern 1: Interface Method Resolution — The Core Case

This is the user-referenced pattern where the AI tried the correct interface approach multiple times and was blocked.

**What the AI was building:** A pickup/interact system — `BPI_Interactable` interface, `BP_Gun` implementing it, `BP_PlayerCharacter` calling `Interact` on nearby actors.

**Attempt 1 — Wrong tool, right intent (01:13:40):**
```
LogOlivePlanResolver: Step 'evt_interact': Non-native event 'Interact' — will be treated as component delegate event
LogOliveNodeFactory: CreateEventNode: looking for event 'Interact' in parent class 'Actor'
LogOliveNodeFactory: Error: Event 'Interact' not found in parent class, as a component delegate, or as an Enhanced Input Action
```

The AI tried to use `op='event', target='Interact'` in a plan for `BP_Gun`. This is incorrect — `Interact` is an interface message call, not an event node in a non-interface Blueprint. **The rejection here was valid.** However, the error message did not explain that. It told the AI "verify the event name or use project.search for InputAction assets," which is misleading and set the AI on an unproductive path.

**Attempt 2 — Wrong approach but caused by error message (01:14:01):**
```
LogOliveWritePipeline: Error: Execution failed for tool 'blueprint.override_function' (BP_OVERRIDE_FUNCTION_FAILED): Function 'Interact' not found in parent class
```

The AI tried `blueprint.override_function` with `function_name='Interact'` on `BP_Gun`. This is the **correct UE pattern** — to implement an interface method in a Blueprint, you override it. But Olive's `override_function` tool only searches the C++ parent class hierarchy, not the Blueprint's interface list. `Interact` had been added to `BPI_Interactable`, and `BPI_Interactable` had been added to `BP_Gun` (line 1879 confirms this), but the tool still rejected it.

**This is the primary Olive false negative.** The operation was 100% valid. The tool failed because its lookup logic did not check interfaces.

**Attempt 3 — `call` op, interface-owned function (01:18:06, 01:24:21, 01:24:58):**
```
LogOliveNodeFactory: Warning: FindFunction('Interact' [resolved='Interact'], class=''): FAILED
-- searched specified class + Blueprint GeneratedClass + parent class hierarchy + SCS component classes
+ library classes [KismetSystemLibrary, KismetMathLibrary, KismetStringLibrary, KismetArrayLibrary,
GameplayStatics, Object, Actor, SceneComponent, PrimitiveComponent, Pawn, Character]
LogOlivePlanResolver: Warning: ResolveCallOp FAILED: function 'Interact' could not be resolved (target_class='')
```

The AI switched to `op='call', target='Interact'` on `BP_PlayerCharacter` (trying to call the interface on another actor). This also failed — the resolver searches a hardcoded list of classes and does not check the target object's implemented interfaces. The function `Interact` exists only on `BPI_Interactable`, but the plan specifies no `target_class`, so the resolver cannot find it.

**This is also an Olive false negative.** In UE Blueprint, calling an interface message on an actor variable is a standard pattern — you create a `Message` node, not a `CallFunction` node. Olive's `call` op resolver does not support the "call interface message" pattern at all.

**What the AI needed to do vs what tools support:**
- To implement an interface: `blueprint.override_function` — but it only checks C++ parent, not interface list
- To call an interface method on a variable: there is no plan op for "call interface message on actor reference" — this is a gap in the `call` op

**Resolution the AI found (01:28:37 success):**
After repeated failures, the AI called `blueprint.add_interface` with the full asset path `/Game/Interfaces/BPI_Interactable` (instead of just the short name `BPI_Interactable`). The first attempt with just the short name also failed:
```
LogOliveWritePipeline: Error: Execution failed for tool 'blueprint.add_interface' (BP_ADD_INTERFACE_FAILED): Interface 'BPI_Interactable' not found
```
Then it tried with the full path and succeeded. This is a **separate Olive false negative** — the interface resolution by short name failed even though the asset existed at `/Game/Interfaces/BPI_Interactable.BPI_Interactable`.

Source: `docs/logs/UE_Olive_AI_Toolkit.log` lines 2188–2244, 2331–2344, 2438–2457, 2552–2570.

---

### Failure Pattern 2: Event Name Mismatches (Olive vs UE vocabulary)

**Session backup-2026.02.27-22.49.58.log:**

```
Event 'FunctionEntry' not found in parent class  (line 1892)
Event 'EventTick' not found in parent class       (line 2229)
```

`FunctionEntry` — The AI tried to use an event op to target the function entry node (treating it like a named event). This is a genuine AI mistake, not an Olive mistake. The AI should have used a different mechanism or just omitted it (the executor auto-chains function entries). The error message did not explain this.

`EventTick` — This is interesting. `Event Tick` in Blueprint is a real built-in event on `Actor`. Olive's event resolver failed to find `EventTick` in its lookup on `Actor`, even though it would succeed for `ReceiveActorBeginOverlap`. The event name alias table maps `ActorBeginOverlap -> ReceiveActorBeginOverlap` (seen in line 2141 of the 22.49.58 log), but `EventTick` has no corresponding alias. The correct internal name is `ReceiveTick`. **This is an Olive false negative** — a missing alias in the event name table. The AI used the canonical Blueprint editor display name, and Olive rejected it instead of translating it.

Source: `docs/logs/UE_Olive_AI_Toolkit-backup-2026.02.27-22.49.58.log` lines 1891–1893, 2228–2230.

---

### Failure Pattern 3: Function Name Resolution — Class-Scoped Functions

**`GetMesh` — two sessions:**
```
FindFunction('GetMesh', class=''): FAILED -- searched Blueprint GeneratedClass + library classes
```
`GetMesh()` is defined on `ACharacter` in C++. The resolver searched a fixed list that includes `Character` in the library scan, but `GetMesh` is on the `Mesh` component accessor and the resolver missed it. The AI tried this in two separate sessions (22.49.58 at line 1955, 20.44.38 at line 2212).

**`WasInputKeyJustPressed` — 22.49.58 session:**
```
FindFunction('WasInputKeyJustPressed', class=''): FAILED
```
This is actually on `APlayerController`, not in the hardcoded library list. The AI did not specify `target_class`. **This is a partial Olive false negative** — the function is real but requires specifying a class the resolver doesn't search by default.

**`K2_AttachActorToComponent` — 22.49.58 session:**
```
FindFunction('K2_AttachActorToComponent', class=''): FAILED
```
This is the correct UE internal name for `AttachToComponent`, and the resolver missed it. However, the resolver correctly handles `K2_AttachToActor` (seen at line 1920 in the main log) via the alias map. The `AttachActorToComponent` variant has no alias. **Olive false negative.**

**`GetRootComponent` — 22.49.58 session:**
```
FindFunction('GetRootComponent', class=''): FAILED (project.batch_write, line 2118)
```
But the same function succeeded 3 minutes earlier (line 1980-1981) via a `K2 prefix resolved 'GetRootComponent' -> 'K2_GetRootComponent' on BP_Gun_C`. The failure happened because the second call specified no `target_class` — the K2 prefix resolver only runs when a class is specified. **Olive limitation** — same function name, different outcomes depending on whether `target_class` is in the call.

**`SetGunInRange` — 20.44.38 session, line 1971:**
```
FindFunction('SetGunInRange', class=''): FAILED
```
`SetGunInRange` is a function the AI had just added to the Blueprint itself in this same session. The resolver cannot find Blueprint-defined functions without a `target_class`. The AI corrected by adding the function first and then using `apply_plan_json` with explicit context. **Olive limitation** — user-defined Blueprint functions not being resolved without class context.

**`Equip` — 20.44.38 session, line 2511:**
```
FindFunction('Equip', class=''): FAILED
```
Same pattern — a Blueprint function the AI created, unresolvable without class context.

**`/Game/Weapons/BP_Gun.Pickup` — main log line 2545:**
The AI tried using a full asset path as a function reference. The resolver does not support cross-Blueprint function calls using asset paths at all. **Olive limitation** — calling a specific Blueprint's method on another Blueprint's variable requires a cast first, then a call. The AI eventually discovered this pattern itself.

Source: Multiple log files as annotated above.

---

### Failure Pattern 4: `connect_pins` — Stale Node ID Assumptions

**`Source pin 'GunRef' not found on node 'node_0'` — two occurrences:**
```
20.44.38 log line 2047: connect_pins: source='node_0.GunRef' -> FAILED
20.44.38 log line 2074: connect_pins: source='node_0.GunRef' -> FAILED (retry)
```
The AI applied a plan_json that created nodes, then tried to manually wire an additional pin with `connect_pins`. But `node_0` is a `SetVariable` node (which has no output pin called `GunRef`), not a function input node. The pin `GunRef` is on the function's `FunctionEntry` node, which has a different node ID. The AI read back the graph with `blueprint.get_node_pins` (successfully once, then failed once) and eventually corrected by using `blueprint.read_function` to get the real layout.

The `get_node_pins` failure:
```
20.44.38 log line 2059: blueprint.get_node_pins -> FAILED
```
This is unexplained — same graph, same asset, succeeded on the call immediately before. Likely a node ID that did not exist in the current graph state.

**`Cannot connect pins: Replace existing output connections` — 22.49.58 log line 2863:**
```
blueprint.connect_pins: node_3.Pressed -> node_6.execute -> FAILED
```
The output pin `Pressed` was already connected. UE disallows adding a second output connection to an exec pin when the existing connection would need to be replaced. **This is valid UE behavior** — Olive is correctly proxying the UE constraint. However, the error message does not explain how to fix it (use `disconnect_pins` first, or use a `Sequence` node).

---

### Failure Pattern 5: `blueprint.add_interface` — Short-Name Resolution Failure

```
main log line 2556: blueprint.add_interface (BP_PlayerCharacter) -> FAILED
Error: Interface 'BPI_Interactable' not found
```
Followed immediately by:
```
line 2564: blueprint.add_interface with '/Game/Interfaces/BPI_Interactable' -> SUCCESS
```
The resolver for `add_interface` requires the full asset path. The AI correctly inferred it on the second try. **This is an Olive UX failure** — the tool's resolver does not fall back to asset registry search when given a short name. The same asset was already referenced successfully in the same session (it was added to `BP_Gun` at line 1879 using the full path). The AI used the short name for `BP_PlayerCharacter`.

---

### Failure Pattern 6: Schema Validation — Stale Node IDs in `exec_after`

```
20.44.38 log line 2087:
Schema validation error [PLAN_INVALID_EXEC_AFTER] at '/steps/1/exec_after':
Step 1 ('set_var'): exec_after references unknown step_id 'K2Node_FunctionEntry_0'
```
The AI used an internal UE node ID (`K2Node_FunctionEntry_0`) as a step reference, having read it from a graph response. Olive's schema validator rejected this because step IDs must be within the current plan. **This is a valid rejection**, but it reveals that graph read responses expose internal UE node IDs that the AI then tries to reuse, which fails at the schema layer.

---

### Failure Pattern 7: `project.batch_write` — Sub-operation Failures

The `project.batch_write` tool failed three times with `GetRootComponent` (22.49.58 log lines 2118–2120) and twice with stale node IDs (22.49.58 log lines 2700–2717). In each case, the failure is within one sub-operation of the batch, but the entire batch is rolled back. The AI then had to retry with corrected parameters.

The stale node ID case:
```
line 2705: FindNodeById: 'node_15' NOT FOUND in graph 'EventGraph' (11 nodes)
```
The AI composed a batch referring to nodes created by a previous `apply_plan_json`, but those nodes had been given new IDs (node_0, node_1, etc. after prior cleanup). **Olive limitation** — node IDs are ephemeral and not stable across plan executions. The AI correctly learned to re-read the graph before composing node-id-dependent operations.

---

### Failure Pattern 8: `blueprint.add_node` — Missing Required Property

```
main log lines 2516, 22.49.58 log line 2760:
InputKey node requires 'key' property (e.g., "E", "SpaceBar")
```
The AI called `blueprint.add_node` with `node_type='InputKey'` but omitted the `key` property. **This is a valid Olive rejection** — the node cannot be created without it. The AI corrected on the next call (3 seconds later) and succeeded. The error message was clear.

---

### Failure Pattern 9: Compile Errors Surfaced as Tool Failures

**22.49.58 log lines 2514–2522:**
```
LogBlueprint: Error: [Compiler] The current value of the 'Object' pin is invalid: Unsupported type Object Wildcard on pin Object
LogOliveWritePipeline: Warning: Compilation produced 2 errors for Blueprint 'BP_Gun'
LogOliveWritePipeline: Warning: StageReport: Compile errors detected — setting bSuccess=false
```
The plan executed (nodes created, wires connected, defaults set) but the compile step in Stage 5 detected type mismatch errors. The pipeline reported failure. The AI then read the graph, disconnected the offending pins, reconnected them correctly, and the compile succeeded.

**This is correct behavior** — Olive surfaces the compile error as a tool failure so the AI knows to fix it. The AI handled this well. This is not an Olive false negative.

---

### Creative Workarounds Observed

1. **Interface call workaround:** After `call op 'Interact'` failed 3 times, the AI dropped the `Interact` call from the plan entirely, then later added `blueprint.add_interface` (with full path) and let the interface be called implicitly in the final working plan (line 2582 shows it finally resolved `Interact` after the interface was added to `BP_PlayerCharacter`).

2. **Cross-Blueprint function call:** After `/Game/Weapons/BP_Gun.Pickup` path failed, the AI switched to a cast-then-call pattern (lines 2541–2544), which is the correct UE approach.

3. **Function entry reference:** After `K2Node_FunctionEntry_0` stale ID failed in schema, the AI rewrote the plan without the explicit `exec_after` reference and let the auto-chain do it (lines 2092–2099), which worked.

4. **GetRootComponent workaround:** After `GetRootComponent` failed without `target_class`, the AI used `GetPlayerCharacter` (GameplayStatics, which is in the hardcoded search list) as a workaround, found the actor reference that way, then worked around the missing `GetRootComponent` by not using it.

---

### Tool Call Volume Analysis

Across the 3 main sessions:
- Total tool calls: ~160 across the 3 sessions
- Tools that never failed (clean): `blueprint.read`, `blueprint.compile`, `blueprint.read_event_graph`, `blueprint.read_function`, `olive.get_recipe`, `blueprint.get_template`, `project.search`, `project.get_relevant_context`, `blueprint.add_variable`, `blueprint.add_function`, `blueprint.add_component`, `blueprint.modify_component`, `blueprint.remove_node`
- Tools that consistently caused friction: `blueprint.apply_plan_json` (resolver failures), `blueprint.override_function` (interface gap), `blueprint.connect_pins` (stale IDs), `blueprint.add_interface` (name resolution), `project.batch_write` (sub-op rollback)

The AI made on average **3–5 extra tool calls per failure** to recover: read the graph, adjust the plan, retry. The interface pattern cost roughly 8 extra tool calls across the session (3 failed apply_plan_json attempts at ~3 tool calls each for retry cycles).

---

## Summary: False Negatives vs Legitimate Rejections

| Failure | Olive False Negative? | Explanation |
|---|---|---|
| `override_function`: Interact not found in parent | YES | Interface methods are valid override targets; tool only checks C++ parent |
| `call`: Interact with no target_class | PARTIAL | No interface-message call support in plan ops; AI needed a cast |
| `add_interface`: short name 'BPI_Interactable' | YES | Asset registry fallback not implemented; full path required |
| `EventTick` not found | YES | Missing alias `EventTick -> ReceiveTick`; other events have aliases |
| `FunctionEntry` as event target | No | AI mistake; correct behavior to reject |
| `GetMesh` with no target_class | PARTIAL | Valid function; resolver doesn't search `Character` component accessors |
| `WasInputKeyJustPressed` with no target_class | PARTIAL | Valid function; `APlayerController` not in default search list |
| `K2_AttachActorToComponent` | YES | Real UE function; missing from alias map |
| `GetRootComponent` without target_class | PARTIAL | Works with target_class; fails without it inconsistently |
| User-defined Blueprint functions without target_class | YES | Functions just created in same session should be resolvable |
| Stale node IDs in batch_write | No | Node IDs are ephemeral; AI must re-read before using |
| `connect_pins`: stale pin name 'GunRef' on wrong node | No | AI node-ID confusion; valid rejection |
| `connect_pins`: Replace existing output connections | No | Valid UE constraint; error message could suggest fix |
| Schema PLAN_INVALID_EXEC_AFTER with K2Node ID | No | AI used internal UE ID as step reference; valid rejection |
| Compile errors (Wildcard type mismatch) | No | Real error; correct to surface as failure |
| `add_node` InputKey missing key property | No | Valid; error message was clear |

**Count: 5 definite Olive false negatives, 4 partial (valid function but resolver gap), 7 legitimate rejections.**

---

## Recommendations

### High Priority — Fix These

1. **`override_function` must check interfaces.** When `Function 'X' not found in parent class` fires, the tool should also check `Blueprint->ImplementedInterfaces` for each interface's functions. The interface was already added to the Blueprint — Olive should be able to find `Interact` there. Source: main log lines 2233–2240.

2. **`add_interface` must accept short names.** The tool should fall back to `FAssetRegistryModule::Get().GetAssetsByClass("Blueprint")` and filter by matching name when the short name doesn't resolve directly. The exact same interface was accepted by full path 3 seconds later. Source: main log lines 2552–2570.

3. **Add `EventTick` alias.** `ReceiveTick` is the internal name for Event Tick. It should be in the same alias table as `ActorBeginOverlap -> ReceiveActorBeginOverlap`. Also audit for `EventBeginPlay -> ReceiveBeginPlay` and other common display-name vs internal-name mismatches. Source: 22.49.58 log lines 2228–2230.

4. **Plan resolver: support interface message calls.** The `call` op with no `target_class` on a function that only exists on an interface fails even when the calling Blueprint has a variable of a type that implements it. The resolver needs to handle "call interface message on actor reference" — this is a first-class Blueprint pattern (the `Message` node category in UE). Source: main log lines 2340, 2432, 2451.

5. **Resolver: include Blueprint's own functions when no `target_class` given.** When the AI calls a Blueprint function it just created (e.g., `SetGunInRange`, `Equip`, `Pickup`) and provides no `target_class`, the resolver fails. The plan's `asset` field already tells the resolver which Blueprint is being modified — it should search that Blueprint's own function list first. Source: 20.44.38 log lines 1971, 2511; main log line 2545.

### Medium Priority — Improve Guidance

6. **Error message for `override_function` interface failure** should explicitly say: "If this function is defined on an interface, use `blueprint.override_function` with `interface_name` parameter, or add the interface first with `blueprint.add_interface`." The current message `Function 'Interact' not found in parent class` caused the AI to try increasingly wrong approaches.

7. **Error message for interface call resolution failure** should say: "If calling a Blueprint Interface method, cast the target variable to the interface type first, then use `call` with the interface as `target_class`." Source: resolver FAILED messages in main log.

8. **`connect_pins` on occupied exec pin** should suggest: "The output pin already has a connection. Use `blueprint.disconnect_pins` first, or insert a `Sequence` node." Source: 22.49.58 log line 2863.

### Low Priority — Quality of Life

9. **`GetMesh`, `WasInputKeyJustPressed` resolver gaps:** The hardcoded library class list in the resolver should include `AController`, `APlayerController`, and `ACharacter` (for `GetMesh` via `USkeletalMeshComponent` accessor). These are extremely common UE patterns.

10. **`K2_AttachActorToComponent` alias:** Add to alias table. Already handles `K2_AttachToActor`. Source: 22.49.58 log line 2022.

11. **Node ID stability note:** The fact that node IDs reset after cleanup operations causes AI confusion in batch operations. Consider documenting this in the tool schema: "Node IDs are valid only for the current graph state. Re-read the graph after any modify operations before issuing ID-dependent calls."
