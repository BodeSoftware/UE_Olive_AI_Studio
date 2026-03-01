# Auto-Wire Type Fixes Design

Three fixes addressing data wiring failures, silent JSON parsing data loss, and exec chain inference robustness in the plan executor pipeline.

---

## Issue 1: `@ref.auto` Type Matching Too Strict (CRITICAL)

### Problem

Three locations in OlivePlanExecutor.cpp compare pin types using exact equality (`PinCategory == PinCategory && PinSubCategoryObject.Get() == PinSubCategoryObject.Get()`). This fails for IS-A relationships: an `Actor Object Reference` output should match an `Object Wildcard` input (Actor IS-A Object), but the exact comparison rejects it.

Similarly, `FindTypeCompatibleOutput` compares `EOliveIRTypeCategory` and `PinSubCategory` strings at the manifest level, which also fails for subclass relationships.

### Log Evidence

```
Data wire FAILED: @overlap_begin.auto: No output pin on step 'overlap_begin'
matches target type 'Object Wildcard'. Available: OtherActor (Actor Object Reference)
```

### Root Cause

UE's K2 schema has `ArePinTypesCompatible()` which handles subclass relationships, wildcard matching, interface compatibility, and implicit conversions. The plan executor bypasses this entirely with manual exact comparisons.

### UE API Reference

```cpp
// In EdGraphSchema_K2.h (line 1091)
// Already included in OlivePlanExecutor.cpp (line 28)
// Already accessed via GetDefault<UEdGraphSchema_K2>() in the same file (lines 900, 1012, 2710)
virtual bool ArePinTypesCompatible(
    const FEdGraphPinType& Output,
    const FEdGraphPinType& Input,
    const UClass* CallingContext = NULL,
    bool bIgnoreArray = false) const;
```

This handles: Object subclass -> Object base class, Actor -> Object, Wildcard compatibility, Interface types, Array element compatibility, etc.

### Affected Locations (3 sites)

**Site A: Phase 3 FunctionResult walkback (lines 1717-1738)**

File: `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`

Current code at lines 1731-1738:
```cpp
if (CandPin->PinType.PinCategory != ResultPin->PinType.PinCategory)
{
    continue;
}
if (CandPin->PinType.PinSubCategoryObject.Get() != ResultPin->PinType.PinSubCategoryObject.Get())
{
    continue;
}
```

Replace with:
```cpp
// Use schema compatibility check instead of exact match.
// This handles IS-A relationships (Actor -> Object),
// wildcard pins, and interface types.
const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
if (!K2Schema->ArePinTypesCompatible(
        CandPin->PinType, ResultPin->PinType,
        Context.Blueprint ? Context.Blueprint->GeneratedClass : nullptr))
{
    continue;
}
```

Note: `ArePinTypesCompatible(Output, Input, ...)` -- first arg is the output (source), second is the input (target). `CandPin` is an output pin, `ResultPin` is an input pin on FunctionResult. This is the correct argument order.

**Site B: Phase 4 Dispatcher walkback (lines 2125-2131)**

File: `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`

Current code at lines 2130-2131:
```cpp
if (CandPin->PinType.PinCategory != DelegatePin->PinType.PinCategory) continue;
if (CandPin->PinType.PinSubCategoryObject.Get() != DelegatePin->PinType.PinSubCategoryObject.Get()) continue;
```

Replace with:
```cpp
const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
if (!K2Schema->ArePinTypesCompatible(
        CandPin->PinType, DelegatePin->PinType,
        Context.Blueprint ? Context.Blueprint->GeneratedClass : nullptr))
{
    continue;
}
```

Note: `CandPin` is output direction, `DelegatePin` is input direction. Correct order for `ArePinTypesCompatible(Output, Input)`.

**Optimization note for Sites A and B:** Move `GetDefault<UEdGraphSchema_K2>()` outside the inner loop. At Site A, declare it before line 1703 (the `while (WalkNode ...)` loop). At Site B, declare it before line 2118 (the `while (WalkNode ...)` loop). The `GetDefault` call is cheap (just returns a cached CDO pointer), but it is cleaner to hoist it.

**Site C: FindTypeCompatibleOutput + WireDataConnection fallback (lines 2273-2301)**

File: `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`

`FindTypeCompatibleOutput` at line 2453 works with manifest entries (not real pins), so we cannot directly use `ArePinTypesCompatible` inside it. Instead, add a **schema-based fallback** in `WireDataConnection` at the call site.

After `FindTypeCompatibleOutput` returns nullptr (after line 2278) and BEFORE the error path (line 2281), insert a fallback block:

```cpp
SourcePin = FindTypeCompatibleOutput(
    *SourceManifest, TargetPin->IRTypeCategory, TargetPin->PinSubCategory,
    TargetPin->PinName);

// Schema-based fallback: when manifest-level matching fails,
// use real UEdGraphPin objects + ArePinTypesCompatible for
// IS-A relationships that manifest categories cannot express
// (e.g., Actor Object Reference -> Object Wildcard).
if (!SourcePin)
{
    UEdGraphNode* FallbackSourceNode = Context.GetNodePtr(SourceStepId);
    UEdGraphNode* FallbackTargetNode = Context.GetNodePtr(TargetStepId);
    UEdGraphPin* RealTargetPin = FallbackTargetNode
        ? FallbackTargetNode->FindPin(FName(*TargetPin->PinName))
        : nullptr;

    if (FallbackSourceNode && RealTargetPin)
    {
        const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
        const FOlivePinManifestEntry* SchemaMatch = nullptr;
        bool bSchemaAmbiguous = false;

        TArray<const FOlivePinManifestEntry*> DataOutputs =
            SourceManifest->GetDataPins(/*bInput=*/false);

        for (const FOlivePinManifestEntry* CandEntry : DataOutputs)
        {
            UEdGraphPin* RealCandPin =
                FallbackSourceNode->FindPin(FName(*CandEntry->PinName));
            if (!RealCandPin)
            {
                continue;
            }

            if (K2Schema->ArePinTypesCompatible(
                    RealCandPin->PinType, RealTargetPin->PinType,
                    Context.Blueprint ? Context.Blueprint->GeneratedClass : nullptr))
            {
                if (!SchemaMatch)
                {
                    SchemaMatch = CandEntry;
                }
                else
                {
                    bSchemaAmbiguous = true;
                    break;
                }
            }
        }

        if (SchemaMatch && !bSchemaAmbiguous)
        {
            SourcePin = SchemaMatch;
            SourceMatchMethod = TEXT("type_auto_schema_fallback");
            UE_LOG(LogOlivePlanExecutor, Log,
                TEXT("  FindTypeCompatibleOutput schema fallback: "
                     "matched '%s' via ArePinTypesCompatible"),
                *SchemaMatch->PinName);
        }
    }
}
```

This block goes between line 2278 (current `FindTypeCompatibleOutput` call) and line 2281 (the `if (!SourcePin)` error block). The existing error block at 2281 remains as the final fallback if both manifest-level and schema-level matching fail.

### Why Not Change FindTypeCompatibleOutput Directly

`FindTypeCompatibleOutput` is designed to work with manifest-level type information (category + subcategory strings). Changing it to require real pins would break its abstraction. The schema fallback at the call site keeps the separation clean: manifest matching is fast and handles 95% of cases; schema matching is the safety net for IS-A relationships.

---

## Issue 2: JSON Array-as-String Silent Data Loss

### Problem

`JsonToStringMap()` in `BlueprintPlanIR.cpp` (line 55) calls `Pair.Value->AsString()` on every JSON value. When the AI sends an array value (e.g., `"exec_outputs": {"True": ["step_a", "step_b"]}`), `AsString()` on a `FJsonValueArray` logs `Json Value of type 'Array' used as a 'String'` and returns `""`. The data is silently lost.

Additionally, `TryGetStringField` for `exec_after` (line 140) returns false for array values, leaving the field as its default empty string.

### Log Evidence

```
LogJson: Error: Json Value of type 'Array' used as a 'String'.
LogJson: Error: Json Value of type 'Array' used as a 'String'.
LogJson: Error: Json Value of type 'Array' used as a 'String'.
LogJson: Error: Json Value of type 'Array' used as a 'String'.
```

### Affected Locations (2 sites)

**Site A: `JsonToStringMap` (BlueprintPlanIR.cpp lines 48-59)**

File: `Source/OliveAIRuntime/Private/IR/BlueprintPlanIR.cpp`

This function is called for `Inputs` (line 145), `Properties` (line 151), and `ExecOutputs` (line 157). All three can receive array values from malformed AI output.

Current code:
```cpp
TMap<FString, FString> JsonToStringMap(const TSharedPtr<FJsonObject>& Json)
{
    TMap<FString, FString> Map;
    if (Json.IsValid())
    {
        for (const auto& Pair : Json->Values)
        {
            Map.Add(Pair.Key, Pair.Value->AsString());
        }
    }
    return Map;
}
```

Replace with:
```cpp
TMap<FString, FString> JsonToStringMap(const TSharedPtr<FJsonObject>& Json)
{
    TMap<FString, FString> Map;
    if (Json.IsValid())
    {
        for (const auto& Pair : Json->Values)
        {
            FString StringValue;
            if (Pair.Value->TryGetString(StringValue))
            {
                // Normal string value
                Map.Add(Pair.Key, StringValue);
            }
            else if (Pair.Value->Type == EJson::Array)
            {
                // AI sent an array where a string was expected.
                // Take the first element as the value. This handles
                // common AI mistakes like:
                //   "exec_outputs": {"True": ["step_a", "step_b"]}
                //   "inputs": {"key": ["@step.pin"]}
                const TArray<TSharedPtr<FJsonValue>>& Arr = Pair.Value->AsArray();
                if (Arr.Num() > 0)
                {
                    FString FirstElement;
                    if (Arr[0]->TryGetString(FirstElement))
                    {
                        Map.Add(Pair.Key, FirstElement);
                        UE_LOG(LogOliveAI, Warning,
                            TEXT("JsonToStringMap: Key '%s' has Array value, "
                                 "coerced to first element '%s' "
                                 "(array had %d elements)"),
                            *Pair.Key, *FirstElement, Arr.Num());
                    }
                    else
                    {
                        // First element is not a string either; skip
                        UE_LOG(LogOliveAI, Warning,
                            TEXT("JsonToStringMap: Key '%s' has Array value "
                                 "with non-string first element, skipping"),
                            *Pair.Key);
                    }
                }
                else
                {
                    // Empty array -- skip
                    UE_LOG(LogOliveAI, Warning,
                        TEXT("JsonToStringMap: Key '%s' has empty Array value, skipping"),
                        *Pair.Key);
                }
            }
            else if (Pair.Value->Type == EJson::Number)
            {
                // AI sent a number where a string was expected.
                // Common for default values like "Duration": 2.0
                double NumVal = Pair.Value->AsNumber();
                // Use SanitizeFloat for clean formatting (no trailing zeros)
                StringValue = FString::SanitizeFloat(NumVal, 0);
                Map.Add(Pair.Key, StringValue);
            }
            else if (Pair.Value->Type == EJson::Boolean)
            {
                // AI sent a boolean where a string was expected.
                bool BoolVal = Pair.Value->AsBool();
                Map.Add(Pair.Key, BoolVal ? TEXT("true") : TEXT("false"));
            }
            else
            {
                // Null or Object -- skip with warning
                UE_LOG(LogOliveAI, Warning,
                    TEXT("JsonToStringMap: Key '%s' has unsupported type '%s', skipping"),
                    *Pair.Key, *Pair.Value->GetType());
            }
        }
    }
    return Map;
}
```

**Rationale for coercing numbers and booleans too:** While the log evidence only shows Array errors, the same `AsString()` pattern would also silently coerce numbers via `SanitizeFloat` (which does work for `FJsonValueNumber`) but without the clean formatting we'd prefer. Booleans via `AsString()` also work but log an error. Making all type handling explicit prevents future surprises and eliminates the `AsString()` error log noise.

Actually, re-checking: `FJsonValueNumber::TryGetString` returns the `SanitizeFloat` value, and `FJsonValueBoolean::TryGetString` returns "true"/"false". So the `TryGetString` path at the top of the if-chain already handles numbers and booleans correctly (they override `TryGetString` to return true). The explicit number/boolean branches are therefore unreachable and can be omitted.

Simplified version:
```cpp
TMap<FString, FString> JsonToStringMap(const TSharedPtr<FJsonObject>& Json)
{
    TMap<FString, FString> Map;
    if (Json.IsValid())
    {
        for (const auto& Pair : Json->Values)
        {
            FString StringValue;
            if (Pair.Value->TryGetString(StringValue))
            {
                // String, Number, Boolean all implement TryGetString
                Map.Add(Pair.Key, StringValue);
            }
            else if (Pair.Value->Type == EJson::Array)
            {
                // AI sent array where string expected -- take first element
                const TArray<TSharedPtr<FJsonValue>>& Arr = Pair.Value->AsArray();
                if (Arr.Num() > 0 && Arr[0]->TryGetString(StringValue))
                {
                    Map.Add(Pair.Key, StringValue);
                    UE_LOG(LogOliveAI, Warning,
                        TEXT("JsonToStringMap: Key '%s' has Array value, "
                             "coerced to first element '%s' (%d elements total)"),
                        *Pair.Key, *StringValue, Arr.Num());
                }
                else
                {
                    UE_LOG(LogOliveAI, Warning,
                        TEXT("JsonToStringMap: Key '%s' has empty or non-string Array, skipping"),
                        *Pair.Key);
                }
            }
            else
            {
                UE_LOG(LogOliveAI, Warning,
                    TEXT("JsonToStringMap: Key '%s' has unsupported type '%s', skipping"),
                    *Pair.Key, *Pair.Value->GetType());
            }
        }
    }
    return Map;
}
```

**Important:** `LogOliveAI` is declared in the OliveAIRuntime module. Verify it is available in BlueprintPlanIR.cpp. If not, use `LogTemp` or declare a local log category. Check the top of the file or the module's header for `DECLARE_LOG_CATEGORY_EXTERN(LogOliveAI, ...)`.

**Site B: `exec_after` field parsing (BlueprintPlanIR.cpp line 140)**

File: `Source/OliveAIRuntime/Private/IR/BlueprintPlanIR.cpp`

Current code:
```cpp
Json->TryGetStringField(TEXT("exec_after"), Step.ExecAfter);
```

Add array fallback after this line:
```cpp
Json->TryGetStringField(TEXT("exec_after"), Step.ExecAfter);

// Fallback: if exec_after was sent as an array, take first element
if (Step.ExecAfter.IsEmpty())
{
    const TArray<TSharedPtr<FJsonValue>>* ExecAfterArr = nullptr;
    if (Json->TryGetArrayField(TEXT("exec_after"), ExecAfterArr)
        && ExecAfterArr && ExecAfterArr->Num() > 0)
    {
        FString FirstVal;
        if ((*ExecAfterArr)[0]->TryGetString(FirstVal) && !FirstVal.IsEmpty())
        {
            Step.ExecAfter = FirstVal;
            UE_LOG(LogOliveAI, Warning,
                TEXT("Plan step '%s': exec_after was Array, coerced to first element '%s'"),
                *Step.StepId, *FirstVal);
        }
    }
}
```

**Why take the first element:** `exec_after` is semantically a single predecessor step ID. If the AI sends an array, the first element is the primary predecessor. Additional elements are ignored (the AI should use `exec_outputs` on the predecessor if it needs multi-target wiring).

### What About `step_id`, `op`, `target`, `target_class`?

These fields use `TryGetStringField` too (lines 136-139). If the AI sends arrays for these, `TryGetStringField` returns false and the fields stay empty. The plan validator will catch these as structural errors (`MISSING_STEP_ID`, `INVALID_OP`, etc.). No defensive parsing needed for these -- an array `step_id` or `op` is not recoverable, it is a malformed plan.

---

## Issue 3: InferMissingExecChain Multi-Event Robustness

### Problem

`InferMissingExecChain` in `OliveBlueprintPlanResolver.cpp` (line 2328) uses a linear scan with a single `PreviousImpureStepId` tracker. Two edge cases cause incorrect cross-event chaining:

1. **Empty strings in HasIncomingExec:** If Issue 2 causes `exec_outputs` values to become empty strings, `HasIncomingExec.Add("")` adds an empty string to the set. The real target step is NOT in `HasIncomingExec` and gets incorrectly inferred as following the previous chain.

2. **Interleaved step ordering:** If the AI lists steps in an order that interleaves two event chains (e.g., both events first, then all followers), the linear `PreviousImpureStepId` tracking chains followers to the wrong event.

### Root Cause Analysis

The current algorithm assumes:
- Step order matches exec flow order within each event chain
- `HasIncomingExec` accurately reflects which steps are wired

Both assumptions can be violated:
- Assumption 1: The AI might group by type (all events first) rather than by chain
- Assumption 2: Issue 2's data loss causes `HasIncomingExec` to be incomplete

### File and Location

File: `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`
Method: `FOliveBlueprintPlanResolver::InferMissingExecChain` (line 2328)

### Fix: Graph-Aware Inference

Replace the single-pass linear algorithm with a graph-aware approach that:
1. Identifies all event roots
2. Groups orphaned steps into per-event chains based on explicit references
3. Only infers within each event's reachable set

**Step 1: Filter empty strings from HasIncomingExec (line 2346-2357)**

After building `HasIncomingExec`, add:
```cpp
// Empty string entries come from malformed exec_outputs (Issue 2 coercion).
// They pollute the set without providing useful data.
HasIncomingExec.Remove(TEXT(""));
```

**Step 2: Build event-to-followers graph**

Before the main loop, build a reachability map from each event to its downstream steps using explicit exec_after and exec_outputs references:

```cpp
// Build event_id -> set of reachable step_ids using explicit references.
// This maps each event to the steps that are transitively wired from it.
TMap<FString, TSet<FString>> EventReachable;
TArray<FString> EventOrder; // Preserves event encounter order

for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
{
    if (Step.Op == OlivePlanOps::Event || Step.Op == OlivePlanOps::CustomEvent)
    {
        EventReachable.Add(Step.StepId, TSet<FString>());
        EventOrder.Add(Step.StepId);
    }
}

// Iterative flood-fill: follow exec_after and exec_outputs links
// to determine which event each step belongs to.
bool bChanged = true;
while (bChanged)
{
    bChanged = false;
    for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
    {
        if (PureStepIds.Contains(Step.StepId))
        {
            continue;
        }

        // Check if this step is directly or transitively reachable from an event
        for (auto& EventPair : EventReachable)
        {
            const FString& EventId = EventPair.Key;
            TSet<FString>& Reachable = EventPair.Value;

            // Direct: step's exec_after is the event or a reachable step
            if (!Step.ExecAfter.IsEmpty() &&
                (Step.ExecAfter == EventId || Reachable.Contains(Step.ExecAfter)))
            {
                bool bAdded = false;
                Reachable.Add(Step.StepId, &bAdded);
                if (bAdded) bChanged = true;
            }

            // Direct: step is targeted by exec_outputs of event or reachable step
            // (Already captured in HasIncomingExec, but also check the source)
            for (const FOliveIRBlueprintPlanStep& Other : Plan.Steps)
            {
                if (Other.StepId == EventId || Reachable.Contains(Other.StepId))
                {
                    for (const auto& ExecOut : Other.ExecOutputs)
                    {
                        if (ExecOut.Value == Step.StepId)
                        {
                            bool bAdded = false;
                            Reachable.Add(Step.StepId, &bAdded);
                            if (bAdded) bChanged = true;
                        }
                    }
                }
            }
        }
    }
}
```

**Step 3: Assign orphaned steps to nearest event**

For orphaned steps (not in any event's reachable set and not in `HasIncomingExec`), assign them to the nearest preceding event based on plan step order:

```cpp
// For orphaned steps, find the closest preceding event in step order.
// This is a heuristic: AIs typically list steps after their event root.
TMap<FString, FString> StepToEventMap;

// First, populate from the reachability graph
for (const auto& EventPair : EventReachable)
{
    for (const FString& StepId : EventPair.Value)
    {
        StepToEventMap.Add(StepId, EventPair.Key);
    }
}
```

**Step 4: Per-event linear inference**

Replace the single-pass loop (lines 2362-2402) with a per-event loop:

```cpp
int32 InferredCount = 0;

for (const FString& EventId : EventOrder)
{
    // Collect this event's orphaned steps in plan order
    FString PreviousImpureStepId = EventId;

    for (FOliveIRBlueprintPlanStep& Step : Plan.Steps)
    {
        if (PureStepIds.Contains(Step.StepId))
        {
            continue;
        }
        if (Step.Op == OlivePlanOps::Event || Step.Op == OlivePlanOps::CustomEvent)
        {
            continue;
        }

        // Only process steps belonging to this event (or unassigned)
        const FString* OwnerEvent = StepToEventMap.Find(Step.StepId);
        if (OwnerEvent && *OwnerEvent != EventId)
        {
            continue; // Belongs to a different event
        }
        if (!OwnerEvent)
        {
            // Unassigned orphan: assign to nearest preceding event
            // Check if this event is the nearest predecessor in step order
            // by checking that no later event appears between this event
            // and this step in the plan.
            bool bThisEventIsNearest = true;
            bool bPassedEvent = false;
            for (const FOliveIRBlueprintPlanStep& Scan : Plan.Steps)
            {
                if (Scan.StepId == EventId)
                {
                    bPassedEvent = true;
                    continue;
                }
                if (Scan.StepId == Step.StepId)
                {
                    break;
                }
                if (bPassedEvent &&
                    (Scan.Op == OlivePlanOps::Event || Scan.Op == OlivePlanOps::CustomEvent))
                {
                    bThisEventIsNearest = false;
                    break;
                }
            }
            if (!bThisEventIsNearest)
            {
                continue;
            }
            // Assign to this event
            StepToEventMap.Add(Step.StepId, EventId);
        }

        // Now chain within this event's scope
        if (HasIncomingExec.Contains(Step.StepId))
        {
            PreviousImpureStepId = Step.StepId;
            continue;
        }

        if (!PreviousImpureStepId.IsEmpty())
        {
            Step.ExecAfter = PreviousImpureStepId;
            InferredCount++;

            FOliveResolverNote Note;
            Note.Field = FString::Printf(TEXT("step '%s' exec_after"), *Step.StepId);
            Note.OriginalValue = TEXT("(empty)");
            Note.ResolvedValue = PreviousImpureStepId;
            Note.Reason = FString::Printf(
                TEXT("Orphaned impure step inferred to follow '%s' within event '%s' chain."),
                *PreviousImpureStepId, *EventId);
            OutNotes.Add(MoveTemp(Note));
        }

        PreviousImpureStepId = Step.StepId;
    }
}
```

### Simplification Alternative

The above graph-aware approach is thorough but complex. If the coder judges it too heavyweight, a simpler fix that addresses the primary failure case is acceptable:

**Minimal fix (3 changes to existing code):**

1. Remove empty strings from `HasIncomingExec` (1 line after line 2357):
```cpp
HasIncomingExec.Remove(TEXT(""));
```

2. Track `CurrentEventId` alongside `PreviousImpureStepId` (add variable at line 2360):
```cpp
FString CurrentEventId;
```

3. When an event resets `PreviousImpureStepId`, also set `CurrentEventId` (modify lines 2371-2375):
```cpp
if (Step.Op == OlivePlanOps::Event || Step.Op == OlivePlanOps::CustomEvent)
{
    PreviousImpureStepId = Step.StepId;
    CurrentEventId = Step.StepId;
    continue;
}
```

This minimal fix does not handle interleaved step ordering, but it does handle:
- Empty strings from Issue 2 polluting HasIncomingExec
- Proper event boundary tracking (already works in current code)

The `CurrentEventId` is for logging only in the minimal fix -- it makes the resolver notes more informative.

### Recommended Approach

Go with the **minimal fix** for now. The interleaved step ordering case is theoretical -- AIs almost always list steps in flow order within a plan. The minimal fix resolves the immediate Issue 2 interaction and costs 3 lines of code.

If interleaved ordering is observed in future logs, escalate to the full graph-aware approach.

---

## Implementation Order

### Order: Issue 2 first, then Issue 1, then Issue 3

**Rationale:**
- Issue 2 is the root cause of much of the observed failure cascade. Fixing it first prevents the cascading data loss that makes Issues 1 and 3 appear worse than they are.
- Issue 1 is the highest-impact fix for wiring success rate. It unblocks Object/Actor/Interface IS-A relationships.
- Issue 3 depends on Issue 2 being fixed (the `HasIncomingExec` empty string problem is caused by Issue 2's data loss).

### Task Breakdown

**Task 1: Fix JsonToStringMap and exec_after parsing (Issue 2)**

File: `Source/OliveAIRuntime/Private/IR/BlueprintPlanIR.cpp`

Changes:
- Replace `JsonToStringMap` (lines 48-59) with the version that handles Array, Number, Boolean types via `TryGetString` + Array fallback
- Add exec_after Array fallback after line 140
- Verify `LogOliveAI` log category is available (it is declared in `OliveAIRuntime` module header)

Estimated: ~30 lines changed, 0 new files

**Task 2: Add schema-based type compatibility (Issue 1)**

File: `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`

Changes:
- Site A (line 1731-1738): Replace exact pin comparison with `ArePinTypesCompatible`. Hoist `GetDefault<UEdGraphSchema_K2>()` before the while loop at line 1703.
- Site B (line 2130-2131): Replace exact pin comparison with `ArePinTypesCompatible`. Hoist `GetDefault<UEdGraphSchema_K2>()` before the while loop at line 2118.
- Site C (after line 2278): Add schema-based fallback block when `FindTypeCompatibleOutput` returns nullptr, using real `UEdGraphPin*` objects from context.

No header changes needed. No new includes needed (`EdGraphSchema_K2.h` already included at line 28).

Estimated: ~45 lines added, ~10 lines replaced

**Task 3: Harden InferMissingExecChain (Issue 3)**

File: `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`

Changes (minimal fix):
- Add `HasIncomingExec.Remove(TEXT(""));` after line 2357
- Add `FString CurrentEventId;` at line 2360
- Modify event handling at lines 2371-2375 to set `CurrentEventId`
- Update resolver note reason string to include `CurrentEventId`

Estimated: ~5 lines changed

### No New Error Codes

None of these fixes introduce new error codes. They improve existing operations to succeed where they previously failed.

### No Header Changes

All changes are to `.cpp` files. No public API changes.

### Testing

After implementation, test with a plan that has:
1. A Cast node followed by a function call using the cast output as input via `@cast.auto` -- this should now wire successfully (Issue 1)
2. An AI response containing `"exec_outputs": {"True": ["step_a"]}` -- should coerce and log warning instead of silently dropping (Issue 2)
3. A two-event plan with minimal explicit wiring -- both event chains should wire independently (Issue 3)
