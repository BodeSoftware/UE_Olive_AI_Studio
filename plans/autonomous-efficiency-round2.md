# Autonomous Efficiency Round 2 -- Implementation Plan

**Context:** Log analysis of a "create a gun blueprint that shoots bullets" autonomous task (10.5 min, 47 tool calls) revealed 4 issues responsible for the remaining inefficiency. Previous prompt fixes (v2.0 schema, read-after-template, fallback strategy) worked but these new issues dominate.

---

## Task 1: Fix Gun Template Event Dispatcher Calls (HIGHEST IMPACT, ~5 min wasted)

### Root Cause

The gun template (`Content/Templates/factory/gun.json`) embeds plan_json inside three functions (Fire, Reload, ResetCanFire). The Fire and Reload plans include steps like:

```json
{"step_id": "fire_event", "op": "call", "target": "OnFired", "inputs": {"RemainingAmmo": "@decrement.auto"}}
{"step_id": "fire_empty", "op": "call", "target": "OnAmmoEmpty"}
{"step_id": "fire_reloaded", "op": "call", "target": "OnReloaded", "inputs": {"NewAmmo": "@get_max.auto"}}
```

`op: "call"` routes through `ResolveCallOp` which uses `FOliveFunctionResolver::Resolve()` and then `FOliveNodeFactory::FindFunction()`. Event dispatchers are **not** UFunctions -- they are `FMulticastDelegateProperty` instances on the Blueprint class. `FindFunction` searches UFunction registries (library classes, parent class, GeneratedClass) and will never find `OnFired` / `OnAmmoEmpty` / `OnReloaded`.

In Unreal, "calling" (broadcasting) an event dispatcher creates a `UK2Node_CallDelegate` node (inherits from `UK2Node_BaseMCDelegate`), not a `UK2Node_CallFunction` node. The `DelegateReference` member on `UK2Node_BaseMCDelegate` is set via `SetFromField()` using the `FMulticastDelegateProperty*` found on the Blueprint's generated class.

The plan system has zero support for `UK2Node_CallDelegate` creation.

### Recommended Fix: Add `call_delegate` op to plan vocabulary

This is the cleanest solution because it properly represents the UE concept and lets the AI (and templates) distinguish between function calls and delegate broadcasts.

#### Changes Required

**File 1: `Source/OliveAIRuntime/Public/IR/BlueprintPlanIR.h`**

Add a new op constant to the `OlivePlanOps` namespace:

```cpp
/** Call (broadcast) an event dispatcher (multicast delegate) */
const FString CallDelegate = TEXT("call_delegate");
```

Also add `CallDelegate` to the `IsValidOp()` implementation and `GetAllOps()` set.

**File 2: `Source/OliveAIRuntime/Private/IR/BlueprintPlanIR.cpp`** (or wherever `IsValidOp`/`GetAllOps` are implemented)

Add `CallDelegate` to the valid ops set. Check this file:

```
Source/OliveAIRuntime/Private/IR/OliveIRSchema.cpp
```

Search for `GetAllOps` or the op set construction to find the right file.

**File 3: `Source/OliveAIEditor/Blueprint/Public/Plan/OliveBlueprintPlanResolver.h`**

Add declaration:

```cpp
static bool ResolveCallDelegateOp(
    const FOliveIRBlueprintPlanStep& Step,
    UBlueprint* BP,
    int32 Idx,
    FOliveResolvedStep& Out,
    TArray<FOliveIRBlueprintPlanError>& Errors);
```

**File 4: `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`**

In `ResolveStep()`, add a case for `OlivePlanOps::CallDelegate`:

```cpp
else if (Op == OlivePlanOps::CallDelegate)
{
    bResult = ResolveCallDelegateOp(Step, Blueprint, StepIndex, OutResolved, OutErrors);
}
```

Implement `ResolveCallDelegateOp`:
- Target = dispatcher name (e.g., "OnFired")
- Set `Out.NodeType` to a new constant `OliveNodeTypes::CallDelegate` (value = `"CallDelegate"`)
- Set `Out.Properties["delegate_name"] = Step.Target`
- Validate: search `Blueprint->NewVariables` for a variable with `FMulticastDelegateProperty` matching the target name. The variable's category `EOliveIRTypeCategory::MulticastDelegate` indicates a dispatcher. Alternatively, iterate `Blueprint->NewVariables` checking if `VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate` and `VarName == Step.Target`.
- `Out.bIsPure = false` (delegate calls have exec pins)
- Error if dispatcher not found: code `DELEGATE_NOT_FOUND`, suggest available dispatchers

**File 5: `Source/OliveAIEditor/Blueprint/Public/Writer/OliveNodeFactory.h`**

Add to `OliveNodeTypes` namespace:

```cpp
const FString CallDelegate = TEXT("CallDelegate");
```

**File 6: `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp`**

Register a new creator in `InitializeNodeCreators()`:

```cpp
NodeCreators.Add(OliveNodeTypes::CallDelegate, [this](UBlueprint* BP, UEdGraph* G, const TMap<FString, FString>& P) -> UEdGraphNode* {
    return CreateCallDelegateNode(BP, G, P);
});
```

Implement `CreateCallDelegateNode`:

```cpp
UK2Node* FOliveNodeFactory::CreateCallDelegateNode(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    const TMap<FString, FString>& Properties)
{
    const FString* DelegateNamePtr = Properties.Find(TEXT("delegate_name"));
    if (!DelegateNamePtr || DelegateNamePtr->IsEmpty())
    {
        LastError = TEXT("CallDelegate node requires 'delegate_name' property");
        return nullptr;
    }

    // Find the multicast delegate property on the Blueprint's generated class
    // Must compile skeleton first so the property exists
    UClass* GenClass = Blueprint->SkeletonGeneratedClass;
    if (!GenClass) GenClass = Blueprint->GeneratedClass;
    if (!GenClass)
    {
        LastError = TEXT("Blueprint has no generated class for delegate lookup");
        return nullptr;
    }

    FMulticastDelegateProperty* DelegateProp = nullptr;
    for (TFieldIterator<FMulticastDelegateProperty> PropIt(GenClass); PropIt; ++PropIt)
    {
        if (PropIt->GetFName() == FName(**DelegateNamePtr))
        {
            DelegateProp = *PropIt;
            break;
        }
    }

    if (!DelegateProp)
    {
        LastError = FString::Printf(TEXT("Event dispatcher '%s' not found on Blueprint class"), **DelegateNamePtr);
        return nullptr;
    }

    UK2Node_CallDelegate* CallDelegateNode = NewObject<UK2Node_CallDelegate>(Graph);
    // SetFromProperty sets the DelegateReference and bSelfContext
    CallDelegateNode->SetFromProperty(DelegateProp, /*bSelfContext=*/true, GenClass);
    CallDelegateNode->AllocateDefaultPins();
    Graph->AddNode(CallDelegateNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

    return CallDelegateNode;
}
```

Required includes in `OliveNodeFactory.cpp`:
```cpp
#include "K2Node_CallDelegate.h"
```

**File 7: `Content/Templates/factory/gun.json`**

Replace all `"op": "call"` steps that target dispatchers with `"op": "call_delegate"`:

- Line 236-242: `fire_event` step: change `"op": "call"` to `"op": "call_delegate"`
- Line 286-289: `fire_empty` step: change `"op": "call"` to `"op": "call_delegate"`
- Line 345-351: `fire_reloaded` step: change `"op": "call"` to `"op": "call_delegate"`

**File 8: `AGENTS.md`**

In the Operations line (line 52), add `call_delegate` to the list.

In the execution wiring rules or a new note, add:
```
- `call_delegate` broadcasts an event dispatcher. Use for dispatchers created via `blueprint.add_event_dispatcher`.
```

**File 9: `Content/SystemPrompts/Knowledge/cli_blueprint.txt`** (if it has an ops list)

Add `call_delegate` to any ops enumeration there as well.

#### Additional Issue: `K2_SetTimerDelegate` in Fire plan

The Fire plan also uses `K2_SetTimerDelegate` (step `set_timer`, line 268-275). This function expects a delegate pin, which is extremely complex to wire via plan JSON. The template should use a simpler pattern.

**Recommended simplification for the Fire plan:**

Replace the `K2_SetTimerDelegate` + `calc_cooldown` + `get_fire_rate` + `disable_fire`/`ResetCanFire` timer pattern with a simpler `Delay` node:

```json
{
    "step_id": "cooldown",
    "op": "delay",
    "inputs": {"Duration": "@calc_cooldown.auto"},
    "exec_after": "set_ammo"
},
{
    "step_id": "enable_fire",
    "op": "set_var",
    "target": "bCanFire",
    "inputs": {"value": "true"},
    "exec_after": "cooldown"
}
```

This eliminates:
- The `disable_fire` step (merged into the flow: fire_event -> cooldown -> enable_fire)
- The `set_timer` step (replaced by `delay`)
- The entire `ResetCanFire` function (no longer needed since delay is inline)

However, `delay` is latent and cannot be used in a function graph. The coder should verify whether UE allows latent nodes in Blueprint functions (it does NOT by default). If not, the timer pattern must stay but should use `SetTimerByFunctionName` instead of `K2_SetTimerDelegate`:

```json
{
    "step_id": "set_timer",
    "op": "call",
    "target": "SetTimerByFunctionName",
    "inputs": {
        "FunctionName": "ResetCanFire",
        "Time": "@calc_cooldown.auto",
        "bLooping": "false"
    },
    "exec_after": "disable_fire"
}
```

This is a regular function call that the resolver CAN find (it's on `UKismetSystemLibrary` as `K2_SetTimerByFunctionName`). The coder should check the exact function name via the resolver/catalog.

### Verification

1. Apply the gun template. All 3 functions (Fire, Reload, ResetCanFire) should have their plans execute without errors.
2. The `OnFired`, `OnAmmoEmpty`, `OnReloaded` delegate call nodes should appear in the function graphs.
3. The Blueprint should compile clean after template application.
4. Run a standalone `apply_plan_json` with a `call_delegate` step targeting an event dispatcher -- should succeed.

---

## Task 2: Fix ExpandComponentRefs Rewrite Not Reaching Executor (HIGH IMPACT, ~3 min wasted)

### Root Cause

The bug is a **data flow disconnect** between the resolver and executor:

1. `FOliveBlueprintPlanResolver::Resolve()` creates a **local copy**: `FOliveIRBlueprintPlan MutablePlan = Plan;` (line 219)
2. `ExpandComponentRefs()` rewrites inputs in `MutablePlan` (e.g., `@MuzzlePoint` -> `@_synth_getcomp_muzzlepoint.auto`)
3. `ResolveStep()` iterates `MutablePlan.Steps` -- the expanded steps resolve correctly, including the synthesized `_synth_getcomp_muzzlepoint` step
4. The resolver outputs `FOlivePlanResolveResult` which contains `ResolvedSteps` (node creation metadata) but **NOT the mutated plan**
5. The tool handler then calls `FOlivePlanExecutor::Execute(Plan, ResolveResult.ResolvedSteps, ...)` passing the **ORIGINAL unmutated `Plan`**
6. `PhaseWireData` iterates `Plan.Steps` and encounters `@MuzzlePoint` (no dot) -- `ParseDataRef` fails because it requires `@stepId.pinHint` format

Preview succeeds because it only calls `Resolve()` and computes a diff -- it never reaches `PhaseWireData`.

### Fix

Add a `MutatedPlan` field to `FOlivePlanResolveResult` so the expanded plan survives past the resolver.

**File 1: `Source/OliveAIEditor/Blueprint/Public/Plan/OliveBlueprintPlanResolver.h`**

Add to `FOlivePlanResolveResult`:

```cpp
/**
 * The plan after all pre-processing expansions (ExpandComponentRefs,
 * ExpandPlanInputs, ExpandBranchConditions). This is the plan that should
 * be passed to the executor, NOT the original plan.
 * Empty if resolution failed.
 */
FOliveIRBlueprintPlan ExpandedPlan;
```

**File 2: `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`**

At the end of `Resolve()`, after all expansion passes and step resolution, assign:

```cpp
Result.ExpandedPlan = MoveTemp(MutablePlan);
```

This should go around line 263, just before the final return.

**File 3: `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`**

In `HandleBlueprintApplyPlanJson`, everywhere the original `Plan` is passed to code that reads step inputs, replace with `ResolveResult.ExpandedPlan`. The key call sites are:

1. **Line ~6693**: `CollapseExecThroughPureSteps(Plan, ...)` -- change to `CollapseExecThroughPureSteps(ResolveResult.ExpandedPlan, ...)`

2. **Line ~6697**: `AutoFixExecConflicts(Plan, ...)` -- change to `AutoFixExecConflicts(ResolveResult.ExpandedPlan, ...)`

3. **Line ~6703**: `FOlivePlanValidator::Validate(Plan, ...)` -- change to `Validate(ResolveResult.ExpandedPlan, ...)`

4. Inside the executor lambda (the v2.0 path), wherever `Plan` is captured and passed to `PlanExecutor.Execute(Plan, ...)` -- change to pass `ResolveResult.ExpandedPlan` instead.

Also do the same in **`HandleBlueprintPreviewPlanJson`** for consistency. The preview handler also calls `CollapseExecThroughPureSteps`, `AutoFixExecConflicts`, `Validate`, and `ComputePlanDiff` with the original `Plan`. While preview doesn't hit the executor bug, using the expanded plan ensures fingerprint/diff accuracy.

**IMPORTANT**: The `Plan` variable is also used for schema version checks, step counts in logging, and `ComputePlanFingerprint`. These should also use `ResolveResult.ExpandedPlan` since it may have additional synthesized steps. Double-check every reference to the local `Plan` variable in both handlers.

**File 4: Template system** (`Source/OliveAIEditor/Blueprint/Private/Template/OliveTemplateSystem.cpp`)

The template system also calls `Resolve()` and then `Execute()`. At lines ~1227-1263 and ~1355-1363, it follows the same pattern:

```cpp
FOlivePlanResolveResult ResolveResult = FOliveBlueprintPlanResolver::Resolve(Plan, Blueprint, FuncContext);
...
FOliveIRBlueprintPlanResult PlanResult = PlanExecutor.Execute(Plan, ResolveResult.ResolvedSteps, ...);
```

Change both to:
```cpp
FOliveIRBlueprintPlanResult PlanResult = PlanExecutor.Execute(ResolveResult.ExpandedPlan, ResolveResult.ResolvedSteps, ...);
```

### Verification

1. Create a Blueprint with an SCS component named "MuzzlePoint"
2. Apply a plan with `"Target": "@MuzzlePoint"` (bare, no dot)
3. Both preview AND apply should succeed
4. Apply a plan with `"Target": "@MuzzlePoint.auto"` (with dot) -- should also still work (this case was already handled by the existing dotted-component-ref path)
5. Verify the gun template's function plans work correctly (since they may also reference components)

---

## Task 3: Strengthen Recipe Prompt to Mandatory (MEDIUM IMPACT)

### Root Cause

In `AGENTS.md` line 24: `Unfamiliar pattern: Call olive.get_recipe before writing plan_json to look up the correct approach.`

The word "unfamiliar" is subjective -- the AI never thinks a pattern is unfamiliar to it. The prompt must make recipe lookup a mandatory pre-step, not a conditional one.

### Changes Required

**File 1: `AGENTS.md`**

Replace line 24:
```
**Unfamiliar pattern:** Call `olive.get_recipe` before writing plan_json to look up the correct approach.
```

With:
```
**Before your first plan_json for each Blueprint:** Call `olive.get_recipe` with the pattern you need (e.g., "fire weapon", "spawn projectile", "health component"). Recipes contain tested wiring patterns that prevent common errors.
```

Also add to the Important Rules section (around line 73-82), as a new bullet:
```
- **Always call `olive.get_recipe` before your first `apply_plan_json`** for each Blueprint. Recipes contain tested wiring patterns. Skip only if you already used `create_from_template` which embeds plans.
```

**File 2: `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp`**

In `SetupAutonomousSandbox()`, add to the Critical Rules section (around line 260-270). After the line about schema_version "2.0":

```cpp
ClaudeMd += TEXT("- Before writing your first plan_json for each Blueprint, call olive.get_recipe to look up the correct wiring pattern. Skip only if create_from_template already provided the logic.\n");
```

Also add to the hardcoded CLAUDE.md completion guidance (around line 267):

```cpp
ClaudeMd += TEXT("- Once ALL Blueprints compile with 0 errors and 0 warnings, the task is COMPLETE. Immediately stop and report what you built.\n");
```

(This also covers Task 4 -- see below.)

**File 3: `Content/SystemPrompts/Knowledge/recipe_routing.txt`**

Currently line 2 says: `- Use olive.get_recipe(query) to look up patterns when stuck or encountering errors.`

Change to:
```
- ALWAYS call olive.get_recipe(query) before your first plan_json for each Blueprint. Contains tested wiring patterns. Skip only after create_from_template.
```

### Verification

1. Delete the autonomous sandbox and let it be recreated
2. Check that the generated CLAUDE.md in `Saved/OliveAI/AgentSandbox/CLAUDE.md` contains the mandatory recipe language
3. Start a new autonomous task -- verify the AI calls `get_recipe` before its first `plan_json`

---

## Task 4: Add Completion Detection Guidance (LOW-MEDIUM IMPACT, ~1 min wasted)

### Root Cause

After all Blueprints compiled clean, the AI continued for another minute doing unnecessary `get_node_pins`, `add_node`, `remove_node`, and an unapplied preview. It lacks a clear "done condition" telling it when to stop.

### Changes Required

**File 1: `AGENTS.md`**

Add to Important Rules section (around line 73-82), as a new bullet:

```
- **Done condition:** Once ALL Blueprints compile with 0 errors and 0 warnings, the task is complete. Stop immediately and report what you built (asset paths, key features, any notes). Do not add cosmetic changes, extra previews, or verification reads after a clean compile.
```

**File 2: `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp`**

Already covered in Task 3's sandbox change. The line to add:

```cpp
ClaudeMd += TEXT("- Once ALL Blueprints compile with 0 errors and 0 warnings, the task is COMPLETE. Immediately stop and report what you built.\n");
```

Place this in the Critical Rules block, right after the "Complete the FULL task" line.

**File 3: Sandbox CLAUDE.md re-emphasis**

In the full `ClaudeMd` string, after the "Complete the FULL task" line (line 267), add the done-condition line. This creates a one-two punch: "Complete the full task" tells it to not stop early, and "Once all compile clean, stop" tells it when to stop.

The ordering matters:
```
- Complete the FULL task: create structures, wire graph logic, compile, and verify. Do not stop partway.
- Once ALL Blueprints compile with 0 errors and 0 warnings, the task is COMPLETE. Immediately stop and report what you built.
```

### Verification

1. Run an autonomous task that creates 1-2 Blueprints
2. After clean compile, the AI should stop within 1-2 tool calls (at most one final read to verify)
3. The AI should produce a summary of what was built

---

## Implementation Order

| Priority | Task | Files Changed | Estimated Effort |
|----------|------|---------------|-----------------|
| 1 | Task 1: call_delegate op | BlueprintPlanIR.h, OliveIRSchema.cpp, OliveBlueprintPlanResolver.h/.cpp, OliveNodeFactory.h/.cpp, gun.json, AGENTS.md, cli_blueprint.txt | ~2 hours |
| 2 | Task 2: ExpandedPlan propagation | OliveBlueprintPlanResolver.h/.cpp, OliveBlueprintToolHandlers.cpp, OliveTemplateSystem.cpp | ~45 min |
| 3 | Task 3: Recipe prompt | AGENTS.md, OliveCLIProviderBase.cpp, recipe_routing.txt | ~15 min |
| 4 | Task 4: Done condition | AGENTS.md, OliveCLIProviderBase.cpp | ~10 min (partially overlaps Task 3) |

Tasks 3 and 4 can be done together since they both touch the same two files. Task 2 is independent of Task 1. Task 1 is the most complex and highest impact.

### Parallel lanes:
- **Lane A**: Task 1 (call_delegate op + template fix)
- **Lane B**: Task 2 (expanded plan propagation) + Tasks 3-4 (prompt fixes)

---

## New Error Codes

| Code | Category | Context |
|------|----------|---------|
| `DELEGATE_NOT_FOUND` | A (FixableMistake) | call_delegate target not found in Blueprint event dispatchers |

## New Constants

| Constant | Location | Value |
|----------|----------|-------|
| `OlivePlanOps::CallDelegate` | BlueprintPlanIR.h | `"call_delegate"` |
| `OliveNodeTypes::CallDelegate` | OliveNodeFactory.h | `"CallDelegate"` |
