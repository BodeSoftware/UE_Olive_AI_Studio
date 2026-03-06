# Agent Behavior Improvements — Design Spec

Based on analysis of the "bow and arrow system" autonomous run log (2026-03-05 20:41–20:46).

---

## 1. Current Behavior Analysis

### Complete Tool Call Timeline (50 calls, 8 failures)

| # | Time | Tool | Target | Result |
|---|------|------|--------|--------|
| 1 | 20:41:49 | list_templates | (query 1) | OK |
| 2 | 20:41:49 | list_templates | (query 2, 100ms later) | OK |
| 3 | 20:41:56 | get_template | template A | OK |
| 4 | 20:41:57 | get_template | template B | OK |
| 5 | 20:41:57 | get_template | template C | OK |
| 6 | 20:42:13 | create | BP_Arrow (template_id='projectile') | OK |
| 7 | 20:42:22 | apply_plan_json | BP_Arrow OnHit function | OK |
| 8 | 20:42:32 | create | BP_Bow (blank Actor) | OK |
| 9–11 | 20:42:36–37 | add_component x3 | BP_Bow (mesh, arrow, spawnpoint) | OK |
| 12–17 | 20:42:41–45 | add_variable x6 | BP_Bow vars | OK |
| 18–19 | 20:42:53–54 | add_function x2 | StartDraw, ReleaseDraw | OK |
| 20 | 20:43:00 | apply_plan_json | BP_Bow StartDraw | OK |
| 21 | 20:43:16 | apply_plan_json | BP_Bow ReleaseDraw | **FAIL** — `SetVelocityInLocalSpace` not found (no target_class) |
| 22 | 20:43:40 | apply_plan_json | BP_Bow ReleaseDraw (retry with target_class) | **FAIL** — 2 data wires failed, unwired FunctionResult |
| 23 | 20:44:02 | connect_pins | fix ReleaseDraw wiring | **FAIL** — stale node refs (plan rolled back) |
| 24 | 20:44:05 | add_node | Cast node | **FAIL** — missing target_class property |
| 25 | 20:44:08 | add_node | Cast node (retry) | OK |
| 26 | 20:44:13 | connect_pins | wire cast | OK |
| 27 | 20:44:14 | connect_pins | wire cast output | **FAIL** |
| 28 | 20:44:28 | disconnect_pins | undo bad wire | OK |
| 29–31 | 20:44:32–38 | connect_pins x3 | fix wiring | OK |
| 32 | 20:44:42 | disconnect_pins | | **FAIL** — pins not connected |
| 33 | 20:44:48 | compile | BP_Bow | OK |
| 34–35 | 20:45:00–01 | add_variable x2 | BP_ThirdPersonCharacter (BowRef, BowClass) | OK |
| 36 | 20:45:10 | apply_plan_json | BP_ThirdPersonCharacter BeginPlay | **FAIL** — `GetActorTransform` + `AttachActorToComponent` not found |
| 37 | 20:45:21 | apply_plan_json | BP_ThirdPersonCharacter BeginPlay (retry) | **FAIL** — `@Mesh` invalid ref format |
| 38–39 | 20:45:30–37 | add_node x2 | granular fallback for equip logic | OK |
| 40 | 20:45:33 | connect_pins | wire equip | OK |
| 41 | 20:45:45 | apply_plan_json | BP_ThirdPersonCharacter input handling | OK (compiled inline) |
| 42–43 | 20:46:00 | add_node x2 | IA_Fire input events | OK |
| 44–49 | 20:46:05–09 | connect_pins x6 | wire input to bow calls | OK |
| 50–52 | 20:46:19–20 | compile x3 | BP_Arrow, BP_Bow, BP_ThirdPersonCharacter | OK |

### What Worked Well

1. **Decomposition directive worked.** The agent listed BP_Bow, BP_Arrow, and modifications to BP_ThirdPersonCharacter before starting. Previously it only created BP_Arrow.
2. **Template usage was appropriate.** Used `projectile` factory template for BP_Arrow (correct), built BP_Bow from scratch (correct — no bow template exists).
3. **Recovery from failures was effective.** When plan_json failed, the agent fell back to granular tools and eventually got working logic.
4. **Build order was correct.** BP_Arrow first (no deps), BP_Bow second (refs BP_Arrow class), BP_ThirdPersonCharacter last (refs BP_Bow class).
5. **All three BPs compiled successfully with zero errors.**

### Where Time Was Wasted

Total run: ~5 minutes, 50 tool calls, 8 failures. Estimated waste: ~15 tool calls and ~90 seconds.

---

## 2. Specific Issues Identified

### Issue A: Duplicate list_templates Calls
**Evidence:** Calls #1 and #2 at 20:41:49, only 100ms apart. Two different queries for the same research phase.
**Impact:** Minor (2ms tool time), but signals the agent is unsure how to search. It batched two queries — likely one for "bow" and one for "projectile" or "arrow". This is actually reasonable batching behavior, just slightly redundant.
**Verdict:** Not worth adding guidance for. The agent was parallelizing research. Leave it.

### Issue B: `SetVelocityInLocalSpace` Without target_class (Call #21)
**Evidence:** Line 2096–2099. The agent called `SetVelocityInLocalSpace` in a plan_json step on BP_Bow without specifying `target_class: "ProjectileMovementComponent"`. This function lives on `UProjectileMovementComponent`, which BP_Bow does not have — it belongs to BP_Arrow.
**Root cause:** The agent confused which Blueprint owns which component. BP_Bow is the launcher; BP_Arrow is the projectile. `SetVelocityInLocalSpace` should have been called on the spawned arrow, not on BP_Bow directly.
**Impact:** 1 failed plan_json + 1 retry that partially succeeded but still failed on data wires = 2 wasted calls, plus ~10 granular tool calls to patch up.
**This is a planning error, not a prompt error.** The agent's UE knowledge is correct (it knows `SetVelocityInLocalSpace` exists), but it confused which entity should call it.

### Issue C: `GetActorTransform` Not Found (Call #36)
**Evidence:** Line 2455. The agent used `GetActorTransform` — this is NOT in the alias map, and the actual UE function is `K2_GetActorToWorld` (aliased from `GetActorTransform` ... wait, the log says alias resolved to itself). The alias map does not have this entry.
**Root cause:** Missing alias. `GetActorTransform` is a natural name the AI would use, but the UE function is `GetTransform` (on Actor) or `K2_GetActorToWorld`. The alias map should cover this.
**Impact:** 1 failed plan_json.
**This is a system gap, not a prompt issue.** Fix: add alias `GetActorTransform` -> `GetTransform` to the alias map in `OliveNodeFactory.cpp`.

### Issue D: `AttachActorToComponent` Not Found (Call #36, same plan)
**Evidence:** Line 2463. The correct UE function is `K2_AttachToComponent` (on Actor) or `K2_AttachRootComponentToActor`.
**Root cause:** Missing alias. The AI used a reasonable name that doesn't exist.
**Impact:** Combined with Issue C, killed the whole plan.
**This is a system gap.** Fix: add alias `AttachActorToComponent` -> `K2_AttachToComponent` to the alias map.

### Issue E: `@Mesh` Invalid Ref Format (Call #37)
**Evidence:** Line 2552. The agent wrote `@Mesh` as an input — missing the `.auto` or `.PinName` suffix. Should have been `@Mesh.auto` or used `@get_mesh.auto` with a get_var step.
**Root cause:** The agent forgot the `@stepId.pinHint` format for component refs. The docs in cli_blueprint.txt say `@ComponentName` auto-expands, but the actual behavior requires dotless `@Ref` to be a component/variable name, not a step ref. The log shows it was treated as a component ref but still failed with "Invalid @ref format."
**Impact:** 1 failed plan_json that had already resolved all steps, so the error hit at execution time.
**This is a prompt clarity issue.** The `@ComponentName` shorthand is documented but the error message confused the agent.

### Issue F: Granular Fallback After Rolled-Back Plan (Calls #23–32)
**Evidence:** Call #22 (apply_plan_json retry) resolved all 22 steps but failed during execution with 2 data wire failures. The pipeline rolled back the transaction (line 2311). Then the agent tried connect_pins using node IDs from the rolled-back plan — those nodes no longer exist.
**Root cause:** The agent does not understand that plan_json is atomic/all-or-nothing — when it fails, all created nodes are rolled back. The agent assumed nodes from the failed plan still existed.
**Impact:** Call #23 wasted (stale refs), then the agent rebuilt manually with add_node + connect_pins. ~10 calls wasted.
**This is partially a prompt issue.** cli_blueprint.txt line 92 says "plan_json is atomic (all-or-nothing)" but the agent clearly didn't internalize it during recovery.

### Issue G: Three Separate Compile Calls (Calls #50–52)
**Evidence:** Three sequential compile calls at 20:46:19–20, one per Blueprint.
**Impact:** Minimal — each takes ~15ms. The calls were batched (near-simultaneous timestamps suggest the agent sent them together).
**Verdict:** Not worth fixing. The agent correctly compiled all three, and they ran in ~500ms total wall time. A "compile all" convenience tool would save token overhead but not meaningful time.

---

## 3. Proposed Changes

### Change 1: Add Missing Function Aliases (C++ code change, not prompt)

**File:** `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp` (alias map)

Add these entries:
```
GetActorTransform -> GetTransform
AttachActorToComponent -> K2_AttachToComponent
AttachToComponent -> K2_AttachToComponent
```

**Rationale:** These are natural function names the AI uses that fail silently. Every alias-map miss costs 1 failed tool call + 1 retry. The alias map exists precisely for this purpose.
**Token cost:** Zero (C++ change).
**Type:** System fix, not prompt.

### Change 2: Reinforce plan_json Atomicity in Recovery Context

**File:** `Content/SystemPrompts/Knowledge/cli_blueprint.txt`

**Location:** After the existing line "plan_json is atomic (all-or-nothing); granular tools are incremental. Choose based on the situation." (line 92), add:

```
- After a plan_json failure, all nodes from that plan are rolled back — they do not exist. Do NOT use connect_pins or add_node with node IDs from a failed plan. Instead: fix the plan and retry apply_plan_json, or start fresh with granular tools using new add_node calls.
```

**Rationale:** The agent demonstrably tried to wire nodes that were rolled back (call #23). This is a ~40-token addition that prevents a specific, observed 10-call waste pattern. The existing one-liner about atomicity is too abstract — the agent needs the concrete implication spelled out.
**Token cost:** ~40 tokens.
**Type:** Prescriptive — justified because violating this wastes many tool calls every time.

### Change 3: Tighten Component Function Ownership Awareness

**File:** `Content/SystemPrompts/Knowledge/cli_blueprint.txt`

**Location:** After line 93 (the component classes list), add:

```
- Component functions (SetVelocityInLocalSpace, SetSimulatePhysics, etc.) require target_class when the component is NOT on the current Blueprint's SCS. If you're calling a function on a spawned actor's component, you need a reference to that component — spawn the actor first, then call GetComponentByClass or access it through a stored variable.
```

**Rationale:** The agent called `SetVelocityInLocalSpace` (a ProjectileMovementComponent function) inside BP_Bow's ReleaseDraw, expecting the resolver to find it. BP_Bow has no ProjectileMovementComponent — BP_Arrow does. This ~45-token note helps the agent think about "which Blueprint owns this component?" before writing the plan.
**Token cost:** ~45 tokens.
**Type:** Suggestive — helps the AI reason about ownership without mandating a specific approach.

### Change 4: Clarify @ref Format in Recovery Scenarios

**File:** `Content/SystemPrompts/Knowledge/cli_blueprint.txt`

**Location:** In the "Data wires" section (after line 46), modify the "Component refs" bullet. Current text:
```
- Component refs: `@ComponentName` auto-expands to get_var
```

Replace with:
```
- Component refs: `@ComponentName` auto-expands to get_var (dotless @refs). To reference a component in an input, write `@ComponentName` — the resolver synthesizes a get_var step. Do NOT use `@ComponentName.auto` (that looks for a step named "ComponentName").
```

**Rationale:** The agent wrote `@Mesh` which the schema validator flagged as "dotless @ref" — it was actually correct syntax but something else went wrong during execution. Clarifying the distinction between `@ComponentName` (dotless, component ref) and `@stepId.suffix` (dotted, step ref) prevents confusion.
**Token cost:** ~30 tokens net increase.
**Type:** Descriptive — clarifies existing behavior.

### Change 5: Trim the Stdin Decomposition Directive

**File:** `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp` (lines 514–539)

The current decomposition block is ~700 tokens including two full examples. It worked — the agent decomposed correctly. But it's verbose. Proposed trimmed version:

```
## Required: Asset Decomposition

Before calling ANY tools, list every game entity that needs its own Blueprint:

ASSETS:
1. BP_Name — Type — purpose
2. ...
N. Modify @ExistingBP — changes

"Does this thing exist in the world with its own transform?" → separate Blueprint.
"Is it a value on an existing actor?" → variable. "Is it a capability?" → component.

Weapons, projectiles, doors, keys, vehicles = always separate actors.

After listing assets, research patterns with blueprint.list_templates(query="..."), then build each asset fully before starting the next.
```

**Rationale:** The current version works but costs ~700 tokens. This version conveys the same information in ~150 tokens. The bow/arrow and door/key examples are redundant with `blueprint_design_patterns.txt` which already has them. The agent read CLAUDE.md which contains the full decomposition patterns — the stdin directive just needs to trigger the behavior, not re-teach it.
**Token cost:** Saves ~550 tokens per run.
**Type:** Suggestive — still says "required" for the list, but cuts the hand-holding.

### Change 6: Remove Duplicate Content Between CLAUDE.md and Design Patterns

**Observation:** The decomposition section in `blueprint_design_patterns.txt` (lines 1–32) AND the stdin directive (lines 514–539 in OliveCLIProviderBase.cpp) both contain the same "Bow and Arrow is TWO actors" example, the same asset list format, and the same entity test. The CLAUDE.md file also gets the full design patterns content.

**Recommendation:** After implementing Change 5 (trimming the stdin directive), NO further changes needed. The design patterns file is the authoritative source; the stdin directive is just the trigger. This is already well-architected — the duplication was intentional (stdin = imperative channel, CLAUDE.md = reference context) and the trimmed version reduces it appropriately.

---

## 4. What NOT to Change

### Keep: The Three-Tool Freedom Philosophy
The agent mixed plan_json and granular tools effectively. When plan_json failed for ReleaseDraw, it fell back to add_node + connect_pins and recovered. This is exactly the behavior the "AI freedom" philosophy encourages. Do not add restrictions on when to use which tool.

### Keep: Template Research Flow
The agent searched templates, read 3 template details, chose the `projectile` factory template for BP_Arrow, and built BP_Bow from scratch. This is good judgment. Do not add rules about "always use templates" or "never use templates."

### Keep: The Decomposition Directive Concept
It worked. The agent listed all three assets before starting. Just trim it (Change 5).

### Keep: Build-One-Fully-Then-Next Pattern
The agent built BP_Arrow completely, then BP_Bow completely, then modified BP_ThirdPersonCharacter. This is correct and efficient. Do not change the ordering guidance.

### Keep: Compile-at-End Pattern for Multi-Asset
Three compile calls at the end is fine. Mid-build compile (call #33 on BP_Bow at 20:44:48) was also fine — it verified BP_Bow before proceeding to BP_ThirdPersonCharacter which depends on it.

### Keep: The Knowledge Pack Architecture
Three files (cli_blueprint.txt, recipe_routing.txt, blueprint_design_patterns.txt) → assembled into CLAUDE.md → injected into sandbox. This separation of concerns is clean. Do not merge them.

---

## 5. Verification

### Test Prompt 1: "create a bow and arrow system for @BP_ThirdPersonCharacter"
(Same as this run — regression test)

**Success criteria:**
- Agent lists 3 assets before first tool call
- No `GetActorTransform` or `AttachActorToComponent` failures (aliases added)
- No connect_pins calls using node IDs from a rolled-back plan_json
- Total tool calls < 45 (down from 50)
- Total failures < 5 (down from 8)
- All 3 BPs compile successfully

### Test Prompt 2: "create a gun system with reloading for @BP_ThirdPersonCharacter"
(Tests decomposition with 3+ assets: Gun, Bullet, ammo UI, character mods)

**Success criteria:**
- Agent lists BP_Gun, BP_Bullet, and character modifications
- Uses `projectile` template for BP_Bullet
- Does not call ProjectileMovementComponent functions on BP_Gun
- Reload function uses correct function names (no alias misses)

### Test Prompt 3: "create an interaction system with a door and a pickup item for @BP_ThirdPersonCharacter"
(Tests interface + multi-asset + communication patterns)

**Success criteria:**
- Agent creates BPI_Interactable interface
- Both BP_Door and BP_Pickup implement the interface
- Character has overlap detection + input handling
- Interface calls use target_class correctly

---

## 6. Priority and Effort

| Change | Priority | Effort | Token Impact |
|--------|----------|--------|-------------|
| 1. Alias map additions | **High** | 5 min | 0 |
| 2. Atomicity reinforcement | **High** | 2 min | +40 tokens |
| 3. Component ownership note | **Medium** | 2 min | +45 tokens |
| 4. @ref format clarification | **Low** | 2 min | +30 tokens |
| 5. Trim stdin directive | **Medium** | 10 min | -550 tokens |

Net token impact: **-435 tokens per run** (saving more than adding).

Changes 1 and 2 address the two highest-cost failure patterns (alias misses and stale-ref chasing). Change 5 recovers token budget. Changes 3 and 4 are preventive.
