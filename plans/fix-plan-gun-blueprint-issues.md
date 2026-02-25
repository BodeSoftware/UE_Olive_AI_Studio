# Fix Plan: BP_Gun Graph Issues (Feb 25 Log)

## What happened

The AI created BP_Gun and BP_Bullet across 5 CLI iterations. Both blueprints got their components and variables created correctly. But the BP_Gun EventGraph has three visible problems:

1. **Orphan nodes** — disabled/disconnected nodes floating in the graph (visible in screenshot)
2. **GetWorldTransform has no Target** — causes compile error: "This blueprint (self) is not a SceneComponent, therefore 'Target' must have a connection"
3. **AI declared success despite the compile error**

---

## Fix 1: Orphan Nodes from Partial Phase 1 Failure

### The problem

On iteration 2, the AI submitted an 11-step plan for BP_Gun. Phase 1 (node creation) got through 4 nodes before failing on step 5 (`GetMuzzlePoint` — a function the AI invented). The executor logged:

```
Phase 1 FAILED: Node creation aborted. 4 of 11 nodes created before failure.
```

Those 4 nodes (the custom event "Fire", a GetVariable for bCanFire, a Branch, and a SetVariable for bCanFire) were created in the graph before the abort. The transaction was rolled back for the *plan tool result* (it returned an error), but the 4 nodes themselves were already added to the UEdGraph and survived because UE's undo system captured them at creation time, not at plan completion.

On iteration 4, the AI resubmitted a corrected 11-step plan. The executor created 11 NEW nodes (node_4 through node_13), wired them correctly, and returned success. But the 4 orphan nodes from the failed first attempt are still sitting in the graph — they're the disabled nodes visible in the screenshot with "This node is disabled and will not be called."

### The fix

In `FOlivePlanExecutor::Execute()`, when Phase 1 fails partway, clean up already-created nodes before returning the error. After the Phase 1 failure is detected, iterate through `Context.StepToNodeMap` and remove each node that was created in this execution:

```
When Phase 1 aborts:
    For each entry in Context.StepToNodeMap:
        Remove the node from the graph (Graph->RemoveNode())
    Clear Context.StepToNodeMap
    Then return the error as normal
```

This ensures a failed plan attempt leaves the graph in the same state it was before the attempt. No orphans.

**File:** `OlivePlanExecutor.cpp`, in the Phase 1 loop where it checks for creation failure and breaks.

---

## Fix 2: AI Can't Reference Components in Plan JSON

### The problem

BP_Gun has a component called `MuzzlePoint` (an ArrowComponent). The AI needs to get its world transform to use as the SpawnActor location. Across two attempts, the AI tried:

**Attempt 1 (iteration 2):** 
```json
{"step_id":"get_muzzle", "op":"call", "target":"GetMuzzlePoint"}
```
Failed — `GetMuzzlePoint` is not a real UE function. The AI invented it.

**Attempt 2 (iteration 4):**
```json
{"step_id":"get_muzzle", "op":"get_var", "target":"MuzzlePoint"}
```
The resolver warned `Variable 'MuzzlePoint' not found on Blueprint 'BP_Gun' or parents` but still created a GetVariable node for it. This node outputs nothing useful. The next step `get_tf` (K2_GetComponentToWorld) has a `Target` pin that needs a SceneComponent reference, but nothing was wired to it — so it defaults to `self`, and since BP_Gun (an Actor) is not a SceneComponent, the compiler throws the error.

The AI doesn't know the correct pattern. The correct approach is:

```json
{"step_id":"get_muzzle", "op":"call", "target":"GetComponentByClass", 
 "inputs":{"ComponentClass":"ArrowComponent"}}
```

Then wire `@get_muzzle.auto` into GetWorldTransform's Target pin.

Interestingly, the validation engine at line 102567 already has guidance text for this exact scenario — when it detects an `@ref` that looks like a component name, it says: *"To access a component, add a 'call' step targeting GetComponentByClass and reference that step's output."* But this message only fires for malformed `@ref` values, not for the `get_var` approach the AI used.

### The fix

**Short term (reference content):** Add a reference entry the AI can find via `olive.reference()`:

```
TAGS: component reference target get muzzle arrow scene getcomponentbyclass
---
To get a component's transform or call functions on it, use GetComponentByClass:
  {"step_id":"get_comp", "op":"call", "target":"GetComponentByClass",
   "inputs":{"ComponentClass":"ArrowComponent"}}
  {"step_id":"get_tf", "op":"call", "target":"GetWorldTransform",
   "inputs":{"Target":"@get_comp.auto"}}
Do NOT use get_var for components — components are not variables.
Do NOT invent functions like "GetMuzzlePoint" — use GetComponentByClass.
```

**Medium term (resolver guard):** In the plan resolver, when `op:"get_var"` targets a name that matches a component on the blueprint (check the SCS/component tree) but NOT a variable, reject the step with a clear error:

```
"'MuzzlePoint' is a component, not a variable. To access a component, 
use op:'call' with target:'GetComponentByClass' and inputs:{ComponentClass:'ArrowComponent'}."
```

This is better than silently creating a broken GetVariable node. The AI gets an actionable error message and can self-correct.

**Long term (plan schema):** Consider adding component access as a first-class plan op or input pattern so the AI doesn't need to know the GetComponentByClass dance. For example, `"inputs":{"Target":"$MuzzlePoint"}` where `$` prefix means "resolve as component reference." The executor would auto-insert a GetComponentByClass node. But this is a bigger change and not needed immediately.

**File (short term):** Reference content file on disk  
**File (medium term):** `OliveBlueprintPlanResolver.cpp`, in the `get_var` resolution path

---

## Fix 3: AI Ignores Compile Error and Declares Success

### The problem

The BP_Gun plan applied successfully (11 nodes, 9 connections, 0 failures). The executor then auto-compiled the blueprint, which produced 1 error:

```
[Compiler] This blueprint (self) is not a SceneComponent, therefore 'Target' must have a connection.
```

This error was included in the tool result sent back to the AI. The AI received it, but on iteration 5 it simply responded with a summary declaring everything done — it never attempted to read the graph, identify the missing Target wire, or fix it.

The outcome was `outcome=0` (Completed) even though BP_Gun has a compile error. The user opens the blueprint and sees a broken graph.

### The fix

**Option A (self-correction policy):** In `FOliveSelfCorrectionPolicy`, check if a tool result from `apply_plan_json` or `blueprint.compile` contains compile errors. If it does, inject a correction note:

```
"Compilation failed with errors. Read the compile errors, identify which node 
has the problem, and fix it using connect_pins or set_pin_default before finishing."
```

This would trigger the self-correction loop, giving the AI another iteration to fix the issue instead of stopping.

**Option B (CLI prompt, simpler):** Add one line to the CLI system prompt rules:

```
If apply_plan_json or compile returns errors, fix them before declaring done.
```

**Option C (both):** Add the prompt line AND the self-correction policy check. Belt and suspenders.

Recommend Option A — it works without prompt space and forces the AI to address compile errors programmatically. The current self-correction only triggers on `bSuccess=false`, but compile errors come back inside a successful tool result (the plan applied fine, the compilation afterward failed). The policy needs to also check for compile errors in the result data.

**File:** `OliveSelfCorrectionPolicy.cpp` — add a check for compile errors in successful tool results from `blueprint.apply_plan_json`

---

## Priority

| Fix | Impact | Why |
|-----|--------|-----|
| **Fix 2** (component reference) | **Root cause** | This is why the graph is broken. Without it, the AI will hit this every time a plan needs to use a component. |
| **Fix 3** (compile error handling) | **Safety net** | Even if Fix 2 resolves the component issue, other compile errors will happen. The AI must not ignore them. |
| **Fix 1** (orphan cleanup) | **Cosmetic but confusing** | Orphan nodes make the graph messy and confuse users. Lower priority but easy to implement. |
