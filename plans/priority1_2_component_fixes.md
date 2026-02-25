# Priority 1 + 2: Component Variable Fixes and Auto-Wire/Pre-Compile Validation

**Author:** Architect Agent
**Date:** 2026-02-25
**Status:** Design Complete
**Depends on:** Priority 0 (universal node manipulation) -- already shipped

---

## Executive Summary

After thorough code review, the majority of the master plan's Phase 1 and Phase 2 changes have already been implemented:

- `BlueprintHasVariable` already checks SCS components (line 55-66 of OliveBlueprintPlanResolver.cpp)
- The lenient SCS fallback in `ResolveGetVarOp` was already removed
- `component_reference.txt` recipe already updated
- Self-correction guidance for `COMPONENT_NOT_VARIABLE` already corrected
- Phase 1.5 (`PhaseAutoWireComponentTargets`) already implemented
- Phase 5.5 (`PhasePreCompileValidation`) already implemented with both checks
- `bIsRequired` already on `FOlivePinManifestEntry`
- Validator already softened (warns on 1 match, errors on 0/>1)

**Remaining gaps found during code review** -- these are the actual tasks for this implementation plan:

| # | Gap | Severity | Why it matters |
|---|-----|----------|----------------|
| T1 | `ReadVariables` omits SCS component variables | High | AI calls `blueprint.read_variables`, sees no components, doesn't know they exist as variables |
| T2 | `ResolveGetVarOp`/`ResolveSetVarOp` warn-but-allow for truly missing variables | Medium | Plan passes resolve with warning, node creation fails silently or creates broken node |
| T3 | `CreateVariableGetNode`/`CreateVariableSetNode` have dead NewVariables-only check | Low | Misleading code; no behavioral bug since `SetSelfMember` works regardless |
| T4 | Phase 1.5 ignores string-literal Target inputs that match SCS components | Medium | Validator accepts string-literal component names, but Phase 1.5 skips them (only checks `@ref`) |
| T5 | `set_var` COMPONENT_NOT_VARIABLE error message could be cleaner | Low | Current message is functional but formatting could be improved per master plan |
| T6 | `variables_components.txt` recipe missing get_var-for-component guidance | Low | Recipe focuses on `add_variable`/`add_component`, not on using get_var for component references |

---

## Task Dependency Graph

```
T1 (ReadVariables)  -- independent
T2 (Resolver fail-early)  -- independent
T3 (NodeFactory cleanup)  -- independent
T4 (Phase 1.5 string Target)  -- independent
T5 (Error message)  -- independent
T6 (Recipe update)  -- independent

All tasks are independent and can run in parallel.
```

---

## Task 1: Include SCS Component Variables in `ReadVariables`

**Severity:** High
**Files:**
- `Source/OliveAIEditor/Blueprint/Private/Reader/OliveBlueprintReader.cpp`

### Problem

`FOliveBlueprintReader::ReadVariables()` (line 123) only iterates `Blueprint->NewVariables`. When the AI calls `blueprint.read_variables`, it sees user-defined variables but NOT component variables. Components are separately available via `blueprint.read_components`, but the AI doesn't know that component names work with `get_var` unless it cross-references both results.

This creates a workflow gap: the AI adds a component (e.g., StaticMeshComponent named "GunMesh"), then later builds a plan with `get_var "GunMesh"`. If it only read variables to understand what's available, "GunMesh" is invisible.

### Solution

Add SCS component variables to the `ReadVariables` output, marked with a source tag so the AI can distinguish them from user-defined variables.

### Code Changes

**File:** `Source/OliveAIEditor/Blueprint/Private/Reader/OliveBlueprintReader.cpp`
**Location:** Inside `ReadVariables(const UBlueprint* Blueprint)`, after the NewVariables loop (line 137), before the parent Blueprint check (line 139).

Add a new block:

```cpp
// Include SCS component variables.
// Every component in the SimpleConstructionScript IS a variable on the
// generated class. UK2Node_VariableGet with SetSelfMember("ComponentName")
// works identically to dragging the component from the Components panel.
if (Blueprint->SimpleConstructionScript)
{
    TArray<USCS_Node*> AllNodes = Blueprint->SimpleConstructionScript->GetAllNodes();
    for (USCS_Node* SCSNode : AllNodes)
    {
        if (!SCSNode || !SCSNode->ComponentClass)
        {
            continue;
        }

        FOliveIRVariable Var;
        Var.Name = SCSNode->GetVariableName().ToString();
        Var.DefinedIn = TEXT("component"); // Distinguishes from "self" (user vars)
        // Type info: use the component class name
        Var.Type.Category = EOliveIRTypeCategory::Object;
        Var.Type.ClassName = SCSNode->ComponentClass->GetName();
        // Components are always BP-visible and always read-only references
        Var.bBlueprintVisible = true;
        Var.bBlueprintReadWrite = false; // Read-only -- you cannot set_var on a component
        Var.bEditAnywhere = false;

        Variables.Add(MoveTemp(Var));
    }
}
```

### FOliveIRVariable Struct Details (VERIFIED)

**File:** `Source/OliveAIRuntime/Public/IR/CommonIR.h` (line 458)

The struct already has all the fields we need:
- `DefinedIn` (FString) -- set to `"self"` for user vars, `ParentBP->GetName()` for inherited. We set it to `"component"` for SCS component variables.
- `bBlueprintReadWrite` (bool, default true) -- we set to `false` for components since you cannot set_var on them.
- `Type` (FOliveIRType) -- has `Category` and `ClassName` fields.

**No struct changes needed.** The existing fields are sufficient.

`ToJson()` already serializes `DefinedIn` as `"defined_in"` and `bBlueprintReadWrite` as `flags.blueprint_read_write`. The AI will see `"defined_in":"component"` and `"flags":{"blueprint_read_write":false}` which clearly communicates that these are component references.

The `PinSerializer` is not available for SCS components (they don't have `FBPVariableDescription`), so we populate `Type` directly. `FOliveIRType` (line 49 of `Source/OliveAIRuntime/Public/IR/OliveIRTypes.h`) has:
- `Category` (EOliveIRTypeCategory) -- set to `EOliveIRTypeCategory::Object`
- `ClassName` (FString) -- set to component class name, e.g., `"StaticMeshComponent"`

These are public UPROPERTY fields. Direct assignment works.

### Additional includes needed

```cpp
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
```

Check if these are already included in OliveBlueprintReader.cpp. If not, add them.

### Test

1. Create a Blueprint with a StaticMeshComponent named "TestMesh" and a variable "Health" (Float).
2. Call `blueprint.read_variables`.
3. Verify result includes BOTH:
   - `{"name":"Health", "type":{...}, "defined_in":"self", "flags":{"blueprint_read_write":true, ...}}`
   - `{"name":"TestMesh", "type":{"category":"object", "type_name":"StaticMeshComponent"}, "defined_in":"component", "flags":{"blueprint_read_write":false, ...}}`
4. Verify that `blueprint.read_components` still works independently and returns the same components with full hierarchy info.

---

## Task 2: Fail-Early in Resolver for Truly Missing Variables

**Severity:** Medium
**Files:**
- `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`

### Problem

Both `ResolveGetVarOp` (line 625-634) and `ResolveSetVarOp` (line 759-766) have a "warn but allow" path when a variable name is not found on the Blueprint OR its parent chain. The comment says "may be created by another step or inherited from native." While the native inheritance case is valid, the "created by another step" case is misleading -- the plan resolver runs BEFORE any steps execute, so no steps have created anything yet.

This lenient path means:
- AI typos a variable name -> plan resolves successfully -> node creation produces a broken VariableGet/Set
- No error is surfaced until compile time, and even then it may be confusing

### Solution

Tighten the check: if the variable is not found on the Blueprint, its parent chain, OR the generated class (which catches native C++ properties), emit an error (not warning) for `get_var` and `set_var`.

However, we must preserve the legitimate case where:
- A variable is inherited from a native C++ parent class (exists on the generated class but not in any Blueprint's `NewVariables`)
- Another step in the SAME plan creates the variable (e.g., step 1 does add_variable, step 2 does get_var on it)

For the second case, we need to check if any earlier step in the plan creates the variable.

### Code Changes

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`

#### 2a. ResolveGetVarOp -- Replace warn-but-allow with conditional error

**Location:** Lines 625-634 (inside `!bFoundInParent` block)

Replace the current code with:

```cpp
if (!bFoundInParent)
{
    // Check native C++ properties on the generated class
    // (catches variables inherited from native parents, e.g., AActor::bHidden)
    bool bFoundOnGeneratedClass = false;
    if (BP->GeneratedClass)
    {
        FProperty* Prop = BP->GeneratedClass->FindPropertyByName(FName(*Step.Target));
        bFoundOnGeneratedClass = (Prop != nullptr);
    }

    if (bFoundOnGeneratedClass)
    {
        UE_LOG(LogOlivePlanResolver, Verbose,
            TEXT("Step '%s': Variable '%s' found on generated class (native property)"),
            *Step.StepId, *Step.Target);
    }
    else
    {
        // Variable not found anywhere. Emit a warning (not error) because
        // another step in the plan may create it, or the generated class
        // may not be fully compiled yet. The node factory will still attempt
        // creation via SetSelfMember which may succeed at compile time.
        Warnings.Add(FString::Printf(
            TEXT("Step '%s': Variable '%s' not found on Blueprint '%s'. "
                 "If this is a typo, the node will fail at compile. "
                 "Components use their variable name from the Components panel."),
            *Step.StepId, *Step.Target, *BP->GetName()));

        UE_LOG(LogOlivePlanResolver, Warning,
            TEXT("Step '%s': Variable '%s' not found on Blueprint '%s' or parents or generated class"),
            *Step.StepId, *Step.Target, *BP->GetName());
    }
}
```

**Rationale for warning (not error):** We keep this as a warning because:
1. The plan may include an `add_variable` step that creates the variable before execution
2. The generated class may be stale (not yet compiled after recent additions)
3. The node creation via `SetSelfMember` may still work if the class has the property

But we improve the warning message to mention components by name and typos, giving the AI useful feedback.

#### 2b. ResolveSetVarOp -- Same improvement to warn-but-allow

**Location:** Lines 759-766 (inside the `!bFoundInParent && MatchedComponentClass.IsEmpty()` block)

Replace the current code with the same pattern as 2a above, adapted for set_var.

### Test

1. Build a plan with `get_var` targeting a misspelled variable name.
2. Verify the resolver emits a warning (not just a log) containing the variable name and "not found" guidance.
3. Build a plan with `get_var` targeting a valid component name. Verify NO warning is emitted (since `BlueprintHasVariable` catches it).
4. Build a plan with `get_var` targeting a native property like "bHidden". Verify it resolves cleanly with a verbose log about generated class.

---

## Task 3: Clean Up Dead Code in NodeFactory Variable Nodes

**Severity:** Low
**Files:**
- `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp`

### Problem

`CreateVariableGetNode` (lines 282-289) loops through `Blueprint->NewVariables` searching for the variable but the result is never used -- it finds `Property` but `Property` is unused. The node is created via `SetSelfMember` regardless. This is dead code that gives the false impression the check matters.

### Solution

Remove the dead `NewVariables` loop from both `CreateVariableGetNode` and `CreateVariableSetNode` (if present). The variable resolution happens at `AllocateDefaultPins` / compile time via `SetSelfMember`, not at creation time.

### Code Changes

**File:** `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp`

#### 3a. CreateVariableGetNode -- Remove dead loop

**Location:** Lines 277-289

Replace:
```cpp
// Find the property in the Blueprint
FName VarName(**VarNamePtr);
FProperty* Property = nullptr;

// Check Blueprint variables
for (const FBPVariableDescription& Var : Blueprint->NewVariables)
{
    if (Var.VarName == VarName)
    {
        // Variable found - property will be resolved from the generated class
        break;
    }
}
```

With:
```cpp
FName VarName(**VarNamePtr);
```

The `FProperty* Property` is declared but never used after the loop.

#### 3b. CreateVariableSetNode -- verify

The `CreateVariableSetNode` (line 300-321) does NOT have this dead loop -- it goes straight to `SetSelfMember`. No change needed.

### Test

1. Build the project. Verify no compilation errors.
2. Run a plan with `get_var` for a Blueprint variable. Verify node still creates correctly.
3. Run a plan with `get_var` for a component variable. Verify node still creates correctly.

---

## Task 4: Phase 1.5 Handle String-Literal Target Inputs

**Severity:** Medium
**Files:**
- `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`

### Problem

Phase 1.5 (`PhaseAutoWireComponentTargets`) at line 517-521 checks if the AI provided a Target input, but it ONLY skips auto-wiring when the Target starts with `@`:

```cpp
const FString* TargetValue = PlanStep.Inputs.Find(TEXT("Target"));
if (TargetValue && TargetValue->StartsWith(TEXT("@")))
{
    continue; // AI provided a target reference -- don't override
}
```

The validator (`CheckComponentFunctionTargets` in OlivePlanValidator.cpp) was already updated in Priority 0 to accept string-literal component names (e.g., `"StaticMeshComp"` instead of `"@get_comp.auto"`). But Phase 1.5 ignores these string literals entirely -- it proceeds to auto-wire as if no Target was provided.

This means: if the AI says `"inputs":{"Target":"GunMesh"}` (string literal matching an SCS variable name), the validator correctly accepts it, but Phase 1.5 then overwrites/duplicates the wiring.

### Solution

Extend Phase 1.5 to check for string-literal Target values that match SCS component variable names. If a match is found, treat it the same as a `@ref` -- the AI has expressed intent, so Phase 1.5 should honor it by wiring to that specific component (or skipping if Phase 4 data wiring will handle it).

Actually, Phase 4 (data wiring) treats non-`@ref` strings as pin defaults, NOT as wiring targets. So a string literal `"GunMesh"` in inputs would try to set the Target pin's default value to the string "GunMesh", which is wrong for an object reference pin.

The correct fix is: when Phase 1.5 sees a string-literal Target that matches an SCS component variable name, it should **wire to that component** (not skip). This is what the AI intended.

### Code Changes

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`
**Location:** Inside `PhaseAutoWireComponentTargets`, around line 514-522

Replace the current @ref-only check:

```cpp
// Check if AI already provided a Target input as @ref
if (i < Plan.Steps.Num())
{
    const FOliveIRBlueprintPlanStep& PlanStep = Plan.Steps[i];
    const FString* TargetValue = PlanStep.Inputs.Find(TEXT("Target"));
    if (TargetValue && TargetValue->StartsWith(TEXT("@")))
    {
        continue; // AI provided a target reference -- don't override
    }
}
```

With:

```cpp
// Check if AI already provided a Target input
if (i < Plan.Steps.Num())
{
    const FOliveIRBlueprintPlanStep& PlanStep = Plan.Steps[i];
    const FString* TargetValue = PlanStep.Inputs.Find(TEXT("Target"));
    if (TargetValue && !TargetValue->IsEmpty())
    {
        if (TargetValue->StartsWith(TEXT("@")))
        {
            continue; // @ref syntax -- Phase 4 data wiring handles this
        }

        // String literal -- check if it matches an SCS component variable.
        // If so, wire to THAT specific component instead of auto-detecting.
        if (BP->SimpleConstructionScript)
        {
            for (USCS_Node* SCSNode : BP->SimpleConstructionScript->GetAllNodes())
            {
                if (SCSNode && SCSNode->GetVariableName().ToString() == *TargetValue)
                {
                    // AI specified a component by name. Wire to it directly.
                    UEdGraphNode* CreatedNode = Context.GetNodePtr(Resolved.StepId);
                    if (CreatedNode)
                    {
                        UEdGraphPin* SelfPin = CreatedNode->FindPin(UEdGraphSchema_K2::PN_Self);
                        if (SelfPin && SelfPin->LinkedTo.Num() == 0)
                        {
                            FName CompVarName = SCSNode->GetVariableName();
                            UK2Node_VariableGet* GetNode = NewObject<UK2Node_VariableGet>(Context.Graph);
                            GetNode->VariableReference.SetSelfMember(CompVarName);
                            Context.Graph->AddNode(GetNode, false, false);
                            GetNode->AllocateDefaultPins();

                            UEdGraphPin* GetOutputPin = nullptr;
                            for (UEdGraphPin* Pin : GetNode->Pins)
                            {
                                if (Pin && Pin->Direction == EGPD_Output &&
                                    Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
                                {
                                    GetOutputPin = Pin;
                                    break;
                                }
                            }

                            if (GetOutputPin)
                            {
                                const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
                                if (Schema->TryCreateConnection(GetOutputPin, SelfPin))
                                {
                                    Context.SuccessfulConnectionCount++;
                                    Context.AutoFixCount++;
                                    Context.Warnings.Add(FString::Printf(
                                        TEXT("Step '%s': Wired Target <- component '%s' (from string-literal input)"),
                                        *Resolved.StepId, *CompVarName.ToString()));
                                    UE_LOG(LogOlivePlanExecutor, Log,
                                        TEXT("Phase 1.5: Wired '%s' Target <- component '%s' (string-literal)"),
                                        *Resolved.StepId, *CompVarName.ToString());
                                }
                                else
                                {
                                    Context.Graph->RemoveNode(GetNode);
                                }
                            }
                            else
                            {
                                Context.Graph->RemoveNode(GetNode);
                            }
                        }
                    }
                    goto NextStep; // Component matched -- skip the auto-detect path below
                }
            }
        }
        // String literal but not a component name -- fall through to auto-detect
    }
}
```

Then wrap the existing auto-detect code (lines 524-622) and at the end of the for loop body, add the `NextStep` label. Alternatively, refactor the inner body into a helper lambda.

**NOTE:** The `goto` pattern is ugly. The coder should prefer extracting the inner loop body into a helper method or using a `bHandled` flag with `continue`. The design intent is: if the string literal matches a component, wire to it and skip auto-detection. The coder has license to choose the cleanest control flow.

### Test

1. Build a plan with `"inputs":{"Target":"GunMesh"}` (string literal, not @ref) targeting a component function.
2. Verify Phase 1.5 wires to the "GunMesh" component specifically (not auto-detected).
3. Build a plan with `"inputs":{"Target":"@get_gun.auto"}` (@ref).
4. Verify Phase 1.5 skips and Phase 4 handles the wiring.
5. Build a plan with `"inputs":{"Target":"SomeNonexistentName"}` (string literal, not matching any SCS component).
6. Verify Phase 1.5 falls through to auto-detection as before.

---

## Task 5: Improve set_var COMPONENT_NOT_VARIABLE Error Message

**Severity:** Low
**Files:**
- `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`

### Problem

The `COMPONENT_NOT_VARIABLE` error message at line 740-748 is functional but could be cleaner. The current message includes `\"SetWorldTransform\"/\"SetRelativeLocation\"/etc.` which looks garbled in JSON output. The master plan (Change 1.3) suggested a cleaner format with concrete step examples.

### Solution

Rewrite the error message and suggestion to be more readable and include a concrete step_id example.

### Code Changes

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`
**Location:** Lines 736-749

Replace:
```cpp
Errors.Add(FOliveIRBlueprintPlanError::MakeStepError(
    TEXT("COMPONENT_NOT_VARIABLE"),
    Step.StepId,
    FString::Printf(TEXT("/steps/%d/target"), Idx),
    FString::Printf(
        TEXT("'%s' is a component (class: %s). Components are read-only references — "
             "use get_var to read them, but you cannot set_var on a component. "
             "To modify component properties, use: "
             "{\"op\":\"call\", \"target\":\"SetWorldTransform\"/"
             "\"SetRelativeLocation\"/etc., "
             "\"inputs\":{\"Target\":\"@<get_var_step>.auto\"}}"),
        *Step.Target, *MatchedComponentClass),
    TEXT("Use get_var to read the component reference, then call "
     "setter functions with Target wired to the get_var output.")));
```

With:
```cpp
// Build a clean step_id suggestion from the component name
FString CleanName = Step.Target;
CleanName = CleanName.ToLower();

Errors.Add(FOliveIRBlueprintPlanError::MakeStepError(
    TEXT("COMPONENT_NOT_VARIABLE"),
    Step.StepId,
    FString::Printf(TEXT("/steps/%d/target"), Idx),
    FString::Printf(
        TEXT("'%s' is a component (class: %s). Components are read-only references "
             "and cannot be assigned with set_var. To READ this component, use get_var. "
             "To MODIFY a property on it, first get_var, then call the setter: "
             "{\"step_id\":\"get_%s\", \"op\":\"get_var\", \"target\":\"%s\"}, "
             "then {\"op\":\"call\", \"target\":\"SetRelativeLocation\", "
             "\"inputs\":{\"Target\":\"@get_%s.auto\", \"NewLocation\":\"...\"}}"),
        *Step.Target, *MatchedComponentClass,
        *CleanName, *Step.Target, *CleanName),
    TEXT("Use get_var to read the component reference, then call "
         "setter functions with Target wired to the get_var output.")));
```

### Test

1. Build a plan with `set_var` targeting a component name.
2. Verify the error message includes a concrete get_var -> call example with the actual component name.

---

## Task 6: Update variables_components.txt Recipe

**Severity:** Low
**Files:**
- `Content/SystemPrompts/Knowledge/recipes/blueprint/variables_components.txt`

### Problem

The `variables_components.txt` recipe focuses on `blueprint.add_variable` / `blueprint.add_component` / `blueprint.modify_component` but says nothing about using `get_var` to reference components in plan JSON. An AI that reads this recipe gets no guidance on the component-as-variable pattern.

### Solution

Add a note about get_var for components.

### Code Changes

**File:** `Content/SystemPrompts/Knowledge/recipes/blueprint/variables_components.txt`

Append after the existing content:

```
NOTE: Components added via add_component ARE variables on the generated class.
In plan JSON, use get_var to read a component reference:
  {"step_id":"get_mesh", "op":"get_var", "target":"MyStaticMesh"}
Then wire it to Target on component functions.
See the component_reference recipe for full patterns.
```

### Test

1. Call `olive.get_recipe("variables components")`.
2. Verify the response includes the get_var note.

---

## Implementation Order

All 6 tasks are independent and can be parallelized fully. However, if coders must be sequenced:

**Recommended order by impact:**
1. **T1** (ReadVariables) -- Highest impact, fixes a significant visibility gap
2. **T4** (Phase 1.5 string literal) -- Fixes a behavioral inconsistency with validator
3. **T2** (Resolver fail-early) -- Improves error quality
4. **T5** (Error message) -- Polish
5. **T3** (NodeFactory cleanup) -- Code hygiene
6. **T6** (Recipe update) -- Content change

**Parallel grouping for 2 coders:**
- Coder A: T1 + T2 + T5 (all resolver/reader changes in the Plan/ and Reader/ directories)
- Coder B: T4 + T3 + T6 (executor, factory, and content changes)

**Parallel grouping for 3 coders:**
- Coder A: T1 + T3 (Reader + Factory -- both in reader/writer area)
- Coder B: T2 + T5 (Both in OliveBlueprintPlanResolver.cpp)
- Coder C: T4 + T6 (Executor + recipe)

---

## Files Modified Summary

| File | Tasks | Description |
|------|-------|-------------|
| `Source/OliveAIEditor/Blueprint/Private/Reader/OliveBlueprintReader.cpp` | T1 | Add SCS component variables to ReadVariables |
| `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp` | T2, T5 | Improve variable-not-found handling; improve COMPONENT_NOT_VARIABLE message |
| `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp` | T3 | Remove dead NewVariables loop |
| `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp` | T4 | Handle string-literal Target in Phase 1.5 |
| `Content/SystemPrompts/Knowledge/recipes/blueprint/variables_components.txt` | T6 | Add get_var-for-components note |

---

## What Was Already Done (No Work Needed)

For reference, these master plan items are already implemented in the current codebase:

- **Change 1.1** (BlueprintHasVariable SCS check): Line 55-66 of OliveBlueprintPlanResolver.cpp
- **Change 1.2** (Remove lenient get_var SCS fallback): Already removed -- ResolveGetVarOp has no SCS block
- **Change 1.4** (component_reference.txt recipe): Already updated with correct content
- **Change 1.5** (COMPONENT_NOT_VARIABLE self-correction guidance): Line 469-474 of OliveSelfCorrectionPolicy.cpp
- **Change 1.6** (Log AI text on zero-tool reprompt): Line 702-705 of OliveConversationManager.cpp
- **Change 1.7** (Log AI text on correction reprompt): Line 733-736 of OliveConversationManager.cpp
- **Change 1.8** (Comment on set_var SCS walk): Line 696-699 of OliveBlueprintPlanResolver.cpp
- **Change 2.1** (Phase 1.5 auto-wire): Lines 485-622 of OlivePlanExecutor.cpp
- **Change 2.2** (Validator softening): Lines 158-169 of OlivePlanValidator.cpp
- **Change 2.3** (Phase 5.5 pre-compile validation): Lines 1777-1961 of OlivePlanExecutor.cpp
- **Change 2.4** (bIsRequired on pin manifest): Line 59 of OlivePinManifest.h, lines 105-125 of OlivePinManifest.cpp

---

## Edge Cases and Error Handling

### T1: ReadVariables component output
- **Empty ComponentClass**: Skip the SCS node (null check already present)
- **No SCS**: Skip the SCS block entirely (Blueprint types like Function Libraries have no SCS)
- **Duplicate names**: Theoretically impossible (UE prevents duplicate component variable names in SCS), but if a component name matches a NewVariable name, both will appear. The `DefinedIn` field distinguishes them (`"self"` vs `"component"`).

### T2: Variable not found
- **Native C++ property**: Must still work. Check `BP->GeneratedClass->FindPropertyByName()` as fallback.
- **Stale generated class**: After `blueprint.add_variable` but before compile, the generated class may not have the new property. The warn-but-allow behavior is correct here.
- **Inter-step dependency**: Step 1 adds variable via `add_variable` tool call, step 2 uses `get_var` in a plan. Since the resolver runs after `add_variable` executes, the variable should exist in `NewVariables` by then. No special handling needed.

### T4: String literal Target
- **String literal not matching any SCS component**: Falls through to normal auto-detection. Same behavior as before.
- **Multiple components with similar names**: String match is exact (case-sensitive), not fuzzy. No ambiguity.
- **Self pin already wired**: The existing `SelfPin->LinkedTo.Num() > 0` check handles this for both string-literal and auto-detect paths.
