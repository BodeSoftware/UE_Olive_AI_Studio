# Olive AI — Unlock AI Freedom Plan

## Overview

Three categories of changes to remove false restrictions and unlock capabilities the AI currently can't access. Ordered by execution priority.

---

## Category 1: Fix the False Wall (Components as Variables)

**Problem:** `BlueprintHasVariable()` only checks `Blueprint->NewVariables`, missing SCS component variables. In UE5, every SCS component IS a variable on the generated class. This single wrong check cascades into resolver rejections, misleading recipes, a bolted-on lenient fallback, and 3x retry loops that burn the iteration budget.

### Change 1.1 — Fix `BlueprintHasVariable()`

**File:** `OliveBlueprintPlanResolver.cpp` (~line 33836)

Replace the function:

```cpp
bool BlueprintHasVariable(const UBlueprint* Blueprint, const FString& VariableName)
{
    if (!Blueprint) return false;

    // Check explicit Blueprint variables
    for (const FBPVariableDescription& Var : Blueprint->NewVariables)
    {
        if (Var.VarName.ToString() == VariableName)
            return true;
    }

    // Check SCS component variables (components ARE variables on the generated class)
    if (Blueprint->SimpleConstructionScript)
    {
        TArray<USCS_Node*> AllNodes = Blueprint->SimpleConstructionScript->GetAllNodes();
        for (USCS_Node* Node : AllNodes)
        {
            if (Node && Node->GetVariableName().ToString() == VariableName)
                return true;
        }
    }

    return false;
}
```

**Why:** Root cause fix. After this, `get_var "MuzzlePoint"` passes the normal variable check — no special path needed.

### Change 1.2 — Remove Lenient Fallback from `ResolveGetVarOp`

**File:** `OliveBlueprintPlanResolver.cpp` (~lines 34396-34443)

Delete the entire `if (BP->SimpleConstructionScript)` block inside the `if (!bFoundInParent)` branch of `ResolveGetVarOp`. This is the block that does lenient component fallback with warnings and `return true`.

**Why:** Dead code once 1.1 lands. The normal check path now handles components. Keeping it means a duplicate SCS traversal and confusing warning messages.

### Change 1.3 — Update `set_var` Rejection Message

**File:** `OliveBlueprintPlanResolver.cpp` (~line 34558)

Change the error message from:
```
'%s' is a component (class: %s), not a variable.
```
To:
```
'%s' is a component (class: %s). Components are read-only — use get_var to read them, but you cannot set_var on a component reference.
```

**Why:** The old message says "not a variable" which is now wrong (they ARE variables, just read-only). The new message is accurate and tells the AI exactly what to do instead.

### Change 1.4 — Update `component_reference.txt` Recipe

**File:** `Content/SystemPrompts/Knowledge/recipes/blueprint/component_reference.txt`

Replace entire content:
```
TAGS: component reference target getcomponentbyclass scene muzzle arrow transform access
---
To reference a named component in plan JSON, use get_var with the component name:
  {"step_id":"get_muzzle", "op":"get_var", "target":"MuzzlePoint"}
  {"step_id":"get_tf", "op":"call", "target":"GetWorldTransform",
   "inputs":{"Target":"@get_muzzle.auto"}}

Components added via add_component ARE variables — get_var works directly.
You CANNOT use set_var on components (they are read-only references).

Alternative: use GetComponentByClass when you need a component by type rather than name:
  {"step_id":"get_comp", "op":"call", "target":"GetComponentByClass",
   "inputs":{"ComponentClass":"ArrowComponent"}}
Do NOT invent functions like "GetMuzzlePoint" — use get_var or GetComponentByClass.
Common component classes: StaticMeshComponent, ArrowComponent, BoxComponent,
SphereComponent, CapsuleComponent, AudioComponent, ParticleSystemComponent,
SkeletalMeshComponent, ProjectileMovementComponent, SceneComponent.
```

**Why:** The old recipe says "Do NOT use get_var for components. Components are NOT variables." — directly wrong after 1.1 and actively misleading if the AI calls `olive.get_recipe("component reference")`.

### What to keep from the prior AI changes (no action needed)

- **Step-level error diagnostics** (Change 1 from prior) — independently valuable for all resolve failures. Keep.
- **Self-correction consuming step_errors** (Change 3 from prior) — independently valuable. Keep.

---

## Category 2: Expand the Ops Vocabulary (Macro Nodes)

**Problem:** `OliveNodeTypes` defines `WhileLoop`, `DoOnce`, `FlipFlop`, `Gate` but `OlivePlanOps` doesn't include them. The AI gets `PLAN_INVALID_OP` if it tries `"op":"do_once"`. These are common Blueprint patterns with no workaround — they're macros, not UFUNCTIONs, so `op:"call"` can't reach them either. The node catalog knows about them but the AI has zero way to create them.

### Change 2.1 — Add Plan Ops

**File:** `OliveIRBlueprintPlan.h` (OlivePlanOps namespace, ~line 104700)

Add:
```cpp
/** While loop (loop while condition is true) */
const FString WhileLoop = TEXT("while_loop");

/** Do Once (execute output only once, can be reset) */
const FString DoOnce = TEXT("do_once");

/** Flip Flop (alternate between two outputs) */
const FString FlipFlop = TEXT("flip_flop");

/** Gate (open/close to control execution flow) */
const FString Gate = TEXT("gate");
```

Also update `IsValidOp()` and `GetAllOps()` to include these.

### Change 2.2 — Add Macro Instance Helper to NodeFactory

**File:** `OliveNodeFactory.cpp`

Add a helper function to create macro instance nodes:

```cpp
UK2Node* FOliveNodeFactory::CreateMacroInstanceNode(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    const FString& MacroName)
{
    // Load the StandardMacros library
    static const TCHAR* StandardMacrosPath =
        TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros");
    UBlueprint* MacroLib = LoadObject<UBlueprint>(nullptr, StandardMacrosPath);
    if (!MacroLib)
    {
        LastError = FString::Printf(
            TEXT("Failed to load StandardMacros library for macro '%s'"), *MacroName);
        return nullptr;
    }

    // Find the macro graph by name
    UEdGraph* MacroGraph = nullptr;
    for (UEdGraph* Graph : MacroLib->MacroGraphs)
    {
        if (Graph && Graph->GetName() == MacroName)
        {
            MacroGraph = Graph;
            break;
        }
    }

    if (!MacroGraph)
    {
        LastError = FString::Printf(
            TEXT("Macro '%s' not found in StandardMacros"), *MacroName);
        return nullptr;
    }

    // Create the macro instance node
    UK2Node_MacroInstance* MacroNode = NewObject<UK2Node_MacroInstance>(Graph);
    MacroNode->SetMacroGraph(MacroGraph);
    MacroNode->AllocateDefaultPins();
    Graph->AddNode(MacroNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
    return MacroNode;
}
```

**Include:** `#include "K2Node_MacroInstance.h"` in the factory cpp.

### Change 2.3 — Register Factory Creators

**File:** `OliveNodeFactory.cpp` (InitNodeCreators, ~line 49126)

Add registrations:
```cpp
NodeCreators.Add(OliveNodeTypes::WhileLoop,
    [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
    return CreateMacroInstanceNode(BP, G, TEXT("WhileLoop"));
});

NodeCreators.Add(OliveNodeTypes::DoOnce,
    [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
    return CreateMacroInstanceNode(BP, G, TEXT("DoOnce"));
});

NodeCreators.Add(OliveNodeTypes::FlipFlop,
    [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
    return CreateMacroInstanceNode(BP, G, TEXT("FlipFlop"));
});

NodeCreators.Add(OliveNodeTypes::Gate,
    [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
    return CreateMacroInstanceNode(BP, G, TEXT("Gate"));
});
```

Also update the existing ForLoop and ForEachLoop creators to use `CreateMacroInstanceNode` as a fallback when `FindFunction` fails (replacing the current `LastError = "macro support not yet implemented"`).

### Change 2.4 — Add Resolver Dispatch

**File:** `OliveBlueprintPlanResolver.cpp` (ResolveStep, ~line 34065)

Add cases in the op dispatch:
```cpp
else if (Op == OlivePlanOps::WhileLoop)
{
    bResult = ResolveSimpleOp(Step, OliveNodeTypes::WhileLoop, OutResolved);
}
else if (Op == OlivePlanOps::DoOnce)
{
    bResult = ResolveSimpleOp(Step, OliveNodeTypes::DoOnce, OutResolved);
}
else if (Op == OlivePlanOps::FlipFlop)
{
    bResult = ResolveSimpleOp(Step, OliveNodeTypes::FlipFlop, OutResolved);
}
else if (Op == OlivePlanOps::Gate)
{
    bResult = ResolveSimpleOp(Step, OliveNodeTypes::Gate, OutResolved);
}
```

### Change 2.5 — Update System Prompt Ops List

**File:** `cli_blueprint.txt` and `blueprint_authoring.txt`

Add the new ops to the `## Ops` line:
```
event, custom_event, call, get_var, set_var, branch, sequence, cast,
for_loop, for_each_loop, while_loop, do_once, flip_flop, gate,
delay, is_valid, print_string, spawn_actor, make_struct, break_struct,
return, comment
```

### Verification

The macro names in StandardMacros are: `DoOnce`, `FlipFlop`, `Gate`, `WhileLoop`, `ForLoop`, `ForEachLoop`. These names need to match exactly in `CreateMacroInstanceNode`. Verify by loading the asset in editor and checking `MacroGraphs` array names. If names differ (e.g., `Do Once` with space), adjust accordingly.

---

## Category 3: Auto-Resolve Component Targets

**Problem:** When the AI calls a component function (e.g., `GetWorldTransform`) without wiring the Target pin, Phase 0 catches it with `COMPONENT_FUNCTION_ON_ACTOR` and errors. The AI must then self-correct, burning an iteration. When there's an unambiguous component match, we can auto-fix instead.

### Change 3.1 — Auto-Insert Component Reference in Phase 0

**File:** `OlivePlanValidator.cpp` (CheckComponentFunctionTargets, ~line 38601)

Instead of erroring when Target isn't wired, attempt auto-resolution:

```cpp
// Current: if (!bHasTargetWired) -> Error
// New: if (!bHasTargetWired) -> try auto-resolve, then error if ambiguous

if (!bHasTargetWired)
{
    // Check if we can unambiguously determine the component
    UClass* RequiredClass = Resolved.ResolvedOwningClass;

    if (Context.Blueprint->SimpleConstructionScript)
    {
        TArray<USCS_Node*> Candidates;
        TArray<USCS_Node*> AllNodes =
            Context.Blueprint->SimpleConstructionScript->GetAllNodes();

        for (USCS_Node* Node : AllNodes)
        {
            if (!Node || !Node->ComponentClass) continue;
            if (Node->ComponentClass->IsChildOf(RequiredClass))
            {
                Candidates.Add(Node);
            }
        }

        if (Candidates.Num() == 1)
        {
            // Unambiguous: exactly one component of the right type.
            // Add a warning (not error) and let the executor auto-wire.
            FString CompName = Candidates[0]->GetVariableName().ToString();
            Result.Warnings.Add(FString::Printf(
                TEXT("Step '%s': Auto-resolved Target to component '%s' "
                     "(%s). For clarity, add: \"inputs\":{\"Target\":"
                     "\"@<get_var_step>.auto\"} explicitly."),
                *Resolved.StepId, *CompName, *ClassDisplay));

            // Store the auto-resolve info for the executor to consume
            Result.AutoResolvedTargets.Add(Resolved.StepId, CompName);
            continue; // Skip error
        }
    }

    // Ambiguous or no candidates — emit the existing error
    Result.Errors.Add(FOliveIRBlueprintPlanError::MakeStepError( ... ));
}
```

**Note:** This requires adding `TMap<FString, FString> AutoResolvedTargets` to `FOlivePlanValidationResult` and having the executor consume it during Phase 4 (data wiring) to inject the component reference.

### Change 3.2 — Executor Consumes Auto-Resolved Targets

**File:** `OlivePlanExecutor.cpp`

Before Phase 4 (wire data), check `ValidationResult.AutoResolvedTargets`. For each entry:
1. Create a `VariableGet` node for the component variable
2. Add it to the step manifest
3. Wire its output to the Target pin of the component function step

This is more involved since it modifies the plan mid-execution. An alternative simpler approach: instead of auto-creating nodes, **mutate the plan before resolution** — inject a synthetic `get_var` step and add the Target input to the calling step.

### Simpler Alternative for 3.1/3.2 — Pre-Resolution Plan Mutation

Instead of the validator auto-resolving post-resolution, add a pre-resolution pass that detects component function calls without Target inputs and injects the missing steps:

**File:** `OliveBlueprintPlanResolver.cpp` (in `Resolve()`, before the per-step loop)

```cpp
// Pre-pass: detect component functions likely missing Target wiring.
// If the Blueprint has exactly one component matching the function's owning class,
// inject a get_var step and wire it.
ExpandMissingComponentTargets(Plan, Blueprint);
```

This is cleaner because resolution then sees a complete plan and no special executor logic is needed.

### Assessment

Category 3 is the most complex of the three and has the most edge cases (multiple components of same type, nested components, etc.). The single-candidate auto-resolve is safe. Multi-candidate should error with the existing guidance. Consider implementing just the single-candidate case first and leaving multi-candidate as an error.

---

## Execution Order

| Priority | Change | Risk | Effort |
|----------|--------|------|--------|
| 1 | 1.1 Fix BlueprintHasVariable | Low | Small — one function edit |
| 2 | 1.2 Remove lenient fallback | Low | Small — delete code |
| 3 | 1.3 Fix set_var message | Low | Trivial — string change |
| 4 | 1.4 Update recipe | Low | Small — text file edit |
| 5 | 2.1 Add plan ops | Low | Small — namespace additions |
| 6 | 2.2 Macro instance helper | Medium | Medium — new factory function, needs UE asset loading |
| 7 | 2.3 Register factory creators | Low | Small — registration calls |
| 8 | 2.4 Resolver dispatch | Low | Small — else-if branches |
| 9 | 2.5 Update prompt ops list | Low | Small — text edit |
| 10 | 3.1/3.2 Auto-resolve targets | Medium | Medium-Large — plan mutation or executor changes |

**Do 1.1–1.4 first** (unblocks the current gun+bullet test case immediately). Then 2.1–2.5 (expands AI vocabulary). Then 3.x (optimization, can be deferred).

---

## Verification Checklist

After all changes, the gun+bullet test case should:

- [ ] `get_var "MuzzlePoint"` resolves without errors or fallback warnings
- [ ] `GetWorldTransform` with `Target: @get_muzzle.auto` passes Phase 0
- [ ] BP_Bullet plan resolves and executes fully
- [ ] BP_Gun plan resolves and executes fully (no more 3x retry loop)
- [ ] `set_var "MuzzlePoint"` still correctly rejects with accurate message
- [ ] `olive.get_recipe("component reference")` returns updated recipe
- [ ] `op:"do_once"` creates a DoOnce macro instance node with correct pins
- [ ] `op:"flip_flop"` creates a FlipFlop with A/B exec outputs
- [ ] Component function without Target on a Blueprint with exactly one matching component auto-resolves (Category 3, if implemented)
