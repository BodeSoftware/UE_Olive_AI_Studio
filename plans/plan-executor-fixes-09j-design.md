# Plan Executor Fixes 09j — Design

Batch fix for 4 bugs identified in bow-arrow session 09j analysis.
Regression from 80% to 59% plan_json success rate.

---

## Bug #1: GetForwardVector — Executor ignores resolved target_class

**Status: ALREADY FIXED** in commit `e14162e`.

The fix is the `_resolved` flag + `bSkipAlias` mechanism added to:
- `OlivePlanExecutor.cpp` lines 822-831: adds `_resolved` property when `NodeType` is `CallFunction` or `CallDelegate`
- `OliveNodeFactory.cpp` line 1824: reads `_resolved` flag, sets `bSkipAlias = true`
- `FindFunction()` line 2006: skips alias map when `bSkipAliasMap = true`

The resolver already stores `target_class` in `Out.Properties` (line 1467), and the factory reads it (lines 1808-1815). Combined with alias skip, this prevents double-aliasing.

**No further action needed.** Verify this fix works in next test run.

---

## Bug #2: Stale exec pin after plan_json rollback (3 failures + 7 min cascade)

### Root Cause

When `apply_plan_json` fails and the write pipeline rolls back the `FScopedTransaction`, nodes created by this plan are undone. But REUSED nodes (pre-existing custom events, native events) are NOT created within the transaction -- they predate it. During plan execution, wiring operations on reused nodes' pins (Phase 3: WireExec, Phase 4: WireData) modify pin state. If execution fails mid-way, the transaction rollback undoes the NEW nodes/connections but does NOT restore the pin state of reused nodes because those pins were never part of the undo snapshot.

Specifically: `bOrphanedPin` is a UE engine flag (`EdGraphPin.h:337`) set when a node is reconstructed and a pin no longer matches. The likely trigger: during a failed plan_json that later rolls back, wiring operations or intermediate `ReconstructNode()` calls mark pins on reused event nodes as orphaned. After rollback, the new nodes disappear but the orphan flag persists.

The evidence: `fire_evt.then -> check_aim.execute: TypesIncompatible` on retry. The `fire_evt` custom event node was reused (created by a previous successful plan). A subsequent plan_json wired from it, failed, rolled back -- but the exec output pin retained `bOrphanedPin=true`.

### Fix

**Post-failure pin sanitization on reused nodes.**

In `FOlivePlanExecutor::Execute()`, after `AssembleResult()` returns and `bSuccess == false`, iterate all nodes in `Context.ReusedStepIds` and sanitize their pins:

```
Location: OlivePlanExecutor.cpp, Execute() method, after AssembleResult()
```

**Logic:**
1. If `Result.bSuccess == false`, iterate `Context.ReusedStepIds`
2. For each reused step, get the node via `Context.StepToNodePtr`
3. For each pin on the node: if `bOrphanedPin == true`, set it to `false`
4. Log each sanitization for transparency

**Why this is safe:** Reused nodes existed before this plan ran. Any `bOrphanedPin=true` flag that appeared during this plan's execution is by definition stale -- the plan is being rolled back. Pre-existing orphaned pins (from manual editor edits, etc.) would have been set before this plan and should not exist on event nodes in normal operation.

**Alternative considered (snapshot/restore):** Snapshot all pin flags before execution, restore on failure. More precise but significantly more complex and allocates more memory. The sanitize approach is sufficient because:
- Event nodes don't normally have orphaned pins
- The only way a pin becomes orphaned mid-plan is through our execution

### Files to Modify

| File | Change |
|------|--------|
| `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp` | Add pin sanitization block in `Execute()` after `AssembleResult()` |

### Task: Junior

Well-specified mechanical change. Add ~15 lines after `AssembleResult()` in `Execute()`:
```cpp
// Sanitize reused nodes on failure — clear stale bOrphanedPin flags
if (!Result.bSuccess && Context.ReusedStepIds.Num() > 0)
{
    for (const FString& StepId : Context.ReusedStepIds)
    {
        UEdGraphNode* Node = Context.GetNodePtr(StepId);
        if (!Node) continue;

        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->bOrphanedPin)
            {
                Pin->bOrphanedPin = false;
                UE_LOG(LogOlivePlanExecutor, Log,
                    TEXT("Post-failure cleanup: cleared bOrphanedPin on '%s' pin '%s'"),
                    *StepId, *Pin->PinName.ToString());
            }
        }
    }
}
```

Include: `#include "EdGraph/EdGraphPin.h"` (likely already included via existing headers).

### Risk

Low. Only fires on failure path. Only touches reused node pins. Only clears a UI-cosmetic flag that prevents wiring.

### Testing

1. Create a Blueprint with a custom event "TestEvt"
2. Run a plan_json that reuses TestEvt and intentionally fails (e.g., reference a nonexistent variable)
3. Run another plan_json that wires from TestEvt -- should succeed
4. Without the fix: second plan would get TypesIncompatible on the exec pin

---

## Bug #3: break_struct — struct type not reaching the node factory

### Root Cause

The resolver correctly stores `struct_type` in `Out.Properties` (line 2762 of OliveBlueprintPlanResolver.cpp). The factory's `CreateBreakStructNode()` correctly reads `struct_type` from Properties and calls `BreakNode->StructType = Struct` before `AllocateDefaultPins()`.

The evidence shows a 1-pin node was created. The factory code should work: `FindStruct("HitResult")` -> tries "HitResult" -> tries "FHitResult" -> finds `FHitResult` in common structs list. If struct resolution succeeds, AllocateDefaultPins produces 20+ pins.

**Two possible sub-causes to investigate:**

**(A) FindStruct failure at runtime.** `FindFirstObject<UScriptStruct>("HitResult")` may fail if the asset registry hasn't loaded `FHitResult` yet. The common-structs fallback tries `FindFirstObject<UScriptStruct>("FHitResult")`. If both fail, `CreateBreakStructNode` returns nullptr and AddNode returns an error -- but the log says the node WAS created ("Created node of type 'BreakStruct'"). This contradicts a null return.

**(B) CreateNodeByClass fallback.** If `ValidateNodeType()` fails for some reason and the node creation falls through to `CreateNodeByClass()`, the universal fallback creates the node WITHOUT struct type initialization. `CreateNodeByClass` handles CallFunction specially (SetFromFunction), but does NOT handle MakeStruct/BreakStruct. It would create a bare `UK2Node_BreakStruct` with null StructType, producing 1 pin.

Most likely (B). Check `ValidateNodeType()` -- it may fail the `struct_type` check, or there may be a code path where the `BreakStruct` NodeCreator is not being reached.

**However**, the node creator IS registered (line 3436-3437: `NodeCreators.Add(OliveNodeTypes::BreakStruct, ...)`), and `CreateNode()` dispatches to it (line 124-126). If `ValidateNodeType` passes (which it should since `struct_type` is in Properties), the creator should be called.

### Investigation Required + Fix

The coder must add diagnostic logging to confirm which code path executes. Then apply one of two fixes:

**Fix A: Add BreakStruct/MakeStruct handling to CreateNodeByClass (defense-in-depth)**

If any code path reaches `CreateNodeByClass` with a BreakStruct/MakeStruct node type, add a handler similar to CallFunction:

```
Location: OliveNodeFactory.cpp, CreateNodeByClass(), after the CallFunction handling block (~line 1872)
```

After `AllocateDefaultPins()` call in `CreateNodeByClass`, check if the node is `UK2Node_BreakStruct` or `UK2Node_MakeStruct`. If so, read `struct_type` from Properties, call `FindStruct()`, set `StructType`, call `ReconstructNode()` to rebuild pins with the correct struct.

**Fix B: Add tilde-strip in data wiring (separate from struct type bug)**

In `PhaseWireData`, when resolving pin names from `@step.~PinName` references, strip the leading `~` before calling `FindPinSmart`. The `~` is Olive's convention for "output from a struct break" but is NOT part of the UE pin name.

```
Location: OlivePlanExecutor.cpp, WireDataConnection() or ParseDataRef()
```

Check: does `ParseDataRef("@break_hit.~HitActor", ...)` produce OutPinHint = "~HitActor"? If so, strip the `~` prefix before pin lookup.

### Files to Modify

| File | Change |
|------|--------|
| `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp` | Add BreakStruct/MakeStruct struct-type handling in `CreateNodeByClass()` |
| `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp` | Strip `~` prefix from pin hints in `WireDataConnection()` or `ParseDataRef()` |

### Task: Senior (Fix A), Junior (Fix B)

**Fix A (Senior):** Requires understanding `UK2Node_BreakStruct` lifecycle and `ReconstructNode()` behavior. ~30 lines.

**Fix B (Junior):** Mechanical string strip. ~5 lines in `ParseDataRef()` or `WireDataConnection()`. After `ParseDataRef` extracts `OutPinHint`, add:
```cpp
if (OutPinHint.StartsWith(TEXT("~")))
{
    OutPinHint = OutPinHint.Mid(1);
}
```

### Risk

Fix A: Medium. `ReconstructNode()` can have side effects (triggers pin rebuild, may break existing connections). Must be called BEFORE `AllocateDefaultPins()` or INSTEAD of it. Actually, setting `StructType` + calling `ReconstructNode()` is the UE pattern used by the BreakStruct details panel. But in `CreateNodeByClass`, `AllocateDefaultPins()` is already called at line ~1800. The fix needs to detect BreakStruct BEFORE AllocateDefaultPins and set StructType first.

Fix B: Low. Simple string strip. No side effects.

### Testing

1. Run plan_json with: `{"op": "break_struct", "target": "HitResult"}` followed by data wires to `@step.HitActor`
2. Verify node has 20+ pins (FHitResult fields)
3. Verify data wires to `HitActor`, `ImpactPoint`, `ImpactNormal` succeed
4. Test with `~HitActor` pin hint syntax -- should work after tilde strip

---

## Bug #4: Phase 0 — VARIABLE_NOT_FOUND check for get_var/set_var

### Root Cause

The resolver warns when a variable is not found (line 2008, OliveBlueprintPlanResolver.cpp) but does NOT emit a blocking error. The plan proceeds to execution, creates a `UK2Node_VariableGet` with an invalid variable reference, which fails at compile time. This wastes one full plan_json attempt.

Phase 0 validator has 3 checks (COMPONENT_FUNCTION_ON_ACTOR, EXEC_WIRING_CONFLICT, LATENT_IN_FUNCTION) but no variable-existence check.

### Fix

Add **Check 4: VARIABLE_NOT_FOUND** to `FOlivePlanValidator`.

For each step where `Op == get_var || Op == set_var`:
1. Extract the variable name from `ResolvedStep.Properties["variable_name"]`
2. Skip if the resolved node type is `FunctionInput` or `FunctionOutput` (function params, not variables)
3. Call `BlueprintHasVariable(Blueprint, VariableName)` (same helper the resolver uses)
4. Also check parent Blueprint chain and generated class native properties (same logic as resolver lines 1972-2001)
5. Also check if ANY other step in the plan creates this variable via `set_var` with the same name (plan-internal forward reference). **Note**: plan_json does NOT have inline variable declaration. Variables must be created via `blueprint.add_variable` BEFORE `apply_plan_json`. So there's no need to track "variables declared in earlier steps."
6. If variable not found anywhere: emit `VARIABLE_NOT_FOUND` error with suggestion to add it first

### Error Format

```
Code: VARIABLE_NOT_FOUND
StepId: <step_id>
Location: /steps/<idx>/target
Message: "Variable 'TrailParticle' not found on Blueprint 'BP_ArrowProjectile' or its parent classes.
         get_var/set_var requires an existing variable. Components use their SCS variable name."
Suggestion: "Add the variable first with blueprint.add_variable, or check the variable name.
            Available variables on this Blueprint: [list first 10]"
```

Include available variable names (first 10, sorted) in the suggestion to help the AI self-correct.

### Files to Modify

| File | Change |
|------|--------|
| `Source/OliveAIEditor/Blueprint/Public/Plan/OlivePlanValidator.h` | Add `CheckVariableExists()` private static method declaration |
| `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanValidator.cpp` | Implement `CheckVariableExists()`, call from `Validate()`. Need `BlueprintHasVariable` helper — either extract from resolver's anonymous namespace or duplicate. |

### Implementation Detail

The `BlueprintHasVariable()` helper is currently in an anonymous namespace in `OliveBlueprintPlanResolver.cpp`. Options:
1. **Duplicate** it into OlivePlanValidator.cpp (6 lines, simple enough)
2. **Extract** to a shared header (OliveBlueprintHelpers.h or similar)

Recommend option 1 (duplicate). It's 6 lines, the function is trivial, and adding a shared header for one helper is overengineering. The validator already includes `Engine/Blueprint.h`, `Engine/SimpleConstructionScript.h`, and `Engine/SCS_Node.h`.

### Task: Junior

Well-specified. The check follows the exact same pattern as `CheckComponentFunctionTargets` and `CheckExecWiringConflicts`. ~60 lines.

### Exclusions from the check

- Steps where `ResolvedStep.NodeType == OliveNodeTypes::FunctionInput` (function params handled by FunctionEntry node)
- Steps where `ResolvedStep.NodeType == OliveNodeTypes::FunctionOutput` (function return handled by FunctionResult node)
- Variables found on parent Blueprints (inherited) — emit WARNING not error
- Variables found on GeneratedClass native properties — valid, no error

### Risk

Low. Read-only check. No mutations. The only risk is false positives:
- Variables added between plan creation and plan execution (unlikely within a single tool call)
- Variables on compiled GeneratedClass but not in NewVariables (native inherited) — already handled by the parent/generated-class fallback

### Testing

1. Run plan_json with `{"op": "get_var", "target": "NonExistentVar"}` on a Blueprint that doesn't have it
2. Phase 0 should block with `VARIABLE_NOT_FOUND` error
3. Preview should also show the error
4. Test with SCS component name — should pass (components are variables)
5. Test with inherited variable — should pass (warning only)
6. Test with function parameter in function graph — should pass (NodeType is FunctionInput)

---

## Summary

| Bug | Status | Files | Assignment | Lines |
|-----|--------|-------|------------|-------|
| #1 GetForwardVector alias | **Already fixed** | -- | -- | 0 |
| #2 Stale exec pin on reused nodes | **New fix** | OlivePlanExecutor.cpp | Junior | ~15 |
| #3a break_struct struct type | **New fix** | OliveNodeFactory.cpp | Senior | ~30 |
| #3b Tilde-strip pin hints | **New fix** | OlivePlanExecutor.cpp | Junior | ~5 |
| #4 VARIABLE_NOT_FOUND | **New fix** | OlivePlanValidator.h/.cpp | Junior | ~60 |

**Total new code: ~110 lines across 3 files.**

## Implementation Order

1. **Bug #4** (VARIABLE_NOT_FOUND) — Highest value, lowest risk, fastest to implement
2. **Bug #2** (stale exec pin) — Eliminates the 7-minute cascade from session 09j
3. **Bug #3b** (tilde strip) — Quick fix, unblocks break_struct wiring
4. **Bug #3a** (BreakStruct in CreateNodeByClass) — Senior task, defense-in-depth. Investigate first whether the primary CreateBreakStructNode path actually fails before adding the fallback. Add diagnostic logging to confirm.

## Diagnostic Step for Bug #3

Before implementing Fix 3a, add temporary logging to confirm which path executes:

In `CreateBreakStructNode()` (OliveNodeFactory.cpp line ~823), add at entry:
```cpp
UE_LOG(LogOliveNodeFactory, Log, TEXT("CreateBreakStructNode: struct_type='%s'"),
    StructTypePtr ? **StructTypePtr : TEXT("(missing)"));
```

In `CreateNodeByClass()` (~line 1722), add a check:
```cpp
if (NodeClassName == TEXT("UK2Node_BreakStruct") || NodeClassName == TEXT("UK2Node_MakeStruct"))
{
    UE_LOG(LogOliveNodeFactory, Warning,
        TEXT("CreateNodeByClass: FALLBACK path for %s — struct_type may not be set"), *NodeClassName);
}
```

Run a test with `break_struct HitResult` and check which log line fires. This determines whether Fix 3a is needed or if there's a different bug (e.g., ValidateNodeType rejecting the Properties).
