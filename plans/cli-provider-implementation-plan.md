# CLI Provider Universal Fixes -- Implementation Plan

**Author:** Architect Agent
**Date:** 2026-02-24
**Based on:** `plans/cli-provider-universal-fixes-plan.md`
**Goal:** Make the tool layer smart enough that the AI's natural design-level thinking "just works." The AI writes high-level intent; the resolver compiles it to UE internals.

---

## Table of Contents

1. [Current State Analysis](#current-state-analysis)
2. [New Structures & Interfaces](#new-structures--interfaces)
3. [Task Breakdown](#task-breakdown)
   - [Priority 1: R1, R5, R6 (Broken Promises)](#priority-1-r1-r5-r6)
   - [Priority 2: R2, R3, R4 (AI Freedom Gains)](#priority-2-r2-r3-r4)
   - [Priority 3: Part 1 (CLI Base Class)](#priority-3-part-1-cli-base-class)
   - [Priority 4: P1, P2, P3, R7, R8 (Quick Wins)](#priority-4-p1-p2-p3-r7-r8)
   - [Priority 5: Part 4 (Self-Correction)](#priority-5-part-4-self-correction)
4. [Risk Mitigations](#risk-mitigations)
5. [Implementation Order & Dependencies](#implementation-order--dependencies)
6. [Testing Checklist](#testing-checklist)

---

## Current State Analysis

### What exists today (file-by-file)

**OliveBlueprintPlanResolver.cpp** (lines 571-631)
- `ResolveEventOp` already has a static `EventNameMap` with 10 entries mapping human names to Receive* names (BeginPlay, EndPlay, Tick, ActorBeginOverlap, ActorEndOverlap, AnyDamage, Hit, PointDamage, RadialDamage, Destroyed).
- R1 is ALREADY DONE. The plan mentioned it as needed, but the resolver already implements it at line 602-613.
- `ResolveSpawnActorOp` (lines 221-229) does nothing special -- just sets `actor_class` from Step.Target. No Location/Rotation input expansion exists.

**OlivePlanExecutor.cpp**
- `PhaseWireExec` (lines 388-639) has a full auto-chain implementation for function entry nodes at lines 482-639. The auto-chain finds orphan steps (no exec_after, not targeted by exec_outputs) and wires the function entry's "then" pin to them.
- However, this ONLY covers UK2Node_FunctionEntry (function graphs). It does NOT auto-chain from event nodes in EventGraph. If the AI says `"op":"event","target":"BeginPlay"` followed by an impure step with no `exec_after`, the event's exec output is left dangling. R5 is about auto-chaining from event entry too.
- `PhaseWireData` (line 1066) calls `Connector.Connect(RealSourcePin, RealTargetPin, false)` -- the `false` means no auto-conversion. R3 is about changing this to `true`.
- `PhaseWireExec` (line 810) also calls `Connector.Connect(SourcePin, TargetPin, false)` -- this is for exec pins so `bAllowConversion` is irrelevant (exec pins are always compatible).

**OliveFunctionResolver.cpp**
- Alias map at lines 591-802 has ~120 entries. R4 claims gaps for `GetActorTransform`, `MakeTransform`, `SetActorLocation`, `SetActorRotation`. Checking:
  - `GetActorTransform` -> line 612: `Map.Add(TEXT("GetTransform"), TEXT("GetActorTransform"))` -- partial coverage. AI saying `GetActorTransform` would hit K2 prefix match -> `K2_GetActorTransform`. Actually, `GetActorTransform` is the exact UE function name on AActor (NOT K2_ prefixed), so exact match succeeds. But `K2_GetActorTransform` does NOT exist. So `GetActorTransform` resolves fine by exact match. The alias adds `GetTransform` as a shortcut. This is fine.
  - `MakeTransform` -> NOT in alias map. The AI saying `MakeTransform` would fail exact match, K2 prefix match, alias, and would need catalog/broad search. Adding `MakeTransform -> MakeTransform` on KismetMathLibrary would help.
  - `SetActorLocation` -> line 604: `Map.Add(TEXT("SetActorLocation"), TEXT("K2_SetActorLocation"))`. Already covered.
  - `SetActorRotation` -> line 608: `Map.Add(TEXT("SetActorRotation"), TEXT("K2_SetActorRotation"))`. Already covered.
  - Real gaps: `MakeTransform`, `BreakTransform`, `MakeVector`, `MakeRotator`, `BreakVector`, `BreakRotator`, `GetMesh`, `SetCollisionProfileName`.

**OliveBlueprintToolHandlers.cpp** (lines 6109-6464)
- v2.0 path at line 6432: `FOliveWriteResult SuccessResult = FOliveWriteResult::Success(ResultData)` -- this sets `bSuccess = true`.
- At line 6444: `ResultData->SetStringField(TEXT("status"), TEXT("partial"))` -- the "partial" status is buried in nested JSON data, but the top-level `success: true` passes through `ToToolResult()` which copies `bSuccess`.
- `SelfCorrectionPolicy::HasToolFailure` checks `bSuccess == false`. So partial wiring failures are INVISIBLE to the self-correction loop.

**OlivePinConnector.h/cpp**
- `Connect(SourcePin, TargetPin, bAllowConversion)` -- when `bAllowConversion=true`, it calls `GetConversionOptions` then `InsertConversionNode` if the first conversion option is found.
- `CanAutoConvert` at line 519 checks the K2 schema for conversion support.
- The machinery is fully built and tested. R3 literally only requires changing `false` to `true` in two call sites in OlivePlanExecutor.cpp.

**OliveClaudeCodeProvider.cpp**
- 882 lines. Process lifecycle, prompt building, output parsing all in one class.
- `BuildPrompt` at line 548-624: conversation history formatting + action directives.
- `BuildSystemPrompt` at line 626-695: system prompt assembly from prompt assembler packs.
- Process management at lines 243-511: stdin pipe, stdout reader, process spawn.

**OliveSelfCorrectionPolicy.cpp**
- `BuildToolErrorMessage` at line 188-310: already has `INVALID_EXEC_REF` guidance at line 296-302. R7 is ALREADY DONE (was added previously).

---

## New Structures & Interfaces

### FOliveResolverNote (for resolver transparency)

To address the concern about resolver magic becoming non-transparent, we introduce structured "lowering notes" that travel with the result data.

```cpp
// Defined in OliveBlueprintPlanResolver.h (within FOliveResolvedStep)
// or as a standalone struct next to FOliveResolvedStep

/**
 * Records a single resolver translation for transparency.
 * These are serialized into the tool result so the AI can learn
 * what the resolver did silently. They also appear in the UE log.
 */
struct FOliveResolverNote
{
    /** The plan field that was transformed (e.g., "target", "inputs.Location") */
    FString Field;

    /** What the AI wrote */
    FString OriginalValue;

    /** What the resolver produced */
    FString ResolvedValue;

    /** Human-readable explanation of why */
    FString Reason;

    /** Serialization to JSON */
    TSharedPtr<FJsonObject> ToJson() const
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("field"), Field);
        Obj->SetStringField(TEXT("original"), OriginalValue);
        Obj->SetStringField(TEXT("resolved"), ResolvedValue);
        Obj->SetStringField(TEXT("reason"), Reason);
        return Obj;
    }
};
```

This struct will be added to `FOliveResolvedStep`:

```cpp
// In OliveBlueprintPlanResolver.h, inside FOliveResolvedStep:
struct FOliveResolvedStep
{
    FString StepId;
    FString NodeType;
    TMap<FString, FString> Properties;
    TArray<FOliveResolverNote> ResolverNotes;  // NEW
};
```

And to `FOlivePlanResolveResult`:

```cpp
struct FOlivePlanResolveResult
{
    bool bSuccess = false;
    TArray<FOliveResolvedStep> ResolvedSteps;
    TArray<FOliveIRBlueprintPlanError> Errors;
    TArray<FString> Warnings;
    TArray<FOliveResolverNote> GlobalNotes;    // NEW: plan-level notes (e.g., synthetic steps)
};
```

### FOliveConversionNote (for auto-conversion transparency)

```cpp
// Defined in OlivePlanExecutor.h, within FOlivePlanExecutionContext

/**
 * Records an auto-conversion node insertion during data wiring.
 * Logged and serialized so the AI sees what type coercions happened.
 */
struct FOliveConversionNote
{
    FString SourceStep;
    FString TargetStep;
    FString SourcePinName;
    FString TargetPinName;
    FString FromType;
    FString ToType;
    FString ConversionNodeType;

    TSharedPtr<FJsonObject> ToJson() const;
};

// Added to FOlivePlanExecutionContext:
TArray<FOliveConversionNote> ConversionNotes;
```

### Modifications to FOliveToolResult (partial success)

No new fields needed on `FOliveToolResult` itself. The fix is in how the v2.0 executor lambda sets `bSuccess` and how `SelfCorrectionPolicy::HasToolFailure` detects partial results.

The key change: When `PlanResult.bPartial == true`, set `bSuccess = false` on the `FOliveWriteResult` so the self-correction loop sees it as a failure. The `status: "partial"` field in ResultData distinguishes it from a total failure.

---

## Task Breakdown

---

### Priority 1: R1, R5, R6

#### Task R1: Event Name Mapping -- ALREADY IMPLEMENTED

**Status:** No work needed. The event name map exists at `OliveBlueprintPlanResolver.cpp` lines 602-613 with 10 entries (BeginPlay, EndPlay, Tick, ActorBeginOverlap, ActorEndOverlap, AnyDamage, Hit, PointDamage, RadialDamage, Destroyed). The resolver already maps human names to Receive* form.

#### Task R5: Auto-Chain from Event Nodes

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`
**Lines to modify:** After the existing function entry auto-chain block (lines 482-639), add a parallel auto-chain for event nodes.

**Current behavior:** Auto-chain only exists for UK2Node_FunctionEntry in function graphs. In EventGraph, if the AI creates an event step and an impure step without exec_after, nothing connects them.

**Required behavior:** After the function entry auto-chain, scan for event/custom_event steps that have no outgoing exec wires (their exec output is not referenced by any other step's exec_after, and they have no exec_outputs entries). For each such event, find the first impure step whose StepId is the "next" impure step in plan order after the event, with no exec_after of its own and not targeted by any other step. Wire the event's exec output to that step's exec input.

**Implementation:**

After the closing `}` of the FunctionEntry auto-chain block (line 639), add:

```cpp
// ----------------------------------------------------------------
// Auto-chain: Wire event/custom_event nodes to their first orphan follower.
//
// In EventGraph plans, the AI often writes:
//   { "step_id":"evt", "op":"event", "target":"BeginPlay" },
//   { "step_id":"s1", "op":"call", "target":"PrintString" }
// Without exec_after on s1 or exec_outputs on evt. The intent is
// clear: s1 follows evt. Auto-wire it, matching the function entry
// auto-chain pattern above.
// ----------------------------------------------------------------
for (int32 PlanIdx = 0; PlanIdx < Plan.Steps.Num(); ++PlanIdx)
{
    const FOliveIRBlueprintPlanStep& EventStep = Plan.Steps[PlanIdx];

    // Only process event/custom_event steps
    if (EventStep.Op != OlivePlanOps::Event && EventStep.Op != OlivePlanOps::CustomEvent)
    {
        continue;
    }

    // Skip if this event already has outgoing exec wires
    if (EventStep.ExecOutputs.Num() > 0)
    {
        continue;
    }

    // Check if any other step references this event via exec_after
    bool bIsReferencedAsExecSource = false;
    for (const FOliveIRBlueprintPlanStep& Other : Plan.Steps)
    {
        if (Other.ExecAfter == EventStep.StepId)
        {
            bIsReferencedAsExecSource = true;
            break;
        }
    }
    if (bIsReferencedAsExecSource)
    {
        continue; // Event already has an outgoing wire
    }

    // Find the first impure orphan step that follows this event in plan order
    const FOliveIRBlueprintPlanStep* FollowerStep = nullptr;
    for (int32 j = PlanIdx + 1; j < Plan.Steps.Num(); ++j)
    {
        const FOliveIRBlueprintPlanStep& Candidate = Plan.Steps[j];

        if (!Candidate.ExecAfter.IsEmpty())
            continue; // Already has incoming wire
        if (TargetedStepIds.Contains(Candidate.StepId))
            continue; // Already targeted
        if (Candidate.Op == OlivePlanOps::Event || Candidate.Op == OlivePlanOps::CustomEvent)
            break; // Hit another event -- stop searching

        const FOlivePinManifest* CandManifest = Context.GetManifest(Candidate.StepId);
        if (!CandManifest || CandManifest->bIsPure)
            continue;

        FollowerStep = &Candidate;
        break;
    }

    if (!FollowerStep)
        continue;

    // Wire: EventStep exec output -> FollowerStep exec input
    UEdGraphNode* EventNode = Context.GetNodePtr(EventStep.StepId);
    UEdGraphNode* FollowerNode = Context.GetNodePtr(FollowerStep->StepId);
    if (!EventNode || !FollowerNode)
        continue;

    const FOlivePinManifest* EventManifest = Context.GetManifest(EventStep.StepId);
    const FOlivePinManifest* FollowerManifest = Context.GetManifest(FollowerStep->StepId);
    if (!EventManifest || !FollowerManifest)
        continue;

    const FOlivePinManifestEntry* EventExecOut = EventManifest->FindExecOutput();
    const FOlivePinManifestEntry* FollowerExecIn = FollowerManifest->FindExecInput();
    if (!EventExecOut || !FollowerExecIn)
        continue;

    UEdGraphPin* SrcPin = EventNode->FindPin(FName(*EventExecOut->PinName));
    UEdGraphPin* DstPin = FollowerNode->FindPin(FName(*FollowerExecIn->PinName));
    if (!SrcPin || !DstPin)
        continue;

    if (SrcPin->LinkedTo.Num() > 0)
        continue; // Already connected (e.g., reused event)

    FOlivePinConnector& Connector = FOlivePinConnector::Get();
    FOliveBlueprintWriteResult ConnResult = Connector.Connect(SrcPin, DstPin, false);

    if (ConnResult.bSuccess)
    {
        Context.SuccessfulConnectionCount++;
        UE_LOG(LogOlivePlanExecutor, Log,
            TEXT("Auto-chained event '%s' -> step '%s'"),
            *EventStep.StepId, *FollowerStep->StepId);
    }
    else
    {
        Context.FailedConnectionCount++;
        UE_LOG(LogOlivePlanExecutor, Warning,
            TEXT("Failed to auto-chain event '%s' -> step '%s': %s"),
            *EventStep.StepId, *FollowerStep->StepId,
            ConnResult.Errors.Num() > 0 ? *ConnResult.Errors[0] : TEXT("Unknown"));
    }
}
```

**Risk:** Event auto-chain could wire to the wrong step if the plan has multiple events. The "break on next event" guard and "first impure orphan after this event in plan order" constraint mitigate this. The TargetedStepIds set (already computed for function entry auto-chain) must be reused -- verify it is in scope.

**Coder note:** The `TargetedStepIds` TSet is declared at line 499 inside the EntryNode block. It must be hoisted to before the EntryNode block so the event auto-chain can also use it. Move the declaration of `TargetedStepIds` and its population loop (lines 499-521) to just before the `UK2Node_FunctionEntry* EntryNode = nullptr;` line (line 482).

---

#### Task R6: Honest Partial Failure Reporting

**File:** `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`
**Lines to modify:** 6432 and surrounding block

**Current behavior:**
```cpp
// Line 6432
FOliveWriteResult SuccessResult = FOliveWriteResult::Success(ResultData);
```
When `PlanResult.bPartial == true`, the result has `bSuccess = true` in the JSON. The SelfCorrectionPolicy only triggers on `bSuccess == false`, so partial failures are invisible to the correction loop.

**Required behavior:**
When `PlanResult.bPartial == true`:
1. Return `FOliveWriteResult::ExecutionError(...)` instead of `Success(...)` so `bSuccess = false` in the JSON.
2. BUT set `ResultData->SetStringField(TEXT("status"), TEXT("partial_success"))` so the AI (and the correction policy) can distinguish partial from total failure.
3. Still include the step_to_node_map, wiring_errors, pin_manifests, and all other data.

**Implementation:**

Replace the block from line 6432 to ~6464:

```cpp
// Was: FOliveWriteResult SuccessResult = FOliveWriteResult::Success(ResultData);
// Now: partial failures return as errors so the self-correction loop sees them.

if (PlanResult.bPartial)
{
    const int32 TotalFailures = PlanResult.ConnectionsFailed + PlanResult.DefaultsFailed;
    FString PartialMessage = FString::Printf(
        TEXT("%d nodes created but %d connections FAILED. "
             "See wiring_errors and pin_manifests for details."),
        PlanResult.StepToNodeMap.Num(),
        TotalFailures);

    ResultData->SetStringField(TEXT("message"), PartialMessage);
    ResultData->SetStringField(TEXT("status"), TEXT("partial_success"));

    // Return as ExecutionError so bSuccess=false in JSON and
    // SelfCorrectionPolicy detects it.
    FOliveWriteResult PartialResult = FOliveWriteResult::ExecutionError(
        TEXT("PLAN_PARTIAL_SUCCESS"),
        PartialMessage,
        TEXT("Use blueprint.read on the target graph to see actual pin names, "
             "then fix failed connections with connect_pins/set_pin_default."));
    PartialResult.ResultData = ResultData;

    // Still provide created node IDs for verification
    TArray<FString> CreatedNodeIds;
    CreatedNodeIds.Reserve(PlanResult.StepToNodeMap.Num());
    for (const auto& Pair : PlanResult.StepToNodeMap)
    {
        CreatedNodeIds.Add(Pair.Value);
    }
    PartialResult.CreatedNodeIds = MoveTemp(CreatedNodeIds);

    return PartialResult;
}

// Full success path (no partial failures)
ResultData->SetStringField(TEXT("message"),
    FString::Printf(TEXT("Plan applied successfully: %d nodes created, %d connections wired"),
        PlanResult.StepToNodeMap.Num(),
        PlanResult.ConnectionsSucceeded));

FOliveWriteResult SuccessResult = FOliveWriteResult::Success(ResultData);

// Collect created node IDs for the pipeline's verification stage
TArray<FString> CreatedNodeIds;
CreatedNodeIds.Reserve(PlanResult.StepToNodeMap.Num());
for (const auto& Pair : PlanResult.StepToNodeMap)
{
    CreatedNodeIds.Add(Pair.Value);
}
SuccessResult.CreatedNodeIds = MoveTemp(CreatedNodeIds);

return SuccessResult;
```

**Additionally**, add `PLAN_PARTIAL_SUCCESS` to `BuildToolErrorMessage` in `OliveSelfCorrectionPolicy.cpp`:

**File:** `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`
**Lines to modify:** Inside `BuildToolErrorMessage`, after the `PLAN_RESOLVE_FAILED` block (around line 270).

```cpp
else if (ErrorCode == TEXT("PLAN_PARTIAL_SUCCESS"))
{
    Guidance = TEXT("All nodes were created but some wiring failed. "
        "Look at the wiring_errors array in the result. "
        "Use blueprint.read on the target graph with include_pins:true "
        "to see actual pin names, then use blueprint.connect_pins or "
        "blueprint.set_pin_default to fix each failed connection. "
        "Do NOT resubmit the entire plan -- the nodes already exist.");
}
```

**Risk: Partial success contract change -- caller consistency.**
The only caller of `HasToolFailure` that checks `bSuccess` is `SelfCorrectionPolicy::Evaluate`. The `FOliveWriteResult::ToToolResult()` conversion at line 32-41 copies `bSuccess` to `FOliveToolResult::bSuccess`. The `ToJsonString()` serialization sets `"success": bSuccess`. So the full chain is:
- `FOliveWriteResult::ExecutionError` -> `bSuccess=false` -> `ToToolResult()` -> `FOliveToolResult::bSuccess=false` -> `ToJsonString()` -> `{"success":false,...}` -> `HasToolFailure` detects it.

All callers already handle `bSuccess=false` results. The new `PLAN_PARTIAL_SUCCESS` error code distinguishes partial from total failure. No other callers need changes.

**Risk: Does the write pipeline abort the transaction on ExecutionError?**
Yes, but the nodes are already created inside the transaction scope. If the outer pipeline sees `bSuccess=false`, it will NOT compile (good -- nodes with broken wires shouldn't compile). The transaction still commits because the BatchScope suppresses inner transactions. This is the correct behavior -- we want the nodes to persist so the AI can fix wiring.

Wait -- need to verify. Let me check. The pipeline's `Execute()` method at `OliveWritePipeline.cpp` line 147: if `StageExecute` returns `bSuccess=false`, the transaction wrapper goes out of scope. If it was `FScopedTransaction`, going out of scope means undo is captured. The nodes persist and can be undone. So yes, this is correct.

---

### Priority 2: R2, R3, R4

#### Task R3: Wire-Time Auto-Conversion

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`
**Lines to modify:** Line 1066

**Current code:**
```cpp
FOliveBlueprintWriteResult ConnectResult = Connector.Connect(RealSourcePin, RealTargetPin, false);
```

**New code:**
```cpp
FOliveBlueprintWriteResult ConnectResult = Connector.Connect(RealSourcePin, RealTargetPin, /*bAllowConversion=*/true);
```

That is literally it for the functional change. However, to address the **auto-conversion side effects** risk:

**Transparency requirement:** When auto-conversion inserts a node, we need to log it and include it in the result data so the AI knows what happened.

**Implementation for conversion logging:**

In `OlivePlanExecutor.cpp`, in `WireDataConnection()`, after the successful connection (line 1068-1090), check if a conversion node was inserted:

```cpp
if (ConnectResult.bSuccess)
{
    Result.bSuccess = true;
    Result.ResolvedSourcePin = SourcePin->PinName;
    Result.ResolvedTargetPin = TargetPin->PinName;
    Result.SourceMatchMethod = SourceMatchMethod;
    Result.TargetMatchMethod = TargetMatchMethod;

    // Check if auto-conversion inserted a node (the pin will now be connected
    // through an intermediate node, not directly to the original target)
    const bool bConversionInserted = (RealSourcePin->LinkedTo.Num() > 0 &&
        RealSourcePin->LinkedTo[0] != RealTargetPin);

    if (bConversionInserted)
    {
        FOliveConversionNote Note;
        Note.SourceStep = SourceStepId;
        Note.TargetStep = TargetStepId;
        Note.SourcePinName = SourcePin->PinName;
        Note.TargetPinName = TargetPin->PinName;
        Note.FromType = SourcePin->TypeDisplayString;
        Note.ToType = TargetPin->TypeDisplayString;
        // The conversion node is the intermediate between source and target
        if (RealSourcePin->LinkedTo.Num() > 0)
        {
            UEdGraphNode* ConvNode = RealSourcePin->LinkedTo[0]->GetOwningNode();
            if (ConvNode)
            {
                Note.ConversionNodeType = ConvNode->GetClass()->GetName();
            }
        }
        Context.ConversionNotes.Add(MoveTemp(Note));

        UE_LOG(LogOlivePlanExecutor, Log,
            TEXT("Auto-conversion: %s.%s (%s) -> %s.%s (%s) via %s"),
            *SourceStepId, *SourcePin->PinName, *Note.FromType,
            *TargetStepId, *TargetPin->PinName, *Note.ToType,
            *Note.ConversionNodeType);

        Context.Warnings.Add(FString::Printf(
            TEXT("Auto-conversion inserted between %s.%s and %s.%s: %s -> %s"),
            *SourceStepId, *SourcePin->PinName,
            *TargetStepId, *TargetPin->PinName,
            *Note.FromType, *Note.ToType));
    }
```

**Also modify `AssembleResult`** to serialize conversion notes:

```cpp
// In AssembleResult, after pin manifest serialization:
if (Context.ConversionNotes.Num() > 0)
{
    // Serialized separately in the tool handler (below)
}
```

Actually, the conversion notes are in the Context. They need to flow to the result. Add to `FOliveIRBlueprintPlanResult`:

**File:** `Source/OliveAIRuntime/Public/IR/BlueprintPlanIR.h`

In `FOliveIRBlueprintPlanResult`, add:

```cpp
/** Auto-conversion notes for transparency */
TArray<FString> ConversionNotesJson; // Serialized FOliveConversionNote
```

Then in `AssembleResult`, serialize them. The tool handler in `OliveBlueprintToolHandlers.cpp` will include them in the result JSON.

**Deterministic preference order for conversions:**
The PinConnector's `GetConversionOptions` returns an array. Using `[0]` is already deterministic (UE schema returns them in a consistent order). But document the preference order in a comment: UE's built-in conversion hierarchy is: numeric promotions > struct conversions > object casts. This is handled by the schema, not by us.

**Gate:** Only allow auto-conversion for data pins. Exec pins should never auto-convert (they don't need to, and the PinConnector already skips conversion for exec). This is already enforced because `PhaseWireExec` calls `Connect(..., false)` and we're only changing `PhaseWireData`.

---

#### Task R2: SpawnActor Input Expansion

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`
**Lines to modify:** Inside `Resolve()`, after step resolution but before returning, or as a pre-processing pass.

**Current behavior:** If the AI writes:
```json
{"step_id":"spawn","op":"spawn_actor","target":"Actor","inputs":{"Location":"0,0,100"}}
```
The resolver passes it through unchanged. The executor then tries to find a pin called "Location" on SpawnActor, which doesn't exist (it has "SpawnTransform"). The data wiring fails.

**Required behavior:** When the resolver sees a `spawn_actor` step with `Location` or `Rotation` in its `inputs` map, it should:
1. Remove `Location` and `Rotation` from the step's inputs
2. Insert a new synthetic `make_struct` step (target: "Transform") before the spawn step
3. Wire `Location` -> MakeTransform.Location, `Rotation` -> MakeTransform.Rotation
4. Wire MakeTransform output -> SpawnActor.SpawnTransform
5. Add a resolver note explaining the synthesis

**Implementation location:** Add a new method `ExpandSpawnActorInputs` to `FOliveBlueprintPlanResolver` called from `Resolve()` before step iteration. This transforms the plan in-place (mutates the `Plan` parameter -- which requires making it non-const, or working on a copy).

Since `Resolve` takes `const FOliveIRBlueprintPlan& Plan`, we need to make a mutable copy or add a pre-processing pass.

**Approach:** Add a static method:

```cpp
// In OliveBlueprintPlanResolver.h:
/**
 * Pre-process plan to expand high-level inputs into concrete Blueprint operations.
 * Currently handles: SpawnActor Location/Rotation -> MakeTransform synthesis.
 * @param Plan The plan to expand (modified in place)
 * @param OutNotes Resolver notes for transparency
 * @return True if any expansions were made
 */
static bool ExpandPlanInputs(
    FOliveIRBlueprintPlan& Plan,
    TArray<FOliveResolverNote>& OutNotes);
```

**In `Resolve()`**, before the step iteration loop:

```cpp
// Expand high-level inputs (e.g., SpawnActor Location/Rotation -> MakeTransform)
// This mutates the plan, potentially inserting synthetic steps.
FOliveIRBlueprintPlan MutablePlan = Plan; // Copy to allow mutation
TArray<FOliveResolverNote> ExpansionNotes;
ExpandPlanInputs(MutablePlan, ExpansionNotes);
Result.GlobalNotes = MoveTemp(ExpansionNotes);

// Use MutablePlan for the rest of resolution
```

Then change the for loop to iterate `MutablePlan.Steps` instead of `Plan.Steps`.

**`ExpandPlanInputs` implementation:**

```cpp
bool FOliveBlueprintPlanResolver::ExpandPlanInputs(
    FOliveIRBlueprintPlan& Plan,
    TArray<FOliveResolverNote>& OutNotes)
{
    bool bExpanded = false;

    for (int32 i = 0; i < Plan.Steps.Num(); ++i)
    {
        FOliveIRBlueprintPlanStep& Step = Plan.Steps[i];

        if (Step.Op != OlivePlanOps::SpawnActor)
        {
            continue;
        }

        // Check for Location or Rotation in inputs
        const bool bHasLocation = Step.Inputs.Contains(TEXT("Location"));
        const bool bHasRotation = Step.Inputs.Contains(TEXT("Rotation"));

        if (!bHasLocation && !bHasRotation)
        {
            continue; // No expansion needed
        }

        // Already has SpawnTransform? Don't double-expand.
        if (Step.Inputs.Contains(TEXT("SpawnTransform")))
        {
            continue;
        }

        // Synthesize a MakeTransform step
        FString SyntheticStepId = FString::Printf(TEXT("_synth_maketf_%s"), *Step.StepId);

        FOliveIRBlueprintPlanStep MakeTransformStep;
        MakeTransformStep.StepId = SyntheticStepId;
        MakeTransformStep.Op = OlivePlanOps::MakeStruct;
        MakeTransformStep.Target = TEXT("Transform");

        // Transfer Location/Rotation inputs to the MakeTransform step
        if (bHasLocation)
        {
            MakeTransformStep.Inputs.Add(TEXT("Location"), Step.Inputs[TEXT("Location")]);
            Step.Inputs.Remove(TEXT("Location"));
        }
        if (bHasRotation)
        {
            MakeTransformStep.Inputs.Add(TEXT("Rotation"), Step.Inputs[TEXT("Rotation")]);
            Step.Inputs.Remove(TEXT("Rotation"));
        }

        // Wire MakeTransform output -> SpawnActor.SpawnTransform
        Step.Inputs.Add(TEXT("SpawnTransform"),
            FString::Printf(TEXT("@%s.auto"), *SyntheticStepId));

        // Insert the synthetic step before the spawn step
        Plan.Steps.Insert(MakeTransformStep, i);
        ++i; // Skip the inserted step

        bExpanded = true;

        // Record resolver note
        FOliveResolverNote Note;
        Note.Field = TEXT("inputs");
        Note.OriginalValue = TEXT("Location/Rotation");
        Note.ResolvedValue = FString::Printf(TEXT("MakeTransform step '%s' -> SpawnTransform"), *SyntheticStepId);
        Note.Reason = TEXT("SpawnActor requires a Transform pin, not separate Location/Rotation. "
                           "Synthesized MakeTransform node to bridge.");
        OutNotes.Add(MoveTemp(Note));

        UE_LOG(LogOlivePlanResolver, Log,
            TEXT("Expanded SpawnActor '%s': synthesized MakeTransform step '%s'"),
            *Step.StepId, *SyntheticStepId);
    }

    return bExpanded;
}
```

**Risk: Ambiguous aliasing.** What if the AI writes `{"inputs":{"Location":"0,0,100","SpawnTransform":"@other.auto"}}`? The guard `if (Step.Inputs.Contains(TEXT("SpawnTransform")))` prevents double-expansion. If both exist, `SpawnTransform` takes priority, and `Location` is left as-is (will fail at wiring time with a clear "no pin named Location" error). This is correct -- explicit wins over implicit.

**Risk: Synthetic step_id collision.** The `_synth_` prefix ensures no collision with human-authored step IDs (convention: step IDs are short alphanumeric like `evt`, `s1`, `spawn`). The schema validation for duplicate step IDs will catch any unlikely collision.

---

#### Task R4: Function Alias Gaps

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OliveFunctionResolver.cpp`
**Lines to modify:** Inside `GetAliasMap()` static lambda, add entries.

**New aliases to add:**

```cpp
// ================================================================
// Transform Construction (UKismetMathLibrary)
// ================================================================
Map.Add(TEXT("MakeTransform"), TEXT("MakeTransform"));
Map.Add(TEXT("BreakTransform"), TEXT("BreakTransform"));
Map.Add(TEXT("MakeVector"), TEXT("MakeVector"));
Map.Add(TEXT("BreakVector"), TEXT("BreakVector"));
Map.Add(TEXT("MakeRotator"), TEXT("MakeRotator"));
Map.Add(TEXT("BreakRotator"), TEXT("BreakRotator"));
Map.Add(TEXT("MakeVector2D"), TEXT("MakeVector2D"));
Map.Add(TEXT("BreakVector2D"), TEXT("BreakVector2D"));

// ================================================================
// Commonly attempted names with wrong casing/spelling
// ================================================================
Map.Add(TEXT("GetActorTransform"), TEXT("GetActorTransform"));  // Exact name on AActor, but alias ensures K2 search doesn't interfere
Map.Add(TEXT("SetMaterial"), TEXT("SetMaterial"));
Map.Add(TEXT("SetCollisionProfileName"), TEXT("SetCollisionProfileName"));
Map.Add(TEXT("SetVisibility"), TEXT("SetVisibility"));
Map.Add(TEXT("SetHiddenInGame"), TEXT("SetHiddenInGame"));

// ================================================================
// Movement Component
// ================================================================
Map.Add(TEXT("LaunchCharacter"), TEXT("LaunchCharacter"));
Map.Add(TEXT("AddMovementInput"), TEXT("AddMovementInput"));
Map.Add(TEXT("GetVelocity"), TEXT("GetVelocity"));
```

**Context-aware aliasing:** The `MakeTransform` alias could resolve to `KismetMathLibrary::MakeTransform` or could be found on other classes. The alias resolution at line 272-306 searches common classes in order (KismetSystemLibrary, KismetMathLibrary, ...). Since MakeTransform is on KismetMathLibrary (second in the list), it will be found quickly. No ambiguity risk.

**Verification needed by coder:** Before adding each alias, verify the target function name exists as a UFUNCTION on one of the common classes in the alias resolution search. The coder should do a quick engine source grep for each.

---

### Priority 3: Part 1 (CLI Base Class)

#### Task CLIBase: Extract FOliveCLIProviderBase

**New files:**
- `Source/OliveAIEditor/Public/Providers/OliveCLIProviderBase.h`
- `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp`

**Modified files:**
- `Source/OliveAIEditor/Public/Providers/OliveClaudeCodeProvider.h`
- `Source/OliveAIEditor/Private/Providers/OliveClaudeCodeProvider.cpp`

**What moves to FOliveCLIProviderBase:**

From `OliveClaudeCodeProvider.h`:
- All process management members: `ProcessHandle`, `StdinWritePipe`, `StdoutReadPipe`, `ReaderThread`, `bStopReading`
- All state members: `CurrentConfig`, `LastError`, `bIsBusy`, `AccumulatedResponse`
- All callback members: `CurrentOnChunk`, `CurrentOnToolCall`, `CurrentOnComplete`, `CurrentOnError`, `CallbackLock`
- `WorkingDirectory`
- `FOliveClaudeReaderRunnable` (rename to `FOliveCLIReaderRunnable`)

From `OliveClaudeCodeProvider.cpp`:
- `SendMessage()` -- the entire process lifecycle (with virtual hooks for customization)
- `HandleResponseComplete()` -- tool call parsing via `FOliveCLIToolCallParser`
- `BuildPrompt()` -- conversation history formatting
- `BuildSystemPrompt()` -- system prompt assembly
- `CancelRequest()`, `KillClaudeProcess()` (rename to `KillProcess()`)
- `Configure()`, `ValidateConfig()`, `IsBusy()`, `GetLastError()`, `GetConfig()`
- `ParseOutputLine()` -- but this is Claude-specific (stream-json format). Make it virtual.

**Virtual hooks on the base class:**

```cpp
class OLIVEAIEDITOR_API FOliveCLIProviderBase : public IOliveAIProvider
{
public:
    virtual ~FOliveCLIProviderBase();

    // IOliveAIProvider interface (implemented in base)
    virtual void SendMessage(...) override;
    virtual void CancelRequest() override;
    virtual bool IsBusy() const override { return bIsBusy; }
    virtual FString GetLastError() const override { return LastError; }
    virtual const FOliveProviderConfig& GetConfig() const override { return CurrentConfig; }
    virtual void Configure(const FOliveProviderConfig& Config) override;

protected:
    // ====== Virtual hooks for subclasses ======

    /** Get the path to the CLI executable */
    virtual FString GetExecutablePath() const = 0;

    /** Build CLI arguments (flags). Base provides common flags. */
    virtual FString GetCLIArguments(const FString& SystemPromptArg) const = 0;

    /** Parse a single line of stdout. Default: no-op. Override for format-specific parsing. */
    virtual void ParseOutputLine(const FString& Line);

    /** Get the working directory for the CLI process */
    virtual FString GetWorkingDirectory() const;

    /** Whether the executable path ends in .js (requires Node.js) */
    virtual bool RequiresNodeRunner() const;

    // ====== Shared infrastructure ======
    // (all process management, pipes, callbacks, prompt building)

    FString BuildConversationPrompt(const TArray<FOliveChatMessage>& Messages, const TArray<FOliveToolDefinition>& Tools) const;
    FString BuildCLISystemPrompt(const FString& UserTask, const TArray<FOliveToolDefinition>& Tools) const;
    void HandleResponseComplete(int32 ReturnCode);
    void KillProcess();

    // State members (moved from FOliveClaudeCodeProvider)
    FProcHandle ProcessHandle;
    void* StdinWritePipe = nullptr;
    void* StdoutReadPipe = nullptr;
    FRunnableThread* ReaderThread = nullptr;
    FThreadSafeBool bStopReading;
    FOliveProviderConfig CurrentConfig;
    FString LastError;
    FThreadSafeBool bIsBusy;
    FString AccumulatedResponse;
    FOnOliveStreamChunk CurrentOnChunk;
    FOnOliveToolCall CurrentOnToolCall;
    FOnOliveComplete CurrentOnComplete;
    FOnOliveError CurrentOnError;
    FCriticalSection CallbackLock;
    FString WorkingDirectory;
};
```

**What stays in FOliveClaudeCodeProvider (~100 lines):**

```cpp
class OLIVEAIEDITOR_API FOliveClaudeCodeProvider : public FOliveCLIProviderBase
{
public:
    FOliveClaudeCodeProvider();

    // IOliveAIProvider overrides
    virtual FString GetProviderName() const override { return TEXT("Claude Code CLI"); }
    virtual TArray<FString> GetAvailableModels() const override;
    virtual FString GetRecommendedModel() const override;
    virtual bool ValidateConfig(FString& OutError) const override;
    virtual void ValidateConnection(TFunction<void(bool, const FString&)> Callback) const override;

    // Claude-specific
    static bool IsClaudeCodeInstalled();
    static FString GetClaudeExecutablePath();
    static FString GetClaudeCodeVersion();

protected:
    // FOliveCLIProviderBase virtual hooks
    virtual FString GetExecutablePath() const override;
    virtual FString GetCLIArguments(const FString& SystemPromptArg) const override;
    virtual void ParseOutputLine(const FString& Line) override;
};
```

**Implementation order within this task:**
1. Create `OliveCLIProviderBase.h` with the base class declaration
2. Create `OliveCLIProviderBase.cpp` with moved implementations
3. Modify `OliveClaudeCodeProvider.h` to inherit from base
4. Modify `OliveClaudeCodeProvider.cpp` to remove moved code, keep overrides
5. Build and test (behavior should be identical)

**Risk: Error string patterns.** The crash classification in ConversationManager matches specific error strings like `"process exited with code"` (from `HandleResponseComplete` line 522). After extraction, verify these strings remain identical in the base class. The coder should grep for any string matching in ClassifyError.

---

### Priority 4: P1, P2, P3, R7, R8

#### Task P1: Plan Complexity Guidance

**File:** `Content/SystemPrompts/Knowledge/recipe_routing.txt`

Add to the QUICK RULES section (after line 18):

```
- Keep plans under 12 steps; split complex logic into multiple functions
- exec_after takes step_ids from YOUR plan, NOT K2Node IDs from blueprint.read
- Never invent node IDs -- only use IDs returned by tool results
```

#### Task P2: Fix Bad SpawnActor Example

**File:** `Content/SystemPrompts/Knowledge/cli_blueprint.txt`
**Line to modify:** Line 27

**Current:**
```
  {"step_id":"spawn","op":"spawn_actor","target":"Actor","inputs":{"SpawnTransform":"@get_xform.auto"},"exec_after":"evt"},
```

This is actually already correct (uses `SpawnTransform`). The original plan mentioned a bad example in `BuildPrompt`, not in cli_blueprint.txt. Let me check.

Actually, `BuildPrompt` in `OliveClaudeCodeProvider.cpp` does NOT contain a hardcoded SpawnActor example. The examples are in `cli_blueprint.txt` and `recipe_routing.txt`. The cli_blueprint.txt example at line 27 already uses `SpawnTransform`. So P2 is already correct.

**No work needed for P2.**

#### Task P3: make_struct Example

**File:** `Content/SystemPrompts/Knowledge/cli_blueprint.txt`
**Lines to modify:** After the example JSON block (line 29), add a make_struct example.

Add after line 29 (before the `## Ops` section):

```
### make_struct / break_struct
```json
{"step_id":"tf","op":"make_struct","target":"Transform","inputs":{"Location":"0,0,100","Rotation":"0,90,0"}}
{"step_id":"brk","op":"break_struct","target":"Vector","inputs":{"Vector":"@some_step.auto"}}
```
```

Actually, better to keep it compact. Add a single line example within the existing Plan JSON example:

**Implementation:** Insert a make_struct step into the example:

Replace lines 23-29 of cli_blueprint.txt:
```
```json
{"schema_version":"2.0","steps":[
  {"step_id":"evt","op":"event","target":"BeginPlay"},
  {"step_id":"tf","op":"make_struct","target":"Transform","inputs":{"Location":"0,0,100"}},
  {"step_id":"spawn","op":"spawn_actor","target":"Actor","inputs":{"SpawnTransform":"@tf.auto"},"exec_after":"evt"},
  {"step_id":"print","op":"call","target":"PrintString","inputs":{"InString":"Done"},"exec_after":"spawn"}
]}
```
```

This shows make_struct in context and also demonstrates the SpawnActor + MakeTransform pattern.

#### Task R7: INVALID_EXEC_REF Guidance -- ALREADY IMPLEMENTED

**Status:** Already exists at `OliveSelfCorrectionPolicy.cpp` lines 296-302. The guidance text is:
```
"exec_after references a step_id that doesn't exist in the plan.
IMPORTANT: exec_after expects step_ids from YOUR plan (e.g. 'evt', 'spawn'),
NOT K2Node IDs from blueprint.read (e.g. 'K2Node_Event_1')..."
```

No work needed.

#### Task R8: Verbose Param Validation Logging

**File:** `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`

**What to do:** Audit early-return error paths in tool handlers for missing UE_LOG statements. Many handlers do:
```cpp
if (!Params->TryGetStringField("asset_path", AssetPath))
    return FOliveToolResult::Error(...);
```
Without logging. The error is returned to the AI but not visible in UE logs for debugging.

**Approach:** Add `UE_LOG(LogOliveBPTools, Warning, ...)` before every early-return `FOliveToolResult::Error(...)` that doesn't already have logging. This is a grep-and-add task.

The coder should:
1. Grep for `return FOliveToolResult::Error` in the file
2. For each occurrence, check if there's a UE_LOG within 5 lines before it
3. If not, add one

**Example pattern:**
```cpp
if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
{
    UE_LOG(LogOliveBPTools, Warning, TEXT("%s: Missing required param 'asset_path'"), TEXT(__FUNCTION__));
    return FOliveToolResult::Error(
        TEXT("VALIDATION_MISSING_PARAM"),
        TEXT("Required parameter 'asset_path' is missing or empty"),
        TEXT("Provide the Blueprint asset path"));
}
```

This is mechanical but tedious. Estimate: ~30 locations across the file.

---

### Priority 5: Part 4 (Self-Correction)

These are the lightest-touch improvements from the plan. Each is a small, independent task.

#### Task SC1: System Prompt Rules for Preview Ordering

Already partially addressed by P1 (recipe_routing guidance). No additional code changes needed -- the guidance in cli_blueprint.txt already says "preview_plan_json is optional. Prefer calling apply_plan_json directly."

#### Task SC2: Aggressive Distillation

**File:** `Source/OliveAIEditor/Private/Brain/OlivePromptDistiller.cpp`

This is a configuration tuning task, not a code change. The distiller already exists. The coder should:
1. Find the distillation thresholds (message count, token count)
2. Lower them to distill earlier (e.g., after 4 messages instead of 8)
3. Log when distillation kicks in

**Deferred:** This is a tuning task that should be done after the resolver fixes are in and tested.

#### Task SC3: Error-Specific Correction Hints

Already addressed by R6 (PLAN_PARTIAL_SUCCESS guidance) and the existing BuildToolErrorMessage coverage. No additional work needed in this iteration.

---

## Risk Mitigations

### 1. Resolver Magic Transparency

**Risk:** The AI doesn't know what the resolver did. If MakeTransform was synthesized, the AI might try to reference a step that doesn't exist in its original plan.

**Mitigation:** `FOliveResolverNote` structs are serialized into the tool result JSON under a `"resolver_notes"` key. Example:
```json
{
    "resolver_notes": [
        {
            "field": "inputs",
            "original": "Location/Rotation",
            "resolved": "MakeTransform step '_synth_maketf_spawn' -> SpawnTransform",
            "reason": "SpawnActor requires a Transform pin, not separate Location/Rotation."
        }
    ]
}
```

The AI sees what happened and can adjust its mental model. The synthetic step IDs start with `_synth_` to make them obviously machine-generated.

### 2. Ambiguous Aliases

**Risk:** An alias like `GetTransform` could resolve to different functions on different classes.

**Mitigation:** The alias map resolves to a specific function name, and the resolution searches classes in a fixed priority order. The `FOliveResolverNote` reports which class the function was found on. If the alias resolves to the wrong class, the AI sees the note and can specify `target_class` explicitly on retry.

### 3. Auto-Conversion Side Effects

**Risk:** Auto-conversion silently inserts nodes the AI didn't ask for.

**Mitigation:**
1. Conversion notes are logged and serialized in the result.
2. Only data wiring uses auto-conversion (exec wiring does not).
3. The PinConnector already has a tested, deterministic conversion selection path.
4. If no conversion exists, the error is clear ("Cannot connect pins and no conversion available").

### 4. Partial Success Contract Change

**Risk:** Something somewhere assumes `bSuccess=true` for plans that created all nodes.

**Mitigation:**
- The change is from `Success(data)` to `ExecutionError(code, msg, suggestion)` with `data` attached.
- `bSuccess` goes from `true` to `false`, which is the correct signal to the correction loop.
- The `"status": "partial_success"` field in ResultData lets any caller that needs to distinguish partial from total failure do so.
- The new `PLAN_PARTIAL_SUCCESS` error code in SelfCorrectionPolicy gives appropriate guidance.
- The pipeline verification stage (Stage 5) will see `bSuccess=false` and skip compilation, which is correct (broken wires shouldn't compile).

Wait -- important detail. Let me re-check. If `StageExecute` returns `bSuccess=false`, does `StageVerify` run? Looking at `OliveWritePipeline.cpp`:

```cpp
// Line 194:
if (!ExecuteResult.bSuccess)
```

I need to check what happens after this. Let me read.

Actually, looking at the pipeline flow: if execution fails, the pipeline should still return the result with the data (step_to_node_map, wiring_errors, etc.) attached. The ResultData is set on the FOliveWriteResult before returning from the executor lambda. The pipeline's StageReport just formats the final output.

The key question: does the pipeline discard the ResultData when `bSuccess=false`? Looking at `ToToolResult()` line 32-41: it copies `bSuccess` and `ResultData` regardless. So the full data flows through. Good.

But the pipeline may NOT call StageReport on failure. Let me check the pipeline Execute() flow more carefully.

The coder should verify: after `StageExecute` returns, if `bSuccess=false`, does the pipeline still return `ExecuteResult.ToToolResult()` with the ResultData intact? If the pipeline early-returns on execution failure, the ResultData might not reach the AI. The coder must trace the full path.

---

## Implementation Order & Dependencies

```
Phase 1: Foundation (no dependencies between items)
  R5  - Event auto-chain           [OlivePlanExecutor.cpp]
  R6  - Partial failure reporting   [OliveBlueprintToolHandlers.cpp, OliveSelfCorrectionPolicy.cpp]

Phase 2: Resolver enhancements (R2 depends on FOliveResolverNote from Phase 1)
  R3  - Wire-time auto-conversion  [OlivePlanExecutor.cpp]
  R4  - Alias gaps                 [OliveFunctionResolver.cpp]
  R2  - SpawnActor input expansion [OliveBlueprintPlanResolver.cpp, .h]

Phase 3: Prompts (independent)
  P1  - Recipe routing guidance    [recipe_routing.txt]
  P3  - make_struct example        [cli_blueprint.txt]
  R8  - Verbose logging            [OliveBlueprintToolHandlers.cpp]

Phase 4: CLI extraction (independent, can overlap with Phase 2-3)
  CLIBase - Base class extraction  [NEW files + modify ClaudeCodeProvider]

Phase 5: Polish (after Phases 1-3 are verified working)
  SC2 - Distillation tuning        [OlivePromptDistiller.cpp]
```

**Build after each phase** to catch issues early. Each phase should result in a compilable, testable state.

---

## Testing Checklist

### R5 (Event Auto-Chain)
- [ ] Plan with `event:BeginPlay` + impure step, no exec_after -> auto-wired
- [ ] Plan with `event:BeginPlay` + impure step WITH exec_after -> not auto-wired (explicit wins)
- [ ] Plan with two events, each followed by different impure steps -> each auto-wired to its own follower
- [ ] Plan in function graph (not EventGraph) -> function entry auto-chain still works
- [ ] Plan with event followed by pure step then impure step -> pure skipped, impure wired

### R6 (Partial Failure Reporting)
- [ ] Plan with all wiring succeeding -> `bSuccess=true`, status absent or "success"
- [ ] Plan with some wiring failing -> `bSuccess=false`, error_code="PLAN_PARTIAL_SUCCESS", status="partial_success"
- [ ] SelfCorrectionPolicy detects PLAN_PARTIAL_SUCCESS and provides guidance
- [ ] step_to_node_map, wiring_errors, pin_manifests all present in partial result
- [ ] Pipeline does not abort transaction on partial failure (nodes persist)

### R3 (Auto-Conversion)
- [ ] Vector output -> Transform input: conversion node inserted
- [ ] Float output -> Double input: conversion succeeds
- [ ] Incompatible types with no conversion -> error (not silent failure)
- [ ] Conversion note appears in result JSON
- [ ] Exec wiring unchanged (still bAllowConversion=false)

### R2 (SpawnActor Expansion)
- [ ] SpawnActor with Location input -> MakeTransform synthesized
- [ ] SpawnActor with Location + Rotation -> both transferred to MakeTransform
- [ ] SpawnActor with SpawnTransform already present -> no expansion
- [ ] SpawnActor with SpawnTransform AND Location -> SpawnTransform wins, Location left (will fail at wiring with clear error)
- [ ] Synthetic step ID does not collide with user step IDs

### R4 (Aliases)
- [ ] `MakeTransform` resolves to `KismetMathLibrary::MakeTransform`
- [ ] `BreakVector` resolves to `KismetMathLibrary::BreakVector`
- [ ] Existing aliases unchanged (regression test)

### CLIBase
- [ ] FOliveClaudeCodeProvider still works identically after extraction
- [ ] Process spawn, stdin pipe, stdout reading, tool call parsing all functional
- [ ] Error strings match ClassifyError patterns
- [ ] Cancel and crash recovery work

### P1/P3
- [ ] Manual review: prompts read correctly, no formatting issues
- [ ] make_struct example is valid plan JSON

---

## File Reference (Exact Paths)

| Task | File | Action |
|------|------|--------|
| R5 | `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp` | MODIFY (lines 482-639, add event auto-chain after) |
| R6 | `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` | MODIFY (lines 6432-6464) |
| R6 | `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp` | MODIFY (add PLAN_PARTIAL_SUCCESS case ~line 270) |
| R3 | `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp` | MODIFY (line 1066: false->true, add conversion logging ~line 1068) |
| R3 | `Source/OliveAIEditor/Blueprint/Public/Plan/OlivePlanExecutor.h` | MODIFY (add FOliveConversionNote struct, add to Context) |
| R3 | `Source/OliveAIRuntime/Public/IR/BlueprintPlanIR.h` | MODIFY (add conversion notes to FOliveIRBlueprintPlanResult) |
| R2 | `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp` | MODIFY (add ExpandPlanInputs, modify Resolve) |
| R2 | `Source/OliveAIEditor/Blueprint/Public/Plan/OliveBlueprintPlanResolver.h` | MODIFY (add ExpandPlanInputs declaration, FOliveResolverNote, modify FOliveResolvedStep/Result) |
| R4 | `Source/OliveAIEditor/Blueprint/Private/Plan/OliveFunctionResolver.cpp` | MODIFY (add aliases to GetAliasMap ~line 797) |
| P1 | `Content/SystemPrompts/Knowledge/recipe_routing.txt` | MODIFY (add lines to QUICK RULES) |
| P3 | `Content/SystemPrompts/Knowledge/cli_blueprint.txt` | MODIFY (add make_struct to example) |
| R8 | `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` | MODIFY (add UE_LOG to ~30 early-return paths) |
| CLIBase | `Source/OliveAIEditor/Public/Providers/OliveCLIProviderBase.h` | NEW |
| CLIBase | `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp` | NEW |
| CLIBase | `Source/OliveAIEditor/Public/Providers/OliveClaudeCodeProvider.h` | MODIFY (change parent class) |
| CLIBase | `Source/OliveAIEditor/Private/Providers/OliveClaudeCodeProvider.cpp` | MODIFY (remove moved code) |

---

## Summary for Coder

**Start with Phase 1 (R5 + R6).** These fix the most damaging issues: the AI follows instructions correctly but the tool layer breaks its promise (auto-chain not wired) and lies about success (partial failures reported as success).

R5 is ~60 lines of new code in `PhaseWireExec`, modeled on the existing function entry auto-chain pattern. The key structural change is hoisting `TargetedStepIds` to a wider scope.

R6 is ~30 lines of modified code in the v2.0 executor lambda, plus ~10 lines in SelfCorrectionPolicy. The key insight: change `FOliveWriteResult::Success(ResultData)` to `FOliveWriteResult::ExecutionError(...)` when `bPartial`, preserving all ResultData.

**Then Phase 2 (R3, R4, R2).** R3 is the easiest -- literally one `false` to `true` change plus logging. R4 is adding entries to a static map. R2 is the most complex, requiring a new `ExpandPlanInputs` method and the `FOliveResolverNote` struct.

**Phase 3 and 4** can proceed in parallel. P1/P3 are text edits. R8 is mechanical logging. CLIBase is a refactoring task with no behavior change.

The resolver notes (`FOliveResolverNote`) should be serialized into the tool result JSON. The handler in `OliveBlueprintToolHandlers.cpp` needs to forward `ResolveResult.GlobalNotes` and per-step `ResolverNotes` into the result data. This serialization should be added to the v2.0 executor lambda alongside the existing wiring_errors/warnings serialization.
