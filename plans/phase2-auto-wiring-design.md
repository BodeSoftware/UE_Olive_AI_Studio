# Phase 2: Auto-Wiring, Pre-Compile Validation & Auto-Fix

**Author:** Architect Agent
**Date:** 2026-02-24
**Status:** Design Complete
**Master Plan Reference:** `plans/olive_master_implementation_plan_v2.md`, Phase 2 (Changes 2.1-2.4)
**Depends on:** Phase 1 (component variable fix) must be shipped first or simultaneously

---

## Table of Contents

1. [Overview](#overview)
2. [Change 2.4: Add `bIsRequired` to Pin Manifest Entry](#change-24)
3. [Change 2.1: Auto-Wire Component Targets (Phase 1.5)](#change-21)
4. [Change 2.2: Soften CheckComponentFunctionTargets to Warning](#change-22)
5. [Change 2.3: Pre-Compile Validation with Auto-Fix (Phase 5.5)](#change-23)
6. [Implementation Order](#implementation-order)
7. [Testing Strategy](#testing-strategy)

---

<a id="overview"></a>
## 1. Overview

Phase 2 adds two new execution phases to `FOlivePlanExecutor` and modifies the Phase 0 validator:

- **Phase 1.5** (new): After node creation, auto-wire component function Target pins when exactly one matching SCS component exists
- **Phase 5.5** (new): Between SetDefaults and AutoLayout, detect orphaned impure nodes and unwired component Targets, auto-fix where intent is clear
- **Validator softening**: `COMPONENT_FUNCTION_ON_ACTOR` downgrades to warning when auto-fixable (exactly 1 matching component)
- **Pin manifest enhancement**: `bIsRequired` flag on `FOlivePinManifestEntry` so AI knows which pins must be wired

### Design Philosophy

"Don't report and hope the AI fixes it. If the fix is unambiguous, just fix it."

Auto-fix silently when safe (exactly 1 match). Warn when ambiguous (>1 match). Error only when impossible (0 matches + no fallback).

### Files Modified

| File | Changes |
|------|---------|
| `Blueprint/Public/Plan/OlivePinManifest.h` | Add `bIsRequired` field to `FOlivePinManifestEntry` |
| `Blueprint/Private/Plan/OlivePinManifest.cpp` | Populate `bIsRequired` in `Build()`, serialize in `ToJson()` |
| `Blueprint/Public/Plan/OlivePlanExecutor.h` | Add new fields to context struct, declare 2 new phase methods |
| `Blueprint/Private/Plan/OlivePlanExecutor.cpp` | Implement Phase 1.5 + Phase 5.5, wire into `Execute()` |
| `Blueprint/Private/Plan/OlivePlanValidator.cpp` | Soften component check to warning when auto-fixable |

### Files NOT Modified

- `OliveBlueprintToolHandlers.cpp` -- `AutoFixCount` and `PreCompileIssues` flow through the existing `PlanResult.Warnings` array. No tool handler changes needed.
- `BlueprintPlanIR.h` -- `FOliveIRBlueprintPlanResult` already has `Warnings` array, which carries auto-fix and pre-compile info.

---

<a id="change-24"></a>
## 2. Change 2.4: Add `bIsRequired` to Pin Manifest Entry

**Rationale:** The AI needs to know which pins MUST be wired for the node to function correctly. This flag surfaces in the pin manifest JSON so the AI can prioritize wiring required pins and know when it forgot one.

### 2.4.1 — Struct Addition

**File:** `Source/OliveAIEditor/Blueprint/Public/Plan/OlivePinManifest.h`
**Location:** Inside `FOlivePinManifestEntry` struct, after the `bIsConnected` field (line 54)

Add:

```cpp
/** Whether this pin is required for the node to function correctly.
 *  true for: exec input on non-event impure nodes, self/Target on non-static member functions.
 *  Surfaced in tool results so the AI knows which pins must be wired. */
bool bIsRequired = false;
```

### 2.4.2 — Populate in Build()

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePinManifest.cpp`
**Location:** Inside `FOlivePinManifest::Build()`, after `Entry.DefaultValue` assignment (line 98), before `Manifest.Pins.Add(MoveTemp(Entry));` (line 100)

Insert the following block:

```cpp
// Determine if this pin is required for correct node operation.
// Required pins that are unwired will cause compile errors or wrong behavior.
Entry.bIsRequired = false;
if (Entry.bIsExec && Entry.bIsInput)
{
    // Exec input is required on non-event impure nodes (events are entry points)
    UK2Node* K2Node = Cast<UK2Node>(Node);
    if (K2Node)
    {
        Entry.bIsRequired = !K2Node->IsA<UK2Node_Event>() &&
                            !K2Node->IsA<UK2Node_CustomEvent>();
    }
}
else if (!Entry.bIsExec && Entry.bIsInput && !Entry.bIsHidden)
{
    // Self/Target pin on non-static member functions
    if (Pin->GetFName() == UEdGraphSchema_K2::PN_Self)
    {
        UK2Node_CallFunction* CallFunc = Cast<UK2Node_CallFunction>(Node);
        if (CallFunc)
        {
            UFunction* Func = CallFunc->GetTargetFunction();
            Entry.bIsRequired = Func && !Func->HasAnyFunctionFlags(FUNC_Static);
        }
    }
}
```

**Required additional includes** in `OlivePinManifest.cpp` (add after existing includes at top):

```cpp
#include "K2Node.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
```

Note: `K2Node_CallFunction.h` is already included at line 9.

### 2.4.3 — Serialize in ToJson()

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePinManifest.cpp`
**Location:** Inside `FOlivePinManifest::ToJson()`, in the pin serialization loop, after the `bIsConnected` block (line 561), before `PinsArray.Add(...)` (line 563)

Insert:

```cpp
if (Entry.bIsRequired)
{
    PinJson->SetBoolField(TEXT("required"), true);
}
```

This ensures `"required": true` appears in the JSON only when the pin is required, keeping output clean for non-required pins.

### Data Flow

```
UEdGraphNode* [after creation]
    |
    v
FOlivePinManifest::Build() -- introspects each pin
    |
    v  checks: is exec input on non-event? is Self pin on non-static call?
    |
FOlivePinManifestEntry.bIsRequired = true/false
    |
    v
ToJson() -- serializes as "required": true
    |
    v
Tool result JSON -- AI sees {"name":"self","required":true,...}
```

### Edge Cases

- **Static functions**: `PN_Self` pin exists but is hidden (`bIsHidden=true`). Since the Build() loop already skips hidden data pins (line 67-69), the self pin on static functions is excluded from the manifest entirely. The `bIsRequired` check only applies to visible self pins, which means non-static member functions. Correct.
- **Event nodes**: Exec input on events is NOT required because events are entry points (they have exec output, the input pin is typically hidden). The `UK2Node_Event/CustomEvent` check handles this.
- **Pure nodes**: Pure nodes have no exec pins at all, so the exec-input-required check never fires. Correct.

---

<a id="change-21"></a>
## 3. Change 2.1: Auto-Wire Component Targets (Phase 1.5)

**Rationale:** When the AI calls a component function (e.g., `SetRelativeLocation` on `USceneComponent`) but forgets to wire the Target pin, and the Blueprint has exactly one component of the matching class, the fix is unambiguous. Auto-inject a `UK2Node_VariableGet` for that component and wire it to Self/Target.

### 3.1.1 — Context Struct Additions

**File:** `Source/OliveAIEditor/Blueprint/Public/Plan/OlivePlanExecutor.h`
**Location:** Inside `FOlivePlanExecutionContext` struct

Add these 4 new fields. Insert them AFTER the existing `ConversionNotes` field (line 115) and BEFORE the `GetManifest` method (line 118):

```cpp
/** Pre-compile validation issues (Phase 5.5) -- things that couldn't be auto-fixed */
TArray<FString> PreCompileIssues;

/** Count of issues auto-fixed by executor (Phase 1.5 + Phase 5.5) */
int32 AutoFixCount = 0;

/** Reverse map: NodeId -> StepId (built from StepToNodeMap during Execute) */
TMap<FString, FString> NodeIdToStepId;

/** Pointer to plan for Phase 5.5 exec recovery lookups. NOT owned. */
const FOliveIRBlueprintPlan* Plan = nullptr;
```

Add one new lookup method after the existing `GetNodeId` declaration (line 124):

```cpp
/** Reverse lookup: get step ID for a given UEdGraphNode*, or empty string.
 *  Iterates StepToNodePtr (small map, typically <50 entries). */
FString FindStepIdForNode(const UEdGraphNode* Node) const;
```

### 3.1.2 — Context Method Implementations

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`
**Location:** After the existing `GetNodeId` implementation (line 60), before the `Execute` entry point comment block (line 62)

Add:

```cpp
FString FOlivePlanExecutionContext::FindStepIdForNode(const UEdGraphNode* Node) const
{
    if (!Node)
    {
        return FString();
    }
    for (const auto& Pair : StepToNodePtr)
    {
        if (Pair.Value == Node)
        {
            return Pair.Key;
        }
    }
    return FString();
}
```

### 3.1.3 — Phase 1.5 Method Declaration

**File:** `Source/OliveAIEditor/Blueprint/Public/Plan/OlivePlanExecutor.h`
**Location:** Inside `FOlivePlanExecutor` private section, AFTER `PhaseCreateNodes` declaration (line 209), BEFORE `PhaseWireExec` declaration (line 212)

Add:

```cpp
/**
 * Phase 1.5: Auto-wire component function Target pins.
 * For each CallFunction node targeting a component-class function:
 * if the Target/Self pin is unwired AND the Blueprint has exactly one
 * SCS component of the required class, inject a UK2Node_VariableGet
 * for that component and wire its output to the Self pin.
 *
 * Skips if: Blueprint is itself a component, AI already provided Target
 * input as @ref, Target pin is already connected, or multiple/zero
 * matching components exist.
 *
 * CONTINUE-ON-FAILURE per step; failure for one step does not block others.
 */
void PhaseAutoWireComponentTargets(
    const FOliveIRBlueprintPlan& Plan,
    const TArray<FOliveResolvedStep>& ResolvedSteps,
    FOlivePlanExecutionContext& Context);
```

### 3.1.4 — Phase 1.5 Implementation

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`
**Location:** Insert as a new section AFTER the `FindExistingEventNode` implementation (ends at line 417), BEFORE `PhaseWireExec` (line 423)

Add these includes at the top of the file (after existing UE includes, around line 32):

```cpp
#include "K2Node_VariableGet.h"
#include "K2Node_CallFunction.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/ActorComponent.h"
```

Implementation:

```cpp
// ============================================================================
// Phase 1.5: Auto-Wire Component Targets
// ============================================================================

void FOlivePlanExecutor::PhaseAutoWireComponentTargets(
    const FOliveIRBlueprintPlan& Plan,
    const TArray<FOliveResolvedStep>& ResolvedSteps,
    FOlivePlanExecutionContext& Context)
{
    UBlueprint* BP = Context.Blueprint;
    if (!BP || !BP->SimpleConstructionScript)
    {
        return;
    }

    // Skip if Blueprint itself is a component (self IS the component)
    if (BP->ParentClass && BP->ParentClass->IsChildOf(UActorComponent::StaticClass()))
    {
        return;
    }

    for (int32 i = 0; i < ResolvedSteps.Num(); ++i)
    {
        const FOliveResolvedStep& Resolved = ResolvedSteps[i];

        // Only care about call ops with a resolved owning class that is a component
        if (!Resolved.ResolvedOwningClass ||
            !Resolved.ResolvedOwningClass->IsChildOf(UActorComponent::StaticClass()))
        {
            continue;
        }

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

        // Get the created node
        UEdGraphNode* CreatedNode = Context.GetNodePtr(Resolved.StepId);
        if (!CreatedNode)
        {
            continue;
        }

        // Find the self/Target pin
        UEdGraphPin* SelfPin = CreatedNode->FindPin(UEdGraphSchema_K2::PN_Self);
        if (!SelfPin || SelfPin->LinkedTo.Num() > 0)
        {
            continue; // Already wired or no self pin
        }

        // Find matching SCS components
        UClass* RequiredClass = Resolved.ResolvedOwningClass;
        TArray<USCS_Node*> MatchingNodes;
        for (USCS_Node* SCSNode : BP->SimpleConstructionScript->GetAllNodes())
        {
            if (SCSNode && SCSNode->ComponentClass &&
                SCSNode->ComponentClass->IsChildOf(RequiredClass))
            {
                MatchingNodes.Add(SCSNode);
            }
        }

        if (MatchingNodes.Num() == 0)
        {
            // No matching components -- Phase 5.5 or compile will catch this
            continue;
        }

        if (MatchingNodes.Num() > 1)
        {
            Context.Warnings.Add(FString::Printf(
                TEXT("Step '%s': %d components match type '%s'. "
                     "Wire Target explicitly to disambiguate."),
                *Resolved.StepId, MatchingNodes.Num(),
                *RequiredClass->GetName()));
            continue;
        }

        // Exactly one match -- inject a VariableGet node and wire it
        USCS_Node* MatchedSCS = MatchingNodes[0];
        FName ComponentVarName = MatchedSCS->GetVariableName();

        UK2Node_VariableGet* GetNode = NewObject<UK2Node_VariableGet>(Context.Graph);
        GetNode->VariableReference.SetSelfMember(ComponentVarName);
        Context.Graph->AddNode(GetNode, /*bUserAction=*/false, /*bSelectNewNode=*/false);
        GetNode->AllocateDefaultPins();

        // Find the first non-exec output pin on the getter (the component reference)
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

        if (!GetOutputPin)
        {
            Context.Warnings.Add(FString::Printf(
                TEXT("Step '%s': Auto-wire failed -- VariableGet for '%s' has no output pin"),
                *Resolved.StepId, *ComponentVarName.ToString()));
            // Remove the orphan getter node we just created
            Context.Graph->RemoveNode(GetNode);
            continue;
        }

        // Wire the getter output to the self pin
        const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
        if (Schema->TryCreateConnection(GetOutputPin, SelfPin))
        {
            Context.SuccessfulConnectionCount++;
            Context.AutoFixCount++;

            Context.Warnings.Add(FString::Printf(
                TEXT("Step '%s': Auto-wired Target <- component '%s' (%s)"),
                *Resolved.StepId, *ComponentVarName.ToString(),
                *RequiredClass->GetName()));

            UE_LOG(LogOlivePlanExecutor, Log,
                TEXT("Phase 1.5: Auto-wired '%s' Target <- component '%s' (%s)"),
                *Resolved.StepId, *ComponentVarName.ToString(),
                *RequiredClass->GetName());
        }
        else
        {
            Context.Warnings.Add(FString::Printf(
                TEXT("Step '%s': Auto-wire Target failed for component '%s'"),
                *Resolved.StepId, *ComponentVarName.ToString()));
            // Remove the orphan getter node
            Context.Graph->RemoveNode(GetNode);
        }
    }
}
```

### 3.1.5 — Wire Phase 1.5 into Execute()

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`
**Location:** Inside `FOlivePlanExecutor::Execute()`, AFTER Phase 1 completion log (line 105), BEFORE the BatchScope block (line 111)

Insert:

```cpp
    // Initialize reverse map and plan pointer for Phase 5.5
    for (const auto& Pair : Context.StepToNodeMap)
    {
        Context.NodeIdToStepId.Add(Pair.Value, Pair.Key);
    }
    Context.Plan = &Plan;

    // Phase 1.5: Auto-wire component function targets (CONTINUE-ON-FAILURE)
    UE_LOG(LogOlivePlanExecutor, Log, TEXT("Phase 1.5: Auto-Wire Component Targets"));
    PhaseAutoWireComponentTargets(Plan, ResolvedSteps, Context);

    if (Context.AutoFixCount > 0)
    {
        UE_LOG(LogOlivePlanExecutor, Log,
            TEXT("Phase 1.5 complete: %d component targets auto-wired"),
            Context.AutoFixCount);
    }
    else
    {
        UE_LOG(LogOlivePlanExecutor, Log,
            TEXT("Phase 1.5 complete: no auto-wiring needed"));
    }
```

### Data Flow

```
Phase 1: PhaseCreateNodes
    |
    v  StepToNodeMap, StepToNodePtr, StepManifests populated
    |
Phase 1.5: PhaseAutoWireComponentTargets
    |
    v  For each resolved step with ResolvedOwningClass (component):
    |    1. Check AI didn't already wire Target (@ref in Inputs)
    |    2. Get UEdGraphNode* from StepToNodePtr
    |    3. Find SelfPin on node
    |    4. Count matching SCS components
    |    5. If exactly 1: inject UK2Node_VariableGet, wire output -> SelfPin
    |
    v  AutoFixCount incremented, warning added to Context.Warnings
    |
Phase 3: PhaseWireExec (unchanged)
```

### Edge Cases

1. **Blueprint IS a component**: Skip entirely (self IS the component, Self pin wires to self correctly).
2. **AI already wired Target**: Check `PlanStep.Inputs["Target"]` starts with `@`. If yes, skip (AI's wire takes priority).
3. **Self pin already connected**: Maybe another Phase 1.5 iteration or the data wiring somehow connected it. Check `SelfPin->LinkedTo.Num() > 0`. If yes, skip.
4. **Zero matching components**: Skip silently. Phase 5.5 or compile will report this. The validator (Change 2.2) already produces an error for this case.
5. **Multiple matching components**: Warn with count and class name. AI must disambiguate.
6. **VariableGet has no output pin**: Defensive check. If the getter somehow has no non-exec output, warn and remove the orphan node.
7. **TryCreateConnection fails**: This can happen if pin types are incompatible (rare but possible with template components). Warn and remove the orphan getter.
8. **Step index mismatch**: `ResolvedSteps[i]` and `Plan.Steps[i]` must correspond. This is guaranteed by the resolver which produces one resolved step per plan step in order. Verify: `FOliveBlueprintPlanResolver::Resolve()` iterates `Plan.Steps` sequentially and appends to `ResolvedSteps` in order. The i-th resolved step corresponds to the i-th plan step. However, if a step fails resolution, it is NOT added to `ResolvedSteps` -- the entire resolve fails. So if we reach the executor, `ResolvedSteps.Num() == Plan.Steps.Num()` is guaranteed.

---

<a id="change-22"></a>
## 4. Change 2.2: Soften CheckComponentFunctionTargets to Warning

**Rationale:** Phase 1.5 will auto-fix the "component function on actor without Target" case when exactly 1 matching component exists. The Phase 0 validator should not block execution in this case -- it should downgrade to a warning. The validator should still ERROR when 0 or >1 components match (since Phase 1.5 can't auto-fix those).

### 4.2.1 — Add SCS Includes

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanValidator.cpp`
**Location:** After existing includes (line 6)

Add:

```cpp
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
```

### 4.2.2 — Modify CheckComponentFunctionTargets

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanValidator.cpp`
**Location:** Inside `CheckComponentFunctionTargets()`, AFTER the `bHasTargetWired` continue (line 103), BEFORE the error emission (line 106)

The current code at line 105-131 unconditionally emits an error. Replace the block from line 105 (`// ERROR: Component function on Actor BP without Target wire`) through line 131 (closing brace of the error `Add`) with:

```cpp
        // Count matching SCS components to determine if Phase 1.5 can auto-fix
        int32 MatchCount = 0;
        FString MatchedComponentName;
        if (Context.Blueprint->SimpleConstructionScript)
        {
            for (USCS_Node* SCSNode : Context.Blueprint->SimpleConstructionScript->GetAllNodes())
            {
                if (SCSNode && SCSNode->ComponentClass &&
                    SCSNode->ComponentClass->IsChildOf(Resolved.ResolvedOwningClass))
                {
                    MatchCount++;
                    if (MatchCount == 1)
                    {
                        MatchedComponentName = SCSNode->GetVariableName().ToString();
                    }
                }
            }
        }

        const FString* FunctionName = Resolved.Properties.Find(TEXT("function_name"));
        const FString* ClassName = Resolved.Properties.Find(TEXT("target_class"));
        const FString FuncDisplay = FunctionName ? *FunctionName : Resolved.StepId;
        const FString ClassDisplay = ClassName ? *ClassName : TEXT("ActorComponent");

        if (MatchCount == 1)
        {
            // Exactly one match -- Phase 1.5 will auto-wire. Downgrade to warning.
            Result.Warnings.Add(FString::Printf(
                TEXT("Step '%s': Component function '%s' (%s) has no Target wire. "
                     "Will be auto-wired to component '%s'."),
                *Resolved.StepId, *FuncDisplay, *ClassDisplay, *MatchedComponentName));

            UE_LOG(LogOlivePlanValidator, Log,
                TEXT("Phase 0: Step '%s' missing Target -- will auto-wire to '%s' (1 match)"),
                *Resolved.StepId, *MatchedComponentName);
            continue; // Skip error emission -- Phase 1.5 will handle it
        }

        // MatchCount == 0 or > 1: can't auto-fix -- emit error
        FString SuggestionText;
        if (MatchCount == 0)
        {
            SuggestionText = FString::Printf(
                TEXT("No components of type '%s' found in the Blueprint's Components panel. "
                     "Add a get_var step for the component, or use GetComponentByClass:\n"
                     "  {\"step_id\":\"get_comp\", \"op\":\"call\", \"target\":\"GetComponentByClass\", "
                     "\"inputs\":{\"ComponentClass\":\"%s\"}}\n"
                     "  Then on step '%s': \"inputs\":{\"Target\":\"@get_comp.auto\"}"),
                *ClassDisplay, *ClassDisplay, *Resolved.StepId);
        }
        else
        {
            SuggestionText = FString::Printf(
                TEXT("%d components of type '%s' found. Ambiguous -- wire Target explicitly:\n"
                     "  {\"step_id\":\"get_comp\", \"op\":\"get_var\", \"target\":\"<component_name>\"}\n"
                     "  Then on step '%s': \"inputs\":{\"Target\":\"@get_comp.auto\"}"),
                MatchCount, *ClassDisplay, *Resolved.StepId);
        }

        Result.Errors.Add(FOliveIRBlueprintPlanError::MakeStepError(
            TEXT("COMPONENT_FUNCTION_ON_ACTOR"),
            Resolved.StepId,
            FString::Printf(TEXT("/steps/%d/target"), *PlanIndexPtr),
            FString::Printf(
                TEXT("Function '%s' belongs to component class '%s', but this Blueprint "
                     "inherits from Actor. Without a Target pin wired to a component "
                     "reference, this will target Self (the Actor), causing a compile error."),
                *FuncDisplay, *ClassDisplay),
            SuggestionText));

        UE_LOG(LogOlivePlanValidator, Warning,
            TEXT("Phase 0: Step '%s' calls component function '%s' (%s) on Actor BP without Target wire (%d matches)"),
            *Resolved.StepId, *FuncDisplay, *ClassDisplay, MatchCount);
```

### Key Behavioral Change

Before: ALL missing-Target component calls produce errors, blocking execution.
After: Exactly-1-match produces a warning (allows execution, Phase 1.5 auto-fixes). Zero or many matches still produce errors (block execution).

---

<a id="change-23"></a>
## 5. Change 2.3: Pre-Compile Validation with Auto-Fix (Phase 5.5)

**Rationale:** After all wiring phases complete, some nodes may still be orphaned (exec input not wired) or have unwired component Targets (Phase 1.5 missed them somehow). Phase 5.5 detects these issues and auto-fixes where the plan provides enough intent.

### 5.3.1 — Phase 5.5 Method Declaration

**File:** `Source/OliveAIEditor/Blueprint/Public/Plan/OlivePlanExecutor.h`
**Location:** Inside `FOlivePlanExecutor` private section, AFTER `PhaseSetDefaults` declaration (line 224), BEFORE `PhaseAutoLayout` declaration (line 227)

Add:

```cpp
/**
 * Phase 5.5: Pre-compile validation with auto-fix.
 * Runs after all wiring phases, before auto-layout.
 *
 * Check 1: Orphaned impure nodes (exec input not wired).
 *   Auto-fix: If the orphan's plan step has exec_after set and the source
 *   step's primary exec output is unwired, reconnect them. This recovers
 *   from pin-name-drift failures in Phase 3.
 *
 * Check 2: Unwired Self/Target on component function calls.
 *   Reports as PreCompileIssue only (Phase 1.5 should have caught these).
 *   This is defense-in-depth.
 *
 * CONTINUE-ON-FAILURE. Never blocks execution.
 */
void PhasePreCompileValidation(
    FOlivePlanExecutionContext& Context);
```

### 5.3.2 — Phase 5.5 Implementation

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`
**Location:** Insert as a new section AFTER `PhaseSetDefaults` (ends at line 1565), BEFORE `PhaseAutoLayout` (line 1571)

```cpp
// ============================================================================
// Phase 5.5: Pre-Compile Validation with Auto-Fix
// ============================================================================

void FOlivePlanExecutor::PhasePreCompileValidation(
    FOlivePlanExecutionContext& Context)
{
    if (!Context.Graph || !Context.Plan)
    {
        return;
    }

    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

    for (UEdGraphNode* Node : Context.Graph->Nodes)
    {
        if (!Node)
        {
            continue;
        }

        UK2Node* K2Node = Cast<UK2Node>(Node);
        if (!K2Node)
        {
            continue;
        }

        // ================================================================
        // Check 1: Orphaned impure nodes -- has exec input, none wired
        // ================================================================
        bool bHasExecInput = false;
        bool bExecInputWired = false;

        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->bHidden)
            {
                continue;
            }
            if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
                Pin->Direction == EGPD_Input)
            {
                bHasExecInput = true;
                if (Pin->LinkedTo.Num() > 0)
                {
                    bExecInputWired = true;
                }
            }
        }

        if (bHasExecInput && !bExecInputWired &&
            !K2Node->IsA<UK2Node_Event>() &&
            !K2Node->IsA<UK2Node_CustomEvent>())
        {
            // This node has an exec input that is not wired.
            // Attempt auto-fix: if this node was created by a plan step that
            // has exec_after set, and the source step's exec output is unwired,
            // reconnect them. This catches cases where Phase 3 failed due to
            // pin name drift but the plan intent is clear.
            bool bAutoFixed = false;

            const FString StepId = Context.FindStepIdForNode(Node);
            if (!StepId.IsEmpty())
            {
                // Find the plan step
                for (const FOliveIRBlueprintPlanStep& Step : Context.Plan->Steps)
                {
                    if (Step.StepId != StepId || Step.ExecAfter.IsEmpty())
                    {
                        continue;
                    }

                    // Find the source node
                    UEdGraphNode* SourceNode = Context.GetNodePtr(Step.ExecAfter);
                    if (!SourceNode)
                    {
                        break;
                    }

                    // Find the first unwired exec output on the source node
                    UEdGraphPin* SourceExecOut = nullptr;
                    for (UEdGraphPin* Pin : SourceNode->Pins)
                    {
                        if (Pin && !Pin->bHidden &&
                            Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
                            Pin->Direction == EGPD_Output &&
                            Pin->LinkedTo.Num() == 0)
                        {
                            SourceExecOut = Pin;
                            break; // Take first unwired exec output
                        }
                    }

                    if (!SourceExecOut)
                    {
                        break; // Source has no unwired exec output
                    }

                    // Find this node's exec input pin
                    UEdGraphPin* TargetExecIn = Node->FindPin(UEdGraphSchema_K2::PN_Execute);
                    if (!TargetExecIn)
                    {
                        // Fallback: find any unwired exec input pin
                        for (UEdGraphPin* Pin : Node->Pins)
                        {
                            if (Pin && !Pin->bHidden &&
                                Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
                                Pin->Direction == EGPD_Input &&
                                Pin->LinkedTo.Num() == 0)
                            {
                                TargetExecIn = Pin;
                                break;
                            }
                        }
                    }

                    if (TargetExecIn && Schema->TryCreateConnection(SourceExecOut, TargetExecIn))
                    {
                        bAutoFixed = true;
                        Context.AutoFixCount++;
                        Context.SuccessfulConnectionCount++;

                        Context.Warnings.Add(FString::Printf(
                            TEXT("Phase 5.5: Auto-fixed orphaned exec on step '%s' "
                                 "<- '%s' (exec_after recovery)"),
                            *StepId, *Step.ExecAfter));

                        UE_LOG(LogOlivePlanExecutor, Log,
                            TEXT("Phase 5.5: Auto-fixed orphaned exec on '%s' <- '%s'"),
                            *StepId, *Step.ExecAfter);
                    }

                    break; // Found the matching step, stop searching
                }
            }

            if (!bAutoFixed)
            {
                FString NodeDesc = FString::Printf(TEXT("%s (%s)"),
                    *Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
                    *StepId);
                Context.PreCompileIssues.Add(FString::Printf(
                    TEXT("ORPHAN_NODE: '%s' has exec pins but is not connected "
                         "to any execution chain. It will never execute."),
                    *NodeDesc));

                UE_LOG(LogOlivePlanExecutor, Warning,
                    TEXT("Phase 5.5: Orphaned node '%s' could not be auto-fixed"),
                    *NodeDesc);
            }
        }

        // ================================================================
        // Check 2: Unwired Self/Target on component function calls
        // (Defense-in-depth for Phase 1.5 misses)
        // ================================================================
        UK2Node_CallFunction* CallFunc = Cast<UK2Node_CallFunction>(K2Node);
        if (CallFunc)
        {
            UFunction* Func = CallFunc->GetTargetFunction();
            if (Func && !Func->HasAnyFunctionFlags(FUNC_Static))
            {
                UEdGraphPin* SelfPin = CallFunc->FindPin(UEdGraphSchema_K2::PN_Self);
                if (SelfPin && !SelfPin->bHidden &&
                    SelfPin->LinkedTo.Num() == 0 &&
                    SelfPin->DefaultObject == nullptr)
                {
                    UClass* FuncClass = Func->GetOwnerClass();
                    if (FuncClass &&
                        FuncClass->IsChildOf(UActorComponent::StaticClass()))
                    {
                        const FString StepId2 = Context.FindStepIdForNode(Node);
                        FString NodeDesc = FString::Printf(TEXT("%s (step: %s)"),
                            *Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
                            StepId2.IsEmpty() ? TEXT("unknown") : *StepId2);
                        Context.PreCompileIssues.Add(FString::Printf(
                            TEXT("UNWIRED_TARGET: '%s' calls component function '%s' "
                                 "but Target pin is not wired. Will cause compile error."),
                            *NodeDesc, *Func->GetName()));

                        UE_LOG(LogOlivePlanExecutor, Warning,
                            TEXT("Phase 5.5: Unwired component Target on '%s' (function '%s')"),
                            *NodeDesc, *Func->GetName());
                    }
                }
            }
        }
    }
}
```

### 5.3.3 — Wire Phase 5.5 into Execute()

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`
**Location:** Inside `FOlivePlanExecutor::Execute()`, AFTER Phase 5 log (line 143), BEFORE the `}` that closes the BatchScope (line 144)

Insert:

```cpp
        // Phase 5.5: Pre-compile validation with auto-fix (CONTINUE-ON-FAILURE)
        UE_LOG(LogOlivePlanExecutor, Log, TEXT("Phase 5.5: Pre-Compile Validation"));
        PhasePreCompileValidation(Context);

        const int32 Phase55Fixes = Context.AutoFixCount - Phase15Fixes;
        UE_LOG(LogOlivePlanExecutor, Log,
            TEXT("Phase 5.5 complete: %d auto-fixes, %d unfixable issues"),
            Phase55Fixes, Context.PreCompileIssues.Num());
```

IMPORTANT: Phase 5.5 must run INSIDE the BatchScope (before line 144's `}`), because it may create connections via `Schema->TryCreateConnection()` which should be part of the batch.

To track Phase 1.5 vs Phase 5.5 fix counts separately in the log, capture the Phase 1.5 count before Phase 5.5 runs. Modify the Phase 1.5 wiring code (from Section 3.1.5) to store the Phase 1.5 count:

In `Execute()`, right before the BatchScope block opens, add:

```cpp
    const int32 Phase15Fixes = Context.AutoFixCount;
```

### 5.3.4 — Forward PreCompileIssues to Warnings

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`
**Location:** Inside `FOlivePlanExecutor::Execute()`, AFTER the BatchScope closing brace (line 144), BEFORE Phase 6 (line 147)

Insert:

```cpp
    // Forward pre-compile issues to warnings so they appear in the tool result
    for (const FString& Issue : Context.PreCompileIssues)
    {
        Context.Warnings.Add(Issue);
    }
```

### 5.3.5 — Include AutoFixCount in Result

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`
**Location:** Inside `AssembleResult()`, AFTER the partial success warning (around line 1655), BEFORE `return Result;` (line 1658)

Insert:

```cpp
    // Auto-fix count for transparency
    if (Context.AutoFixCount > 0)
    {
        Result.Warnings.Add(FString::Printf(
            TEXT("Executor auto-fixed %d issue(s) (component target wiring, orphaned exec recovery)."),
            Context.AutoFixCount));
    }
```

This adds a summary warning to the result. The individual auto-fix details are already in `Context.Warnings` from Phase 1.5 and Phase 5.5. The summary provides a quick count.

### Complete Phase Execution Order

After all changes, `Execute()` calls phases in this order:

```
Phase 1:   PhaseCreateNodes          -- FAIL-FAST
  (build NodeIdToStepId, set Context.Plan)
Phase 1.5: PhaseAutoWireComponentTargets -- CONTINUE-ON-FAILURE
  (capture Phase15Fixes = AutoFixCount)
  {
    FOliveBatchExecutionScope
    Phase 3:   PhaseWireExec          -- CONTINUE-ON-FAILURE
    Phase 4:   PhaseWireData          -- CONTINUE-ON-FAILURE
    Phase 5:   PhaseSetDefaults       -- CONTINUE-ON-FAILURE
    Phase 5.5: PhasePreCompileValidation -- CONTINUE-ON-FAILURE
  }
  (forward PreCompileIssues to Warnings)
Phase 6:   PhaseAutoLayout           -- ALWAYS SUCCEEDS
  (assemble result)
```

Note: Phase 1.5 runs OUTSIDE the BatchScope. It creates nodes (`UK2Node_VariableGet`) and uses `Schema->TryCreateConnection()` directly. These operations should participate in the outer transaction from the write pipeline but do NOT need batch scope suppression because Phase 1.5 does not use `FOlivePinConnector::Connect()` (which is what BatchScope suppresses). Phase 1.5 uses `UEdGraphSchema_K2::TryCreateConnection()` directly -- this is a lower-level API that does not create inner transactions.

### Data Flow for Phase 5.5

```
Phase 5 (SetDefaults) completes
    |
    v
Phase 5.5: PhasePreCompileValidation
    |
    v  Iterate ALL nodes in Context.Graph->Nodes
    |   (not just plan-created nodes -- catches any orphans)
    |
    |  Check 1: For each impure node with unwired exec input:
    |    1. FindStepIdForNode(Node) -- reverse lookup
    |    2. If step found, look up exec_after in Plan->Steps
    |    3. Get source node via Context.GetNodePtr(exec_after)
    |    4. Find first unwired exec output on source
    |    5. If found, TryCreateConnection(source_exec_out, target_exec_in)
    |    6. If success: AutoFixCount++, warning added
    |    7. If fail: add to PreCompileIssues
    |
    |  Check 2: For each UK2Node_CallFunction with unwired Self pin:
    |    1. Check if function owner is UActorComponent subclass
    |    2. If yes: add to PreCompileIssues (Phase 1.5 should have caught this)
    |
    v  PreCompileIssues forwarded to Warnings after BatchScope closes
    |
Phase 6 (AutoLayout)
```

### Edge Cases

1. **Non-plan nodes in graph**: Phase 5.5 iterates ALL nodes in the graph, including pre-existing ones. `FindStepIdForNode()` returns empty for non-plan nodes, so they only get reported as PreCompileIssues (not auto-fixed). This is correct -- we should NOT auto-wire nodes we didn't create.
2. **Phase 3 already wired this connection**: If Phase 3 succeeded for a step, its exec input is already wired. Check 1's `bExecInputWired` check skips it. No double-wiring.
3. **Source node has no unwired exec output**: If the source step's exec outputs are all used, we can't auto-fix. This happens when exec_outputs explicitly wires all outputs. Report as PreCompileIssue.
4. **PN_Execute not found**: Some nodes name the exec input differently (e.g., sequence nodes). The fallback searches for any unwired exec input pin. This matches UE's behavior where any node can have at most one primary exec input.
5. **Node not in plan**: `FindStepIdForNode()` returns empty for nodes not created by the plan. These are either pre-existing graph nodes or auto-injected nodes (from Phase 1.5). For auto-injected VariableGet nodes, they have no step ID and no exec pins, so they won't trigger either check. Correct.
6. **DefaultObject on Self pin**: Some self pins have `DefaultObject` set (e.g., when the self-pin targets a specific default object). The `SelfPin->DefaultObject == nullptr` check in Check 2 ensures we don't flag these as unwired. This matches the master plan's behavior.

---

<a id="implementation-order"></a>
## 6. Implementation Order

The coder should implement in this order:

### Step 1: Change 2.4 (bIsRequired)

**Files:** `OlivePinManifest.h`, `OlivePinManifest.cpp`
**Why first:** No dependencies on other changes. Smallest change. Adds the struct field and populates it. Can be built and tested independently.
**Verification:** Build. Trigger a `blueprint.apply_plan_json` with a call to a non-static member function. Check the pin manifest JSON in the result contains `"required": true` on the self pin and exec input.

### Step 2: Change 2.1 (Phase 1.5 Auto-Wire)

**Files:** `OlivePlanExecutor.h`, `OlivePlanExecutor.cpp`
**Why second:** Adds the new context fields (`AutoFixCount`, `NodeIdToStepId`, `Plan`, `PreCompileIssues`, `FindStepIdForNode`), the Phase 1.5 method, and wires it into Execute(). These context fields are also needed by Phase 5.5.
**Verification:** Build. Create a Blueprint with one StaticMeshComponent. Apply a plan with `{"op":"call","target":"SetRelativeLocation"}` (no Target input). Check: node is created, Phase 1.5 log says "Auto-wired", the self pin is connected to a VariableGet for the mesh component.

### Step 3: Change 2.2 (Soften Validator)

**Files:** `OlivePlanValidator.cpp`
**Why third:** Depends on understanding Phase 1.5's behavior (exactly-1-match = auto-fixable). Must add SCS includes.
**Verification:** Build. Apply the same plan as Step 2. Check: Phase 0 produces a WARNING (not an error). The plan proceeds to execution instead of being rejected.

### Step 4: Change 2.3 (Phase 5.5 Pre-Compile Validation)

**Files:** `OlivePlanExecutor.h` (already modified in Step 2), `OlivePlanExecutor.cpp`
**Why last:** Depends on context fields from Step 2. Largest change. Uses `FindStepIdForNode` and `Context.Plan`.
**Verification:** Build. Create a plan where Phase 3 exec wiring intentionally fails (misspell an exec_after step ID, then correct it in a separate fix). Run with Phase 5.5 enabled. Check: Phase 5.5 detects the orphaned node and auto-fixes by reconnecting via exec_after intent.

---

<a id="testing-strategy"></a>
## 7. Testing Strategy

### Manual Test Cases

**Test 1: Single Component Auto-Wire (Phase 1.5)**
- Create BP_TestGun (Actor BP) with one `UStaticMeshComponent` named "GunMesh"
- Plan: `[{op:"event", target:"BeginPlay"}, {op:"call", target:"SetRelativeLocation", inputs:{"NewLocation":"0,0,100"}}]`
- Expected: Phase 1.5 injects VariableGet("GunMesh"), wires to SetRelativeLocation's Self pin. Compiles.

**Test 2: Multiple Components Ambiguity (Phase 1.5)**
- Create BP_TestGun with two `UStaticMeshComponent` ("GunBody" and "GunBarrel")
- Same plan as Test 1
- Expected: Phase 1.5 warns "2 components match type 'StaticMeshComponent'". Does NOT auto-wire. Compile error in Self pin.

**Test 3: AI Provides Target (No Auto-Wire)**
- Plan includes `{"Target":"@get_mesh.auto"}` with a preceding get_var step
- Expected: Phase 1.5 skips this step (AI already wired Target)

**Test 4: Validator Downgrade (Phase 0)**
- Same setup as Test 1, use `preview_plan_json`
- Expected: Phase 0 returns warning "Will be auto-wired" instead of error. Preview succeeds.

**Test 5: Orphan Recovery (Phase 5.5)**
- Craft a plan where Phase 3 fails an exec wire (use a step with `exec_after` pointing to a valid step, but the exec pin name hint is wrong)
- Expected: Phase 5.5 detects the orphaned node, finds the source step via exec_after, reconnects. Log shows "Auto-fixed orphaned exec".

**Test 6: bIsRequired in Pin Manifest**
- Apply any plan with a call to a non-static function
- Check result JSON's `pin_manifests` for `"required": true` on the self pin and exec input

### Automated Test

Consider adding a test in `Source/OliveAIEditor/Private/Tests/Blueprint/` that:
1. Creates a transient Blueprint with one component
2. Builds a plan with a component function call (no Target)
3. Runs FOlivePlanExecutor::Execute()
4. Asserts: `Context.AutoFixCount == 1`, Self pin is connected, compile succeeds

---

## Appendix: Full Execute() Method After All Changes

For reference, here is the complete `Execute()` method structure after all Phase 2 changes:

```cpp
FOliveIRBlueprintPlanResult FOlivePlanExecutor::Execute(
    const FOliveIRBlueprintPlan& Plan,
    const TArray<FOliveResolvedStep>& ResolvedSteps,
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    const FString& AssetPath,
    const FString& GraphName)
{
    const double StartTime = FPlatformTime::Seconds();
    // ... existing logging ...

    // Initialize execution context
    FOlivePlanExecutionContext Context;
    Context.Blueprint = Blueprint;
    Context.Graph = Graph;
    Context.AssetPath = AssetPath;
    Context.GraphName = GraphName;

    // Phase 1: Create all nodes + build pin manifests (FAIL-FAST)
    UE_LOG(LogOlivePlanExecutor, Log, TEXT("Phase 1: Create Nodes"));
    const bool bNodesCreated = PhaseCreateNodes(Plan, ResolvedSteps, Context);

    if (!bNodesCreated)
    {
        // ... existing fail-fast return ...
        return AssembleResult(Plan, Context);
    }

    UE_LOG(LogOlivePlanExecutor, Log,
        TEXT("Phase 1 complete: %d nodes created"), Context.CreatedNodeCount);

    // NEW: Build reverse map and set plan pointer for Phase 5.5
    for (const auto& Pair : Context.StepToNodeMap)
    {
        Context.NodeIdToStepId.Add(Pair.Value, Pair.Key);
    }
    Context.Plan = &Plan;

    // NEW: Phase 1.5: Auto-wire component function targets
    UE_LOG(LogOlivePlanExecutor, Log, TEXT("Phase 1.5: Auto-Wire Component Targets"));
    PhaseAutoWireComponentTargets(Plan, ResolvedSteps, Context);
    // ... Phase 1.5 logging ...

    // Capture Phase 1.5 fix count for Phase 5.5 delta logging
    const int32 Phase15Fixes = Context.AutoFixCount;

    // Phases 3-5.5: Wiring + defaults + pre-compile validation
    {
        FOliveBatchExecutionScope BatchScope;

        // Phase 3: Wire exec connections (CONTINUE-ON-FAILURE)
        UE_LOG(LogOlivePlanExecutor, Log, TEXT("Phase 3: Wire Exec Connections"));
        PhaseWireExec(Plan, Context);
        // ... existing Phase 3 logging ...

        // Phase 4: Wire data connections (CONTINUE-ON-FAILURE)
        // ... existing Phase 4 code ...

        // Phase 5: Set pin defaults (CONTINUE-ON-FAILURE)
        // ... existing Phase 5 code ...

        // NEW: Phase 5.5: Pre-compile validation with auto-fix
        UE_LOG(LogOlivePlanExecutor, Log, TEXT("Phase 5.5: Pre-Compile Validation"));
        PhasePreCompileValidation(Context);
        const int32 Phase55Fixes = Context.AutoFixCount - Phase15Fixes;
        UE_LOG(LogOlivePlanExecutor, Log,
            TEXT("Phase 5.5 complete: %d auto-fixes, %d unfixable issues"),
            Phase55Fixes, Context.PreCompileIssues.Num());

    } // BatchScope destructor

    // NEW: Forward pre-compile issues to warnings
    for (const FString& Issue : Context.PreCompileIssues)
    {
        Context.Warnings.Add(Issue);
    }

    // Phase 6: Auto-layout (ALWAYS SUCCEEDS)
    // ... existing Phase 6 code ...

    return AssembleResult(Plan, Context);
}
```

---

## Appendix: Discrepancies with Master Plan

The master plan's Phase 2 code references several APIs that do not exist in the current codebase. This design resolves them as follows:

| Master Plan Reference | Actual Status | Resolution |
|---|---|---|
| `Context.GetCreatedNode(stepId)` returning `UK2Node*` | Does NOT exist | Use `Context.GetNodePtr(stepId)` which returns `UEdGraphNode*`, then `Cast<UK2Node>()` where needed |
| `Context.NodeIdToStepId` | Does NOT exist | Added as new field on `FOlivePlanExecutionContext` (Section 3.1.1), populated in `Execute()` |
| `Context.Plan` | Does NOT exist | Added as `const FOliveIRBlueprintPlan* Plan` on context (Section 3.1.1), set in `Execute()` |
| `Context.AutoFixCount` | Does NOT exist | Added as new field (Section 3.1.1) |
| `Context.PreCompileIssues` | Does NOT exist | Added as new field (Section 3.1.1) |
| `Context.GetNodeId(Node)` taking `UEdGraphNode*` | Does NOT exist (only `GetNodeId(FString StepId)` exists) | Added `FindStepIdForNode(const UEdGraphNode*)` method instead (Section 3.1.1). Phase 5.5 uses this to go from Node -> StepId, then uses StepId to look up plan intent |
| `UK2Node_VariableGet` in OlivePlanExecutor.cpp | Not currently included | Added to new includes (Section 3.1.4) along with SCS headers |
| `UK2Node_CallFunction` in OlivePlanExecutor.cpp | Not currently included | Added to new includes (Section 3.1.4) |
