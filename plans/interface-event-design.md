# Interface Event Resolution Design

## Problem

When the AI writes `op: "event", target: "Interact"` on a Blueprint that implements `BPI_Interactable`, the plan pipeline fails. Neither `ResolveEventOp` (resolver) nor `CreateEventNode` (node factory) searches `ImplementedInterfaces`. The event name falls through every detection path and errors out with "Event 'Interact' not found in parent class, as a component delegate, or as an Enhanced Input Action."

Interface functions with no output parameters are implemented as `UK2Node_Event` nodes in the EventGraph (not in their own function graph). This is determined by `UEdGraphSchema_K2::FunctionCanBePlacedAsEvent()`. The resolver and factory must handle this case.

## Scope

4 changes across 3 files. No new files. No Build.cs changes. No IR struct changes.

---

## Change 1: New Field on FOliveResolvedStep

**File:** `Source/OliveAIEditor/Blueprint/Public/Plan/OliveBlueprintPlanResolver.h`

Add one new field to `FOliveResolvedStep` after the existing `bIsInterfaceCall` field (line 134):

```cpp
/**
 * Whether this event step resolved to an interface event.
 * When true, the executor should check for existing interface event nodes
 * via FindOverrideForFunction with the interface class, and CreateEventNode
 * should use SetFromField<UFunction> with bIsSelfContext=false.
 * Set by ResolveEventOp when a matching interface function is found.
 */
bool bIsInterfaceEvent = false;
```

**Rationale:** The resolver already tags interface *calls* with `bIsInterfaceCall`. Adding a parallel `bIsInterfaceEvent` flag keeps the pattern consistent. The factory needs this to choose between `SetExternalMember(Name, ParentClass)` (native events) and `SetFromField<UFunction>(Func, false)` (interface events). It also allows the executor's reuse detection to search with the correct class.

The `interface_class` information is also needed. This goes in the `Properties` map (not a new struct field) since it is a string that the factory consumes. See Change 2.

---

## Change 2: ResolveEventOp Interface Search

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`

**Insertion point:** Between line 2128 (end of the SCS delegate block's closing brace `}`) and line 2130 (the native-event pass-through `Out.Properties.Add(TEXT("event_name"), ResolvedEventName);`).

### New Code Block (pseudocode)

```
// ----------------------------------------------------------------
// Interface event detection: no-output interface functions are
// implemented as UK2Node_Event in EventGraph (not as function
// graphs). Search ImplementedInterfaces for a function matching
// the target name that passes FunctionCanBePlacedAsEvent.
// ----------------------------------------------------------------
if (!bIsNativeEvent && BP)
{
    for (const FBPInterfaceDescription& InterfaceDesc : BP->ImplementedInterfaces)
    {
        if (!InterfaceDesc.Interface) continue;

        // For Blueprint Interfaces, functions live on SkeletonGeneratedClass.
        // For native C++ interfaces, they live on InterfaceDesc.Interface directly.
        const UClass* SearchClass = InterfaceDesc.Interface;
        if (InterfaceDesc.Interface->ClassGeneratedBy)
        {
            UBlueprint* InterfaceBP = Cast<UBlueprint>(InterfaceDesc.Interface->ClassGeneratedBy);
            if (InterfaceBP && InterfaceBP->SkeletonGeneratedClass)
            {
                SearchClass = InterfaceBP->SkeletonGeneratedClass;
            }
        }

        UFunction* InterfaceFunc = SearchClass->FindFunctionByName(FName(*Step.Target));
        if (!InterfaceFunc) continue;

        // Only match if this function can be placed as an event (no outputs).
        // Interface functions WITH outputs are implemented as function graphs,
        // not events. Those are handled by graph_target, not op: "event".
        if (!UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(InterfaceFunc))
        {
            UE_LOG(LogOlivePlanResolver, Log,
                TEXT("Step '%s': Interface function '%s' on '%s' has outputs -- "
                     "cannot be placed as event. Use graph_target instead."),
                *Step.StepId, *Step.Target, *InterfaceDesc.Interface->GetName());
            continue;
        }

        // Found a valid interface event.
        Out.Properties.Add(TEXT("event_name"), InterfaceFunc->GetName());
        Out.Properties.Add(TEXT("interface_class"), InterfaceDesc.Interface->GetPathName());
        Out.bIsInterfaceEvent = true;

        Out.ResolverNotes.Add(FOliveResolverNote{
            TEXT("event_type"),
            FString::Printf(TEXT("event: %s"), *Step.Target),
            FString::Printf(TEXT("interface_event: %s on %s"),
                *InterfaceFunc->GetName(), *InterfaceDesc.Interface->GetName()),
            TEXT("Detected as interface event (no-output interface function placed as UK2Node_Event)")
        });

        UE_LOG(LogOlivePlanResolver, Log,
            TEXT("Step '%s': Resolved as interface event '%s' on interface '%s'"),
            *Step.StepId, *InterfaceFunc->GetName(), *InterfaceDesc.Interface->GetName());

        return true;
    }
}

// (existing pass-through code continues below)
```

### Key Decisions

1. **Position after SCS scan, before pass-through.** This respects the priority order: native events > SCS delegates > interface events > unresolved pass-through. A component delegate named "Interact" on an SCS component would take precedence over an interface function "Interact" (reasonable because component delegates are more specific).

2. **`InterfaceDesc.Interface->GetPathName()` stored in Properties.** The factory needs to look up the interface class to call `FindOverrideForFunction` and `SetFromField<UFunction>`. A full path name is unambiguous and loadable via `FindObject<UClass>`.

3. **First matching interface wins.** If two interfaces both define `Interact`, the first one in `ImplementedInterfaces` is used. This matches UE's own iteration order in `ConformImplementedInterfaces`.

4. **No `BlueprintEditorUtils.h` include needed.** The resolver only uses `FBPInterfaceDescription`, `UEdGraphSchema_K2::FunctionCanBePlacedAsEvent()`, and `UFunction::FindFunctionByName()`. All are already available from existing includes (`Engine/Blueprint.h` provides `FBPInterfaceDescription`, `EdGraphSchema_K2.h` provides `FunctionCanBePlacedAsEvent`).

5. **SkeletonGeneratedClass access for BPI.** The research confirmed that Blueprint Interface functions live on `SkeletonGeneratedClass`, not on `InterfaceDesc.Interface` directly. Native C++ interface classes have no `ClassGeneratedBy`, so we fall through to using `InterfaceDesc.Interface` directly (which is the correct class for native interfaces).

---

## Change 3: CreateEventNode Interface Fallback

**File:** `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp`

**Insertion point:** Inside `CreateEventNode()`, between the parent class `FindFunctionByName` failure (line 402 `if (!EventFunction)`) and the existing SCS delegate scan (line 411). The new block sits at the top of the `if (!EventFunction)` branch.

### Current Structure (lines 401-532)

```
EventFunction = ParentClass->FindFunctionByName(EventName);  // line 401
if (!EventFunction)                                           // line 402
{
    // SCS delegate scan                                      // lines 411-481
    // Enhanced Input Action check                            // lines 488-518
    // Error: not found                                       // lines 521-531
    return nullptr;
}
// Duplicate check + create native event                      // lines 534-558
```

### New Structure

```
EventFunction = ParentClass->FindFunctionByName(EventName);  // line 401
if (!EventFunction)                                           // line 402
{
    // NEW: Interface event check                             // INSERT HERE
    // SCS delegate scan                                      // existing
    // Enhanced Input Action check                            // existing
    // Error: not found (updated message)                     // existing
    return nullptr;
}
// Duplicate check + create native event                      // existing
```

### New Code Block (pseudocode)

```cpp
// ----------------------------------------------------------------
// Check 1b: Interface event -- a no-output function defined on
// an interface this Blueprint implements. These are placed as
// UK2Node_Event in EventGraph with bOverrideFunction = true
// and EventReference pointing to the interface class.
// ----------------------------------------------------------------
{
    // Check if the resolver already tagged this as an interface event
    const FString* InterfaceClassPath = Properties.Find(TEXT("interface_class"));

    UClass* InterfaceClass = nullptr;
    UFunction* InterfaceFunc = nullptr;

    if (InterfaceClassPath && !InterfaceClassPath->IsEmpty())
    {
        // Fast path: resolver already did the search
        InterfaceClass = FindObject<UClass>(nullptr, **InterfaceClassPath);
        if (InterfaceClass)
        {
            // Resolve the function on the correct class (skeleton for BPI)
            const UClass* SearchClass = InterfaceClass;
            if (InterfaceClass->ClassGeneratedBy)
            {
                if (UBlueprint* IBP = Cast<UBlueprint>(InterfaceClass->ClassGeneratedBy))
                {
                    if (IBP->SkeletonGeneratedClass)
                        SearchClass = IBP->SkeletonGeneratedClass;
                }
            }
            InterfaceFunc = SearchClass->FindFunctionByName(EventName);
        }
    }
    else
    {
        // Slow path: factory invoked directly (not via plan pipeline).
        // Search all implemented interfaces.
        for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
        {
            if (!InterfaceDesc.Interface) continue;

            const UClass* SearchClass = InterfaceDesc.Interface;
            if (InterfaceDesc.Interface->ClassGeneratedBy)
            {
                if (UBlueprint* IBP = Cast<UBlueprint>(InterfaceDesc.Interface->ClassGeneratedBy))
                {
                    if (IBP->SkeletonGeneratedClass)
                        SearchClass = IBP->SkeletonGeneratedClass;
                }
            }

            UFunction* Func = SearchClass->FindFunctionByName(EventName);
            if (Func && UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(Func))
            {
                InterfaceClass = InterfaceDesc.Interface;
                InterfaceFunc = Func;
                break;
            }
        }
    }

    if (InterfaceClass && InterfaceFunc)
    {
        // Duplicate check: use FindOverrideForFunction with the interface class
        UK2Node_Event* ExistingInterfaceEvent = FBlueprintEditorUtils::FindOverrideForFunction(
            Blueprint, InterfaceClass, EventName);

        if (ExistingInterfaceEvent)
        {
            UE_LOG(LogOliveNodeFactory, Warning,
                TEXT("CreateEventNode: interface event '%s' (from %s) already exists at (%d, %d)"),
                *EventNamePtr, *InterfaceClass->GetName(),
                ExistingInterfaceEvent->NodePosX, ExistingInterfaceEvent->NodePosY);
            LastError = FString::Printf(
                TEXT("Interface event '%s' already exists in this Blueprint (node at %d, %d). "
                     "Each interface event can only appear once."),
                *EventNamePtr, ExistingInterfaceEvent->NodePosX, ExistingInterfaceEvent->NodePosY);
            return nullptr;
        }

        // Create the interface event node
        UK2Node_Event* InterfaceEventNode = NewObject<UK2Node_Event>(Graph);
        InterfaceEventNode->EventReference.SetFromField<UFunction>(InterfaceFunc, /*bIsSelfContext=*/false);
        InterfaceEventNode->bOverrideFunction = true;
        InterfaceEventNode->AllocateDefaultPins();
        Graph->AddNode(InterfaceEventNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

        UE_LOG(LogOliveNodeFactory, Log,
            TEXT("CreateEventNode: created interface event '%s' from interface '%s'"),
            *EventNamePtr, *InterfaceClass->GetName());

        return InterfaceEventNode;
    }
}
```

### Key Decisions

1. **Two paths: fast (resolver-tagged) and slow (direct factory call).** The plan pipeline goes through the resolver first, so `interface_class` will already be in Properties. But `CreateEventNode` can also be called directly from `blueprint.add_node` (which bypasses the resolver), so the factory must be self-sufficient with its own interface scan.

2. **`SetFromField<UFunction>(InterfaceFunc, false)` for creation.** This is the canonical engine path (used by `UBlueprintEventNodeSpawner`). The `false` parameter means "not self context" -- the EventReference will point to the interface class, not `Self`. This correctly sets `MemberGuid` for rename resilience with Blueprint Interfaces.

3. **`FindOverrideForFunction(Blueprint, InterfaceClass, EventName)` for duplicate detection.** This matches by `bOverrideFunction == true` AND `EventReference.GetMemberParentClass()` being a child of `InterfaceClass`. This is the correct check -- passing `ParentClass` instead would miss interface events.

4. **Position before SCS scan.** Interface events are more specific than SCS delegate guessing. If the AI writes `op: "event", target: "Interact"`, and "Interact" is an interface function, that should take priority over an SCS delegate that might fuzzy-match. In the resolver, SCS was checked first because SCS matching includes fuzzy "On" prefix logic. In the factory, the resolver has already done its work (if coming from the plan pipeline), so the interface check here is purely a fallback for direct factory calls.

5. **No new include needed.** `BlueprintEditorUtils.h` is already included at line 51 of `OliveNodeFactory.cpp`. `FBPInterfaceDescription` comes from `Engine/Blueprint.h` (already included). `FunctionCanBePlacedAsEvent` comes from `EdGraphSchema_K2.h` (already included).

---

## Change 4: Executor Reuse Detection for Interface Events

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`

**Insertion point:** Inside `PhaseCreateNodes`, in the existing event reuse check block (lines 346-392). The existing `FindExistingEventNode` at line 357 only searches with `ParentClass` as the signature class. We need to also search with the interface class.

### Current Code (lines 740-755 in FindExistingEventNode)

```cpp
else  // not custom event
{
    if (Blueprint->ParentClass)
    {
        const FName EventFName(*EventName);
        UK2Node_Event* ExistingEvent = FBlueprintEditorUtils::FindOverrideForFunction(
            Blueprint, Blueprint->ParentClass, EventFName);

        if (ExistingEvent && ExistingEvent->GetGraph() == Graph)
        {
            return ExistingEvent;
        }
    }
}
```

### Modified Code

```cpp
else  // not custom event
{
    const FName EventFName(*EventName);

    // 1. Check parent class (native event overrides like ReceiveBeginPlay)
    if (Blueprint->ParentClass)
    {
        UK2Node_Event* ExistingEvent = FBlueprintEditorUtils::FindOverrideForFunction(
            Blueprint, Blueprint->ParentClass, EventFName);

        if (ExistingEvent && ExistingEvent->GetGraph() == Graph)
        {
            return ExistingEvent;
        }
    }

    // 2. Check implemented interfaces (interface events like Interact, Execute)
    for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
    {
        if (!InterfaceDesc.Interface) continue;

        UK2Node_Event* ExistingEvent = FBlueprintEditorUtils::FindOverrideForFunction(
            Blueprint, InterfaceDesc.Interface, EventFName);

        if (ExistingEvent && ExistingEvent->GetGraph() == Graph)
        {
            return ExistingEvent;
        }
    }
}
```

**No header changes needed.** The `FindExistingEventNode` signature already takes `UBlueprint*` and the `bIsCustomEvent` flag. We are only changing the implementation to search more classes internally.

**No new `FindExistingInterfaceEventNode` method needed.** The existing `FindExistingEventNode` is the correct place for this. The method's contract is "find an existing event node matching this name" -- interface events are just another category of event.

This is important because the PhaseCreateNodes caller (lines 346-392) already calls `FindExistingEventNode` for all `OliveNodeTypes::Event` resolved steps, including those with `bIsInterfaceEvent = true` since the NodeType is still `Event`. The fix is self-contained in the Find method.

---

## Edge Cases

### Interface function WITH outputs

`FunctionCanBePlacedAsEvent()` returns `false`. The resolver's interface scan `continue`s past it. The function falls through to the existing pass-through path. The AI should use `graph_target: "FunctionName"` instead, which routes through the function graph path (already handled by the existing `FOliveGraphContext`).

No special error message is needed here -- the pass-through path will attempt to create a native event, which will fail with the standard "not found" error. The resolver note (logged but not blocking) mentions "has outputs -- cannot be placed as event" which helps debugging.

### Parent class AND interface define the same function name

In the resolver: the native event map is checked first (line 1996). If the name is in the map (e.g., "Tick"), it resolves as a native event before reaching the interface scan. If it is NOT in the map, and `ParentClass->FindFunctionByName()` would succeed in the factory, the factory's parent class check at line 401 runs first and finds it.

In the factory: the parent class `FindFunctionByName` (line 401) runs before the interface check. Parent class wins.

This is correct. Parent class overrides are more specific than interface implementations.

### Multiple interfaces define the same function name

First match wins in both resolver and factory (first in `ImplementedInterfaces` array). This matches UE's own behavior in `ConformImplementedInterfaces`.

### Blueprint Interface vs native C++ interface

Handled by the `ClassGeneratedBy` check:
- **Blueprint Interface** (e.g., `BPI_Interactable`): `InterfaceDesc.Interface->ClassGeneratedBy` is non-null. We dereference to `SkeletonGeneratedClass` to find the function.
- **Native C++ interface** (e.g., `IGameplayTagAssetInterface`): `ClassGeneratedBy` is null. We search `InterfaceDesc.Interface` directly.

### Event name does not match any interface function

The interface scan finds nothing, falls through to the existing SCS delegate scan, then Enhanced Input Action check, then the error path. Existing behavior is preserved exactly.

### Plan with `bIsInterfaceEvent = true` encounters existing interface event

The executor's `FindExistingEventNode` (Change 4) finds the existing node and reuses it, adding it to `ReusedStepIds`. This is the same behavior as native event reuse and component bound event reuse.

### Direct `blueprint.add_node` call (not through plan pipeline)

The factory's slow path (Change 3) handles this. No resolver tag needed. The factory searches `ImplementedInterfaces` itself.

---

## Risk Assessment

**Overall risk: LOW**

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Interface function found incorrectly (false positive) | Very Low | Medium | `FunctionCanBePlacedAsEvent` gate ensures only no-output functions match |
| Existing interface event not detected (duplicate created) | Low | Low | `FindOverrideForFunction` is the engine's own check; if it misses, UE's compile step catches it |
| `SkeletonGeneratedClass` is null | Low | Medium | Guarded by null check; falls through to pass-through path |
| Breaks existing native event resolution | Very Low | High | Interface scan only runs AFTER native event map and SCS delegate checks fail; no existing code paths are modified, only new branches added |
| Performance: scanning all interfaces for every non-native event | Very Low | None | `ImplementedInterfaces` is typically 0-3 entries; `FindFunctionByName` is O(1) hash lookup |

---

## Files Changed Summary

| File | Change | Lines Affected |
|------|--------|---------------|
| `Source/OliveAIEditor/Blueprint/Public/Plan/OliveBlueprintPlanResolver.h` | Add `bIsInterfaceEvent` field | ~1 field after line 134 |
| `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp` | Interface event detection in `ResolveEventOp` | Insert ~35 lines between lines 2128 and 2130 |
| `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp` | Interface event fallback + duplicate check in `CreateEventNode` | Insert ~55 lines between lines 402 and 411 |
| `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp` | Interface search in `FindExistingEventNode` | Modify lines 740-755, add ~10 lines |

**No new files. No new error codes. No Build.cs changes. No include changes.**

---

## Implementation Order

1. **Change 1** (header field) -- 2 minutes. Must be first because Change 2 references `bIsInterfaceEvent`.
2. **Change 2** (resolver) -- 10 minutes. This is the primary fix. After this, the plan pipeline tags interface events correctly.
3. **Change 3** (factory) -- 15 minutes. This makes `CreateEventNode` actually create the node. Has two code paths (fast + slow) so slightly more complex.
4. **Change 4** (executor reuse) -- 5 minutes. Small change inside `FindExistingEventNode`. Prevents duplicate creation when a plan references an interface event that already exists.

**Test scenario:** Create a Blueprint Interface (`BPI_Interactable`) with a no-output function `Interact`. Create an Actor Blueprint that implements `BPI_Interactable`. Run `blueprint.apply_plan_json` with:

```json
{
  "graph_target": "EventGraph",
  "steps": [
    {"step_id": "evt", "op": "event", "target": "Interact"},
    {"step_id": "s1", "op": "call", "target": "PrintString", "inputs": {"InString": "Interacted!"}, "exec_after": "evt"}
  ]
}
```

**Expected result:** A `UK2Node_Event` node appears in EventGraph with `EventReference` pointing to `BPI_Interactable::Interact`, `bOverrideFunction = true`, and the PrintString node wired to its exec output.

**Second run expected result:** The same plan reuses the existing interface event node instead of creating a duplicate.
