# Olive AI — Master Implementation Plan v2

**Last updated:** Feb 25, 2026  
**Scope:** All phases from diagnosis through templates  
**Guiding principle:** Don't make AI smarter. Make the executor smarter.

---

## Table of Contents

1. [Phase 1: Stop the Bleeding — Component Variables & Logging](#phase-1)
2. [Phase 2: Auto-Wiring, Pre-Compile Validation & Auto-Fix](#phase-2)
3. [Phase 3: Error Recovery, Loop Prevention & Context Injection](#phase-3)
4. [Phase 4: Blueprint Templates](#phase-4)
5. [Phase 5: Expand Capabilities — Macros, Schema Simplification, Multi-Asset Budgeting](#phase-5)
6. [Appendix A: File Index](#appendix-a)
7. [Appendix B: Verification Checklist](#appendix-b)
8. [Appendix C: Schema Simplification Direction](#appendix-c)

---

<a id="phase-1"></a>
## Phase 1: Stop the Bleeding — Component Variables & Logging

**Impact:** Unblocks BP_Gun/BP_Bullet test case. Eliminates 3x retry loop.  
**Risk:** Low — surgical fixes to existing functions.  
**Effort:** ~2 hours.  
**Files touched:** 4

### The Root Cause

`BlueprintHasVariable()` only checks `Blueprint->NewVariables`, missing SCS component variables. In UE5, every SCS component IS a variable on the generated class (`USceneComponent* MuzzlePoint`). `UK2Node_VariableGet` with `SetSelfMember("MuzzlePoint")` creates the same node as dragging a component from the Components panel into the Event Graph. This one wrong check cascades:

1. Resolver rejects `get_var "MuzzlePoint"` → falls through to lenient fallback
2. Lenient fallback accepts but warns "use GetComponentByClass instead"
3. AI sees warning on retry → switches to GetComponentByClass pattern → more steps → more chances to fail
4. If AI sticks with get_var, `PLAN_RESOLVE_FAILED` error → 3 retries → loop detection → stop

---

### Change 1.1: Fix `BlueprintHasVariable()`

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`  
**Line:** ~33836

Replace the entire function:

```cpp
bool BlueprintHasVariable(const UBlueprint* Blueprint, const FString& VariableName)
{
    if (!Blueprint)
    {
        return false;
    }

    // Check explicit Blueprint variables (NewVariables array)
    for (const FBPVariableDescription& Var : Blueprint->NewVariables)
    {
        if (Var.VarName.ToString() == VariableName)
        {
            return true;
        }
    }

    // Check SCS component variables — every component in the SimpleConstructionScript
    // IS a variable on the generated class (e.g. USceneComponent* MuzzlePoint).
    // UK2Node_VariableGet::SetSelfMember("MuzzlePoint") works identically to
    // dragging the component from the Components panel into the Event Graph.
    if (Blueprint->SimpleConstructionScript)
    {
        for (USCS_Node* SCSNode : Blueprint->SimpleConstructionScript->GetAllNodes())
        {
            if (SCSNode && SCSNode->GetVariableName().ToString() == VariableName)
            {
                return true;
            }
        }
    }

    return false;
}
```

**Why:** `GetAllNodes()` returns a flat array of all SCS nodes (no manual tree walk). Components now pass validation at the same checkpoint as regular variables.

---

### Change 1.2: Remove Lenient Fallback from `ResolveGetVarOp`

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`  
**Lines:** ~34396–34453

After Change 1.1, components pass `BlueprintHasVariable` at the top of `ResolveGetVarOp`, so the SCS fallback block is dead code. **Delete the entire block** from:

```cpp
// Check if the name matches a component in the SCS.
// This catches a common AI mistake: using get_var for a component name.
if (BP->SimpleConstructionScript)
{
    FString MatchedComponentClass;
    TArray<USCS_Node*> NodesToSearch;
    // ... ~55 lines through the lenient return true ...
}
```

Through the closing brace and the `if (!MatchedComponentClass.IsEmpty())` block that returns `true`.

**Why:** Removes misleading "use GetComponentByClass instead" warnings. The AI's instinct to use `get_var` for components is actually correct — the system was rejecting it incorrectly.

---

### Change 1.3: Fix `set_var` Error Message for Components

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`  
**Line:** ~34554 (the `COMPONENT_NOT_VARIABLE` error in `ResolveSetVarOp`)

The rejection is correct (components are read-only), but the guidance is wrong. Replace the error message:

**Old:**
```cpp
TEXT("'%s' is a component (class: %s), not a variable. "
     "Use set_var only for Blueprint variables. "
     "To access a component, use: "
     "{\"op\":\"call\", \"target\":\"GetComponentByClass\", "
     "\"inputs\":{\"ComponentClass\":\"%s\"}}")
```

**New:**
```cpp
TEXT("'%s' is a component (class: %s), not a settable variable. "
     "Components are read-only references created in the Components panel. "
     "To READ this component, use get_var (returns the component reference). "
     "To MODIFY a property on it, first get_var, then call the setter: "
     "{\"step_id\":\"get_%s\", \"op\":\"get_var\", \"target\":\"%s\"}, "
     "then {\"op\":\"call\", \"target\":\"SetRelativeLocation\", "
     "\"inputs\":{\"Target\":\"@get_%s.auto\", ...}}")
```

Also replace the suggestion string:
```cpp
TEXT("Use get_var to read the component reference, then call "
     "setter functions with Target wired to the get_var output.")
```

---

### Change 1.4: Update `component_reference.txt` Recipe

**File:** `Content/SystemPrompts/Knowledge/recipes/blueprint/component_reference.txt`

**⚠️ CRITICAL: This must ship simultaneously with Change 1.1.** The current live recipe says "Do NOT use get_var for components" which actively fights the fix. If code ships without recipe update (or vice versa), behavior gets worse.

Replace entire file:

```
TAGS: component reference target getcomponentbyclass scene muzzle arrow transform access variable get_var
---
Components added in the Components panel ARE variables on the Blueprint.
Use get_var to get a reference, then call functions with Target wired to it:

  {"step_id":"get_muzzle", "op":"get_var", "target":"MuzzlePoint"}
  {"step_id":"get_tf",     "op":"call",    "target":"GetWorldTransform",
   "inputs":{"Target":"@get_muzzle.auto"}}

To modify a component property:
  {"step_id":"get_mesh",   "op":"get_var", "target":"GunMesh"}
  {"step_id":"set_vis",    "op":"call",    "target":"SetVisibility",
   "inputs":{"Target":"@get_mesh.auto", "bNewVisibility":"true"}}

RULES:
- get_var works for BOTH Blueprint variables AND components.
- set_var does NOT work for components (they are read-only references).
- Always wire Target on component functions to the get_var output.
- GetComponentByClass also works but get_var is simpler when you know the name.
- Common component classes: StaticMeshComponent, ArrowComponent, BoxComponent,
  SphereComponent, CapsuleComponent, AudioComponent, SkeletalMeshComponent.
```

---

### Change 1.5: Update Self-Correction Guidance for `COMPONENT_NOT_VARIABLE`

**File:** `Source/OliveAIEditor/Private/OliveSelfCorrectionPolicy.cpp`  
**Line:** ~69583 (inside `BuildToolErrorMessage`, the `COMPONENT_NOT_VARIABLE` case)

**Old:**
```cpp
else if (ErrorCode == TEXT("COMPONENT_NOT_VARIABLE"))
{
    Guidance = TEXT("You tried to use get_var on a component name. Components are NOT variables. "
        "The error message contains the exact correct pattern using GetComponentByClass. "
        "Replace the get_var step with the call pattern shown in the error message.");
}
```

**New:**
```cpp
else if (ErrorCode == TEXT("COMPONENT_NOT_VARIABLE"))
{
    Guidance = TEXT("You tried to use set_var on a component. Components are read-only and "
        "cannot be assigned with set_var. To READ a component, use get_var (this works). "
        "To MODIFY a component property, use get_var to get the reference, then call "
        "the setter function with Target wired to the get_var output.");
}
```

---

### Change 1.6: Log AI Text Responses on Zero-Tool Reprompt

**File:** `Source/OliveAIEditor/Private/OliveConversationManager.cpp`  
**Line:** ~70555 (inside the `ZeroToolRepromptCount < MaxZeroToolReprompts` block)

Add **before** the existing "AI responded text-only" log:

```cpp
// Log what the AI actually said so we can debug text-only responses
UE_LOG(LogOliveAI, Warning,
    TEXT("AI text-only response (first %d chars): %.500s"),
    FMath::Min(500, AssistantMessage.Content.Len()),
    *AssistantMessage.Content.Left(500));
```

---

### Change 1.7: Log AI Text Responses on Correction Reprompt

**File:** `Source/OliveAIEditor/Private/OliveConversationManager.cpp`  
**Line:** ~70585 (inside the `bHasPendingCorrections` reprompt block)

Same pattern:

```cpp
UE_LOG(LogOliveAI, Warning,
    TEXT("AI text-only with pending corrections (first %d chars): %.500s"),
    FMath::Min(500, AssistantMessage.Content.Len()),
    *AssistantMessage.Content.Left(500));
```

---

### Change 1.8: Add Comment to `ResolveSetVarOp` SCS Walk

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`  
**Line:** ~34520

```cpp
// NOTE: We keep this SCS walk even though BlueprintHasVariable() now also
// checks SCS components. We need the component CLASS NAME for the error
// message, which BlueprintHasVariable() doesn't return. If we later refactor
// BlueprintHasVariable to return component class info, this can be simplified.
```

---

<a id="phase-2"></a>
## Phase 2: Auto-Wiring, Pre-Compile Validation & Auto-Fix

**Impact:** Eliminates COMPONENT_FUNCTION_ON_ACTOR errors. Catches and auto-fixes most compile errors before compilation.  
**Risk:** Medium — adds new execution paths in the plan executor.  
**Effort:** ~8-10 hours.  
**Files touched:** 3-4

### Design Principle (Updated from Feedback)

The original plan had pre-compile validation that *reported* issues. The feedback correctly pointed out: if you can detect it, and the fix is unambiguous, just fix it. Reporting-and-hoping-the-AI-fixes-it is the pattern we're moving away from. Phase 2 now **auto-fixes** where safe and only reports when the fix is ambiguous.

---

### Change 2.1: Auto-Wire Component Targets in Phase 1.5

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`  
**Location:** After `PhaseCreateNodes` completes, before `FOliveBatchExecutionScope`

```cpp
// Phase 1.5: Auto-wire component function targets
// For each CallFunction node targeting a component-class function,
// if Target pin is unwired AND the Blueprint has exactly one component
// of the required class, inject a VariableGet node and wire it.
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

        // Only care about call ops targeting component classes
        if (!Resolved.ResolvedOwningClass ||
            !Resolved.ResolvedOwningClass->IsChildOf(UActorComponent::StaticClass()))
        {
            continue;
        }

        // Check if AI already wired Target
        const FOliveIRBlueprintPlanStep& PlanStep = Plan.Steps[i];
        const FString* TargetValue = PlanStep.Inputs.Find(TEXT("Target"));
        if (TargetValue && TargetValue->StartsWith(TEXT("@")))
        {
            continue; // AI provided a target reference — don't override
        }

        // Find the created node
        UK2Node* CreatedNode = Context.GetCreatedNode(Resolved.StepId);
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

        // Find matching components in SCS
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

        if (MatchingNodes.Num() != 1)
        {
            if (MatchingNodes.Num() > 1)
            {
                Context.Warnings.Add(FString::Printf(
                    TEXT("Step '%s': %d components match type '%s'. "
                         "Wire Target explicitly to disambiguate."),
                    *Resolved.StepId, MatchingNodes.Num(),
                    *RequiredClass->GetName()));
            }
            continue;
        }

        // Exactly one match — inject a VariableGet node and wire it
        USCS_Node* MatchedSCS = MatchingNodes[0];
        FName ComponentVarName = MatchedSCS->GetVariableName();

        UK2Node_VariableGet* GetNode = NewObject<UK2Node_VariableGet>(Context.Graph);
        GetNode->VariableReference.SetSelfMember(ComponentVarName);
        GetNode->AllocateDefaultPins();
        Context.Graph->AddNode(GetNode, false, false);

        // Wire the GetNode output to self pin
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
            }
        }
    }
}
```

**Call after Phase 1, declare in header, add `AutoFixCount` to context struct.**

---

### Change 2.2: Soften `CheckComponentFunctionTargets` to Warning When Auto-Fixable

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanValidator.cpp`  
**Line:** ~38650

When exactly one component matches, Phase 1.5 will auto-fix. Downgrade to warning:

```cpp
// Count matching components to determine if Phase 1.5 can auto-fix
int32 MatchCount = 0;
if (Context.Blueprint->SimpleConstructionScript)
{
    for (USCS_Node* SCSNode : Context.Blueprint->SimpleConstructionScript->GetAllNodes())
    {
        if (SCSNode && SCSNode->ComponentClass &&
            SCSNode->ComponentClass->IsChildOf(Resolved.ResolvedOwningClass))
        {
            MatchCount++;
        }
    }
}

if (MatchCount == 1)
{
    Result.Warnings.Add(FString::Printf(
        TEXT("Step '%s': Component function '%s' (%s) has no Target wire. "
             "Will be auto-wired to the single matching component."),
        *Resolved.StepId, *FuncDisplay, *ClassDisplay));
    UE_LOG(LogOlivePlanValidator, Log,
        TEXT("Phase 0: Step '%s' missing Target — will auto-wire (1 match)"),
        *Resolved.StepId, *FuncDisplay);
    continue; // Skip error emission — Phase 1.5 will handle it
}
// MatchCount == 0 or > 1: keep existing error (can't auto-fix)
```

---

### Change 2.3: Pre-Compile Validation with Auto-Fix (Phase 5.5)

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`  
**Location:** Between Phase 5 (SetDefaults) and Phase 6 (AutoLayout)

**Key change from v1:** This phase now *fixes* what it can, not just reports.

```cpp
void FOlivePlanExecutor::PhasePreCompileValidation(
    FOlivePlanExecutionContext& Context)
{
    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

    for (UEdGraphNode* Node : Context.Graph->Nodes)
    {
        if (!Node) continue;
        UK2Node* K2Node = Cast<UK2Node>(Node);
        if (!K2Node) continue;

        // ============================================================
        // Check 1: Orphaned impure nodes — has exec pins, none wired
        // ============================================================
        bool bHasExecInput = false;
        bool bExecInputWired = false;

        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->bHidden) continue;
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
            // AUTO-FIX ATTEMPT: If this node was created by a plan step that
            // has exec_after set, and the source step's "then" pin is unwired,
            // reconnect them. This catches cases where exec wiring failed due
            // to pin name drift but the intent is clear from the plan.
            bool bAutoFixed = false;
            const FString NodeId = Context.GetNodeId(Node);
            const FString* StepId = Context.NodeIdToStepId.Find(NodeId);

            if (StepId)
            {
                // Find the plan step
                for (const FOliveIRBlueprintPlanStep& Step : Context.Plan->Steps)
                {
                    if (Step.StepId == *StepId && !Step.ExecAfter.IsEmpty())
                    {
                        // Find the source node's exec output
                        UK2Node* SourceNode = Context.GetCreatedNode(Step.ExecAfter);
                        if (SourceNode)
                        {
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

                            if (SourceExecOut)
                            {
                                UEdGraphPin* TargetExecIn = Node->FindPin(
                                    UEdGraphSchema_K2::PN_Execute);
                                if (TargetExecIn &&
                                    Schema->TryCreateConnection(SourceExecOut, TargetExecIn))
                                {
                                    bAutoFixed = true;
                                    Context.AutoFixCount++;
                                    UE_LOG(LogOlivePlanExecutor, Log,
                                        TEXT("Phase 5.5: Auto-fixed orphaned exec on '%s' "
                                             "<- '%s' (exec_after recovery)"),
                                        *Step.StepId, *Step.ExecAfter);
                                }
                            }
                        }
                        break;
                    }
                }
            }

            if (!bAutoFixed)
            {
                FString NodeDesc = FString::Printf(TEXT("%s (%s)"),
                    *Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
                    *NodeId);
                Context.PreCompileIssues.Add(FString::Printf(
                    TEXT("ORPHAN_NODE: '%s' has exec pins but is not connected "
                         "to any execution chain. It will never execute."),
                    *NodeDesc));
            }
        }

        // ============================================================
        // Check 2: Unwired self/Target on component function calls
        // (Backup for anything Phase 1.5 missed)
        // ============================================================
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
                        // Phase 1.5 should have caught this — report as issue
                        FString NodeDesc = FString::Printf(TEXT("%s (%s)"),
                            *Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
                            *Context.GetNodeId(Node));
                        Context.PreCompileIssues.Add(FString::Printf(
                            TEXT("UNWIRED_TARGET: '%s' calls component function "
                                 "but Target pin is not wired."),
                            *NodeDesc));
                    }
                }
            }
        }
    }
}
```

**Add to context struct:**
```cpp
/** Pre-compile validation issues (Phase 5.5) — things that couldn't be auto-fixed */
TArray<FString> PreCompileIssues;

/** Count of issues auto-fixed by executor (Phase 1.5 + Phase 5.5) */
int32 AutoFixCount = 0;

/** Map from node ID to step ID for exec recovery */
TMap<FString, FString> NodeIdToStepId;

/** Pointer to plan for Phase 5.5 exec recovery */
const FOliveIRBlueprintPlan* Plan = nullptr;
```

**Include `AutoFixCount` in result JSON** so the AI (and logs) can see how much the executor corrected silently.

---

### Change 2.4: Add `bIsRequired` Flag to Pin Manifest Entry

**File:** `Source/OliveAIEditor/Blueprint/Public/Plan/OlivePlanExecutor.h`  
**Line:** ~53614 (FOlivePinManifestEntry struct)

Add a new field:

```cpp
/** Whether this pin is required for the node to function.
 *  true for: exec input on impure nodes, self/Target on non-static member functions.
 *  Surfaced in tool results so the AI knows which pins must be wired. */
bool bIsRequired = false;
```

**Populate it** in the manifest builder (~line 36380, where `FOlivePinManifestEntry` is created):

```cpp
// Determine if pin is required
Entry.bIsRequired = false;
if (Entry.bIsExec && Entry.bIsInput)
{
    // Exec input is required on non-event nodes
    Entry.bIsRequired = !K2Node->IsA<UK2Node_Event>() &&
                        !K2Node->IsA<UK2Node_CustomEvent>();
}
else if (!Entry.bIsExec && Entry.bIsInput && !Entry.bIsHidden)
{
    // Self/Target pin on non-static member functions
    if (Entry.PinName == UEdGraphSchema_K2::PN_Self.ToString())
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

**Include in JSON serialization** of pin manifests so the AI sees `"required": true` on critical pins.

---

<a id="phase-3"></a>
## Phase 3: Error Recovery, Loop Prevention & Context Injection

**Impact:** Breaks identical-plan retry loops. Provides blueprint context to reduce hallucination.  
**Risk:** Low-Medium.  
**Effort:** ~6 hours.  
**Files touched:** 3-4

---

### Change 3.1: Plan Content Deduplication (Proper Implementation)

**Rationale (from feedback):** The v1 plan hashed result JSON as a proxy — this false-positives on same-plan-different-error and false-negatives on different-plan-same-error. The correct approach: hash the actual plan JSON from tool call arguments, which is already stored in `FOliveOperationRecord.Params`.

**File:** `Source/OliveAIEditor/Private/OliveSelfCorrectionPolicy.cpp`  
**Location:** In `Evaluate()`, before the tool failure check (~line 69265)

```cpp
// Plan deduplication: detect when AI submits identical plans
if (ToolName == TEXT("apply_plan_json") || ToolName == TEXT("preview_plan_json"))
{
    const FString PlanHash = BuildPlanHash(ToolName, ResultJson);

    if (!PlanHash.IsEmpty())
    {
        const int32* PrevCount = PreviousPlanHashes.Find(PlanHash);
        if (PrevCount)
        {
            PreviousPlanHashes[PlanHash] = *PrevCount + 1;

            Decision.Action = EOliveCorrectionAction::FeedBackErrors;
            Decision.EnrichedMessage = FString::Printf(
                TEXT("[IDENTICAL PLAN - Seen %d time(s)] Your plan is identical to a "
                     "previous submission that failed with: %s\n\n"
                     "You MUST change the failing step's approach. Specifically:\n"
                     "- If a function wasn't found, use blueprint.search_nodes first\n"
                     "- If pin connection failed, use @step.auto instead of exact names\n"
                     "- If component Target was missing, add a get_var step and wire it\n"
                     "- Consider using olive.get_recipe for the correct pattern\n"
                     "Do NOT resubmit the same plan."),
                *PrevCount + 1, *ErrorMessage);

            UE_LOG(LogOliveAI, Warning,
                TEXT("SelfCorrection: Identical plan (hash %s, attempt %d)"),
                *PlanHash, *PrevCount + 1);

            // If seen 3+ times, escalate to stop
            if (*PrevCount >= 2)
            {
                Decision.Action = EOliveCorrectionAction::StopWorker;
                Decision.LoopReport = FString::Printf(
                    TEXT("Stopped: identical plan submitted %d times"), *PrevCount + 1);
            }

            return Decision;
        }
        else
        {
            PreviousPlanHashes.Add(PlanHash, 1);
        }
    }
}
```

**New helper** — hashes the actual plan content, not the result:

```cpp
FString FOliveSelfCorrectionPolicy::BuildPlanHash(
    const FString& ToolName,
    const FString& ResultJson) const
{
    // Get the most recent operation record for this tool from HistoryStore.
    // FOliveOperationRecord.Params contains the original tool call arguments
    // including the plan JSON.
    const FOliveOperationHistoryStore& History =
        FOliveConversationManager::Get().GetHistoryStore();

    // Find the most recent record for this tool
    const FOliveOperationRecord* LatestRecord = History.FindLatest(ToolName);
    if (!LatestRecord || !LatestRecord->Params.IsValid())
    {
        return FString(); // Can't hash — no record found
    }

    // Extract plan JSON from params
    // apply_plan_json params have: asset_path, graph_name, plan (the actual plan object)
    FString AssetPath = LatestRecord->Params->GetStringField(TEXT("asset_path"));
    FString GraphName = LatestRecord->Params->GetStringField(TEXT("graph_name"));

    // Serialize the plan object to get stable content for hashing
    const TSharedPtr<FJsonObject>* PlanObj = nullptr;
    FString PlanString;
    if (LatestRecord->Params->TryGetObjectField(TEXT("plan"), PlanObj) && PlanObj)
    {
        auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&PlanString);
        FJsonSerializer::Serialize((*PlanObj).ToSharedRef(), Writer);
        Writer->Close();
    }

    if (PlanString.IsEmpty())
    {
        return FString();
    }

    // Hash: tool + asset + graph + normalized plan content
    const FString Composite = FString::Printf(TEXT("%s|%s|%s|%s"),
        *ToolName, *AssetPath, *GraphName, *PlanString);

    return FOliveLoopDetector::HashString(Composite);
}
```

**Note:** This requires `FindLatest(ToolName)` on `FOliveOperationHistoryStore`. If that method doesn't exist, add it — it's a simple reverse scan of the Records array filtering by ToolName.

**Add to class:**
```cpp
TMap<FString, int32> PreviousPlanHashes;
FString BuildPlanHash(const FString& ToolName, const FString& ResultJson) const;
```

**Reset in `Reset()`:**
```cpp
PreviousPlanHashes.Empty();
```

---

### Change 3.2: Progressive Error Disclosure

**File:** `Source/OliveAIEditor/Private/OliveSelfCorrectionPolicy.cpp`  
**Location:** In `BuildToolErrorMessage()`

Escalate detail with each attempt:

```cpp
FString FOliveSelfCorrectionPolicy::BuildToolErrorMessage(
    const FString& ToolName,
    const FString& ErrorCode,
    const FString& ErrorMessage,
    int32 AttemptNum,
    int32 MaxAttempts) const
{
    FString Header = FString::Printf(
        TEXT("[TOOL FAILED - Attempt %d/%d] Tool '%s' error %s"),
        AttemptNum, MaxAttempts, *ToolName, *ErrorCode);

    FString Guidance;

    // ... [ALL existing error-code-specific if/else blocks unchanged] ...

    // Progressive escalation suffix
    FString Suffix;
    if (AttemptNum == 1)
    {
        // First attempt: just the error type + guidance. Don't overwhelm.
        // Omit raw error message — guidance is usually enough.
    }
    else if (AttemptNum == 2)
    {
        // Second attempt: add full error details
        Suffix = FString::Printf(
            TEXT("\n\nFull error: %s"), *ErrorMessage);
    }
    else
    {
        // Final attempt(s): add details + "try fundamentally different approach"
        Suffix = FString::Printf(
            TEXT("\n\nFull error: %s"
                 "\n\nThis is attempt %d of %d. Consider a fundamentally different approach: "
                 "use olive.get_recipe to find the correct pattern, "
                 "use @step.auto for all data wires, "
                 "or simplify the plan by breaking it into smaller operations."),
            *ErrorMessage, AttemptNum, MaxAttempts);
    }

    return FString::Printf(TEXT("%s\n%s%s"), *Header, *Guidance, *Suffix);
}
```

---

### Change 3.3: Update Stale Guidance Strings

**File:** `Source/OliveAIEditor/Private/OliveSelfCorrectionPolicy.cpp`

**3.3a:** `PLAN_RESOLVE_FAILED` case (~line 69555):  
Change `"get_var for a component"` → `"set_var for a component"`

**3.3b:** `PLAN_VALIDATION_FAILED` case (~line 69600):  
Add note about auto-wiring:
```cpp
"COMPONENT_FUNCTION_ON_ACTOR: if only one matching component exists, "
"the executor will auto-wire it. Otherwise, add a get_var step for the "
"component and wire its output to Target."
```

---

### Change 3.4: Blueprint Context Injection (Lightweight)

**Rationale (from feedback):** Injecting component names + classes into the prompt context reduces hallucination. NeoStack does this via "@ mentions" and "Attach to Agent Prompt." We can do a lightweight version: when a plan targets an existing Blueprint, inject its component list.

**File:** `Source/OliveAIEditor/Private/OlivePromptAssembler.cpp` (or equivalent prompt builder)

**When assembling the prompt for a blueprint task** and the target Blueprint already exists, append a compact context block:

```cpp
// If we know which Blueprint the AI is targeting, inject its component list
// This is cheap context (~200 tokens) that prevents hallucinated component names
FString BuildBlueprintContextBlock(const UBlueprint* Blueprint)
{
    if (!Blueprint || !Blueprint->SimpleConstructionScript)
    {
        return FString();
    }

    FString Block = FString::Printf(
        TEXT("\n[BLUEPRINT CONTEXT: %s (parent: %s)]\nComponents:\n"),
        *Blueprint->GetName(),
        *Blueprint->ParentClass->GetName());

    for (USCS_Node* SCSNode : Blueprint->SimpleConstructionScript->GetAllNodes())
    {
        if (!SCSNode) continue;
        FString ClassName = SCSNode->ComponentClass
            ? SCSNode->ComponentClass->GetName() : TEXT("Unknown");
        // Strip U prefix for display
        if (ClassName.StartsWith(TEXT("U")))
        {
            ClassName = ClassName.Mid(1);
        }
        Block += FString::Printf(TEXT("  - %s (%s)\n"),
            *SCSNode->GetVariableName().ToString(), *ClassName);
    }

    // Also list Blueprint variables
    if (Blueprint->NewVariables.Num() > 0)
    {
        Block += TEXT("Variables:\n");
        for (const FBPVariableDescription& Var : Blueprint->NewVariables)
        {
            Block += FString::Printf(TEXT("  - %s (%s)\n"),
                *Var.VarName.ToString(),
                *Var.VarType.PinCategory.ToString());
        }
    }

    Block += TEXT("[/BLUEPRINT CONTEXT]\n");
    return Block;
}
```

**Inject this** into the prompt when the task references an existing Blueprint asset path. The injection point depends on your prompt assembly flow — likely in the system prompt's dynamic context section.

**Scope carefully for CLI:** This adds ~200 tokens for a typical Blueprint. Worth the budget since it prevents entire categories of "component not found" / "variable doesn't exist" errors.

---

<a id="phase-4"></a>
## Phase 4: Blueprint Templates

**Impact:** Bypasses the entire resolve-execute-compile pipeline for common patterns. 5-52% improvement in pass@1 (per iEcoreGen study).  
**Risk:** Medium — new system, but each template is independently testable.  
**Effort:** ~12-16 hours (system + 5 templates).  
**Files touched:** New files + 1 tool registration

### Architecture

Templates are pre-validated Plan JSON files stored in `Content/Templates/`. A new tool `blueprint.create_from_template` loads a template, substitutes parameters, and feeds it directly into the existing plan executor. The AI's job reduces from "generate entire graph" to "select template and customize parameters."

---

### Change 4.1: Template File Format

Each template is a JSON file:

**File:** `Content/Templates/health_component.json`
```json
{
    "template_id": "health_component",
    "display_name": "Health Component",
    "description": "ActorComponent with CurrentHealth, MaxHealth, ApplyDamage, Heal, OnDeath",
    "parameters": {
        "max_health": { "type": "float", "default": "100.0", "description": "Maximum health" },
        "component_name": { "type": "string", "default": "HealthComponent", "description": "Name" }
    },
    "blueprint_type": "ActorComponent",
    "parent_class": "ActorComponent",
    "variables": [
        { "name": "CurrentHealth", "type": "Float", "default": "${max_health}" },
        { "name": "MaxHealth", "type": "Float", "default": "${max_health}" }
    ],
    "event_dispatchers": [
        { "name": "OnHealthChanged", "params": [
            { "name": "NewHealth", "type": "Float" },
            { "name": "Delta", "type": "Float" }
        ]},
        { "name": "OnDeath", "params": [] }
    ],
    "functions": [
        {
            "name": "ApplyDamage",
            "inputs": [{ "name": "DamageAmount", "type": "Float" }],
            "plan": {
                "schema_version": "2.0",
                "steps": [
                    { "step_id": "get_hp", "op": "get_var", "target": "CurrentHealth" },
                    { "step_id": "get_dmg", "op": "get_var", "target": "DamageAmount" },
                    { "step_id": "sub", "op": "call", "target": "Subtract_FloatFloat",
                      "inputs": { "A": "@get_hp.auto", "B": "@get_dmg.auto" } },
                    { "step_id": "clamp", "op": "call", "target": "FClamp",
                      "inputs": { "Value": "@sub.auto", "Min": "0.0", "Max": "${max_health}" },
                      "exec_after": "sub" },
                    { "step_id": "set_hp", "op": "set_var", "target": "CurrentHealth",
                      "inputs": { "value": "@clamp.auto" }, "exec_after": "clamp" },
                    { "step_id": "fire_change", "op": "call", "target": "OnHealthChanged",
                      "exec_after": "set_hp" },
                    { "step_id": "check_dead", "op": "call", "target": "LessEqual_FloatFloat",
                      "inputs": { "A": "@clamp.auto", "B": "0.0" } },
                    { "step_id": "branch", "op": "branch",
                      "inputs": { "Condition": "@check_dead.auto" },
                      "exec_after": "fire_change",
                      "exec_outputs": { "True": "fire_death" } },
                    { "step_id": "fire_death", "op": "call", "target": "OnDeath" }
                ]
            }
        }
    ]
}
```

**Key design decisions:**
- `${parameter}` syntax for substitution — simple string replacement before plan execution
- Plans use the same schema as `apply_plan_json` — no new executor code needed
- Variables, dispatchers, and functions are created via existing tools before plans execute
- Each template is self-contained and independently testable

---

### Change 4.2: Template Loader & Executor

**New File:** `Source/OliveAIEditor/Blueprint/Private/OliveTemplateSystem.h/.cpp`

```cpp
class OLIVEAIEDITOR_API FOliveTemplateSystem
{
public:
    static FOliveTemplateSystem& Get();

    /** Load all templates from Content/Templates/ */
    void LoadTemplates();

    /** List available templates with descriptions */
    TArray<FOliveTemplateInfo> GetAvailableTemplates() const;

    /** Apply a template with parameter overrides */
    FOliveBlueprintWriteResult ApplyTemplate(
        const FString& TemplateId,
        const TMap<FString, FString>& Parameters,
        const FString& AssetPath);

private:
    FString SubstituteParameters(
        const FString& TemplateJson,
        const TMap<FString, FString>& Params,
        const TMap<FString, FString>& Defaults) const;

    TMap<FString, TSharedPtr<FJsonObject>> LoadedTemplates;
};
```

**`ApplyTemplate` workflow:**
1. Load template JSON
2. Create Blueprint with specified type and parent class
3. Add variables with defaults
4. Add event dispatchers with parameter signatures
5. For each function: create function graph, substitute parameters in plan, run through existing `FOlivePlanExecutor`
6. Compile
7. Return result

---

### Change 4.3: Register `blueprint.create_from_template` Tool

Register in tool handlers with parameters:
- `template_id`: string (required)
- `asset_path`: string (required)
- `parameters`: object (optional) — parameter overrides
- `list_templates`: bool (optional) — if true, return available templates instead of creating

---

### Change 4.4: Initial Template Library (5 Templates)

| Template | File | Description |
|----------|------|-------------|
| `health_component` | `health_component.json` | ActorComponent: CurrentHealth, MaxHealth, ApplyDamage, Heal, OnHealthChanged, OnDeath |
| `projectile` | `projectile.json` | Actor: ProjectileMovement, collision sphere, speed/damage params, OnHit |
| `hitscan_weapon` | `hitscan_weapon.json` | Actor: LineTrace from muzzle, damage, fire rate, ammo |
| `pickup` | `pickup.json` | Actor: Overlap trigger, collected state, visual mesh, OnPickedUp |
| `door_trigger` | `door_trigger.json` | Actor: Box trigger, timeline open/close, locked state |

Each follows UE5 best practices:
- Event-driven (never tick-based) where possible
- Blueprint Interfaces for cross-BP communication
- Component-based architecture
- Proper clamping and null checks

---

### Change 4.5: Template Recipe

**New File:** `Content/SystemPrompts/Knowledge/recipes/blueprint/templates.txt`

```
TAGS: template health weapon projectile pickup door create_from_template common pattern
---
For common Blueprint patterns, use create_from_template instead of building from scratch:

Available templates:
- health_component: Health with damage/heal, clamping, death event
- projectile: Moving projectile actor with collision and damage
- hitscan_weapon: Line trace weapon with fire rate and ammo
- pickup: Collectible with trigger overlap
- door_trigger: Door with open/close timeline

Usage:
  blueprint.create_from_template({
    "template_id": "health_component",
    "asset_path": "/Game/Blueprints/BP_HealthComp",
    "parameters": {"max_health": "200.0"}
  })

Templates create fully-wired, pre-validated Blueprints. Use apply_plan_json
for custom logic that doesn't match any template.
```

---

<a id="phase-5"></a>
## Phase 5: Expand Capabilities — Macros, Schema Simplification, Multi-Asset Budgeting

**Impact:** New flow control nodes, reduced pin name failures, better multi-asset orchestration.  
**Risk:** Medium.  
**Effort:** ~10-12 hours.  
**Files touched:** 5-6

---

### Change 5.1: Add Macro Node Operations

**File:** `Source/OliveAIEditor/Blueprint/Private/OliveNodeFactory.cpp`

```cpp
UK2Node* FOliveNodeFactory::CreateMacroInstanceNode(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    const FString& MacroName)
{
    // Load StandardMacros library
    static const FString MacroLibPath =
        TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros");
    UBlueprint* MacroLib = Cast<UBlueprint>(
        StaticLoadObject(UBlueprint::StaticClass(), nullptr, *MacroLibPath));

    if (!MacroLib)
    {
        LastError = TEXT("Failed to load StandardMacros library");
        return nullptr;
    }

    UEdGraph* MacroGraph = nullptr;
    for (UEdGraph* FuncGraph : MacroLib->MacroGraphs)
    {
        if (FuncGraph && FuncGraph->GetFName().ToString() == MacroName)
        {
            MacroGraph = FuncGraph;
            break;
        }
    }

    if (!MacroGraph)
    {
        LastError = FString::Printf(
            TEXT("Macro '%s' not found in StandardMacros"), *MacroName);
        return nullptr;
    }

    UK2Node_MacroInstance* MacroNode = NewObject<UK2Node_MacroInstance>(Graph);
    MacroNode->SetMacroGraph(MacroGraph);
    MacroNode->AllocateDefaultPins();
    Graph->AddNode(MacroNode, false, false);

    return MacroNode;
}
```

**Macro mappings:**

| Plan Op | StandardMacros Graph Name |
|---------|--------------------------|
| `do_once` | `DoOnce` |
| `flip_flop` | `FlipFlop` |
| `gate` | `Gate` |
| `while_loop` | `WhileLoop` |
| `for_loop` | `ForLoop` |
| `for_each_loop` | `ForEachLoop` |

**Register in resolver dispatch** and update prompts/recipes.

---

### Change 5.2: Strengthen `@step.auto` as Default Wiring Mode

**File:** `Content/SystemPrompts/Knowledge/blueprint_authoring.txt`  
**And:** `Content/SystemPrompts/Knowledge/cli_blueprint.txt`

Update wiring guidance:

**Old:**
```
Data: @step.auto (type-match, use ~80%), @step.~hint (fuzzy), @step.PinName (exact)
```

**New:**
```
Data wires: ALWAYS use @step.auto unless the source step has multiple outputs of the
same type (rare). @step.auto type-matches using ground-truth pin info and handles 95%
of cases. Only fall back to @step.~hint or @step.PinName when auto is ambiguous
(e.g., a BreakTransform node with X/Y/Z float outputs that you need individually).
```

---

### Change 5.3: Improve `@step.auto` Fallback for Multi-Output Nodes

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`  
**Location:** In `FindTypeCompatibleOutput()`

When multiple outputs match, add heuristics instead of failing:

```cpp
if (MatchingPins.Num() > 1)
{
    // Heuristic 1: prefer ReturnValue (most common function output)
    for (const FOlivePinManifestEntry* Pin : MatchingPins)
    {
        if (Pin->PinName == TEXT("ReturnValue"))
        {
            UE_LOG(LogOlivePlanExecutor, Log,
                TEXT("Type auto-match: %d outputs match, using 'ReturnValue'"),
                MatchingPins.Num());
            return Pin;
        }
    }

    // Heuristic 2: use first match (ordered by pin index)
    UE_LOG(LogOlivePlanExecutor, Log,
        TEXT("Type auto-match: %d outputs match, using first ('%s')"),
        MatchingPins.Num(), *MatchingPins[0]->PinName);
    return MatchingPins[0];
}
```

---

### Change 5.4: Multi-Asset Iteration Budgeting

**Rationale (from feedback):** When the user request implies multiple assets (gun + bullet), the system burns all retries on asset A and never starts asset B. Prompts warn about this but the executor doesn't enforce it.

**File:** `Source/OliveAIEditor/Private/OliveSelfCorrectionPolicy.cpp`  
**Or:** `Source/OliveAIEditor/Private/Brain/OliveBrainLayer.cpp` (depending on where multi-asset tracking lives)

**Soft budget approach** (not hard force-switch, which can waste partial progress):

```cpp
// Track per-asset iteration counts
TMap<FString, int32> AssetIterationCounts;

// In Evaluate(), when a tool targets a specific asset:
int32& AssetCount = AssetIterationCounts.FindOrAdd(AssetPath, 0);
AssetCount++;

const int32 MaxPerAssetBudget = Policy.MaxTotalIterations / EstimatedAssetCount;

if (AssetCount >= MaxPerAssetBudget)
{
    // Soft nudge: suggest moving on
    Decision.EnrichedMessage += FString::Printf(
        TEXT("\n\n[BUDGET WARNING: You have spent %d of %d iterations on '%s'. "
             "Other assets still need work. Consider accepting the current state "
             "and moving to the next asset. You can return to fix issues later.]"),
        AssetCount, MaxPerAssetBudget,
        *FPaths::GetBaseFilename(AssetPath));
}

if (AssetCount >= MaxPerAssetBudget + 2)
{
    // Hard limit: force move on
    Decision.Action = EOliveCorrectionAction::StopWorker;
    Decision.LoopReport = FString::Printf(
        TEXT("Asset '%s' exceeded iteration budget (%d/%d). "
             "Partial result preserved. Remaining assets: %s"),
        *FPaths::GetBaseFilename(AssetPath),
        AssetCount, MaxPerAssetBudget,
        *BuildRemainingAssetsString());
}
```

**`EstimatedAssetCount`** comes from the Brain layer's task analysis (if it estimates N assets from the user prompt) or defaults to 2 for safety.

**Key design:** Soft warning at budget, hard stop at budget+2. The +2 buffer lets the AI finish what it's doing if it's close. This prevents the "99% done but force-switched" problem from a hard cut.

---

### Change 5.5: Schema Simplification Direction (Non-Breaking)

This isn't a code change — it's a prompt/recipe update that moves the AI toward simpler plans without changing the executor. See Appendix C for the full rationale.

**Update `blueprint_authoring.txt` and `cli_blueprint.txt` with new guidance:**

```
PLAN SIMPLIFICATION RULES:
1. exec_after is IMPLIED by step order for linear flows.
   Only specify exec_after when connecting to a non-previous step.
   The executor wires sequential steps automatically.

2. Target on component functions is AUTO-WIRED when unambiguous.
   If the Blueprint has exactly one component of the required type,
   you can omit the Target input. The executor injects it.

3. Use @step.auto for ALL data wires unless you need a specific pin
   from a multi-output node.

4. Omit step_id when the step is not referenced by any other step's
   inputs or exec_after. The executor assigns IDs internally.
```

**Important:** These are **prompt-only changes** that leverage executor capabilities from Phases 1-2. The executor already handles auto-wiring (Phase 2.1) and sequential exec wiring (Phase 2.3's auto-fix). The prompt changes just tell the AI it's safe to rely on them.

---

<a id="appendix-a"></a>
## Appendix A: File Index

| File | Changes |
|------|---------|
| `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp` | 1.1, 1.2, 1.3, 1.8, 5.1 (resolver dispatch) |
| `Content/SystemPrompts/Knowledge/recipes/blueprint/component_reference.txt` | 1.4 ⚠️ **Must ship with 1.1** |
| `Source/OliveAIEditor/Private/OliveSelfCorrectionPolicy.cpp` | 1.5, 3.1, 3.2, 3.3, 5.4 |
| `Source/OliveAIEditor/Private/OliveConversationManager.cpp` | 1.6, 1.7 |
| `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp` | 2.1, 2.3, 5.3 |
| `Source/OliveAIEditor/Blueprint/Public/Plan/OlivePlanExecutor.h` | 2.1, 2.3 (context struct), 2.4 |
| `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanValidator.cpp` | 2.2 |
| `Source/OliveAIEditor/Private/OlivePromptAssembler.cpp` | 3.4 (context injection) |
| `Source/OliveAIEditor/Blueprint/Private/OliveNodeFactory.cpp` | 5.1 |
| `Source/OliveAIEditor/Blueprint/Private/OliveTemplateSystem.h/.cpp` | 4.2 (new) |
| `Source/OliveAIEditor/Tools/OliveToolRegistration.cpp` | 4.3 |
| `Content/Templates/*.json` | 4.4 (new files) |
| `Content/SystemPrompts/Knowledge/recipes/blueprint/templates.txt` | 4.5 (new) |
| `Content/SystemPrompts/Knowledge/blueprint_authoring.txt` | 5.2, 5.5 |
| `Content/SystemPrompts/Knowledge/cli_blueprint.txt` | 5.2, 5.5 |
| `Source/OliveAIEditor/Private/Brain/OliveOperationHistory.cpp` | 3.1 (`FindLatest` helper) |

---

<a id="appendix-b"></a>
## Appendix B: Verification Checklist

### Phase 1 Tests

| Test | Expected |
|------|----------|
| `get_var "MuzzlePoint"` (component exists) | ✅ Resolves immediately, no warnings |
| `get_var "CurrentHealth"` (real variable) | ✅ Still works |
| `set_var "MuzzlePoint"` (component) | ❌ Rejected with NEW get_var→setter guidance |
| `set_var "CurrentHealth"` (real variable) | ✅ Still works |
| `get_var "NonExistent"` | ⚠️ Warning, allowed |
| BP_Gun full plan | ✅ No 3x retry loop |
| Text-only AI response | Log shows first 500 chars |

### Phase 2 Tests

| Test | Expected |
|------|----------|
| `GetWorldTransform` without Target (1 matching component) | ✅ Auto-wired, log shows Phase 1.5 fix |
| `GetWorldTransform` without Target (2+ components) | ❌ Error with disambiguation guidance |
| `GetWorldTransform` without Target (0 components) | ❌ Error as before |
| `GetWorldTransform` WITH explicit Target | ✅ Uses AI's target, no auto-wire |
| Orphaned node with clear exec_after in plan | ✅ Auto-fixed by Phase 5.5, log shows recovery |
| Orphaned node with no exec_after | ⚠️ Reported in PreCompileIssues |
| Result JSON includes `auto_fix_count` | ✅ Shows how many executor corrections |
| Pin manifest includes `required: true` for self pin | ✅ Visible in tool result |

### Phase 3 Tests

| Test | Expected |
|------|----------|
| Submit identical plan twice | ⚠️ IDENTICAL PLAN message with previous error |
| Submit identical plan 3 times | 🛑 Stop with loop report |
| Different plan, same error | ✅ Normal retry (not flagged as duplicate) |
| Same plan, different error | ✅ Normal retry (different result ≠ false positive... wait, we hash plan now not result) → ⚠️ Correctly flagged as duplicate |
| First tool failure | Error type + guidance only |
| Second tool failure | + full error details |
| Third tool failure | + "try different approach" |
| Blueprint with 3 components prompt | Context block shows all 3 with types |

### Phase 4 Tests

| Test | Expected |
|------|----------|
| `create_from_template("health_component", ...)` | ✅ BP created, compiles |
| `create_from_template("health_component", {max_health: "200"})` | ✅ MaxHealth = 200 |
| `create_from_template("nonexistent", ...)` | ❌ Template not found |
| `list_templates: true` | Returns 5 template descriptions |

### Phase 5 Tests

| Test | Expected |
|------|----------|
| Plan with `"op": "do_once"` | ✅ Creates DoOnce macro node |
| `@step.auto` on node with ReturnValue + other output | ✅ Picks ReturnValue |
| Multi-asset: 5 iterations on asset A, budget is 3 | ⚠️ Budget warning at 3, hard stop at 5 |
| Plan without exec_after (sequential steps) | ✅ Executor wires sequentially |

---

<a id="appendix-c"></a>
## Appendix C: Schema Simplification Direction

### Why Not Drop JSON

Plan JSON is the right abstraction. The AI describes intent, the executor handles implementation. This separation enables auto-wiring, validation, error recovery — everything in this plan.

Alternatives are worse:
- **Raw C++ / clipboard text**: Brittle, version-dependent, no validation layer
- **Natural language → executor**: Too ambiguous, needs intermediate representation anyway
- **Node-by-node (NeoStack style)**: Requires multi-turn interactive sessions, incompatible with CLI

### What to Simplify

The problem isn't JSON — it's asking the AI to specify things the executor already knows:

| Current (AI specifies) | Simplified (executor infers) |
|------------------------|------------------------------|
| `"exec_after": "prev_step"` for sequential steps | Implicit from step order |
| `"inputs": {"Target": "@get_comp.auto"}` for component functions | Auto-wired when unambiguous |
| `@step.ReturnValue` exact pin name | `@step.auto` handles it |
| `"step_id": "get_comp"` when unreferenced | Auto-assigned |

### Migration Path

1. **Phase 2 lands** → executor can auto-wire and auto-fix
2. **Phase 5.5 prompt update** → tell AI it can omit what executor infers
3. **Future: Schema 3.0** → formalize the simplified schema where exec_after is optional, Target is optional for component functions, step_id is optional for unreferenced steps
4. **Future: Deprecate Schema 1.0** → lowerer path becomes legacy

This is a gradual simplification, not a breaking change. Schema 2.0 plans with explicit exec_after/Target still work — they're just unnecessary.

---

## Implementation Order Summary

```
Phase 1 (~2h)   → Unblocks gun+bullet test case
   1.1 Fix BlueprintHasVariable
   1.2 Remove lenient fallback
   1.3 Fix set_var error message
   1.4 Update component_reference recipe  ⚠️ SHIP WITH 1.1
   1.5 Update COMPONENT_NOT_VARIABLE correction guidance
   1.6 Log AI text on zero-tool reprompt
   1.7 Log AI text on correction reprompt
   1.8 Comment on set_var SCS walk

Phase 2 (~8-10h) → Eliminates most compile errors via auto-fix
   2.1 Auto-wire component targets (Phase 1.5 in executor)
   2.2 Soften validator to warning when auto-fixable
   2.3 Pre-compile validation WITH auto-fix (Phase 5.5)
   2.4 Add bIsRequired to pin manifest entries

Phase 3 (~6h)   → Breaks retry loops, adds context
   3.1 Plan content dedup (hash actual plan JSON from HistoryStore)
   3.2 Progressive error disclosure
   3.3 Update stale guidance strings
   3.4 Blueprint context injection (component list in prompt)

Phase 4 (~12-16h) → Bypasses pipeline for common patterns
   4.1 Template file format
   4.2 Template loader & executor
   4.3 Register tool
   4.4 Write 5 templates
   4.5 Template recipe

Phase 5 (~10-12h) → New capabilities + orchestration
   5.1 Macro node operations
   5.2 Strengthen @step.auto default
   5.3 Improve auto fallback for multi-output
   5.4 Multi-asset iteration budgeting (soft + hard)
   5.5 Schema simplification prompts
```

**Total estimated effort:** ~38-46 hours  
**Expected outcome:** Gun+bullet succeeds on first attempt with templates, or within 1-2 attempts with plan generation. Common errors auto-fixed by executor. Retry loops broken by content-based dedup. Multi-asset tasks don't burn budget on one asset.
